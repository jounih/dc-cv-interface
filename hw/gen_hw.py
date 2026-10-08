#!/usr/bin/env python3
"""Single source of truth for the CV interface hardware.

Generates from the tables below:
  hw/netlist.csv            every component pin -> net (4-out board; 8-out adds U4b + channels 5-8)
  hw/bom.csv                parts, part numbers, approximate GBP prices, suppliers
  hw/schematic/*.svg        schematic sheets (needs `pip install schemdraw`; optional)

    python3 hw/gen_hw.py            # tables + SVG (if schemdraw is importable)
"""
import csv, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
N_OUT, N_IN = 4, 4

# ------------------------------------------------------------------ netlist
# (ref, value, footprint, {pin: net})
parts = []
def part(ref, value, fp, pins):
    parts.append((ref, value, fp, pins))

# Power entry: Eurorack 10-pin (2x5) header, red stripe = -12 V (pins 1-2).
part("J1", "Eurorack 2x5 shrouded header", "IDC 2x5 2.54 mm",
     {"1": "-12V_IN", "2": "-12V_IN", "3": "GND", "4": "GND", "5": "GND", "6": "GND",
      "7": "GND", "8": "GND", "9": "+12V_IN", "10": "+12V_IN"})
part("D1", "SS14", "SMA / DO-214AC", {"A": "+12V_IN", "K": "+12V"})       # reverse-polarity block
part("D2", "SS14", "SMA / DO-214AC", {"A": "-12V", "K": "-12V_IN"})
part("C1", "10uF 25V", "0805 X5R / radial", {"1": "+12V", "2": "GND"})
part("C2", "10uF 25V", "0805 X5R / radial", {"1": "GND", "2": "-12V"})

# Pico 2: USB powered. Only the pins used are listed.
part("U1", "Raspberry Pi Pico 2 (RP2350)", "Pico module",
     {"4": "DAC_SCLK", "5": "DAC_SYNC", "6": "DAC_DIN",
      "14": "ADC_SCLK", "15": "ADC_CS", "16": "ADC_DOUT", "17": "ADC_DRDY",
      "19": "ADC_DIN", "20": "ADC_RESET", "27": "ADC_CLKIN",
      "36": "+3V3_D", "40": "VBUS",
      "3": "GND", "8": "GND", "13": "GND", "18": "GND", "23": "GND", "28": "GND", "33": "GND", "38": "GND"})

# Analog 3.3 V for the converters, from USB VBUS (same power domain as the Pico).
part("U5", "MCP1700-3302E (LDO 3.3 V)", "TO-92 (breadboard) / SOT-23",
     {"IN": "VBUS", "GND": "GND", "OUT": "+3V3_A"})
part("C3", "1uF", "0805", {"1": "VBUS", "2": "GND"})
part("C4", "1uF", "0805", {"1": "+3V3_A", "2": "GND"})

# DAC8568BIPW: 8 x 16-bit, midscale power-on reset (B grade), internal 2.5 V ref off by default.
part("U2", "DAC8568BIPW", "TSSOP-16",
     {"1": "+3V3_A",        # LDAC tied high: outputs change only on software update
      "2": "DAC_SYNC", "3": "+3V3_A", "4": "DAC_OUT1", "5": "DAC_OUT3", "6": "DAC_OUT5",
      "7": "DAC_OUT7", "8": "VREF", "9": "+3V3_A",   # CLR tied high (a CLR edge would go to zero-scale = +10 V)
      "10": "DAC_OUT8", "11": "DAC_OUT6", "12": "DAC_OUT4", "13": "DAC_OUT2",
      "14": "GND", "15": "DAC_DIN", "16": "DAC_SCLK"})
part("C5", "100nF", "0805", {"1": "+3V3_A", "2": "GND"})
part("C6", "1uF", "0805", {"1": "+3V3_A", "2": "GND"})
part("C7", "150nF C0G/X7R", "0805", {"1": "VREF", "2": "GND"})        # datasheet: >= 150 nF on VREFOUT
part("R1", "10k", "0805", {"1": "DAC_SYNC", "2": "+3V3_D"})            # SYNC idle high during Pico reset

