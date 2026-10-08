#!/usr/bin/env python3
"""SPICE checks for the CV interface analog front end (ngspice, batch mode).

Writes the netlists next to this file, runs them, and stores results in
hw/results/spice_results.json and hw/results/spice_summary.md.

    python3 hw/spice/run_spice.py          # needs ngspice on PATH (brew install ngspice)

Component values mirror firmware/src/cal.h (HW_* constants) and hw/bom.csv.
"""
import json, math, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "results")
os.makedirs(RES, exist_ok=True)

# ---- nominal values (keep in sync with firmware/src/cal.h) ----
VREF, R1, RF, RT, RB, RS, RL, CF = 2.5, 10e3, 82.5e3, 10e3, 8.06e3, 1e3, 100e3, 47e-12
IN_RS1 = IN_RS2 = 49.9e3
IN_RSH, IN_C, ADC_RIN, ADC_FS = 9.09e3, 330e-12, 330e3, 1.2
EN_DAC = 90e-9    # DAC8568 output noise density at 1 kHz (datasheet)
RAIL = 11.0       # v2: post-LDO analog rails (+-11 V) from the USB DC-DC or the rack
EN_REF = 50e-9    # DAC8568 internal reference noise density
KT4 = 4 * 1.380649e-23 * 300.15
ADC_NOISE_UV = 75.34  # ADS131M04 datasheet Table 7-1, OSR 64, gain 1 (uVrms)

def rnoise(en):  # resistor whose thermal noise equals en
    return en * en / KT4

def nominal_jack(code):
    g = RF / R1
    vb = VREF * RB / (RT + RB)
    return (vb * (1 + g) - g * VREF * code / 65536) * RL / (RL + RS)

def zero_code():
    g = RF / R1
    vb = VREF * RB / (RT + RB)
    return round(vb * (1 + g) / g * 65536 / VREF)

def code_for(v):
    g = RF / R1
    vb = VREF * RB / (RT + RB)
    kl = RL / (RL + RS)
    return max(0, min(65535, round((vb * (1 + g) - v / kl) / g * 65536 / VREF)))

def run(name, netlist):
    path = os.path.join(HERE, name + ".cir")
    with open(path, "w") as f:
        f.write(netlist)
    p = subprocess.run(["ngspice", "-b", path], cwd=HERE, capture_output=True, text=True, timeout=300)
    out = p.stdout + p.stderr
    with open(os.path.join(RES, name + ".log"), "w") as f:
        f.write(out)
    vals = {}
    for m in re.finditer(r"^RES\s+(\w+)\s*=\s*([-+0-9.eE]+)", out, re.M):
        vals[m.group(1)] = float(m.group(2))
    for m in re.finditer(r"^(\w+)\s*=\s*([-+0-9.eE]+)", out, re.M):
        vals.setdefault(m.group(1), float(m.group(2)))
    if p.returncode != 0 and not vals:
        sys.exit(f"ngspice failed on {name}; see hw/results/{name}.log")
    return vals

OUT_STAGE = f"""
Bref vref 0 V={{VREFV}}+V(rn)
Rrn rn 0 {rnoise(EN_REF):.6g}
Bdac dac 0 V=V(vref)*V(code)/65536+V(dn)
Rdn dn 0 {rnoise(EN_DAC):.6g}
RT vref vb {RT:g}
RB vb 0 {RB:g}
CB vb 0 {{CBV}}
R1 dac inn {R1:g}
RF inn opo {RF:g}
CF inn opo {CF:g}
XU vb inn opo vcc vee OPA
Vmeas opo opo2 0
Dp opo2 vcc BAT54
Dn vee opo2 BAT54
RS opo2 jack {RS:g}
"""

def out_deck(title, load, analysis, params, rails=RAIL):
    return f"""* {title}
.include models.lib
.param VREFV={params.get('VREFV', VREF)} CBV={params.get('CBV', 10e-9)}
Vcc vcc 0 {rails}
Vee vee 0 {-rails}
Vcode code 0 DC {params.get('CODE', 32768)} AC 1
{OUT_STAGE}
{load}
.control
{analysis}
.endc
.end
"""

results = {}

