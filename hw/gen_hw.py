#!/usr/bin/env python3
"""Single source of truth for the v2 carrier board (ESP32-S3 T-Display-S3 + PCM3168A + DAC8568 + ADS131M08).

Generates:
  hw/netlist.csv                 every component pin -> net, with population group and assembly side
  hw/kicad/cv_interface.net      KiCad netlist (pcbnew: File > Import > Netlist) with footprints + LCSC fields
  hw/jlc/bom_<population>.csv    JLCPCB BOM upload format (Comment, Designator, Footprint, LCSC Part #)
  hw/bom_<population>.csv        full BOM with hand-soldered parts, prices and suppliers
  hw/results/bom_summary.json    cost summary per population
  hw/schematic/*.svg             schematic sheets (needs schemdraw)

Populations: full (8 audio out + 6 audio in + 8 CV out + 8 CV in), precision (CV group only),
audio (codec group only). The PCB is the same; unpopulated groups are simply not placed.

    python3 hw/gen_hw.py
"""
import csv, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(HERE, "results", "jlc_stock.json")

# ------------------------------------------------------------------ footprints (KiCad standard libraries)
FP = {
    "R0805": "Resistor_SMD:R_0805_2012Metric", "R1206": "Resistor_SMD:R_1206_3216Metric",
    "C0805": "Capacitor_SMD:C_0805_2012Metric", "C1206": "Capacitor_SMD:C_1206_3216Metric",
    "SOIC14": "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm", "TSSOP14": "Package_SO:TSSOP-14_4.4x5mm_P0.65mm",
    "TSSOP16": "Package_SO:TSSOP-16_4.4x5mm_P0.65mm", "TQFP32": "Package_QFP:TQFP-32_5x5mm_P0.5mm",
    "HTQFP64": "Package_QFP:HTQFP-64-1EP_10x10mm_P0.5mm_EP8x8mm",
    "MSOP8EP": "Package_SO:MSOP-8-1EP_3x3mm_P0.65mm_EP1.68x1.88mm",
    "SOT23": "Package_TO_SOT_SMD:SOT-23", "SOT23-5": "Package_TO_SOT_SMD:SOT-23-5",
    "SOT223": "Package_TO_SOT_SMD:SOT-223-3_TabPin2", "SC70-6": "Package_TO_SOT_SMD:SOT-363_SC-70-6",
    "SMA": "Diode_SMD:D_SMA", "L4030": "Inductor_SMD:L_Taiyo-Yuden_NR-40xx",
    "XTAL3225": "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
    "JACK": "Connector_Audio:Jack_3.5mm_QingPu_WQP-PJ398SM_Vertical_CircularHoles",
    "IDC2x5": "Connector_IDC:IDC-Header_2x05_P2.54mm_Vertical",
    "SOCK1x12": "Connector_PinSocket_2.54mm:PinSocket_1x12_P2.54mm_Vertical",
    "SIP5": "Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical",
    "HDR1x3": "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
}

# ------------------------------------------------------------------ parts
# part(ref, value, fp_key, pins{pad: net}, group, mpn, assembly)  assembly: "smt" (JLC) or "hand" (through-hole)
parts = []
def part(ref, value, fp, pins, group="core", mpn="", assembly="smt"):
    parts.append(dict(ref=ref, value=value, fp=fp, pins=pins, group=group, mpn=mpn, assembly=assembly))

R = lambda ref, val, a, b, group="core", mpn="": part(ref, val, "R0805", {"1": a, "2": b}, group, mpn)
C = lambda ref, val, a, b, group="core", mpn="": part(ref, val, "C0805", {"1": a, "2": b}, group, mpn)

# ---- controller: LilyGO T-Display-S3 on two 1x12 sockets (pin order: check the LilyGO pinmap before layout)
part("J2", "T-Display-S3 header A", "SOCK1x12",
     {"1": "GND", "2": "GND", "3": "+5V_USB", "4": "NC_S3_3V3", "5": "RAIL_SENSE", "6": "RACK_SENSE", "7": "CS_CODEC",
      "8": "CS_ADC", "9": "SPI_MOSI", "10": "SPI_SCLK", "11": "SPI_MISO", "12": "NC_A12"}, assembly="hand",
     mpn="female header 1x12 2.54 mm")
