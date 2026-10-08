> **Status: parked (2026-10-08).** Design v2 (ESP32-S3 + PCM3168A audio group at 32 kHz + DAC8568/ADS131M08 CV group over USB-MIDI SysEx) is complete on paper: firmware builds, `./check.sh` passes (host tests + ngspice), netlist/BOM/JLC steps in `hw/`. Nothing has been built or powered. Before resuming: verify the T-Display-S3 socket pin order and the A0515S DC-DC pinout, and do the human safety checklist below. For 48 kHz on all channels, see the ESP32-P4 note in the bandwidth section.

# DIY DC-coupled USB interface for Circuit Studio (v2, ESP32-S3)

An ESP32-S3 display board (LilyGO T-Display-S3) on a factory-assembled carrier PCB. The
computer sees a normal **USB Audio Class 2** device plus a **USB-MIDI** port; BLE-MIDI works
without a computer. Jouni only solders through-hole parts.

| Group | Channels | Converter | Path to the browser | Use |
|---|---|---|---|---|
| A audio-rate | 8 out, 6 in | PCM3168A codec, HPF bypassed, DC-coupled | UAC2, **32 kHz / 16-bit** (Web Audio) | audio, FM, envelopes, LFOs |
| P precision CV | 8 out, 8 in | DAC8568 (16-bit) + ADS131M08 (24-bit) | SysEx CV frames over USB-MIDI (Web MIDI), 2 kHz device rate | 1 V/oct pitch, gates, slow CV |

Plus: MIDI->CV mapper on the P outputs (USB-MIDI and BLE-MIDI), a status display (meters,
USB/BLE/calibration/power), USB-C power as the main supply, optional Eurorack +-12 V and LiPo.

Status: **designed and verified in software, not built.** `./check.sh` passes: 468 host-test checks
(+110 for the breadboard build), 18/18 ngspice checks, both ESP-IDF builds clean, netlist/KiCad
netlist/BOMs/schematic generated. The Pico 2 v1 design is in `archive/pico-v1-firmware/` and git history.

## SAFETY: a person reviews the board before it touches rack power or a battery

Nothing has been built or measured. Before the first power-up, someone who can read the schematic
checks, with a **current-limited bench supply first** (5 V, 1 A limit on the USB side):

- [ ] **USB 5 V:** +5V_SYS present through U20 (LM66100); no short to GND; idle draw under ~0.3 A before
      the codec is configured.
- [ ] **Analog rails:** +11 V / -11 V at every op-amp supply pin; A0515S module orientation matches its
      datasheet; pi-filter inductors fitted.
- [ ] **Converter rails:** +3V3_A, +3V3_D, +4V5_A correct; nothing from the +-11 V or rack reaches them.
- [ ] **Eurorack (optional):** red stripe = pin 1 = -12 V; D22/D23 (SS14) in series with each rail; a
      reversed ribbon draws ~3 uA (SPICE). Rack and USB DC-DC are diode-ORed, never back-feed the rack.
- [ ] **Output current limits:** 1 k (0.66 W 1206 anti-surge) on every output jack, BAT54S to both rails;
      a hard +-15 V fault on an output costs 0.59 W in the 1 k and 24 mA in the op-amp (SPICE).
- [ ] **Power-on state:** every jack reads 0 V (+-50 mV) at power-up, USB unplug, rail dip and reboot.
      DAC8568 must be the **B** grade (midscale reset), LDAC and **CLR tied high**. Firmware mutes all
      outputs until the +11 V rail has been above 10.5 V for 100 ms.
- [ ] **Inputs:** 100 k (precision) / 100 k into a 4.5 V-supplied op-amp (audio) before anything active:
      +-24 V at a jack keeps the ADC/codec pins inside their limits (SPICE).
- [ ] **LiPo (optional):** use only the T-Display-S3's own protected charger and a polarity-keyed JST 1.25
      battery with a protection circuit; the carrier never charges the battery and the rails cannot feed it
      (LM66100 ideal diodes, boost module output only). Check polarity before plugging the cell.
- [ ] **Ground loop:** computer USB ground and rack ground join on this board. If other modules hum, use a
      full-speed USB isolator (ADuM3160-based).
- [ ] Then the rack with nothing patched, then one cable at a time.

## Decisions

### Controller: ESP32-S3 display board (Jouni's choice), with the honest limits

| | ESP32-S3 (T-Display-S3) | STM32H7 (Daisy / H7 boards) | ESP32-P4 board |
|---|---|---|---|
| Board with display | ~GBP 17-20 | ~GBP 30-60 | ~GBP 30-45 |
| USB | full speed (12 Mbit/s) | FS on most boards, HS needs an external ULPI PHY | **high speed (480 Mbit/s)** on chip |
| UAC2 | TinyUSB 0.21 (dwc2), async feedback, works | TinyUSB / ST stack | TinyUSB HS |
| DSP | 2 x 240 MHz LX7, single-precision FPU, PIE int SIMD | 480 MHz M7, double FPU | 2 x 400 MHz RISC-V, FPU, SIMD |
| Wireless | Wi-Fi + **BLE** on chip | none | via an ESP32-C6 companion on most boards |