# 1) DC transfer, code -> jack (100 k load)
codes = list(range(0, 65536, 4096)) + [65535]
dc = out_deck("output DC transfer", f"RL jack 0 {RL:g}",
              "dc Vcode 0 65535 4096\n" + "\n".join(
                  f"let j{i} = v(jack)[{i}]\necho RES jack_{i} = $&j{i}" for i in range(17)), {})
v = run("out_dc", dc)
pts = [(c, v[f"jack_{i}"]) for i, c in enumerate(range(0, 65536, 4096)) if f"jack_{i}" in v]
n = len(pts)
mx = sum(c for c, _ in pts) / n
my = sum(y for _, y in pts) / n
slope = sum((c - mx) * (y - my) for c, y in pts) / sum((c - mx) ** 2 for c, _ in pts)
icpt = my - slope * mx
inl_mv = max(abs(y - (icpt + slope * c)) for c, y in pts) * 1e3
model_err_mv = max(abs(y - nominal_jack(c)) for c, y in pts) * 1e3
results["out_dc"] = {
    "jack_at_code0_V": pts[0][1], "jack_at_code61440_V": pts[-1][1],
    "volts_per_code": slope, "zero_code_sim": -icpt / slope, "zero_code_model": zero_code(),
    "max_nonlinearity_mV": inl_mv, "max_dev_from_firmware_model_mV": model_err_mv,
}

# 2) Faults on the output jack
faults = {}
cases = [
    ("hard_+15V_while_-10V", code_for(-10), "Vf jack 0 15", RAIL),
    ("hard_-15V_while_+10V", code_for(10), "Vf jack 0 -15", RAIL),
    ("module_+12V_1k_while_-10V", code_for(-10), "Rsrc jack fx 1k\nVf fx 0 12", RAIL),
    ("hard_+15V_rails_off", zero_code(), "Vf jack 0 15", 0.0),
    ("hard_-15V_rails_off", zero_code(), "Vf jack 0 -15", 0.0),
]
for name, code, load, rails in cases:
    deck = out_deck(name, load,
                    "op\n"
                    "echo RES i_opamp = $&@vmeas[i]\n"   # placeholder, replaced below
                    , {"CODE": code}, rails=rails)
    deck = deck.replace("echo RES i_opamp = $&@vmeas[i]",
                        "let iop = i(vmeas)\necho RES i_opamp = $&iop\n"
                        "let irs = (v(opo2)-v(jack))/" + f"{RS:g}" + "\necho RES i_rs = $&irs\n"
                        "let vop = v(opo)\necho RES v_opamp = $&vop\n"
                        "let ivcc = i(vcc)\necho RES i_vcc = $&ivcc\n"
                        "let ivee = i(vee)\necho RES i_vee = $&ivee")
    r = run("out_fault_" + re.sub(r"[^a-z0-9]+", "_", name.lower().replace("+", "p").replace("-", "m")), deck)
    i_rs = r.get("i_rs", float("nan"))
    faults[name] = {
        "opamp_out_current_mA": r.get("i_opamp", float("nan")) * 1e3,
        "series_R_current_mA": i_rs * 1e3,
        "series_R_power_W": i_rs * i_rs * RS,
        "clamp_diode_current_mA": (r.get("i_opamp", 0) - i_rs) * 1e3,
        "opamp_out_V": r.get("v_opamp", float("nan")),
    }
results["out_faults"] = faults

# 3) Output noise, 20 Hz - 20 kHz, at 0 V and at +10 V
noise = {}
for label, code in (("at_0V", zero_code()), ("at_+10V", code_for(10))):
    deck = out_deck("output noise", f"RL jack 0 {RL:g}",
                    "noise v(jack) Vcode dec 50 20 20k\nsetplot noise2\n"
                    "let tot = onoise_total\necho RES onoise = $&tot", {"CODE": code})
    r = run("out_noise_" + label.replace("+", "p"), deck)
    noise[label + "_uVrms"] = r.get("onoise", float("nan")) * 1e6
noise["note"] = (f"assumes {EN_DAC*1e9:.0f} nV/rtHz DAC output noise and {EN_REF*1e9:.0f} nV/rtHz "
                 "reference noise; 1 cent at 1 V/oct = 833 uV")
