# Breadboard path (Tier A): pre-soldered modules only, no SMT

Use this to try the whole chain (Circuit Studio <-> USB <-> modular) before ordering a PCB.
It is honest about its limits:

| | Tier A breadboard | PCB (Tier B precision + Tier C codec) |
|---|---|---|
| CV outputs | 8 x 12-bit (MCP4728), ~1 kHz update | 8 x 16-bit (DAC8568) at 2 kHz + 8 audio-rate (PCM3168A, 32 kHz) |
| CV inputs | 8 x 16-bit (ADS1115), ~125 Hz per channel | 8 x 24-bit (ADS131M08) at 2 kHz + 6 audio-rate |
| Pitch accuracy | 12-bit over +-10 V = 5 mV steps (about 6 cents): fine for modulation, gates and envelopes, **not** for 1 V/oct melodies | 0.31 mV steps (0.4 cent), 0.16 mV worst error in the mapper test |
| Audio-rate DC | none (UAC2 still enumerates, so Circuit Studio's device code can be tested) | yes, 8 out / 6 in |
| Soldering | headers on modules (often pre-soldered), perfboard through-hole for the op-amp stages | through-hole only (jacks, headers, sockets, DC-DC module) |

## Parts (all pre-assembled or through-hole)

| Qty | Part | Notes, approx price |
|---|---|---|
| 1 | LilyGO T-Display-S3 (non-touch) | ~GBP 16-20 (LilyGO store, The Pi Hut, AliExpress) |
| 2 | MCP4728 breakout (Adafruit 4470 or generic) | ~GBP 7 each (generic ~GBP 3); default address 0x60, keep it |
| 2 | ADS1115 breakout (Adafruit 1085 or generic) | ~GBP 12 / ~GBP 3; default address 0x48, keep it |
| 2 | TL074CN (DIP-14) | ~GBP 0.50 each; output stages for 8 outputs |
| 1 | Bench supply +-12 V with current limit, or a Mornsun A0512S-1WR3 (+-12 V from USB 5 V) on the perfboard | |
| 16 | Thonkiconn PJ398SM + perfboard + 1 % resistors + 100 nF caps | ~GBP 10 |

The two module pairs sit on **two separate I2C buses**, so nothing needs re-addressing:
bus 0 = SDA GPIO18 / SCL GPIO17 (outputs and inputs 1-4), bus 1 = SDA GPIO16 / SCL GPIO21 (5-8).
Build the firmware with `idf.py -B build-tiera -DCV_TIER_A=1 -DSDKCONFIG=build-tiera/sdkconfig build`.

## Output stage (per channel, TL074 quarter, +-12 V)

`Vjack = 0.99 x (6 x Vb - 4.99 x Vdac)`, Vb = 3.3 V x 10.7k/20.7k = 1.706 V:

- MCP4728 VOUTx -> 10 k -> op-amp -IN; 49.9 k from -IN to OUT.
- +IN = Vb from a 10 k / 10.7 k divider off the module's 3.3 V (shared by all four sections).
- OUT -> 1 k -> jack tip. SPICE (TL074-like swing): code 0 -> +10.04 V, midscale -> 0 V, full -> -10.03 V.

**Power-on:** a fresh MCP4728 starts at code 0, which this stage turns into **+10 V**. On first boot the
firmware writes the 0 V codes into the MCP4728 EEPROM; after that every power-up starts at 0 V. So do the
first boot with nothing patched.

## Input network (per channel, passive)

Jack -> 100 k -> ADS1115 AINx, with 22 k to 3.3 V and 22 k to GND at AINx:
`Vadc = 1.486 V + 0.0991 x Vjack` (+-10 V -> 0.50..2.48 V). +-15 V stays inside the ADS1115's limits;
+-24 V exceeds them by under 0.6 V through 100 k (0.2 mA, inside its 10 mA input-current rating).

## Steps

1. Flash the Tier A build; the display shows the meters (the audio group streams silence).
2. Wire bus 0 only (one MCP4728 + one ADS1115), no op-amp stage: `python3 tools/cvcal.py info` shows
   `precision_ok: True` once any MCP4728 answers; its outputs read ~2.05 V (the 0 V code) on a DMM.
3. Add the TL074 stages on +-12 V **from a current-limited supply** and check 0 V at every jack.
4. `python3 tools/cvcal.py cal-out` (DMM), patch out -> in, `cal-in`, `save`.
5. Send MIDI from any keyboard app over BLE or USB: P1 = pitch, P2 = gate, P3 = velocity.