The S3 wins on board cost, built-in display boards and BLE. No dealbreaker for **8 audio-rate channels
each way at 32 kHz** or for CV. The dealbreaker for Jouni's option "8 + 8 at 48 kHz" is USB full speed
itself, which applies equally to the H7 boards without HS: see the next section. If 48 kHz / 24-bit audio
on all channels matters, the fix is an **ESP32-P4 carrier** (HS USB; same ESP-IDF/TinyUSB firmware layout).

### USB full-speed budget (why 32 kHz, and why the CV group uses a second path)

Full speed is one shared 12 Mbit/s bus: OUT and IN share each 1 ms frame, and periodic (isochronous)
traffic may use at most 90 % of it. With worst-case bit stuffing (USB 2.0 section 5.11.3) that is about
1125 payload bytes per frame across all audio endpoints, not 1023 per direction.

| Scheme | OUT B/frame | IN B/frame | Bus time | Fits |
|---|---|---|---|---|
| 8 out + 8 in, 48 kHz/16 (option a as asked) | 784 | 784 | 1248 us | no (more than the raw bus) |
| 16 out + 16 in, 24 kHz/16 (option b) | 800 | 800 | 1273 us | no |
| 8 out + 6 in, 48 kHz/16 | 784 | 588 | 1095 us | no |
| 4 out + 4 in, 48 kHz/24 | 588 | 588 | 943 us | no |
| 6 out + 4 in, 48 kHz/16 | 588 | 392 | 790 us | yes |
| 8 out + 8 in, 32 kHz/16 | 528 | 528 | 849 us | yes (tight) |
| **8 out + 6 in, 32 kHz/16 (chosen)** | 528 | 396 | **746 us** | **yes, 17 % margin** |

So the audio-rate group runs at 32 kHz (14 kHz audio bandwidth; Chrome resamples a 48 kHz
AudioContext to the device transparently, or Circuit Studio opens its context at 32 kHz). The 8
precision channels each way go over the second path below, so the browser still sees all 30 jacks.
The host tests check the descriptor against this budget.

### CV path: USB-MIDI SysEx (Web MIDI), not WebUSB

| | USB-MIDI SysEx (chosen) | WebUSB vendor bulk |
|---|---|---|
| Browser support | Chrome, Edge, Opera, Firefox (site permission); not Safari | Chromium only |
| Permission | Web MIDI SysEx prompt (Circuit Studio needs it for calibration anyway) | device chooser prompt |
| macOS with the audio driver attached | works (class-compliant MIDI) | composite-device claiming is fragile |
| Also usable by | DAWs, BLE-MIDI, the on-device MIDI->CV mapper | only our page |
| Rate | 1 kHz host frames (32 B each, 44 B on the wire), 500 Hz CV-in frames | several kHz |

Format: one SysEx frame carries all 8 outputs as signed 21-bit values in 10 uV units (`firmware/common/proto.h`).
The device glides each new value in over 2 ms, so 1 kHz frames with ~1 ms Web MIDI jitter give smooth
2 kHz DAC updates without zipper steps. Latency host->jack is about 2-4 ms plus main-thread delays in the
page (send with timestamps to keep jitter near 1 ms). CV inputs stream back at up to 1 kHz (default 500 Hz).
The frames double as a heartbeat: if none arrive for 2 s, host-driven outputs glide to 0 V.

### Converters: three tiers, one recommendation

| | Tier A (breadboard) | Tier B precision | Tier C audio codec |
|---|---|---|---|
| Parts | 2x MCP4728 + 2x ADS1115 modules | DAC8568B + ADS131M08 | PCM3168A (8 out / 6 in) |
| Resolution / rate | 12-bit ~1 kHz out; 16-bit ~125 Hz/ch in | 16-bit out, 24-bit in; 2 kHz here | 24-bit, 8-96 kHz (32 kHz here) |
| DC accuracy | 5 mV steps (6 cents) | offset drift 0.5 uV/C, ref 2-5 ppm/C: pitch-grade after calibration | gain error +-2 %, bipolar zero +-1 % FSR (calibrated), **drift not specified**, gain follows the 4.5 V supply |
| Audio quality | none | DAC: SNR 83 dB, THD -63 dB at 1 kHz (poor); ADC 102 dB DR | DAC 112 dB DR / -94 dB THD+N; ADC 107 dB / -93 dB |
| Converter latency | ms | < 0.5 ms | ~1 ms (delta-sigma filters) |
| Price | modules ~GBP 20-40 | ~USD 17 + 4.39 | USD 6.30 |
| JLC stock (checked) | n/a | ADS131M08 15k; DAC8568B: global sourcing | PCM3168APAPR 378 |

