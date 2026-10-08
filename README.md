# DIY DC-coupled USB interface for Circuit Studio

A Raspberry Pi Pico 2 that enumerates as a normal **USB Audio Class 2** device with
**4 outputs and 4 inputs at 48 kHz**, every channel DC-coupled to Eurorack levels
(±10 V). The browser sees an ordinary multichannel sound card, so Circuit Studio's
Milestone 2 interface work (getUserMedia with processing off, discrete multichannel
output, calibration wizard, limiters, Interface In/Out modules) runs unchanged.
A USB-MIDI port carries SysEx for status and the calibration table in flash.

Status: **designed and verified in software, not built yet.**
`./check.sh` passes: firmware builds for Pico 2 (4 and 8 outputs), 230 host unit-test
checks, 10/10 SPICE checks on the analog front end, netlist/BOM/schematic generated.

| | |
|---|---|
| Outputs | 4 (8 with a second op-amp), 16-bit DAC8568, updated at 48 kHz, ±10.2 V reach, 1 k series, 40 kHz smoothing pole |
| Inputs | 4, 24-bit simultaneous ADS131M04 at 48 kSPS, 108 k input impedance, ±14.7 V full scale |
| Scale | digital 1.0 = +10 V both directions (1 V/oct = 0.1 per octave) |
| Latency (device side) | ~3.5 ms out (2 ms USB FIFO + 1.5 ms DAC queue), ~1-2 ms in |
| Power | converters and Pico from USB; op-amps from the rack's ±12 V (10-pin header, reverse-blocked) |
| Cost | ~£39 for 4 out / 4 in, ~£47 for 8 out / 4 in (hw/bom.csv, single quantity, approximate) |

## SAFETY: a person reviews the board before it touches rack power

Nothing here has been built or measured. Before connecting the 10-pin header to a rack,
someone who can read the schematic checks the following, with the board on a **current-limited
bench supply first** (±12 V, 50 mA limit):

- [ ] **Polarity.** Red stripe of the ribbon = pin 1 = −12 V. J1 pins 1-2 go to −12 V, 9-10 to +12 V.
      Check the header orientation against the busboard with the rack off.
- [ ] **Reverse block.** D1/D2 (SS14) are in series with each rail, cathode toward +12 V on the top
      rail, anode toward −12 V on the bottom rail. With the ribbon reversed, no current flows
      (SPICE: 3 µA).
- [ ] **Rails.** With the bench supply: +12 V and −12 V at the op-amp pins 4 and 11 (minus ~0.3 V
      diode drop), no rail shorted to GND, supply current under ~15 mA idle.
- [ ] **Converter rails come from USB only.** No path from ±12 V into +3V3_A, +3V3_D or VBUS
      (meter it with everything unpowered).
- [ ] **Output current limits.** 1 k series resistor on every output jack (0.66 W 1206 anti-surge
      part: a hard ±15 V fault against an output at the opposite rail dissipates 0.63 W in it, SPICE),
      BAT54S from the op-amp output to both rails, op-amp current under 26 mA in that fault.
- [ ] **Power-on state.** Every jack reads 0 V (±50 mV) when USB is plugged in, when it is unplugged,
      and while the rack powers up first. DAC8568 **B grade** (midscale reset), LDAC and **CLR tied
      high** (a CLR edge would load zero-scale, which is +10 V).
- [ ] **Inputs.** Each input is 100 k series before anything active; ±15 V at a jack puts −1.22 V on
      the ADC pin (limit −1.6 V).
- [ ] **Ground.** One ground point between rack GND, USB GND and the converters. If other modules hum,
      add a full-speed USB isolator (ADuM3160-based).
- [ ] Then the rack, with nothing patched, then one patch cable at a time.

## Platform choice

