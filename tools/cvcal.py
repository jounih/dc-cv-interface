#!/usr/bin/env python3
"""Calibration / status tool for the Circuit Studio CV interface (USB-MIDI SysEx).

    pip install mido python-rtmidi       # only needed to talk to the device
    python3 tools/cvcal.py info
    python3 tools/cvcal.py cal-out        # outputs, with a multimeter (2 points per channel)
    python3 tools/cvcal.py cal-in         # inputs, loopback: patch Out n -> In n (after cal-out)
    python3 tools/cvcal.py save           # write the table to flash
    python3 tools/cvcal.py dump           # print the table
    python3 tools/cvcal.py range 2 bi5    # limiter per output: bi10 | bi5 | uni10 | uni5
    python3 tools/cvcal.py bootsel        # reboot into the UF2 bootloader (outputs parked at 0 V)
    python3 tools/cvcal.py selftest       # encoding self-test, no device

Protocol: firmware/src/proto.h.
"""
import struct, sys, time

HDR = [0xF0, 0x7D, 0x43, 0x56]
GET_INFO, GET_CAL, SET_CAL, SET_RANGE, SAVE, DEFAULTS, RAW_OUT, MEASURE, BOOTSEL, ACK = 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x7F
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
        return {"fw": f"{p[0]}.{p[1]}", "n_out": p[2], "n_in": p[3],
                "cal_from_flash": bool(p[4] & 1), "out_stream": bool(p[4] & 2),
                "in_stream": bool(p[4] & 4), "adc_ok": bool(p[4] & 8), "underruns": get_u32(p[5:10])}

    def get_cal(self, kind, ch):
        p = self.call(GET_CAL, [kind, ch])
        return get_f32(p[2:7]), get_f32(p[7:12]), p[12]

    def set_cal(self, kind, ch, gain, offset):
        self.call(SET_CAL, [kind, ch] + f32(gain) + f32(offset), want=ACK)

    def raw_out(self, ch, code):
        self.call(RAW_OUT, [ch] + u32(code), want=ACK)

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
    c1, c2 = 8192, 57344   # ~ +7.6 V and ~ -7.7 V with the nominal stage
    print("Output calibration. Measure each jack with a DMM against the rack ground, 100 k load if you have one.")
    for ch in range(info["n_out"]):
        dev.raw_out(ch, c1); time.sleep(0.3)
        v1 = ask_volts(f"  Out {ch + 1}: reading now (code {c1}) in volts: ")
        dev.raw_out(ch, c2); time.sleep(0.3)
        v2 = ask_volts(f"  Out {ch + 1}: reading now (code {c2}) in volts: ")
        dev.raw_out(ch, 1 << 20)   # release
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
        for ch in range(info["n_in"]):
            g, o, _ = dev.get_cal(0, ch)
            dev.raw_out(ch, int(round(o + g * v)))
        time.sleep(0.3)
        pts[v] = dev.measure(24000)
    for ch in range(info["n_out"]):
        dev.raw_out(ch, 1 << 20)
    for ch in range(info["n_in"]):
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
        for ch in range(info["n_out"]):
            g, o, r = dev.get_cal(0, ch)
            print(f"out {ch + 1}: {g:10.3f} codes/V  zero {o:9.2f}  range {r}")
        for ch in range(info["n_in"]):
            g, o, _ = dev.get_cal(1, ch)
            print(f"in  {ch + 1}: {g * 1e6:10.5f} uV/count  offset {o:9.1f}")
    elif cmd == "cal-out":
        cal_out(dev, info)
    elif cmd == "cal-in":
        cal_in(dev, info)
    elif cmd == "range":
        dev.call(SET_RANGE, [int(argv[2]) - 1, RANGES[argv[3]]], want=ACK)
    elif cmd == "save":
        dev.call(SAVE, want=ACK); time.sleep(0.3)
        print("saved" if dev.info()["cal_from_flash"] else "save FAILED")
    elif cmd == "defaults":
        dev.call(DEFAULTS, want=ACK)
    elif cmd == "bootsel":
        dev.call(BOOTSEL, want=ACK)
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