The PCM3168A holds up as the audio-rate engine: register 82 `BYP = 111` bypasses the ADC high-pass
filter (datasheet 9.3.7), its DAC drives DC-coupled loads >= 15 k, and both sides take an op-amp level
shift. Its unspecified offset drift and supply-ratiometric gain rule it out for 1 V/oct pitch, so it is the
**audio** group, and the DAC8568/ADS131M08 pair is the **pitch** group. The S3's I2S does 8 x 32-bit TDM
slots at 32 kHz with an exact MCLK (160 MHz / 9.765625, 9-bit fractional divider; checked in IDF's
`i2s_ll.h`). Other codecs (CS42448: HPF disable bit; PCM186x) look possible but were not verified.

### Power

USB-C 5 V is the main supply: LM66100 ideal diode -> +5V_SYS -> isolated A0515S-2WR3 (+-15 V, 2 W) ->
pi filters -> TPS7A4901/TPS7A3001 -> +-11 V for the op-amps; LP5907 4.5 V for the codec, LP5907 3.3 V for
the precision converters, AMS1117 3.3 V for logic. Ripple: 100 mVpp at the module -> 0.08 mVpp after the
pi filter (ngspice) -> sub-microvolt at the jacks after LDO and op-amp PSRR (datasheet curves, conservative).
The rack +-12 V is diode-ORed into the LDO inputs (an alternative supply, not needed), and a LiPo boost
module can feed +5V_SYS through a second LM66100 (optional). Switching between sources is seamless at
+5V_SYS; the firmware still mutes all outputs while the rail sense is below 10.5 V or not yet stable for 100 ms.

| Population | 5 V current | Supply | LiPo 1000 mAh runtime |
|---|---|---|---|
| Full (A + P) | ~0.65-0.75 A | USB-C 1.5 A or a USB 3 port (USB 2 ports give 0.5 A) | ~1 h |
| Precision only | ~0.3 A | any USB port | ~2 h |
| Audio only | ~0.55 A | USB-C / USB 3 | ~1.2 h |

The PCM3168A alone draws ~1.2 W, so the battery is a nice-to-have. The `low_power` flag limits outputs to
+-5 V, but op-amp quiescent current dominates, so it saves little; dimming the display saves more (~0.25 W).
Battery %, auto-dim (30 s) and a 0 V shutdown + deep sleep below 3.3 V are in `firmware/common/power.c`.

## Hardware

`hw/schematic/index.html` (6 sheets), `hw/netlist.csv`, `hw/kicad/cv_interface.net`, BOMs per population
(`hw/bom_full.csv`, `hw/bom_precision.csv`, `hw/bom_audio.csv`; JLC upload versions in `hw/jlc/`),
`hw/LAYOUT.md` (layout rules, form factors, **JLCPCB order steps**), `hw/breadboard.md` (Tier A).
Everything comes from `hw/gen_hw.py`; stock and prices from `tools/jlc_stock.py` (JLC's public parts search,
fetched 2026-10-08; recheck before ordering). Every SMT line has a stocked LCSC part except the DAC8568**B**
(JLC Global Sourcing from Mouser/Digi-Key; JLC only stocks the C grade, which needs a 5 V supply and resets to
zero scale). Substitutions made for stock: Yageo RT0805 0.1 % resistors (the audio difference amp uses
28.7 k / 82.5 k, gain 2.87), 3PEAK TPLP5907 (LP5907 pin-compatible) for 3.3 V, Mornsun A0515S-2WR3 (439 in stock).

| Population | One assembled board (USD) | Per board when ordering 5 | JLC extended part types |
|---|---|---|---|
| Full (8+6 audio, 8+8 CV, 30 jacks) | ~178 | ~95 | 26 |
| Precision only (8+8 CV) | ~139 | ~74 | 19 |
| Audio only (8+6) | ~111 | ~55 | 15 |

Most of a single-board order is JLC's one-off fees (about USD 3 per extended part type, setup, stencil)
and the 4-layer PCB; the parts themselves are USD 44 (full) including the USD 17 DAC. Estimates include the
T-Display-S3 (~USD 16) and the through-hole parts (jacks, sockets, DC-DC module, header). Cost breakdown:
`hw/results/bom_summary.json`.

Display boards that fit the carrier idea: **LilyGO T-Display-S3** (1.9" 170x320, LiPo charger, uses all 13
free header GPIOs, the reference), LilyGO T-Display-S3 AMOLED (same pinout family, ~GBP 25), Waveshare
ESP32-S3-LCD-1.69 / -1.47 (SPI displays leave more GPIOs free, but need a different socket footprint).