| | RP2350 Pico 2 (chosen) | RP2040 Pico | ESP32-S3 | Daisy Seed / Patch.SM (STM32H7) |
|---|---|---|---|---|
| Cost | ~£4.80 | ~£3.60 | ~£6-10 | ~£25-30 / ~£35 |
| USB | FS, TinyUSB in the SDK | same | FS, TinyUSB via IDF | FS (HS PHY not wired on Seed) |
| UAC2 maturity | TinyUSB 0.18 UAC2 + feedback EP, widely used on RP2040/RP2350 | same | works, less used, IDF layer in between | libDaisy has no UAC2 class; would need TinyUSB/ST port |
| Determinism | PIO + DMA: converter timing in hardware, no ISR per sample | same | no PIO; SPI + ISR, Wi-Fi stack competes | good DMA/SAI, built-in codec is AC-coupled |
| Clocking | 150 MHz / 3125 = 48 kHz exactly; 6.144 MHz ADC clock exact in the 16.16 GPOUT divider | 125 MHz: DAC exact, ADC clock 70 ppm off | fractional PLL | audio PLL |
| Channels | limited by FS USB (~1 KB/frame): 4/4 at 24-bit in, 8/4 at 16-bit | same | same | same |

Picked RP2350: cheapest, already in Jouni's minimal-parts synth, proven TinyUSB UAC2 path,
and PIO makes the converter SPI exactly periodic without interrupts. The Daisy's strength
(codec, HS) is irrelevant here because the codec is AC-coupled. Rust/embassy has no maintained
UAC2 class yet; C with the Pico SDK + TinyUSB is the proven route.

## How it works

```
USB OUT iso ─► TinyUSB FIFO ─► out_engine (limiter, cal, mute) ─► DAC frame ring ─► DMA (timer-paced) ─► PIO ─► DAC8568
                (FIFO-count async feedback keeps it half full)                        4 x 48 kHz, 32-bit frames
ADS131M04 DRDY ─► PIO frame reader ─► DMA ring ─► in_engine (cal) ─► TinyUSB IN FIFO ─► USB IN iso (47/48/49-sample packets)
```

- **One clock domain.** The DAC pacing (DMA timer 4/3125 of 150 MHz) and the ADC clock
  (150 MHz / 24.4140625 = 6.144 MHz, ADC OSR 64 → 48 kSPS) both come from the Pico's crystal,
  so DAC and ADC run at the same 48 kHz. The host follows the device: asynchronous OUT endpoint
  with a feedback endpoint (TinyUSB FIFO-count method; 3-byte 10.14 format on macOS full speed,
  4-byte elsewhere, chosen at enumeration).
- **No per-sample interrupts.** Both rings are DMA with a control channel that re-arms the data
  channel (works on RP2040 too). The main loop on core 0 tops up the DAC ring to 1.5 ms ahead and
  drains the ADC ring; it also writes 2 ms of "hold last value" beyond the write point, so a late
  loop repeats the last sample instead of replaying old data.
- **Every output updates at the same instant.** Channels 1..n−1 are written to input registers,
  the last frame is "write and update all".
- **Safe power-on.** Boot order: load calibration, DAC software reset (B grade → midscale,
  internal reference off, so the ratiometric output stage sits at 0 V), write the calibrated 0 V
  codes, then enable the reference. The output offset Vb is a divider from the DAC's own reference,
  so with the reference off every jack is 0 V, and drift of the reference cancels at 0 V.
- **Watchdog.** Host data stops (stream closed, USB suspend, unplug): the outputs hold the last value
  for 20 ms, then ramp to 0 V in 5 ms. The hardware watchdog (200 ms) reboots a hung loop, and the
  boot path above returns the jacks to 0 V. Unplugging a bus-powered board removes the DAC supply,
  which also gives 0 V. Before rebooting into the bootloader the firmware parks every output at 0 V.

### Converter trade-offs

- **Audio-rate outputs.** 4 channels x 32-bit frames at 25 MHz SCLK take 5.5 µs of each 20.8 µs
  period (8 channels: 11 µs), and the DAC settles in 5-10 µs, so 48 kHz is fine and the outputs
  can carry audio, FM and sharp gates. The price is zero-order-hold images above 24 kHz (a single
  40 kHz pole in the output stage softens them) and a pricier DAC. A CV-only design at 8 kHz could
  use a cheap I²C DAC (MCP4728, 12-bit) but adds zipper steps on slow sweeps and ~1 ms of extra
  jitter on gates.
- **Input noise.** At 48 kSPS the ADS131M04 must run at OSR 64, its noisiest setting
  (75 µVrms at the pin, ~0.93 mVrms at the jack over the full band; about 1.1 cent at 1 V/oct).
  That noise is shaped toward Nyquist, so a 1 kHz low-pass on pitch inputs in Circuit Studio
  removes most of it. Divider noise is 20 µVrms (SPICE).