results["out_noise"] = noise

# 4) Small-signal bandwidth of the output stage
deck = out_deck("output AC", f"RL jack 0 {RL:g}",
                "ac dec 50 10 1meg\nlet g = abs(v(jack))\nlet g0 = g[0]\n"
                "meas ac f3db when g=0.7071*g0 fall=1\necho RES f3db = $&f3db", {"CODE": zero_code()})
r = run("out_ac", deck)
results["out_ac"] = {"f_minus3dB_kHz": r.get("f3db", float("nan")) / 1e3}

# 5) Reference turn-on transient (firmware: 0 V codes written, then ref enabled)
ton = {}
for cb in (10e-9, 100e-12):
    deck = f"""* ref turn-on, CB={cb}
.include models.lib
Vcc vcc 0 {RAIL}
Vee vee 0 {-RAIL}
Vcode code 0 DC {zero_code()}
{OUT_STAGE.replace('{VREFV}', 'VR(time)').replace('{CBV}', f'{cb:g}')}
.func VR(t) {{ {VREF}*min(max((t-100u)/50u,0),1) }}
RL jack 0 {RL:g}
.control
tran 1u 30m
let ja = abs(v(jack))
meas tran pk max ja
echo RES peak = $&pk
.endc
.end
"""
    r = run("out_refon_cb" + ("10n" if cb > 1e-9 else "100p"), deck)
    ton[f"CB_{cb*1e9:g}nF_peak_mV"] = r.get("peak", float("nan")) * 1e3
results["out_ref_turn_on"] = ton

# 6) Input stage
IN = f"""
.include models.lib
Vin jack 0 DC 0 AC 1
R1 jack m {IN_RS1:g}
R2 m adc {IN_RS2:g}
Rsh adc 0 {IN_RSH:g}
Csh adc 0 {IN_C:g}
Vavdd avdd 0 3.3
X1 adc avdd ADCIN
"""
pts = [-24, -15, -12, -10, 0, 10, 12, 15, 24]
deck = "* input DC\n" + IN + ".control\n" + "\n".join(
    f"alter Vin dc={vin}\nop\nlet a = v(adc)\necho RES vadc_{i} = $&a\nlet ii = -i(vin)\necho RES iin_{i} = $&ii"
    for i, vin in enumerate(pts)) + "\n.endc\n.end\n"
r = run("in_dc", deck)
ind = {}
for i, vin in enumerate(pts):
    va, ii = r.get(f"vadc_{i}", float("nan")), r.get(f"iin_{i}", float("nan"))
    i_esd = ii - va / IN_RSH - va / ADC_RIN   # current not explained by the linear loads
    ind[f"{vin:+d}V"] = {"adc_pin_V": va, "input_current_uA": ii * 1e6, "esd_current_uA": i_esd * 1e6}
results["in_dc"] = ind
z10 = 10 / (r.get("iin_5", float("nan")) or float("nan"))
results["in_summary"] = {
    "input_impedance_kOhm": z10 / 1e3,
    "adc_full_scale_at_jack_V": ADC_FS / (r.get("vadc_5", 1) / 10),
    "within_recommended_-1.3V_at_-15V": r.get("vadc_1", -9) > -1.3,
}
deck = "* input AC/noise\n" + IN + """.control
ac dec 50 10 10meg
let g = abs(v(adc))
let g0 = g[0]
meas ac f3db when g=0.7071*g0 fall=1
echo RES f3db = $&f3db
noise v(adc) Vin dec 50 20 20k
setplot noise2
let tot = onoise_total
echo RES onoise = $&tot
.endc
.end
"""
r = run("in_ac_noise", deck)
k = results["in_dc"]["+10V"]["adc_pin_V"] / 10
res_noise_in = r.get("onoise", float("nan")) / k
adc_noise_in = ADC_NOISE_UV * 1e-6 / k
results["in_ac_noise"] = {
    "f_minus3dB_kHz": r.get("f3db", float("nan")) / 1e3,
    "divider_noise_referred_to_jack_uVrms": res_noise_in * 1e6,
    "adc_noise_referred_to_jack_uVrms_full_band": adc_noise_in * 1e6,
    "total_referred_to_jack_uVrms": math.hypot(res_noise_in, adc_noise_in) * 1e6,
    "note": "ADC noise at OSR 64 is shaped toward Nyquist; a 1 kHz software low-pass cuts it several-fold",
}

