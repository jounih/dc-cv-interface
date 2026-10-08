# Breadboard path (before any PCB)

Five phases. Each one adds parts only after the previous one works, and nothing touches the
rack until phase 4 has passed the safety checklist in the README.

Parts that are not breadboard-friendly go on cheap adapter boards:
DAC8568 (TSSOP-16) and ADS131M04 (TSSOP-20) on 0.65 mm TSSOP-to-DIP adapters, OPA4172 (SOIC-14)
on a SOIC adapter. Drag-solder with flux and check for bridges with a loupe. A TL074CN (DIP) works
in place of the OPA4172 on the breadboard, but it only reaches about +-9.5 V on +-12 V rails.

Pico 2 pin numbers are the physical header pins (pin 1 = GP0, top left with USB at the top).

## Phase 0: Pico only (no other parts)

1. Build the firmware (README, "Build") and flash it: hold BOOTSEL, plug USB, drag
   `firmware/build/cv_interface.uf2` onto the RP2350 drive.
2. The device appears as **Circuit Studio CV 4x4** (audio 4 in / 4 out at 48 kHz) plus a MIDI port
   **CV Interface Control**. The LED blinks fast: no ADC found, which is expected here.
3. Check in Audio MIDI Setup (macOS) that 4 in / 4 out at 48 kHz show up with the channel names
   CV Out 1-4 and CV In 1-4. Point Circuit Studio's Milestone 2 Interface modules at it: the inputs
   read silence and the outputs go nowhere, but enumeration, channel count, feedback and latency
   can all be tested.
4. `python3 tools/cvcal.py info` (with `pip install mido python-rtmidi`) shows `adc_ok: False`.

## Phase 1: DAC only, 3.3 V side (no +-12 V)

| From | To | Note |
|---|---|---|
| Pico pin 40 (VBUS) | MCP1700 VIN | 1 uF VIN to GND. Pin order differs between TO-92 and SOT-23: check the datasheet for your package |
| MCP1700 VOUT | rail `+3V3_A` | 1 uF to GND |
| MCP1700 GND | GND | |
| Pico pin 38 (GND) | GND rail | |
| Pico pin 4 (GP2) | DAC8568 pin 16 SCLK | short wire |
| Pico pin 5 (GP3) | DAC8568 pin 2 SYNC | 10 k pull-up to Pico pin 36 (3V3) |
| Pico pin 6 (GP4) | DAC8568 pin 15 DIN | |
| `+3V3_A` | DAC8568 pin 3 AVDD, pin 1 LDAC, pin 9 CLR | 100 nF + 1 uF at pin 3. CLR must never float |
| GND | DAC8568 pin 14 | |
| DAC8568 pin 8 VREF | 150 nF to GND | |

Check with a DMM: with USB plugged and no host stream, every DAC output reads the 0 V code,
about **1.251 V** (code 32793 of 65536 x 2.5 V), and VREF reads 2.500 V.
`python3 tools/cvcal.py` is not needed yet. Unplug USB: VREF and the outputs fall to 0 V.

## Phase 2: one output stage on +-12 V (bench supply, 50 mA current limit)

| From | To |
|---|---|
| bench +12 V, through SS14 (anode at supply) | rail `+12V`, 10 uF + 100 nF to GND |
| bench -12 V, through SS14 (cathode at supply) | rail `-12V`, 10 uF + 100 nF to GND |
| bench 0 V | GND (one point, next to the op-amp) |
| DAC8568 pin 8 VREF | 10 k 0.1 % to node `VB`; `VB` 8.06 k 0.1 % to GND; 100 pF VB-GND (no bigger) |
| OPA4172 pin 4 / pin 11 | `+12V` / `-12V` |
| OPA4172 pin 3 (+IN A) | `VB` |
| DAC8568 pin 4 (VOUTA) | 10 k 0.1 % to OPA4172 pin 2 (-IN A) |
| OPA4172 pin 2 to pin 1 | 82.5 k 0.1 % in parallel with 47 pF |
| OPA4172 pin 1 (OUT A) | BAT54S pin 3; BAT54S pin 2 to `+12V`, pin 1 to `-12V` |
| OPA4172 pin 1 | 1 k to the jack tip (jack sleeve to GND) |

Check: with no stream the jack reads 0 V (+-50 mV before calibration). Run
`python3 tools/cvcal.py cal-out` with the DMM on the jack, then play a slow ramp from Circuit Studio
(or any DAW) and confirm the jack follows 0.1 digital = 1 V. Stop the stream: the jack holds for
20 ms, then ramps to 0 V.

Then build channels B-D the same way (op-amp pins 5/6/7, 10/9/8, 12/13/14; DAC pins 13, 5, 12).

## Phase 3: inputs (ADS131M04)

| From | To | Note |
|---|---|---|
| Pico pin 14 (GP10) | ADS131M04 pin 14 SCLK | |
| Pico pin 15 (GP11) | ADS pin 12 CS | 10 k pull-up to Pico 3V3 |
| Pico pin 16 (GP12) | ADS pin 15 DOUT | |
| Pico pin 17 (GP13) | ADS pin 13 DRDY | |
| Pico pin 19 (GP14) | ADS pin 16 DIN | |
| Pico pin 20 (GP15) | ADS pin 11 SYNC/RESET | |
| Pico pin 27 (GP21) | ADS pin 17 CLKIN | 6.144 MHz; keep this wire short and away from the inputs |
| `+3V3_A` | ADS pin 1 AVDD | 1 uF |
| Pico pin 36 (3V3) | ADS pin 20 DVDD | 1 uF |
| GND | ADS pins 2, 19 and AIN0N/1N/2N/3N (pins 4, 5, 8, 9) | |
| ADS pin 18 CAP | 220 nF to GND | |
| each jack tip | 49.9 k + 49.9 k in series to AINxP (pins 3, 6, 7, 10) | 9.09 k and 330 pF from AINxP to GND |

Check: the LED stops fast-blinking (ADC found), `cvcal.py info` shows `adc_ok: True`. Patch
Out n -> In n and run `python3 tools/cvcal.py cal-in`, then `save`.

## Phase 4: into the rack

Only after the README safety checklist has been ticked by a person. Power the board from the
rack's 10-pin header (red stripe = -12 V) through the SS14 diodes instead of the bench supply.
Mind the ground loop: computer USB ground and rack ground are now joined through the board; if
you hear hum on other modules, use a full-speed USB isolator (ADuM3160-based, 12 Mbit/s is enough).