# Ratiometric offset node Vb = VREF * 8.06/(10+8.06) = 1.116 V. Keep CB small:
# a big cap here makes Vb lag the DAC at reference turn-on (SPICE: 5.8 V blip at 10 nF).
part("R2", "10k 0.1%", "0805", {"1": "VREF", "2": "VB"})
part("R3", "8.06k 0.1%", "0805", {"1": "VB", "2": "GND"})
part("C8", "100pF C0G", "0805", {"1": "VB", "2": "GND"})

# OPA4172ID quad: output stages 1-4.
amp_pins = {1: ("1", "2", "3"), 2: ("7", "6", "5"), 3: ("8", "9", "10"), 4: ("14", "13", "12")}  # out, -in, +in
u4 = {"4": "+12V", "11": "-12V"}
for ch, (o, m, p) in amp_pins.items():
    u4.update({o: f"OPO{ch}", m: f"SUM{ch}", p: "VB"})
part("U4", "OPA4172ID", "SOIC-14", u4)
part("C9", "100nF", "0805", {"1": "+12V", "2": "GND"})
part("C10", "100nF", "0805", {"1": "GND", "2": "-12V"})
for ch in range(1, N_OUT + 1):
    part(f"R1{ch}", "10k 0.1%", "0805", {"1": f"DAC_OUT{ch}", "2": f"SUM{ch}"})
    part(f"RF{ch}", "82.5k 0.1%", "0805", {"1": f"SUM{ch}", "2": f"OPO{ch}"})
    part(f"CF{ch}", "47pF C0G", "0805", {"1": f"SUM{ch}", "2": f"OPO{ch}"})
    part(f"DC{ch}", "BAT54S", "SOT-23", {"1": "-12V", "2": "+12V", "3": f"OPO{ch}"})
    part(f"RS{ch}", "1k 0.66 W anti-surge", "1206", {"1": f"OPO{ch}", "2": f"OUT{ch}"})
    part(f"JO{ch}", "Thonkiconn PJ398SM", "3.5 mm jack", {"T": f"OUT{ch}", "S": "GND", "TN": "NC"})

# ADS131M04IPW: 4 simultaneous 24-bit inputs, +-1.2 V around AGND from a single 3.3 V.
part("U3", "ADS131M04IPW", "TSSOP-20",
     {"1": "+3V3_A", "2": "GND", "3": "AIN1", "4": "GND", "5": "GND", "6": "AIN2",
      "7": "AIN3", "8": "GND", "9": "GND", "10": "AIN4",
      "11": "ADC_RESET", "12": "ADC_CS", "13": "ADC_DRDY", "14": "ADC_SCLK", "15": "ADC_DOUT",
      "16": "ADC_DIN", "17": "ADC_CLKIN", "18": "ADC_CAP", "19": "GND", "20": "+3V3_D"})
part("C11", "1uF", "0805", {"1": "+3V3_A", "2": "GND"})
part("C12", "1uF", "0805", {"1": "+3V3_D", "2": "GND"})
part("C13", "220nF", "0805", {"1": "ADC_CAP", "2": "GND"})
part("R4", "10k", "0805", {"1": "ADC_CS", "2": "+3V3_D"})
for ch in range(1, N_IN + 1):
    part(f"JI{ch}", "Thonkiconn PJ398SM", "3.5 mm jack", {"T": f"IN{ch}", "S": "GND", "TN": "GND"})
    part(f"RA{ch}", "49.9k 1%", "0805", {"1": f"IN{ch}", "2": f"INM{ch}"})
    part(f"RB{ch}", "49.9k 1%", "0805", {"1": f"INM{ch}", "2": f"AIN{ch}"})
    part(f"RC{ch}", "9.09k 1%", "0805", {"1": f"AIN{ch}", "2": "GND"})
    part(f"CA{ch}", "330pF C0G", "0805", {"1": f"AIN{ch}", "2": "GND"})