- **Output noise.** 115 µVrms at the jack, 20 Hz-20 kHz (SPICE, assuming 100 nV/√Hz DAC noise),
  about 0.14 cent.

## Hardware

Files: `hw/schematic/*.svg` (open `hw/schematic/index.html`), `hw/netlist.csv` (every pin → net,
the authoritative connection list), `hw/bom.csv`, `hw/breadboard.md`, SPICE in `hw/spice/`.
All of them come from `hw/gen_hw.py` and `hw/spice/run_spice.py`; KiCad was not installed, so there
is no .kicad_sch. The netlist is complete enough to enter into KiCad by hand later.

- **Output stage** (per channel, OPA4172 quarter): inverting amplifier referenced to Vb,
  `Vjack = 0.990 x (9.25·Vb − 8.25·Vdac)`, Vb = 2.5 V x 8.06/18.06 = 1.116 V. Code 0 → +10.22 V,
  code 65535 → −10.20 V into 100 k. 0.1 % resistors set gain and offset drift; calibration removes
  the initial error. CB on Vb stays at 100 pF: a 10 nF cap makes Vb lag the DAC at reference
  turn-on and gave a 5.8 V blip in SPICE (5 mV with 100 pF).
- **Output protection:** 1 k series (outside the loop, calibrated for a 100 k load), BAT54S clamps
  from the op-amp output to both rails. Rails off and ±15 V forced on a jack: 14.7 mA through the
  1 k, carried by the Schottky, not the op-amp's own ESD diodes.