part("J3", "T-Display-S3 header B", "SOCK1x12",
     {"1": "S3_VBAT", "2": "GND", "3": "I2S_DIN", "4": "CS_DAC", "5": "I2S_MCLK", "6": "I2S_BCLK", "7": "I2S_WS",
      "8": "I2S_DOUT", "9": "NC_B9", "10": "NC_B10", "11": "NC_B11", "12": "NC_B12"}, assembly="hand",
     mpn="female header 1x12 2.54 mm")

# ---- power: USB 5 V (primary) and optional LiPo boost, ideal-diode OR -> +5V_SYS
part("U20", "LM66100 (USB path)", "SC70-6", {"1": "+5V_USB", "2": "GND", "3": "GND", "4": "NC_U20", "5": "NC_U20B", "6": "+5V_SYS"}, mpn="LM66100DCKR")
part("U21", "LM66100 (battery boost path)", "SC70-6", {"1": "+5V_BOOST", "2": "GND", "3": "GND", "4": "NC_U21", "5": "NC_U21B", "6": "+5V_SYS"}, mpn="LM66100DCKR")
part("J4", "LiPo boost module (optional, 3.7 V -> 5 V)", "HDR1x3", {"1": "S3_VBAT", "2": "GND", "3": "+5V_BOOST"}, assembly="hand",
     mpn="e.g. Pololu U3V16F5 or MT3608 module set to 5.1 V")
C("C20", "22uF", "+5V_SYS", "GND", mpn="CL21A226MAQNNNE")
# Isolated DC-DC 5 V -> +-15 V (2 W), input LC, output pi filters, diode-OR with the rack
part("L20", "10uH", "L4030", {"1": "+5V_SYS", "2": "DCDC_VIN"}, mpn="SWPA4030S100MT")
C("C21", "10uF", "DCDC_VIN", "GND")
part("U22", "A0515S-2WR3 (5 V -> +-15 V, 2 W)", "SIP5", {"1": "DCDC_VIN", "2": "GND", "3": "DCDC_P", "4": "GND", "5": "DCDC_N"},
     assembly="hand", mpn="A0515S-2WR3")
C("C22", "22uF", "DCDC_P", "GND"); C("C23", "22uF", "GND", "DCDC_N")
part("L21", "10uH", "L4030", {"1": "DCDC_P", "2": "DCDC_PF"}, mpn="SWPA4030S100MT")
part("L22", "10uH", "L4030", {"1": "DCDC_NF", "2": "DCDC_N"}, mpn="SWPA4030S100MT")
C("C24", "22uF", "DCDC_PF", "GND"); C("C25", "22uF", "GND", "DCDC_NF")
part("D20", "SS14", "SMA", {"2": "DCDC_PF", "1": "VPOS_RAW"}, mpn="SS14")     # pad 1 = cathode
part("D21", "SS14", "SMA", {"2": "VNEG_RAW", "1": "DCDC_NF"}, mpn="SS14")
# Eurorack 10-pin (optional supply): red stripe = pin 1 = -12 V
part("J1", "Eurorack 2x5 shrouded header", "IDC2x5",
     {"1": "RACK_M12", "2": "RACK_M12", "3": "GND", "4": "GND", "5": "GND", "6": "GND", "7": "GND", "8": "GND",
      "9": "RACK_P12", "10": "RACK_P12"}, assembly="hand", mpn="2x5 box header 2.54 mm")