def write_netlist():
    with open(os.path.join(HERE, "netlist.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ref", "value", "footprint", "pin", "net"])
        for ref, val, fp, pins in parts:
            for pin, net in pins.items():
                w.writerow([ref, val, fp, pin, net])
    # sanity: every signal net has >= 2 pins
    nets = {}
    for ref, _, _, pins in parts:
        for pin, net in pins.items():
            nets.setdefault(net, []).append(f"{ref}.{pin}")
    single = [n for n, p in nets.items() if len(p) < 2 and n != "NC" and not n.startswith("DAC_OUT")]
    return nets, single

# ------------------------------------------------------------------ BOM
# (group, qty_4out, qty_extra_for_8out, description, manufacturer part, approx GBP each, where)
BOM = [
    ("core", 1, 0, "Raspberry Pi Pico 2 (RP2350, with headers for breadboard)", "Raspberry Pi SC1631 / SC1632 (H)", 4.80, "The Pi Hut, Pimoroni, Farnell"),
    ("core", 1, 0, "16-bit 8-ch DAC, midscale reset, int. ref (U2)", "TI DAC8568BIPW", 14.50, "Mouser UK, DigiKey UK, Farnell"),
    ("core", 1, 0, "24-bit 4-ch simultaneous delta-sigma ADC (U3)", "TI ADS131M04IPWR", 4.60, "Mouser UK, DigiKey UK, LCSC"),
    ("core", 1, 1, "Quad RRO op-amp, 36 V (U4)", "TI OPA4172IDR", 3.20, "Mouser UK, DigiKey UK, LCSC"),
    ("power", 1, 0, "3.3 V LDO, 250 mA (U5)", "Microchip MCP1700-3302E/TO", 0.40, "Mouser UK, Farnell, Rapid"),
    ("power", 2, 0, "Schottky 1 A 40 V, rail reverse-polarity block", "SS14 (or 1N5819 through-hole)", 0.10, "LCSC, Tayda, Rapid"),
    ("power", 1, 0, "Eurorack 10-pin shrouded header + 10-to-16 ribbon", "2x5 IDC box header; Thonk/Tayda ribbon", 1.20, "Thonk, Tayda"),
    ("protect", 4, 4, "Dual Schottky clamp per output (to +-12 V)", "Nexperia BAT54S", 0.10, "LCSC, Mouser"),
    ("protect", 4, 4, "1 k series output resistor, 0.66 W anti-surge 1206", "Panasonic ERJ-P08J102V", 0.15, "Mouser, DigiKey"),
    ("analog", 5, 4, "10 k 0.1 % 25 ppm (R1x gain set + Vb top)", "Panasonic ERA-6AEB103V", 0.30, "Mouser, DigiKey"),
    ("analog", 4, 4, "82.5 k 0.1 % 25 ppm (RFx)", "Panasonic ERA-6AEB8252V", 0.30, "Mouser, DigiKey"),
    ("analog", 1, 0, "8.06 k 0.1 % (Vb bottom)", "Panasonic ERA-6AEB8061V", 0.30, "Mouser, DigiKey"),
    ("analog", 8, 0, "49.9 k 1 % 50 ppm thin film (input series, 2 per input; calibrated)", "Yageo RT0805FRE0749K9L", 0.05, "Mouser, LCSC"),
    ("analog", 4, 0, "9.09 k 1 % 50 ppm thin film (input shunt; calibrated)", "Yageo RT0805FRE079K09L", 0.05, "Mouser, LCSC"),
    ("analog", 4, 4, "47 pF C0G (output filter, 41 kHz)", "0805 C0G 50 V", 0.05, "LCSC"),
    ("analog", 4, 0, "330 pF C0G (input filter, 59 kHz)", "0805 C0G 50 V", 0.05, "LCSC"),
    ("analog", 1, 0, "100 pF C0G (Vb)", "0805 C0G 50 V", 0.05, "LCSC"),
    ("decoupling", 3, 1, "100 nF X7R", "0805 X7R 50 V", 0.03, "LCSC"),
    ("decoupling", 1, 0, "150 nF X7R (VREF)", "0805 X7R 25 V", 0.05, "LCSC"),
    ("decoupling", 1, 0, "220 nF X7R (ADC CAP)", "0805 X7R 25 V", 0.05, "LCSC"),
    ("decoupling", 5, 0, "1 uF X7R", "0805 X7R 25 V", 0.05, "LCSC"),
    ("decoupling", 2, 0, "10 uF 25 V (rails)", "0805 X5R 25 V or radial electrolytic", 0.10, "LCSC, Tayda"),
    ("misc", 2, 0, "10 k pull-up (DAC SYNC, ADC CS)", "0805 1 %", 0.02, "LCSC"),
    ("jacks", 8, 4, "3.5 mm Eurorack jack", "Thonkiconn PJ398SM", 0.30, "Thonk, Tayda"),
    ("board", 1, 0, "Stripboard / prototype board (2-layer PCB later: 5 boards ~GBP 5 + post)", "-", 2.00, "JLCPCB, PCBWay, Rapid"),
]
BREADBOARD_EXTRAS = [
    ("breadboard", 1, "TSSOP-16 to DIP adapter (DAC8568)", "SOIC/TSSOP-to-DIP adapter, 0.65 mm pitch", 0.80, "Amazon, eBay, Proto-PIC"),
    ("breadboard", 1, "TSSOP-20 to DIP adapter (ADS131M04)", "TSSOP-20 adapter, 0.65 mm pitch", 0.80, "Amazon, eBay, Proto-PIC"),
    ("breadboard", 1, "SOIC-14 to DIP adapter (OPA4172) - or TL074CN DIP (range ~+-9.5 V)", "SOIC-14 adapter", 0.60, "Amazon, eBay"),
    ("breadboard", 1, "Bench supply +-12 V with current limit (or Eurorack PSU via ribbon)", "-", 0.0, "existing"),
]

