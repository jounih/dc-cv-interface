#!/usr/bin/env python3
"""Calibration / status tool for the Circuit Studio CV interface v2 (USB-MIDI SysEx).

Channel numbering: outputs 1-8 = audio group A (codec), 9-16 = precision group P;
inputs 1-6 = A, 9-16 = P (slots 7-8 do not exist).

    pip install mido python-rtmidi       # only needed to talk to the device
    python3 tools/cvcal.py info
    python3 tools/cvcal.py cal-out        # outputs, with a multimeter (2 points per channel)
    python3 tools/cvcal.py cal-in         # inputs, loopback: patch Out n -> In n (after cal-out)
    python3 tools/cvcal.py save           # write the table to flash
    python3 tools/cvcal.py dump           # print the table
    python3 tools/cvcal.py range 2 bi5    # limiter per output: bi10 | bi5 | uni10 | uni5
    python3 tools/cvcal.py dfu            # reboot into the ESP32-S3 ROM USB downloader (outputs parked at 0 V)
    python3 tools/cvcal.py selftest       # encoding self-test, no device

Protocol: firmware/src/proto.h.
"""
import struct, sys, time

HDR = [0xF0, 0x7D, 0x43, 0x56]
GET_INFO, GET_CAL, SET_CAL, SET_RANGE, SAVE, DEFAULTS, RAW_OUT, MEASURE, DFU, ACK = 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x7F
CV_OUT, CV_IN, CV_SRC, MAP_CFG = 0x10, 0x11, 0x12, 0x13
OUTS = list(range(0, 8)) + list(range(8, 16))
INS = list(range(0, 6)) + list(range(8, 16))
RANGES = {"bi10": 0, "bi5": 1, "uni10": 2, "uni5": 3}
STATUS = {0: "ok", 1: "bad args", 2: "unknown command"}


def u32(v):
    return [(v >> (7 * i)) & 0x7F for i in range(5)]

def get_u32(b):
    return sum((b[i] & 0x7F) << (7 * i) for i in range(5))

def f32(x):
    return u32(struct.unpack("<I", struct.pack("<f", x))[0])

def get_f32(b):
    return struct.unpack("<f", struct.pack("<I", get_u32(b)))[0]

def msg(cmd, payload=()):
    return HDR + [cmd] + [p & 0x7F for p in payload] + [0xF7]