part("D22", "SS14", "SMA", {"2": "RACK_P12", "1": "VPOS_RAW"}, mpn="SS14")
part("D23", "SS14", "SMA", {"2": "VNEG_RAW", "1": "RACK_M12"}, mpn="SS14")
R("R20", "33k", "RACK_P12", "RACK_SENSE"); R("R21", "10k", "RACK_SENSE", "GND")
C("C26", "22uF", "VPOS_RAW", "GND"); C("C27", "22uF", "GND", "VNEG_RAW")
# Low-noise LDOs to +-11 V (FB divider 82.5 k / 10 k)
part("U23", "TPS7A4901 (+11 V)", "MSOP8EP", {"1": "+11V", "2": "FB_P", "3": "NC_U23", "4": "GND", "5": "VPOS_RAW",
     "6": "NR_P", "7": "NC_U23B", "8": "VPOS_RAW", "9": "GND"}, mpn="TPS7A4901DGNR")
R("R22", "82.5k", "+11V", "FB_P"); R("R23", "10k", "FB_P", "GND")
C("C28", "10nF", "NR_P", "GND"); C("C29", "10uF", "+11V", "GND")
part("U24", "TPS7A3001 (-11 V)", "MSOP8EP", {"1": "-11V", "2": "FB_N", "3": "NC_U24", "4": "GND", "5": "VNEG_RAW",
     "6": "NR_N", "7": "NC_U24B", "8": "VNEG_RAW", "9": "GND"}, mpn="TPS7A3001DGNR")
R("R24", "82.5k", "-11V", "FB_N"); R("R25", "10k", "FB_N", "GND")
C("C30", "10nF", "NR_N", "GND"); C("C31", "10uF", "GND", "-11V")
R("R26", "100k", "+11V", "RAIL_SENSE"); R("R27", "10k", "RAIL_SENSE", "GND")
# 3.3 V digital (codec VDD, ADC DVDD, pull-ups) and 3.3 V analog (DAC/ADC AVDD), 4.5 V codec analog
part("U25", "AMS1117-3.3", "SOT223", {"1": "GND", "2": "+3V3_D", "3": "+5V_SYS"}, mpn="AMS1117-3.3")
C("C32", "22uF", "+3V3_D", "GND")
part("U26", "LP5907-3.3", "SOT23-5", {"1": "+5V_SYS", "2": "GND", "3": "+5V_SYS", "4": "NC_U26", "5": "+3V3_A"}, group="P", mpn="TPLP5907MFX-3.3")
C("C33", "1uF", "+3V3_A", "GND", group="P")
part("U27", "LP5907-4.5", "SOT23-5", {"1": "+5V_SYS", "2": "GND", "3": "+5V_SYS", "4": "NC_U27", "5": "+4V5_A"}, group="A", mpn="LP5907MFX-4.5/NOPB")
C("C34", "10uF", "+4V5_A", "GND", group="A")

# ---- group P: DAC8568 + 8 output stages (2x OPA4172) + ADS131M08 + 8 input dividers
part("U2", "DAC8568BIPW", "TSSOP16",
     {"1": "+3V3_A", "2": "CS_DAC", "3": "+3V3_A", "4": "DAC_OUT1", "5": "DAC_OUT3", "6": "DAC_OUT5", "7": "DAC_OUT7",
      "8": "VREF", "9": "+3V3_A", "10": "DAC_OUT8", "11": "DAC_OUT6", "12": "DAC_OUT4", "13": "DAC_OUT2",
      "14": "GND", "15": "SPI_MOSI", "16": "SPI_SCLK"}, group="P", mpn="DAC8568BIPW (JLC global sourcing)")
C("C1", "100nF", "+3V3_A", "GND", group="P"); C("C2", "220nF", "VREF", "GND", group="P")
R("R1", "10k", "CS_DAC", "+3V3_D")
R("R2", "10k 0.1%", "VREF", "VB", group="P", mpn="RT0805BRD0710KL"); R("R3", "8.06k 0.1%", "VB", "GND", group="P", mpn="RT0805BRD078K06L")
C("C3", "100pF C0G", "VB", "GND", group="P")
quad = {1: ("1", "2", "3"), 2: ("7", "6", "5"), 3: ("8", "9", "10"), 4: ("14", "13", "12")}   # out, -in, +in
for u, chans in (("U4", range(1, 5)), ("U5", range(5, 9))):
    pins = {"4": "+11V", "11": "-11V"}
    for k, ch in enumerate(chans, start=1):
        o, m, p = quad[k]
        pins.update({o: f"OPO{ch}", m: f"SUM{ch}", p: "VB"})
    part(u, "OPA4172", "SOIC14", pins, group="P", mpn="OPA4172IDR")
    C(f"C{u[1:]}P", "100nF", "+11V", "GND", group="P"); C(f"C{u[1:]}N", "100nF", "GND", "-11V", group="P")