# 7) Rail protection (series Schottky per rail)
deck = """* rail reverse protection
.include models.lib
Vp hp 0 DC {vp}
Dp hp p12 SS14
Rl p12 0 300
.control
alter Vp dc=12
op
let v1 = v(p12)
echo RES fwd_rail_V = $&v1
alter Vp dc=-12
op
let i2 = -i(vp)
echo RES rev_current = $&i2
.endc
.end
""".replace("{vp}", "12")
r = run("power_reverse", deck)
results["power"] = {"forward_rail_V_at_40mA": r.get("fwd_rail_V", float("nan")),
                    "reversed_header_current_uA": abs(r.get("rev_current", float("nan"))) * 1e6}

# 8) Group A output: PCM3168A differential DAC -> difference amplifier (gain 2.87) -> 1 k -> jack
CODEC_VCC, AO_RI, AO_RF = 4.5, 28.7e3, 82.5e3
VCOM = CODEC_VCC / 2
def ao_deck(name, vd, load="RL jack 0 100k", rails=RAIL, extra=""):
    return f"""* {name}
.include models.lib
Vcc vcc 0 {rails}
Vee vee 0 {-rails}
Vp vp 0 {VCOM + vd / 2}
Vm vm 0 {VCOM - vd / 2}
R1 vm inn {AO_RI:g}
RF inn opo {AO_RF:g}
CF inn opo 47p
R2 vp inp {AO_RI:g}
RG inp 0 {AO_RF:g}
CG inp 0 47p
XU inp inn opo vcc vee OPA
Vmeas opo opo2 0
Dp opo2 vcc BAT54
Dn vee opo2 BAT54
RS opo2 jack 1k
{load}
{extra}
.control
op
let j = v(jack)
echo RES jack = $&j
let ip = -i(vp)
echo RES ivp = $&ip
.endc
.end
"""
fs_diff = 0.8 * CODEC_VCC        # full-scale differential peak (1.6 x VCC Vpp)
ao = {}
for label, vd in (("+FS", fs_diff), ("-FS", -fs_diff), ("zero", 0.0)):
    r = run("ao_" + label.replace("+", "p").replace("-", "m"), ao_deck(label, vd))
    ao[label] = r.get("jack", float("nan"))
r = run("ao_codec_off", ao_deck("codec unpowered", 0.0).replace(f"Vp vp 0 {VCOM}", "Vp vp 0 0").replace(f"Vm vm 0 {VCOM}", "Vm vm 0 0"))
ao["codec_unpowered"] = r.get("jack", float("nan"))
r = run("ao_load", ao_deck("codec pin current at +FS", fs_diff))
results["codec_out"] = {"jack_at_+FS_V": ao["+FS"], "jack_at_-FS_V": ao["-FS"], "jack_at_zero_V": ao["zero"],
                        "jack_codec_unpowered_V": ao["codec_unpowered"],
                        "codec_pin_load_kOhm": (VCOM + fs_diff / 2) / max(abs(r.get("ivp", 1e-9)), 1e-12) / 1e3}

# 9) Group A input: jack -> 100 k -> inverting stage on the codec's 4.5 V, V+ = VCC*8.2/18.2 so 0 V maps to VCOM
AI_RI, AI_RF = 100e3, 11e3
def ai_deck(vin):
    return f"""* codec input {vin}
.include models.lib
Vcc vcc 0 {CODEC_VCC}
Vee vee 0 0
Vcm vcm 0 {VCOM}
Vin jack 0 {vin}
RT vcc vplus 10k
RB vplus 0 8.2k
CB vplus 0 1u
RI jack inn {AI_RI:g}
RF inn opo {AI_RF:g}
CF inn opo 100p
XU vplus inn opo vcc vee OPA
RO opo adc 100
RADC adc vcm 45k
.control
op
let a = v(adc)
echo RES vadc = $&a
let ii = -i(vin)
echo RES iin = $&ii
.endc
.end
"""
ai = {}
for vin in (-24, -15, -10, 0, 10, 15, 24):
    r = run(f"ai_{'m' if vin < 0 else 'p'}{abs(vin)}", ai_deck(vin))
    ai[f"{vin:+d}V"] = {"codec_pin_V": r.get("vadc", float("nan")), "input_current_uA": r.get("iin", float("nan")) * 1e6}