class Device:
    def __init__(self):
        try:
            import mido
        except ImportError:
            sys.exit("needs: pip install mido python-rtmidi")
        self.mido = mido
        name_in = next((n for n in mido.get_input_names() if "CV Interface" in n or "Circuit Studio CV" in n), None)
        name_out = next((n for n in mido.get_output_names() if "CV Interface" in n or "Circuit Studio CV" in n), None)
        if not name_in or not name_out:
            sys.exit("CV interface MIDI port not found. Ports: %s" % mido.get_output_names())
        self.inp = mido.open_input(name_in)
        self.out = mido.open_output(name_out)

    def call(self, cmd, payload=(), want=None, timeout=2.0):
        m = msg(cmd, payload)
        self.out.send(self.mido.Message("sysex", data=m[1:-1]))
        want = want if want is not None else (cmd | 0x40)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for r in self.inp.iter_pending():
                if r.type != "sysex":
                    continue
                d = list(r.data)
                if d[:3] != HDR[1:]:
                    continue
                if d[3] == ACK and d[4] == cmd and want == ACK:
                    if d[5] != 0:
                        raise RuntimeError(f"cmd {cmd}: {STATUS.get(d[5], d[5])}")
                    return d[4:]
                if d[3] == ACK and d[4] == cmd and d[5] != 0:
                    raise RuntimeError(f"cmd {cmd}: {STATUS.get(d[5], d[5])}")
                if d[3] == want:
                    return d[4:]
            time.sleep(0.005)
        raise TimeoutError(f"no reply to cmd {cmd}")

    def info(self):
        p = self.call(GET_INFO)
        f = p[6]
        return {"fw": f"{p[0]}.{p[1]}", "audio_out": p[2], "audio_in": p[3], "cv_out": p[4], "cv_in": p[5],
                "cal_from_flash": bool(f & 1), "audio_out_stream": bool(f & 2), "audio_in_stream": bool(f & 4),
                "codec_ok": bool(f & 8), "precision_ok": bool(f & 16), "ble": bool(f & 32),
                "power": ["usb", "rack", "battery"][p[7]] if p[7] < 3 else p[7],
                "battery_pct": None if p[8] == 127 else p[8], "underruns": get_u32(p[9:14])}

    def get_cal(self, kind, ch):
        p = self.call(GET_CAL, [kind, ch])
        return get_f32(p[2:7]), get_f32(p[7:12]), p[12]

    def set_cal(self, kind, ch, gain, offset):
        self.call(SET_CAL, [kind, ch] + f32(gain) + f32(offset), want=ACK)

    def raw_out(self, ch, code):
        """code None releases the channel; codec channels take signed 24-bit codes."""
        on = 0 if code is None else 1
        self.call(RAW_OUT, [ch, on] + u32((code or 0) & 0xFFFFFFFF), want=ACK)

    def measure(self, n=24000):
        self.call(MEASURE, u32(n), want=ACK)
        p = self.call(MEASURE, want=MEASURE | 0x40, timeout=5.0)
        return [get_f32(p[5 * i:5 * i + 5]) for i in range(len(p) // 5)]


def ask_volts(prompt):
    while True:
        s = input(prompt).strip().replace(",", ".")
        try:
            return float(s)
        except ValueError:
            print("  enter a number in volts, e.g. -7.4931")


def cal_out(dev, info):
    print("Output calibration. Measure each jack with a DMM against ground, 100 k load if you have one.")
    for ch in OUTS:
        g, o, _ = dev.get_cal(0, ch)
        c1, c2 = int(round(o + g * -7.5)), int(round(o + g * 7.5))   # about -7.5 V and +7.5 V
        name = f"A{ch + 1}" if ch < 8 else f"P{ch - 7}"
        dev.raw_out(ch, c1); time.sleep(0.3)
        v1 = ask_volts(f"  Out {name}: reading now (code {c1}) in volts: ")
        dev.raw_out(ch, c2); time.sleep(0.3)
        v2 = ask_volts(f"  Out {name}: reading now (code {c2}) in volts: ")
        dev.raw_out(ch, None)
        gain = (c2 - c1) / (v2 - v1)
        offset = c1 - gain * v1
        dev.set_cal(0, ch, gain, offset)
        print(f"    gain {gain:.3f} codes/V, 0 V at code {offset:.1f}")
    print("Done (RAM only). Run `save` to keep it.")


def cal_in(dev, info):
    print("Input calibration by loopback: patch Out n -> In n for every input, outputs must be calibrated.")
    input("  Press Enter when patched... ")
    pts = {}
    for v in (-5.0, 5.0):
        for ch in INS:                  # Out A1..A6 -> In A1..A6, Out P1..P8 -> In P1..P8
            g, o, _ = dev.get_cal(0, ch)
            dev.raw_out(ch, int(round(o + g * v)))
        time.sleep(0.3)
        pts[v] = dev.measure(16000)
    for ch in OUTS:
        dev.raw_out(ch, None)
    for ch in INS:
        r1, r2 = pts[-5.0][ch], pts[5.0][ch]
        gain = 10.0 / (r2 - r1)
        offset = r1 - (-5.0) / gain
        dev.set_cal(1, ch, gain, offset)
        print(f"  In {ch + 1}: {gain * 1e6:.4f} uV/count, offset {offset:.0f} counts")
    print("Done (RAM only). Run `save` to keep it.")


def selftest():
    b = u32(0xDEADBEEF)
    assert all(x < 0x80 for x in b) and get_u32(b) == 0xDEADBEEF
    assert get_f32(f32(-3252.5)) == -3252.5
    # CV values: signed 21-bit in 10 uV units
    def cv(v):
        x = int(round(v * 100000)) & 0x1FFFFF
        return [x & 0x7F, (x >> 7) & 0x7F, (x >> 14) & 0x7F]
    assert len(cv(-3.5)) == 3 and all(b < 0x80 for b in cv(10.48))
    m = msg(GET_CAL, [0, 1])
    assert m == [0xF0, 0x7D, 0x43, 0x56, 0x02, 0, 1, 0xF7]
    print("cvcal selftest ok")


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "info"
    if cmd == "selftest":
        return selftest()
    dev = Device()
    info = dev.info()
    if cmd == "info":
        print(info)
    elif cmd == "dump":
        for ch in OUTS:
            g, o, r = dev.get_cal(0, ch)
            print(f"out {ch + 1}: {g:10.3f} codes/V  zero {o:9.2f}  range {r}")
        for ch in INS:
            g, o, _ = dev.get_cal(1, ch)
            print(f"in  {ch + 1}: {g * 1e6:10.5f} uV/count  offset {o:9.1f}")
    elif cmd == "cal-out":
        cal_out(dev, info)
    elif cmd == "cal-in":
        cal_in(dev, info)
    elif cmd == "range":
        dev.call(SET_RANGE, [int(argv[2]) - 1, RANGES[argv[3]]], want=ACK)   # channel 1-16
    elif cmd == "save":
        dev.call(SAVE, want=ACK); time.sleep(0.3)
        print("saved" if dev.info()["cal_from_flash"] else "save FAILED")
    elif cmd == "defaults":
        dev.call(DEFAULTS, want=ACK)
    elif cmd == "dfu":
        dev.call(DFU, want=ACK)
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