for ch in range(1, 9):
    R(f"R1{ch}", "10k 0.1%", f"DAC_OUT{ch}", f"SUM{ch}", group="P", mpn="RT0805BRD0710KL")
    R(f"RF{ch}", "82.5k 0.1%", f"SUM{ch}", f"OPO{ch}", group="P", mpn="RT0805BRD0782K5L")
    C(f"CF{ch}", "47pF C0G", f"SUM{ch}", f"OPO{ch}", group="P")
    part(f"DC{ch}", "BAT54S", "SOT23", {"1": "-11V", "2": "+11V", "3": f"OPO{ch}"}, group="P", mpn="BAT54S,215")
    part(f"RS{ch}", "1k 0.66W", "R1206", {"1": f"OPO{ch}", "2": f"CVOUT{ch}"}, group="P", mpn="ERJ-P08J102V")
    part(f"JPO{ch}", f"CV Out P{ch}", "JACK", {"T": f"CVOUT{ch}", "S": "GND", "TN": f"NC_JPO{ch}"}, group="P",
         assembly="hand", mpn="Thonkiconn PJ398SM")
part("U3", "ADS131M08IPBS", "TQFP32",
     {"29": "AIN1", "30": "GND", "32": "AIN2", "31": "GND", "1": "AIN3", "2": "GND", "4": "AIN4", "3": "GND",
      "5": "AIN5", "6": "GND", "8": "AIN6", "7": "GND", "9": "AIN7", "10": "GND", "12": "AIN8", "11": "GND",
      "13": "GND", "28": "GND", "14": "NC_REFIN", "15": "+3V3_A", "16": "+3V3_D", "17": "CS_ADC", "18": "NC_DRDY",
      "19": "SPI_SCLK", "20": "SPI_MISO", "21": "SPI_MOSI", "22": "XTAL2", "23": "XTAL1", "24": "ADC_CAP",
      "25": "GND", "26": "+3V3_D", "27": "GND"}, group="P", mpn="ADS131M08IPBSR")
C("C4", "1uF", "+3V3_A", "GND", group="P"); C("C5", "1uF", "+3V3_D", "GND", group="P"); C("C6", "220nF", "ADC_CAP", "GND", group="P")
R("R4", "10k", "CS_ADC", "+3V3_D")
part("Y1", "8.192MHz", "XTAL3225", {"1": "XTAL1", "2": "GND", "3": "XTAL2", "4": "GND"}, group="P", mpn="0132M4-8.192F20DTNJL")
C("C7", "12pF C0G", "XTAL1", "GND", group="P"); C("C8", "12pF C0G", "XTAL2", "GND", group="P")
for ch in range(1, 9):
    part(f"JPI{ch}", f"CV In P{ch}", "JACK", {"T": f"CVIN{ch}", "S": "GND", "TN": "GND"}, group="P", assembly="hand", mpn="Thonkiconn PJ398SM")
    R(f"RA{ch}", "49.9k 1%", f"CVIN{ch}", f"CVINM{ch}", group="P", mpn="RC0805FR-0749K9L"); R(f"RB{ch}", "49.9k 1%", f"CVINM{ch}", f"AIN{ch}", group="P", mpn="RC0805FR-0749K9L")
    R(f"RC{ch}", "9.09k 1%", f"AIN{ch}", "GND", group="P", mpn="RC0805FR-079K09L"); C(f"CA{ch}", "330pF C0G", f"AIN{ch}", "GND", group="P")