results["codec_in"] = ai

# 10) USB DC-DC ripple: isolated 5 V -> +-15 V module, pi filter, LDO to +-11 V, op-amp PSRR
FSW, RIPPLE_PP = 100e3, 0.10          # B0515S-2WR3: assume 100 mVpp at ~100 kHz (check the datasheet)
LDO_PSRR_DB, OPA_PSRR_DB = 45.0, 60.0  # TPS7A4901 / OPA4172 at 100 kHz, conservative readings of the curves
deck = f"""* DC-DC ripple through the pi filter
.include models.lib
Vdc src 0 DC 15 SIN(0 {RIPPLE_PP / 2} {FSW})
Rs src a 0.5
C1 a 0 22u
L1 a b 10u
RL1 b c 0.08
C2 c 0 22u
C3 c 0 22u
Rload c 0 500
.control
tran 0.2u 3m 2m
let r = v(c)
meas tran vmax max r
meas tran vmin min r
let pp = vmax - vmin
echo RES ripple_pp = $&pp
.endc
.end
"""
r = run("dcdc_ripple", deck)
pp_ldo_in = r.get("ripple_pp", float("nan"))
pp_rail = pp_ldo_in * 10 ** (-LDO_PSRR_DB / 20)
pp_jack = pp_rail * 10 ** (-OPA_PSRR_DB / 20) * (1 + 82.5 / 10)    # supply ripple is referred to the input, times noise gain
results["dcdc_ripple"] = {"module_ripple_mVpp": RIPPLE_PP * 1e3, "after_pi_filter_mVpp": pp_ldo_in * 1e3,
                          "rail_after_ldo_uVpp": pp_rail * 1e6, "estimated_at_jack_uVpp": pp_jack * 1e6,
                          "assumptions": f"{FSW/1e3:.0f} kHz, LDO PSRR {LDO_PSRR_DB} dB, op-amp PSRR {OPA_PSRR_DB} dB"}

# 11) Tier A (breadboard): MCP4728 0..4.096 V -> TL074 inverting stage, ADS1115 input network
def ta_out_deck(vdac):
    return f"""* tier A out
.include models.lib
Vcc vcc 0 12
Vee vee 0 -12
Vd dac 0 {vdac}
Vdd vdd 0 3.3
RT vdd vb 10k
RB vb 0 10.7k
R1 dac inn 10k
RF inn opo 49.9k
XU vb inn opo vccl veel OPA
Vcl vccl 0 10.5
Vel veel 0 -10.5
RS opo jack 1k
RL jack 0 100k
.control
op
let j = v(jack)
echo RES jack = $&j
.endc
.end
"""
ta = {f"dac_{v}V": run(f"ta_out_{v}".replace(".", "_"), ta_out_deck(v)).get("jack", float("nan")) for v in (0.0, 2.048, 4.095)}
ta_in = {}
for vin in (-24, -15, -10, 10, 15, 24):
    deck = f"""* tier A in
.include models.lib
Vin jack 0 {vin}
Vdd vdd 0 3.3
R1 jack n 100k
R2 vdd n 22k
R3 n 0 22k
.control
op
let a = v(n)
echo RES vadc = $&a
.endc
.end
"""
    ta_in[f"{vin:+d}V"] = run(f"ta_in_{'m' if vin < 0 else 'p'}{abs(vin)}", deck).get("vadc", float("nan"))
results["tier_a"] = {"out_jack_V (TL074 swing 10.5 V on +-12 V)": ta, "in_adc_pin_V": ta_in}

with open(os.path.join(RES, "spice_results.json"), "w") as f:
    json.dump(results, f, indent=2)