## Firmware (ESP-IDF 5.4, TinyUSB 0.21)

`firmware/common/` is plain C shared with the host tests: calibration, signal engines, SysEx protocol, USB
descriptors, MIDI->CV mapper, BLE-MIDI codec, power supervision. `firmware/esp32s3/main/` has the drivers:
`codec.c` (I2S TDM + PCM3168A control), `precision.c` (SPI2, 2 kHz gptimer), `usb.c`, `midi_router.c`,
`ble_midi_nimble.c`, `display.c` (ST7789 i80), `power_mgr.c`, `storage.c` (NVS), `tier_a.c` (breadboard).

- **Cores:** core 1 runs TinyUSB, the codec block loop (1 ms) and the precision loop (2 kHz), nothing
  else; core 0 runs NimBLE (host and controller pinned there), the display, power and the MIDI router.
  BLE never preempts the audio core. Estimated load: core 1 ~25 %, core 0 ~20 %, leaving most of core 0
  for on-device DSP later.
- **Glitch check with BLE active (to do on hardware):** loop Audio Out 1 -> Audio In 1, play a sine, flood
  BLE-MIDI with 100 notes/s for 10 min, and read the xrun counters on the display / `cvcal.py info`
  (`underruns`) plus discontinuities in the recorded loopback.
- **On-device DSP later:** our Rust factory kernels can run as `no_std` f32 code via `espup` (Xtensa
  LLVM) + `esp-hal`, or linked into this C firmware as a static library. Keep them f32 (the S3 FPU is single
  precision; f64 is software), and call `esp-dsp` (C, uses the PIE SIMD) for FIRs/FFTs; Rust has no PIE
  intrinsics. A ladder-class filter at 32 kHz is a few percent of one core.
- **Safety in firmware:** P outputs: DAC soft reset (midscale, reference off -> 0 V) -> 0 V codes -> reference
  on; A outputs: codec reset keeps VOUT at VCOM (0 V differential). Host stream stop -> hold 20 ms -> ramp
  to 0 V in 5 ms. USB unmount, rail fault, low battery and DFU all force 0 V.

Build:

```sh
# one-time: ESP-IDF v5.4.2 (installed in ~/esp/esp-idf on Jouni's Mac)
git clone -b v5.4.2 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh
cd firmware/esp32s3 && idf.py build                    # PCB build
idf.py -B build-tiera -DCV_TIER_A=1 -DSDKCONFIG=build-tiera/sdkconfig build   # breadboard build
idf.py -p /dev/cu.usbmodem* flash                      # first flash: hold BOOT, plug USB-C
./check.sh                                             # all checks, logs in logs/
```

Later updates: `python3 tools/cvcal.py dfu` reboots into the ROM USB downloader (outputs parked at 0 V).

## Calibration

```sh
pip install mido python-rtmidi
python3 tools/cvcal.py cal-out    # DMM on each of the 16 outputs, two readings each
python3 tools/cvcal.py cal-in     # patch Out A1..A6 -> In A1..A6 and Out P1..P8 -> In P1..P8
python3 tools/cvcal.py save
```

## Verification without hardware

| Check | Result |
|---|---|
| `make -C firmware/test`: descriptors + USB budget, codec/DAC/ADC encodings, exact clock dividers, calibration, engines, mapper (1 V/oct worst error 0.16 mV through calibration, bend, legato/retrigger, last-note priority), BLE-MIDI codec (running status, SysEx across packets, MTU split), power supervision, SysEx protocol | 468 checks pass; Tier A build 110 |
| `hw/spice/run_spice.py`: precision out/in (gain vs firmware model 0.09 mV, faults at +-15/24 V, noise 104 uVrms, ref turn-on), codec out (+-10.25 V at full scale, 0 V unpowered, 111 k load on the codec), codec in (pin stays 0.37-4.13 V for +-24 V), DC-DC ripple, rail reverse block, Tier A stages | 18/18 |
| ESP-IDF builds (PCB and Tier A), 0 warnings | 640 KB of 1 MB partition |

## Open items

- Untested on hardware: I2S TDM framing vs the PCM3168A (LJ TDM, 256 fs BCK), ADS131M08 bring-up,
  TinyUSB feedback on macOS/Windows at 32 kHz, BLE coexistence numbers, display orientation offsets.
- Verify before layout: T-Display-S3 socket pin order, A0515S pinout, DAC8568B sourcing (see `hw/LAYOUT.md`).
- Chrome must expose the 6-channel input and 8-channel output (`getSettings().channelCount`); noted in the
  Circuit Studio queue.
- VID:PID 1209:0001 is a pid.codes test ID: private use only.