# ---- group A: PCM3168A + 8 difference-amp outputs (2x OPA1679) + 6 inverting inputs on 4.5 V (2x TLV9064)
codec = {"1": "VCOMAD", "2": "GND", "3": "+4V5_A", "4": "CODEC_RST", "5": "NC_OVF", "6": "I2S_WS", "7": "I2S_BCLK",
         "8": "I2S_DIN", "9": "NC_DOUT2", "10": "NC_DOUT3", "11": "GND", "12": "+3V3_D", "13": "NC_ZERO",
         "14": "+4V5_A", "15": "VCOMDA", "16": "GND", "33": "GND", "34": "+4V5_A", "35": "I2S_WS", "36": "I2S_BCLK",
         "37": "I2S_DOUT", "38": "GND", "39": "GND", "40": "GND", "41": "I2S_MCLK", "42": "SPI_SCLK", "43": "SPI_MOSI",
         "44": "SPI_MISO", "45": "CS_CODEC", "46": "+3V3_D", "47": "GND", "48": "+3V3_D", "49": "+4V5_A", "50": "GND",
         "59": "VREFAD1", "60": "VREFAD2", "65": "GND"}
vout = {8: (17, 18), 7: (19, 20), 6: (21, 22), 5: (23, 24), 4: (25, 26), 3: (27, 28), 2: (29, 30), 1: (31, 32)}
for ch, (pp, pm) in vout.items():
    codec[str(pp)] = f"AOP{ch}"; codec[str(pm)] = f"AOM{ch}"
vin = {1: (52, 51), 2: (54, 53), 3: (56, 55), 4: (58, 57), 5: (62, 61), 6: (64, 63)}   # (+, -)
for ch, (pp, pm) in vin.items():
    codec[str(pp)] = f"AIN_A{ch}"; codec[str(pm)] = "VCOMAD"
part("U6", "PCM3168APAP", "HTQFP64", codec, group="A", mpn="PCM3168APAPR")
for n, net in (("C40", "VCOMAD"), ("C41", "VCOMDA"), ("C42", "VREFAD1"), ("C43", "VREFAD2")):
    C(n, "10uF", net, "GND", group="A")
for n in ("C44", "C45", "C46"):
    C(n, "10uF", "+4V5_A", "GND", group="A")
C("C47", "100nF", "+3V3_D", "GND", group="A")
R("R5", "10k", "CS_CODEC", "+3V3_D")
R("R6", "10k", "CODEC_RST", "+3V3_D", group="A"); C("C48", "1uF", "CODEC_RST", "GND", group="A")
for u, chans in (("U7", range(1, 5)), ("U8", range(5, 9))):
    pins = {"4": "+11V", "11": "-11V"}
    for k, ch in enumerate(chans, start=1):
        o, m, p = quad[k]
        pins.update({o: f"AOPO{ch}", m: f"AOSUM{ch}", p: f"AOREF{ch}"})
    part(u, "OPA1679", "TSSOP14", pins, group="A", mpn="OPA1679IPWR")
    C(f"C{u[1:]}P", "100nF", "+11V", "GND", group="A"); C(f"C{u[1:]}N", "100nF", "GND", "-11V", group="A")