# ---- summary + pass/fail checks ----
o = results["out_dc"]
checks = [
    ("output reaches beyond +-10 V into 100 k", o["jack_at_code0_V"] > 10.0 and nominal_jack(65535) < -10.0),
    ("sim matches firmware cal model within 5 mV", o["max_dev_from_firmware_model_mV"] < 5.0),
    ("hard +-15 V fault: series R < 0.66 W (ERJ-P08 1206)",
     all(fa["series_R_power_W"] < 0.66 for fa in faults.values())),
    ("hard +-15 V fault: op-amp current < 40 mA",
     all(abs(fa["opamp_out_current_mA"]) < 40 for fa in faults.values())),
    ("rails-off fault: Schottky clamp carries > 80 % of the fault current",
     all(abs(faults[n]["clamp_diode_current_mA"]) > 0.8 * abs(faults[n]["series_R_current_mA"])
         for n in faults if "rails_off" in n)),
    ("ref turn-on transient < 200 mV with CB = 100 pF (50 us ref ramp)", ton["CB_0.1nF_peak_mV"] < 200),
    ("input impedance ~100 k", 95 < results["in_summary"]["input_impedance_kOhm"] < 115),
    ("ADC pin inside abs max (-1.6 V) at -15 V", results["in_dc"]["-15V"]["adc_pin_V"] > -1.6),
    ("ADC ESD current < 1 mA at +-24 V",
     max(abs(results["in_dc"][k2]["esd_current_uA"]) for k2 in ("+24V", "-24V")) < 1000),
    ("reversed rail header blocks (< 10 uA)", results["power"]["reversed_header_current_uA"] < 10),
    ("codec out: +-full scale reaches beyond +-10 V", results["codec_out"]["jack_at_+FS_V"] > 10.0 and results["codec_out"]["jack_at_-FS_V"] < -10.0),
    ("codec out: 0 V when the codec is unpowered or at VCOM", abs(results["codec_out"]["jack_codec_unpowered_V"]) < 0.01 and abs(results["codec_out"]["jack_at_zero_V"]) < 0.01),
    ("codec out: DAC pin load >= 15 k (datasheet DC-coupled minimum)", results["codec_out"]["codec_pin_load_kOhm"] >= 15),
    ("codec in: pin stays within 0..4.5 V for +-24 V at the jack", all(-0.05 < x["codec_pin_V"] < 4.55 for x in results["codec_in"].values())),
    ("codec in: 0 V maps to VCOM within 10 mV", abs(results["codec_in"]["+0V"]["codec_pin_V"] - VCOM) < 0.01),
    ("codec in: +-10 V inside the ADC full scale (VCOM +-1.27 V)", all(abs(results["codec_in"][k2]["codec_pin_V"] - VCOM) < 0.2 * CODEC_VCC * 2 ** 0.5 for k2 in ("+10V", "-10V"))),
    ("DC-DC ripple at the jack < 50 uVpp (estimate)", results["dcdc_ripple"]["estimated_at_jack_uVpp"] < 50),
    ("tier A in: ADS1115 pin inside -0.3..3.6 V (abs max) for +-15 V", all(-0.3 <= results["tier_a"]["in_adc_pin_V"][k2] <= 3.6 for k2 in ("+15V", "-15V"))),
]
results["checks"] = {n: bool(ok) for n, ok in checks}
with open(os.path.join(RES, "spice_results.json"), "w") as f:
    json.dump(results, f, indent=2)

lines = ["# SPICE summary (generated by hw/spice/run_spice.py)", ""]
lines += [f"- [{'x' if ok else ' '}] {n}" for n, ok in checks]
lines += ["", "```json", json.dumps({k2: v2 for k2, v2 in results.items() if k2 != "checks"}, indent=1), "```"]
with open(os.path.join(RES, "spice_summary.md"), "w") as f:
    f.write("\n".join(lines) + "\n")
npass = sum(ok for _, ok in checks)
print(f"spice: {npass}/{len(checks)} checks pass")
for n_, ok in checks:
    if not ok:
        print("FAIL", n_)
sys.exit(0 if npass == len(checks) else 1)