- **Input stage:** 49.9 k + 49.9 k series, 9.09 k shunt, 330 pF (59 kHz pole) into the ADC, whose
  inputs accept ±1.2 V around ground from a single 3.3 V supply. ±10 V → ±0.814 V. No external
  clamp diodes: the 100 k limits a ±24 V fault to 0.22 mA (the ADC's limit is 10 mA), and a Schottky
  to ground would clip the negative half of the signal.
- **Power:** the Pico, DAC and ADC run from USB (MCP1700 3.3 V LDO from VBUS for the analog
  supply), so nothing can back-power the converters from the rack. Only the op-amps use the
  rack's ±12 V. With the rack off and USB on, the op-amps are unpowered and the DAC's 1.25 V
  pushes only ~60 µA into them through 10 k. **USB-only option:** a Mornsun B0512S-1WR3
  (5 V → ±12 V, 1 W) on VBUS replaces J1, for use without a rack.
- **8 outputs:** populate DAC channels E-H with a second OPA4172 and the same stage; build with
  `-DCV_N_OUT=8`. The USB input stream then drops to 16-bit to stay inside the full-speed
  isochronous budget (1180 bytes/frame).

Where to buy (UK; no orders placed): Pico 2 from The Pi Hut / Pimoroni; DAC8568BIPW,
ADS131M04IPWR, OPA4172IDR and 0.1 % resistors from Mouser UK / DigiKey UK / Farnell; passives and
SS14/BAT54S from LCSC; Thonkiconn jacks and Eurorack ribbons from Thonk or Tayda; TSSOP/SOIC adapters
from any electronics seller. Prices in `hw/bom.csv` are estimates to check before ordering.

## Firmware

`firmware/src/`: `main.c` (data path + USB callbacks), `converters.c` + `cv.pio` (PIO SPI and DMA
rings), `usb_desc.c` (descriptors), `engine.c` (per-sample path, mute state machine, ring maths),
`cal.c` (calibration maths), `cal_flash.c`, `proto.c` (SysEx), `conv_codec.h` (DAC/ADC frame
encoding, clock maths). Pin map: `board.h`.

### Build on the Mac

```sh
# one-time: toolchain + SDK (already in ~/.pico-sdk on Jouni's Mac)
mkdir -p ~/.pico-sdk && cd ~/.pico-sdk
curl -LO https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi.tar.xz
tar xf arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi.tar.xz
git clone --depth 1 -b 2.2.0 https://github.com/raspberrypi/pico-sdk.git sdk
git -C sdk submodule update --init --depth 1 lib/tinyusb

# build
export PICO_SDK_PATH=~/.pico-sdk/sdk
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi
export PATH=$PICO_TOOLCHAIN_PATH/bin:$PATH
cmake -S firmware -B firmware/build -DPICO_BOARD=pico2      # -DCV_N_OUT=8 for 8 outputs
cmake --build firmware/build -j8                            # -> firmware/build/cv_interface.uf2

./check.sh     # everything: host tests, SPICE, hw generation, both firmware builds (logs/)
```

Flash: hold BOOTSEL while plugging USB, copy the `.uf2` to the drive. Later updates:
`python3 tools/cvcal.py bootsel`, then copy. CI: `ci/github-actions.yml` (Ubuntu, apt toolchain,
same `check.sh`). The repo is local-only.

### USB device

- VID:PID `1209:0001` (pid.codes test PID: fine on your own machines; apply for a free pid.codes PID
  before giving builds to anyone else).
- Product "Circuit Studio CV 4x4", IAD composite: UAC2 (AC, AS OUT alt 0/1 with async iso data +
  feedback, AS IN alt 0/1 async iso) + USB-MIDI 1.0. Clock source: internal fixed 48 kHz.
- UAC2 channel names "CV Out 1..4", "CV In 1..4" (shown in Audio MIDI Setup).
- OUT 16-bit, IN 24-bit (4x4) or 16-bit (8x4). Terminal type "line connector".

### Calibration

The table (gain/offset per output and input, output range limiter) lives in the last flash
sector with a CRC; without one the firmware uses values computed from the nominal resistors
(within ~1 %). With `pip install mido python-rtmidi`:

```sh
python3 tools/cvcal.py cal-out      # DMM on each jack, 2 readings per channel
python3 tools/cvcal.py cal-in       # patch Out n -> In n; measures -5 V and +5 V
python3 tools/cvcal.py save
python3 tools/cvcal.py range 3 uni10   # optional per-output limiter: bi10 | bi5 | uni10 | uni5
```

Protocol (SysEx `F0 7D 43 56 cmd ... F7`, 7-bit packed u32/f32) is in `firmware/src/proto.h`;
Circuit Studio's wizard could speak it over WebMIDI later.

## Verification without hardware

| Check | What it covers | Result |
|---|---|---|
| `make -C firmware/test` | descriptor walk (lengths, IAD, endpoints, FS bandwidth, feedback EP variants), DAC8568/ADS131M04 frame encoding, clock dividers, calibration maths and CRC, limiter, mute/hold/ramp state machine, ring arithmetic under jitter, SysEx round trips (ASan/UBSan) | 230 + 234 checks pass (4 and 8 outputs) |
| `hw/spice/run_spice.py` (ngspice) | output transfer vs the firmware model (0.09 mV), nonlinearity, ±15 V faults powered and unpowered, output noise, bandwidth (40 kHz), reference turn-on transient, input transfer, impedance, ±15/±24 V faults, input noise, reverse-polarity block | 10/10 pass, `hw/results/spice_summary.md` |
| firmware build | Pico 2 4-out and 8-out, RP2040 compiles | clean, no warnings |

The op-amp in SPICE is a behavioural OPA172-class model (10 MHz, rail-to-rail, 65 mA limit), not
TI's vendor model; the ADC input is 330 k with ESD diodes. Datasheet facts used were read from the
TI datasheets (DAC8568 control matrix, timing, POR; ADS131M04 registers, timing, noise table).

## Known limits and open items

- Untested on hardware. First bring-up risks: PIO SPI timing margins (DAC 25 MHz, ADC 9.4 MHz),
  the ADS131M04 register write/ack sequence, and feedback behaviour on Windows.
- Chrome's multichannel capture: check that `getSettings().channelCount` really is 4 for this
  device on macOS (Chrome has downmixed some devices to 2). Noted for Milestone 2 in the Circuit
  Studio queue.
- RP2040 builds, but its 125 MHz clock puts the ADC 70 ppm off the DAC rate; use a Pico 2.
- The DAC (~£14.50) is a third of the cost. A DAC80504 (WQFN, midscale reset) is a cheaper route
  for a machine-assembled PCB, but it is not hand-solderable.
- Ground loop between computer and rack: optional USB isolator.