for ch in range(1, 9):
    R(f"RAM{ch}", "28.7k 0.1%", f"AOM{ch}", f"AOSUM{ch}", group="A", mpn="RT0805BRD0728K7L")
    R(f"RAF{ch}", "82.5k 0.1%", f"AOSUM{ch}", f"AOPO{ch}", group="A", mpn="RT0805BRD0782K5L")
    R(f"RAP{ch}", "28.7k 0.1%", f"AOP{ch}", f"AOREF{ch}", group="A", mpn="RT0805BRD0728K7L")
    R(f"RAG{ch}", "82.5k 0.1%", f"AOREF{ch}", "GND", group="A", mpn="RT0805BRD0782K5L")
    C(f"CAF{ch}", "47pF C0G", f"AOSUM{ch}", f"AOPO{ch}", group="A"); C(f"CAG{ch}", "47pF C0G", f"AOREF{ch}", "GND", group="A")
    part(f"DA{ch}", "BAT54S", "SOT23", {"1": "-11V", "2": "+11V", "3": f"AOPO{ch}"}, group="A", mpn="BAT54S,215")
    part(f"RSA{ch}", "1k 0.66W", "R1206", {"1": f"AOPO{ch}", "2": f"AUOUT{ch}"}, group="A", mpn="ERJ-P08J102V")
    part(f"JAO{ch}", f"Audio Out A{ch}", "JACK", {"T": f"AUOUT{ch}", "S": "GND", "TN": f"NC_JAO{ch}"}, group="A",
         assembly="hand", mpn="Thonkiconn PJ398SM")
R("RV1", "10k 1%", "+4V5_A", "AI_VPLUS", group="A"); R("RV2", "8.2k 1%", "AI_VPLUS", "GND", group="A", mpn="RC0805FR-078K2L")
C("CV1", "1uF", "AI_VPLUS", "GND", group="A")
for u, chans in (("U9", range(1, 5)), ("U10", range(5, 9))):
    pins = {"4": "+4V5_A", "11": "GND"}
    for k, ch in enumerate(chans, start=1):
        o, m, p = quad[k]
        if ch <= 6:
            pins.update({o: f"AIPO{ch}", m: f"AISUM{ch}", p: "AI_VPLUS"})
        else:                                  # spare sections: followers tied to the bias, outputs open
            pins.update({o: f"AISPARE{ch}", m: f"AISPARE{ch}", p: "AI_VPLUS"})
    part(u, "TLV9064", "TSSOP14", pins, group="A", mpn="TLV9064IPWR")
    C(f"C{u[1:]}A", "100nF", "+4V5_A", "GND", group="A")
for ch in range(1, 7):
    part(f"JAI{ch}", f"Audio In A{ch}", "JACK", {"T": f"AUIN{ch}", "S": "GND", "TN": "GND"}, group="A", assembly="hand", mpn="Thonkiconn PJ398SM")
    R(f"RAI{ch}", "100k 1%", f"AUIN{ch}", f"AISUM{ch}", group="A"); R(f"RAIF{ch}", "11k 1%", f"AISUM{ch}", f"AIPO{ch}", group="A", mpn="RC0805FR-0711KL")
    C(f"CAI{ch}", "100pF C0G", f"AISUM{ch}", f"AIPO{ch}", group="A")
    R(f"RAO{ch}", "100", f"AIPO{ch}", f"AIN_A{ch}", group="A")

GROUPS = {"full": {"core", "A", "P"}, "precision": {"core", "P"}, "audio": {"core", "A"}}

# ------------------------------------------------------------------ prices (JLC stock file + fallbacks)
def stock_index():
    try:
        with open(STOCK) as f:
            return {p.get("query", "") + "|" + p.get("mpn", ""): p for p in json.load(f)["parts"] if "mpn" in p}
    except Exception:
        return {}

FALLBACK_USD = {   # generic JLC basic parts and hand parts (approximate, USD each)
    "0805_R": 0.002, "0805_C": 0.005, "0805_C_big": 0.02, "1206_R": 0.05, "0.1%": 0.25, "JACK": 0.40,
    "IDC2x5": 0.35, "SOCK1x12": 0.40, "HDR1x3": 0.05, "XTAL": 0.15, "DAC8568BIPW (JLC global sourcing)": 17.0,
    "A0515S-2WR3": 3.2, "LM66100DCKR": 0.45, "TLV9064IPWR": 0.75, "LP5907MFX-3.3/NOPB": 0.45, "AMS1117-3.3": 0.08,
    "SWPA4030S100MT": 0.06,
}

