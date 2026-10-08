"""Schematic sheets for the v2 carrier (schemdraw). Called by gen_hw.py. Connections are
authoritative in netlist.csv; these sheets explain the circuits."""
import os


def draw(out):
    try:
        import schemdraw
        import schemdraw.elements as elm
    except ImportError:
        return False
    schemdraw.use("svg")
    os.makedirs(out, exist_ok=True)

    def ic(pins_left, pins_right, label, at, anchor):
        p = [elm.IcPin(name=n, pin=num, side="left") for num, n in pins_left]
        p += [elm.IcPin(name=n, pin=num, side="right") for num, n in pins_right]
        return elm.Ic(pins=p, edgepadW=1.4, pinspacing=0.7).label(label, "top").at(at).anchor(anchor)

    def stubs(d, part, side, items):
        for pin, net in items:
            d += (elm.Line().left() if side == "left" else elm.Line().right()).at(getattr(part, pin)).length(0.8)
            d += elm.Label().label(net, loc=side)

    # ---------------------------------------------------------------- 1 power (block diagram)
    import schemdraw.flow as flow
    with schemdraw.Drawing(file=os.path.join(out, "1_power.svg"), show=False) as d:
        d.config(fontsize=10)
        def box(x, y, text, w=3.4, h=1.2):
            b = flow.Box(w=w, h=h).label(text).at((x, y)).anchor("center")
            d.add(b)
            return b
        def arrow(a, b):
            d.add(flow.Arrow().at(a).to(b))
        d += elm.Label().at((-1, 3.2)).label("Sheet 1 - power tree. USB-C 5 V is primary; the Eurorack header and the LiPo boost are optional.", loc="right")
        usb = box(0, 1.5, "S3 VBUS\n(USB-C 5 V)")
        bat = box(0, -0.5, "LiPo boost J4\n(optional, 5.1 V)")
        o1 = box(4.5, 1.5, "U20 LM66100\nideal diode")
        o2 = box(4.5, -0.5, "U21 LM66100\nideal diode")
        sys5 = box(9, 0.5, "+5V_SYS\n22 uF", w=2.6)
        dc = box(13.5, 0.5, "L20 10 uH ->\nU22 A0515S-2WR3\n5 V -> +-15 V, 2 W", w=3.8, h=1.6)
        pi = box(18.5, 0.5, "pi filters\n22u-10uH-22u\nSS14 diode-OR", w=3.6, h=1.6)
        ldo = box(23.5, 0.5, "TPS7A4901 +11 V\nTPS7A3001 -11 V\n(op-amp rails)", w=3.8, h=1.6)
        rack = box(18.5, -3, "Eurorack J1 +-12 V\nSS14 per rail\nRACK_SENSE -> GPIO2", w=3.8, h=1.6)
        arrow(usb.E, o1.W); arrow(bat.E, o2.W); arrow(o1.E, sys5.W); arrow(o2.E, sys5.W)
        arrow(sys5.E, dc.W); arrow(dc.E, pi.W); arrow(pi.E, ldo.W); arrow(rack.N, pi.S)
        l1 = box(5, -4, "AMS1117-3.3 -> +3V3_D\ncodec VDD, ADC DVDD", w=4.2)
        l2 = box(9.5, -4, "LP5907-3.3 -> +3V3_A\nDAC8568 / ADS131M08", w=4.2)
        l3 = box(14, -4, "LP5907-4.5 -> +4V5_A\nPCM3168A, input amps", w=4.2)
        arrow(sys5.S, l1.N); arrow(sys5.S, l2.N); arrow(sys5.S, l3.N)
        d += elm.Label().at((-1, -6.2)).label("RAIL_SENSE = +11 V x 10k/110k -> GPIO1: outputs forced to 0 V below 10.5 V and until 100 ms stable.", loc="right")
        d += elm.Label().at((-1, -7.0)).label("Budget, full population: ~0.75 A at 5 V (USB-C 1.5 A or a USB 3 port). Precision-only: ~0.35 A.", loc="right")

    # ---------------------------------------------------------------- 2 controller + converters
    with schemdraw.Drawing(file=os.path.join(out, "2_controller.svg"), show=False) as d:
        d.config(fontsize=9)
        s3 = ic([("1", "GPIO1"), ("2", "GPIO2"), ("3", "GPIO3"), ("10", "GPIO10"), ("11", "GPIO11"), ("12", "GPIO12"),
                 ("13", "GPIO13"), ("16", "GPIO16"), ("17", "GPIO17"), ("18", "GPIO18"), ("21", "GPIO21"),
                 ("43", "GPIO43"), ("44", "GPIO44")], [("5V", "VBUS"), ("BAT", "VBAT"), ("G", "GND")],
                "LilyGO T-Display-S3 (on 2 x 1x12 sockets)", (0, 0), "GPIO1")
        d += s3
        stubs(d, s3, "left", [("GPIO1", "RAIL_SENSE"), ("GPIO2", "RACK_SENSE"), ("GPIO3", "CS_CODEC"), ("GPIO10", "CS_ADC"),
                              ("GPIO11", "SPI_MOSI"), ("GPIO12", "SPI_SCLK"), ("GPIO13", "SPI_MISO"), ("GPIO16", "I2S_MCLK"),
                              ("GPIO17", "I2S_BCLK"), ("GPIO18", "I2S_WS"), ("GPIO21", "I2S_DOUT"), ("GPIO43", "I2S_DIN"),
                              ("GPIO44", "CS_DAC")])
        stubs(d, s3, "right", [("VBUS", "+5V_USB"), ("VBAT", "S3_VBAT"), ("GND", "GND")])
        cod = ic([("41", "SCKI"), ("7", "BCKAD"), ("36", "BCKDA"), ("6", "LRCKAD"), ("35", "LRCKDA"), ("37", "DIN1"),
                  ("8", "DOUT1"), ("42", "MC"), ("43", "MDI"), ("44", "MDO"), ("45", "MS"), ("4", "RST"), ("48", "MODE")],
                 [("31/32", "VOUT1+/-"), ("...", "VOUT2..8"), ("52", "VIN1+"), ("51", "VIN1-"), ("...", "VIN2..6"),
                  ("1", "VCOMAD"), ("15", "VCOMDA"), ("3,14,34,49", "VCC (4.5V)"), ("12,46", "VDD (3.3V)")],
                 "U6 PCM3168A (HTQFP-64): TDM, ADC HPF bypassed", (13, 0), "SCKI")
        d += cod
        stubs(d, cod, "left", [("SCKI", "I2S_MCLK"), ("BCKAD", "I2S_BCLK"), ("BCKDA", "I2S_BCLK"), ("LRCKAD", "I2S_WS"),
                               ("LRCKDA", "I2S_WS"), ("DIN1", "I2S_DOUT"), ("DOUT1", "I2S_DIN"), ("MC", "SPI_SCLK"),
                               ("MDI", "SPI_MOSI"), ("MDO", "SPI_MISO"), ("MS", "CS_CODEC"), ("RST", "RC 10k/1uF"), ("MODE", "+3V3_D (SPI)")])
        stubs(d, cod, "right", [("VOUT1+/-", "AOP1/AOM1 -> sheet 5"), ("VIN1+", "AIN_A1 <- sheet 6"), ("VIN1-", "VCOMAD"),
                                ("VCOMAD", "10u"), ("VCOMDA", "10u")])
        dac = ic([("16", "SCLK"), ("2", "SYNC"), ("15", "DIN"), ("1", "LDAC"), ("9", "CLR")],
                 [("4..13", "VOUTA..H"), ("8", "VREF")], "U2 DAC8568BIPW", (0, -14), "SCLK")
        d += dac
        stubs(d, dac, "left", [("SCLK", "SPI_SCLK"), ("SYNC", "CS_DAC"), ("DIN", "SPI_MOSI"), ("LDAC", "+3V3_A"), ("CLR", "+3V3_A")])
        stubs(d, dac, "right", [("VOUTA..H", "DAC_OUT1..8 -> sheet 3"), ("VREF", "220n, R2/R3 -> VB")])
        adc = ic([("19", "SCLK"), ("17", "CS"), ("20", "DOUT"), ("21", "DIN"), ("23", "XTAL1"), ("22", "XTAL2")],
                 [("29..12", "AIN0P..7P"), ("30..11", "AIN0N..7N"), ("24", "CAP")], "U3 ADS131M08 (TQFP-32)", (13, -14), "SCLK")
        d += adc
        stubs(d, adc, "left", [("SCLK", "SPI_SCLK"), ("CS", "CS_ADC"), ("DOUT", "SPI_MISO"), ("DIN", "SPI_MOSI"),
                               ("XTAL1", "8.192 MHz"), ("XTAL2", "8.192 MHz")])
        stubs(d, adc, "right", [("AIN0P..7P", "AIN1..8 <- sheet 4"), ("AIN0N..7N", "GND"), ("CAP", "220n")])

    # ---------------------------------------------------------------- 3 precision out
    with schemdraw.Drawing(file=os.path.join(out, "3_precision_out.svg"), show=False) as d:
        d.config(fontsize=11)
        d += elm.Label().at((0, 4.5)).label("Sheet 3 - precision CV out Pn (x8, OPA4172): Vjack = 0.990 x (9.25 Vb - 8.25 Vdac), +-10.2 V on +-11 V rails", loc="right")
        d += (op := elm.Opamp(leads=True).at((6, 0)).label("OPA4172 1/4", "bottom", ofst=(0, -0.3)))
        d += elm.Line().left().at(op.in1).length(0.5)
        d += (s := elm.Dot().label("SUMn", "bottom"))
        d += elm.Resistor().left().label("R1n 10k 0.1%")
        d += elm.Label().label("DAC_OUTn (0..2.5 V)", loc="left")
        d += elm.Line().up().at(s.center).length(1.6)
        d += elm.Resistor().right().length(4.2).label("RFn 82.5k 0.1%")
        d += elm.Line().down().toy(op.out)
        d += (o := elm.Dot())
        d += elm.Line().up().at(s.center).length(3.0)
        d += elm.Capacitor().right().length(4.2).label("CFn 47p")
        d += elm.Line().down().length(1.4)
        d += elm.Line().right().at(op.out).length(0.3)
        d += elm.Resistor().right().label("RSn 1k 0.66W")
        d += elm.Dot().label("CV Out Pn", "right")
        d += elm.Line().down().at(o.center).length(0.6)
        d += elm.Dot().label("BAT54S to +-11V", "right")
        d += elm.Line().left().at(op.in2).length(0.5)
        d += elm.Line().down().length(1.2)
        d += elm.Label().label("VB = VREF x 8.06/18.06 (100 pF only)", loc="bottom")

    # ---------------------------------------------------------------- 4 precision in
    with schemdraw.Drawing(file=os.path.join(out, "4_precision_in.svg"), show=False) as d:
        d.config(fontsize=11)
        d += elm.Label().at((0, 3)).label("Sheet 4 - precision CV in Pn (x8): Zin 108.6 k, +-10 V -> +-0.814 V at the ADS131M08", loc="right")
        d += elm.Dot().at((0, 0)).label("CV In Pn", "left")
        d += elm.Resistor().right().label("RAn 49.9k")
        d += elm.Resistor().right().label("RBn 49.9k")
        d += (a := elm.Dot())
        d += elm.Resistor().down().label("RCn 9.09k", "bottom")
        d += elm.Ground()
        d += elm.Line().right().at(a.center).length(2.5)
        d += (c := elm.Dot())
        d += elm.Capacitor().down().label("CAn 330p", "bottom")
        d += elm.Ground()
        d += elm.Line().right().at(c.center).length(2.5)
        d += elm.Label().label("AINn (ADS131M08 AINxP; AINxN = GND)", loc="right")

    # ---------------------------------------------------------------- 5 codec out
    with schemdraw.Drawing(file=os.path.join(out, "5_audio_out.svg"), show=False) as d:
        d.config(fontsize=11)
        d += elm.Label().at((0, 4.5)).label("Sheet 5 - audio out An (x8, OPA1679): difference amp, gain 2.87; codec FS (+-3.6 V diff) -> +-10.25 V", loc="right")
        d += (op := elm.Opamp(leads=True).at((6, 0)).label("OPA1679 1/4", "bottom", ofst=(0, -0.3)))
        d += elm.Line().left().at(op.in1).length(0.5)
        d += (s := elm.Dot())
        d += elm.Resistor().left().label("RAMn 28.7k 0.1%")
        d += elm.Label().label("VOUTn- (codec)", loc="left")
        d += elm.Line().up().at(s.center).length(1.6)
        d += elm.Resistor().right().length(4.2).label("RAFn 82.5k 0.1% || 47p")
        d += elm.Line().down().toy(op.out)
        d += (o := elm.Dot())
        d += elm.Line().left().at(op.in2).length(0.5)
        d += (pnode := elm.Dot())
        d += elm.Resistor().left().label("RAPn 28.7k 0.1%")
        d += elm.Label().label("VOUTn+ (codec)", loc="left")
        d += elm.Resistor().down().at(pnode.center).label("RAGn 82.5k || 47p", "bottom")
        d += elm.Ground()
        d += elm.Line().right().at(op.out).length(0.3)
        d += elm.Resistor().right().label("RSAn 1k 0.66W")
        d += elm.Dot().label("Audio Out An", "right")
        d += elm.Line().down().at(o.center).length(0.6)
        d += elm.Dot().label("BAT54S to +-11V", "right")

    # ---------------------------------------------------------------- 6 codec in
    with schemdraw.Drawing(file=os.path.join(out, "6_audio_in.svg"), show=False) as d:
        d.config(fontsize=11)
        d += elm.Label().at((0, 4.5)).label("Sheet 6 - audio in An (x6, TLV9064 on the codec's 4.5 V): inverting, gain -0.11, 0 V -> VCOM", loc="right")
        d += (op := elm.Opamp(leads=True).at((6, 0)).label("TLV9064 1/4 (4.5 V / GND)", "bottom", ofst=(0, -0.3)))
        d += elm.Line().left().at(op.in1).length(0.5)
        d += (s := elm.Dot())
        d += elm.Resistor().left().label("RAIn 100k")
        d += elm.Label().label("Audio In An", loc="left")
        d += elm.Line().up().at(s.center).length(1.6)
        d += elm.Resistor().right().length(4.2).label("RAIFn 11k || 100p")
        d += elm.Line().down().toy(op.out)
        d += elm.Line().left().at(op.in2).length(0.5)
        d += elm.Line().down().length(1.2)
        d += elm.Label().label("AI_VPLUS = 4.5 V x 8.2k/18.2k (1 uF)", loc="bottom")
        d += elm.Line().right().at(op.out).length(0.3)
        d += elm.Resistor().right().label("RAOn 100")
        d += elm.Dot().label("VINn+ (codec, single-ended; VINn- = VCOMAD)", "right")
        d += elm.Label().at((0, -4)).label("The op-amp runs from the codec's own 4.5 V, so +-24 V at the jack cannot drive the codec pin outside its rails.", loc="right")
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write("<!doctype html><meta charset=utf-8><title>CV Interface Schematic</title>"
                "<style>body{font:14px system-ui;margin:16px;background:#fff;color:#111}img{width:100%;max-width:1200px;"
                "border:1px solid #ccc;margin:8px 0 24px;display:block}</style><h1>CV interface v2 - schematic sheets</h1>"
                "<p>Generated by hw/gen_hw.py. Authoritative connections: hw/netlist.csv and hw/kicad/cv_interface.net. "
                "A person reviews everything before it touches rack power.</p>")
        for name in ("1_power", "2_controller", "3_precision_out", "4_precision_in", "5_audio_out", "6_audio_in"):
            f.write(f'<h2>{name.replace("_", " ")}</h2><img src="{name}.svg">')
    return True