def write_bom():
    tot4 = tot8 = 0.0
    with open(os.path.join(HERE, "bom.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["group", "qty_4x4", "qty_8x4", "description", "part_number", "approx_gbp_each", "line_gbp_4x4", "where_to_buy"])
        for g, q4, qx, d, pn, gbp, where in BOM:
            q8 = q4 + qx
            tot4 += q4 * gbp
            tot8 += q8 * gbp
            w.writerow([g, q4, q8, d, pn, f"{gbp:.2f}", f"{q4 * gbp:.2f}", where])
        w.writerow([])
        w.writerow(["TOTAL", "", "", "4 out / 4 in board", "", "", f"{tot4:.2f}", "approx, single quantity, ex. shipping"])
        w.writerow(["TOTAL", "", "", "8 out / 4 in board", "", "", f"{tot8:.2f}", ""])
        w.writerow([])
        for g, q, d, pn, gbp, where in BREADBOARD_EXTRAS:
            w.writerow([g, q, q, d, pn, f"{gbp:.2f}", f"{q * gbp:.2f}", where])
    return tot4, tot8

# ------------------------------------------------------------------ schematic SVGs
def draw():
    try:
        import schemdraw
        import schemdraw.elements as elm
    except ImportError:
        print("schemdraw not installed: skipping SVG sheets (pip install schemdraw)")
        return False
    schemdraw.use("svg")
    out = os.path.join(HERE, "schematic")
    os.makedirs(out, exist_ok=True)

    def ic(name, pins_left, pins_right, label):
        p = [elm.IcPin(name=n, pin=num, side="left") for num, n in pins_left]
        p += [elm.IcPin(name=n, pin=num, side="right") for num, n in pins_right]
        return elm.Ic(pins=p, edgepadW=1.2, pinspacing=0.75).label(label, "top")

    # Sheet 1: power
    with schemdraw.Drawing(file=os.path.join(out, "1_power.svg"), show=False) as d:
        d.config(fontsize=11)
        d += elm.Label().at((0, 4)).label("Sheet 1 - power. Rack +-12 V feeds ONLY the op-amps; converters run from USB VBUS.", loc="right")
        # +12 rail
        d += elm.Dot().at((0, 2)).label("J1 pins 9,10  +12V_IN", "left")
        d += elm.Diode().right().label("D1 SS14")
        d += (p12 := elm.Dot().label("+12V", "top"))
        d += elm.Line().right().length(1.5)
        d += elm.Capacitor().down().length(1.5).label("C1 10u", "bottom")
        d += elm.Ground()
        # -12 rail
        d += elm.Dot().at((0, -3)).label("J1 pins 1,2  -12V_IN", "left")
        d += elm.Diode().right().reverse().label("D2 SS14 (anode at rail)")
        d += elm.Dot().label("-12V", "bottom")
        d += elm.Line().right().length(1.5)
        d += elm.Capacitor().up().length(1.5).label("C2 10u", "bottom")
        d += elm.Ground().flip()
        d += elm.Label().at((0, -4.5)).label("J1 pins 3-8 = GND. Red stripe = pin 1 = -12 V. D1/D2 block a reversed ribbon.", loc="right")
        # LDO
        d += elm.Dot().at((0, -6.5)).label("Pico VBUS (pin 40)", "left")
        d += elm.Line().right().length(1)
        d += (ldo := elm.Ic(pins=[elm.IcPin(name="IN", side="left"), elm.IcPin(name="OUT", side="right"),
                                  elm.IcPin(name="GND", side="bottom")], edgepadW=1.0).right().anchor("IN").label("U5 MCP1700-3302", "top"))
        d += elm.Line().right().at(ldo.OUT).length(1.5)
        d += elm.Dot().label("+3V3_A  (DAC8568 AVDD, ADS131M04 AVDD)", "right")
        d += elm.Ground().at(ldo.GND)
        d += elm.Label().at((0, -10)).label("C3 1u on VBUS, C4 1u on +3V3_A. +3V3_D = Pico 3V3 (pin 36): ADC DVDD and pull-ups.", loc="right")
        d += elm.Label().at((0, -11)).label("USB-only option: Mornsun B0512S-1WR3 (5 V -> +-12 V, 1 W) from VBUS feeds +12V/-12V instead of J1.", loc="right")

    # Sheet 2: digital
    with schemdraw.Drawing(file=os.path.join(out, "2_digital.svg"), show=False) as d:
        d.config(fontsize=10)
        pico = ic("Pico2", [("4", "GP2"), ("5", "GP3"), ("6", "GP4"), ("14", "GP10"), ("15", "GP11"),
                            ("16", "GP12"), ("17", "GP13"), ("19", "GP14"), ("20", "GP15"), ("27", "GP21")],
                  [("36", "3V3"), ("40", "VBUS"), ("38", "GND")], "U1 Raspberry Pi Pico 2")
        d += pico
        nets = ["DAC_SCLK", "DAC_SYNC", "DAC_DIN", "ADC_SCLK", "ADC_CS", "ADC_DOUT", "ADC_DRDY", "ADC_DIN", "ADC_RESET", "ADC_CLKIN"]
        for pin, net in zip(["GP2", "GP3", "GP4", "GP10", "GP11", "GP12", "GP13", "GP14", "GP15", "GP21"], nets):
            d += elm.Line().left().at(getattr(pico, pin)).length(0.8)
            d += elm.Label().label(net, loc="left")
        for pin, net in (("3V3", "+3V3_D"), ("VBUS", "VBUS"), ("GND", "GND")):
            d += elm.Line().right().at(getattr(pico, pin)).length(0.8)
            d += elm.Label().label(net, loc="right")
        dac = ic("DAC8568", [("16", "SCLK"), ("2", "SYNC"), ("15", "DIN"), ("1", "LDAC"), ("9", "CLR"), ("3", "AVDD"), ("14", "GND")],
                 [("4", "VOUTA"), ("13", "VOUTB"), ("5", "VOUTC"), ("12", "VOUTD"), ("6", "VOUTE"), ("11", "VOUTF"),
                  ("7", "VOUTG"), ("10", "VOUTH"), ("8", "VREF")], "U2 DAC8568BIPW (TSSOP-16)").at((10, 3)).anchor("SCLK")
        d += dac
        for pin, net in (("SCLK", "DAC_SCLK"), ("SYNC", "DAC_SYNC (10k pull-up)"), ("DIN", "DAC_DIN"),
                         ("LDAC", "+3V3_A"), ("CLR", "+3V3_A (never pulse!)"), ("AVDD", "+3V3_A, 100n+1u"), ("GND", "GND")):
            d += elm.Line().left().at(getattr(dac, pin)).length(0.8)
            d += elm.Label().label(net, loc="left")
        for pin, net in (("VOUTA", "DAC_OUT1"), ("VOUTB", "DAC_OUT2"), ("VOUTC", "DAC_OUT3"), ("VOUTD", "DAC_OUT4"),
                         ("VOUTE", "DAC_OUT5 (8-out)"), ("VOUTF", "DAC_OUT6 (8-out)"), ("VOUTG", "DAC_OUT7 (8-out)"),
                         ("VOUTH", "DAC_OUT8 (8-out)"), ("VREF", "VREF, 150n to GND")):
            d += elm.Line().right().at(getattr(dac, pin)).length(0.8)
            d += elm.Label().label(net, loc="right")
        adc = ic("ADS131M04", [("14", "SCLK"), ("12", "CS"), ("15", "DOUT"), ("13", "DRDY"), ("16", "DIN"), ("11", "SYNC/RST"),
                               ("17", "CLKIN"), ("1", "AVDD"), ("20", "DVDD"), ("18", "CAP"), ("2", "AGND"), ("19", "DGND")],
                 [("3", "AIN0P"), ("4", "AIN0N"), ("6", "AIN1P"), ("5", "AIN1N"), ("7", "AIN2P"), ("8", "AIN2N"),
                  ("10", "AIN3P"), ("9", "AIN3N")], "U3 ADS131M04IPW (TSSOP-20)").at((10, -10)).anchor("SCLK")
        d += adc
        for pin, net in (("SCLK", "ADC_SCLK"), ("CS", "ADC_CS (10k pull-up)"), ("DOUT", "ADC_DOUT"), ("DRDY", "ADC_DRDY"),
                         ("DIN", "ADC_DIN"), ("SYNC/RST", "ADC_RESET"), ("CLKIN", "ADC_CLKIN 6.144 MHz"),
                         ("AVDD", "+3V3_A, 1u"), ("DVDD", "+3V3_D, 1u"), ("CAP", "220n to GND"), ("AGND", "GND"), ("DGND", "GND")):
            d += elm.Line().left().at(getattr(adc, pin)).length(0.8)
            d += elm.Label().label(net, loc="left")
        for pin, net in (("AIN0P", "AIN1"), ("AIN0N", "GND"), ("AIN1P", "AIN2"), ("AIN1N", "GND"),
                         ("AIN2P", "AIN3"), ("AIN2N", "GND"), ("AIN3P", "AIN4"), ("AIN3N", "GND")):
            d += elm.Line().right().at(getattr(adc, pin)).length(0.8)
            d += elm.Label().label(net, loc="right")

    # Sheet 3: output channel (x4 / x8) + shared Vb divider
    with schemdraw.Drawing(file=os.path.join(out, "3_output_stage.svg"), show=False) as d:
        d.config(fontsize=11)
        d += elm.Label().at((0, 4.5)).label("Sheet 3 - output channel n (x4; x8 with a second OPA4172). Vjack = 0.990 * (9.25*Vb - 8.25*Vdac)", loc="right")
        d += (op := elm.Opamp(leads=True).at((6, 0)).label("U4 OPA4172 (1/4)", "bottom", ofst=(0, -0.3)))
        d += elm.Line().left().at(op.in1).length(0.5)
        d += (sumn := elm.Dot().label("SUMn", "bottom"))
        d += elm.Resistor().left().label("R1n 10k 0.1%")
        d += elm.Label().label("DAC_OUTn (0..2.5 V)", loc="left")
        d += elm.Line().up().at(sumn.center).length(1.6)
        d += (fb := elm.Resistor().right().length(4.2).label("RFn 82.5k 0.1%"))
        d += elm.Line().down().toy(op.out)
        d += (o := elm.Dot())
        d += elm.Line().up().at(sumn.center).length(3.0)
        d += elm.Capacitor().right().length(4.2).label("CFn 47p")
        d += elm.Line().down().length(1.4)
        d += elm.Line().right().at(op.out).length(0.3)
        d += elm.Resistor().right().label("RSn 1k 0.66W")
        d += (jk := elm.Dot().label("OUTn jack tip", "right"))
        d += elm.Line().down().at(o.center).length(0.6)
        d += (dc := elm.Dot().label("BAT54S DCn: to +12V and from -12V", "right"))
        d += elm.Line().left().at(op.in2).length(0.5)
        d += elm.Line().down().length(1.2)
        d += elm.Label().label("VB (shared)", loc="bottom")
        # Vb divider
        d += elm.Label().at((0, -5)).label("VREF (DAC8568 pin 8, 150n) -- R2 10k 0.1% -- VB -- R3 8.06k 0.1% -- GND ; C8 100p VB-GND", loc="right")
        d += elm.Label().at((0, -6)).label("Ratiometric: with the reference off (power-on / soft reset) Vdac = Vb = 0 -> 0 V at the jack.", loc="right")
        d += elm.Label().at((0, -7)).label("Keep C8 small: Vb must track VREF as fast as the DAC does (SPICE: 10 nF -> 5.8 V blip, 100 pF -> 5 mV).", loc="right")

    # Sheet 4: input channel
    with schemdraw.Drawing(file=os.path.join(out, "4_input_stage.svg"), show=False) as d:
        d.config(fontsize=11)
        d += elm.Label().at((0, 3)).label("Sheet 4 - input channel n (x4). Zin 108.6 k, +-10 V -> +-0.814 V, ADC FS = +-14.7 V at the jack", loc="right")
        d += elm.Dot().at((0, 0)).label("INn jack tip\n(switch -> GND)", "left")
        d += elm.Resistor().right().label("RAn 49.9k")
        d += elm.Resistor().right().label("RBn 49.9k")
        d += (a := elm.Dot())
        d += elm.Resistor().down().label("RCn 9.09k", "bottom")
        d += elm.Ground()
        d += elm.Line().right().at(a.center).length(2.5)
        d += (c := elm.Dot())
        d += elm.Capacitor().down().label("CAn 330p C0G", "bottom")
        d += elm.Ground()
        d += elm.Line().right().at(c.center).length(2.5)
        d += elm.Label().label("AINn -> ADS131M04 AINxP", loc="right")
        d += elm.Label().at((0, -5.5)).label("AINxN -> GND. No external clamp diodes: 100 k limits a +-24 V fault to 0.22 mA", loc="right")
        d += elm.Label().at((0, -6.3)).label("(ADC abs max 10 mA); a Schottky to GND would clip the -0.8 V signal range.", loc="right")
    return True

if __name__ == "__main__":
    nets, single = write_netlist()
    t4, t8 = write_bom()
    drawn = draw()
    print(f"netlist: {len(parts)} parts, {len(nets)} nets; single-pin nets: {single or 'none'}")
    print(f"bom: 4x4 ~GBP {t4:.2f}, 8x4 ~GBP {t8:.2f}; svg: {'yes' if drawn else 'skipped'}")
    sys.exit(1 if single else 0)