GENERIC = {   # (value, footprint) -> query in tools/jlc_stock.py EXTRA (JLC basic parts preferred)
    ("100nF", "C0805"): "100nF 0805 X7R 50V", ("1uF", "C0805"): "1uF 0805 X7R 25V", ("10nF", "C0805"): "10nF 0805 X7R",
    ("220nF", "C0805"): "220nF 0805 X7R", ("100pF C0G", "C0805"): "100pF 0805 C0G", ("12pF C0G", "C0805"): "12pF 0805 C0G",
    ("47pF C0G", "C0805"): "47pF 0805 C0G", ("56pF C0G", "C0805"): "56pF 0805 C0G", ("330pF C0G", "C0805"): "330pF 0805 C0G",
    ("10uF", "C0805"): "10uF 0805 25V", ("22uF", "C0805"): "22uF 0805", ("33k", "R0805"): "33kΩ 0805 ±1%",
    ("10k", "R0805"): "10kΩ 0805 ±1%", ("10k 1%", "R0805"): "10kΩ 0805 ±1%", ("100k", "R0805"): "100kΩ 0805 ±1%",
    ("100k 1%", "R0805"): "100kΩ 0805 ±1%", ("100", "R0805"): "100Ω 0805 ±1%", ("82.5k", "R0805"): "82.5kΩ 0805 ±0.1%",
}

def unit_price(p, stock):
    m = p["mpn"]
    q = GENERIC.get((p["value"], p["fp"])) if not m else None
    if q:
        for v in stock.values():
            if v.get("query") == q and v.get("usd_qty10"):
                return float(v["usd_qty10"]), v.get("lcsc", ""), v.get("library", "")
    for k, v in stock.items():
        km = k.split("|", 1)[1]
        if km and m and (m.split()[0] in km or km in m) and v.get("usd_qty10"):
            return float(v["usd_qty10"]), v.get("lcsc", ""), v.get("library", "")
    if p["fp"] == "JACK": return FALLBACK_USD["JACK"], "", "hand"
    if p["fp"] in ("IDC2x5", "SOCK1x12", "HDR1x3"): return FALLBACK_USD[p["fp"]], "", "hand"
    if p["fp"] == "XTAL3225": return FALLBACK_USD["XTAL"], "", "expand"
    if "0.1%" in p["value"]: return FALLBACK_USD["0.1%"], "", "expand"
    for k, v in FALLBACK_USD.items():
        if k in m: return v, "", "expand"
    if p["fp"] == "R1206": return FALLBACK_USD["1206_R"], "", "expand"
    if p["fp"] == "R0805": return FALLBACK_USD["0805_R"], "", "base"
    if p["fp"] == "C0805": return (FALLBACK_USD["0805_C_big"] if "u" in p["value"] else FALLBACK_USD["0805_C"]), "", "base"
    return 0.5, "", "expand"

def write_outputs():
    os.makedirs(os.path.join(HERE, "kicad"), exist_ok=True)
    os.makedirs(os.path.join(HERE, "jlc"), exist_ok=True)
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    with open(os.path.join(HERE, "netlist.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ref", "value", "footprint", "pin", "net", "group", "assembly", "mpn"])
        for p in parts:
            for pin, net in p["pins"].items():
                w.writerow([p["ref"], p["value"], FP[p["fp"]], pin, net, p["group"], p["assembly"], p["mpn"]])
    nets = {}
    for p in parts:
        for pin, net in p["pins"].items():
            nets.setdefault(net, []).append((p["ref"], pin))
    single = sorted(n for n, l in nets.items() if len(l) < 2 and not n.startswith("NC"))
    # KiCad netlist (s-expression, version E) - importable into pcbnew
    stock = stock_index()
    with open(os.path.join(HERE, "kicad", "cv_interface.net"), "w") as f:
        f.write('(export (version "E")\n  (design (source "hw/gen_hw.py") (tool "gen_hw.py"))\n  (components\n')
        for p in parts:
            _, lcsc, _ = unit_price(p, stock)
            f.write(f'    (comp (ref "{p["ref"]}") (value "{p["value"]}") (footprint "{FP[p["fp"]]}")\n'
                    f'      (fields (field (name "MPN") "{p["mpn"]}") (field (name "LCSC") "{lcsc}") '
                    f'(field (name "Group") "{p["group"]}") (field (name "Assembly") "{p["assembly"]}")))\n')
        f.write('  )\n  (nets\n')
        for code, (net, nodes) in enumerate(sorted(nets.items()), start=1):
            if net.startswith("NC"):
                continue
            f.write(f'    (net (code "{code}") (name "{net}")\n')
            for ref, pin in nodes:
                f.write(f'      (node (ref "{ref}") (pin "{pin}"))\n')
            f.write('    )\n')
        f.write('  )\n)\n')
    # BOMs per population
    summary = {}
    for pop, groups in GROUPS.items():
        lines = {}
        for p in parts:
            if p["group"] not in groups:
                continue
            key = (p["value"], p["fp"], p["mpn"], p["assembly"])
            lines.setdefault(key, []).append(p["ref"])
        smt_cost = hand_cost = 0.0
        extended = set()
        with open(os.path.join(HERE, f"bom_{pop}.csv"), "w", newline="") as f, \
             open(os.path.join(HERE, "jlc", f"bom_{pop}.csv"), "w", newline="") as fj:
            w = csv.writer(f); wj = csv.writer(fj)
            w.writerow(["qty", "value", "designators", "footprint", "mpn", "lcsc", "jlc_library", "assembly", "usd_each", "usd_line"])
            wj.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
            for (value, fp, mpn, asm), refs in sorted(lines.items(), key=lambda x: x[1][0]):
                usd, lcsc, lib = unit_price({"value": value, "fp": fp, "mpn": mpn}, stock)
                line = usd * len(refs)
                if asm == "smt":
                    smt_cost += line
                    if lib != "base": extended.add(mpn or value)
                    wj.writerow([value, ",".join(refs), FP[fp].split(":")[1], lcsc])
                else:
                    hand_cost += line
                w.writerow([len(refs), value, " ".join(refs), FP[fp].split(":")[1], mpn, lcsc, lib, asm, f"{usd:.3f}", f"{line:.2f}"])
        n_smt = sum(len(r) for (v, fp, m, a), r in lines.items() if a == "smt")
        controller = 16.0     # T-Display-S3 (non-touch), approximate
        pcb = 15.0            # 5 boards, 4-layer, ~100 x 120 mm, JLC economic, per-order share for 1 unit (approx)
        pcba_fee = 8.0 + 1.5 + 3.0 * len(extended) + 0.0017 * 2 * n_smt
        dcdc_hand = 0.0
        summary[pop] = {
            "smt_parts_usd": round(smt_cost, 2), "hand_parts_usd": round(hand_cost, 2),
            "extended_part_types": len(extended), "jlc_assembly_fees_usd": round(pcba_fee, 2),
            "pcb_usd_per_order": pcb, "controller_board_usd": controller,
            "total_one_unit_usd": round(smt_cost + hand_cost + pcba_fee + pcb + controller + dcdc_hand, 2),
            "smt_placements": n_smt,
        }
    with open(os.path.join(HERE, "results", "bom_summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    return nets, single, summary

if __name__ == "__main__":
    nets, single, summary = write_outputs()
    try:
        sys.path.insert(0, HERE)
        from draw_sheets import draw
        drawn = draw(os.path.join(HERE, "schematic"))
    except ImportError as e:
        drawn = False
        print("schematic skipped:", e)
    print(f"netlist: {len(parts)} parts, {len(nets)} nets; single-pin nets: {single or 'none'}")
    for pop, s in summary.items():
        print(f"bom {pop}: ~USD {s['total_one_unit_usd']} for one unit (SMT {s['smt_parts_usd']}, hand {s['hand_parts_usd']}, "
              f"JLC fees {s['jlc_assembly_fees_usd']}, {s['extended_part_types']} extended types)")
    print("svg:", "yes" if drawn else "skipped")
    sys.exit(1 if single else 0)
