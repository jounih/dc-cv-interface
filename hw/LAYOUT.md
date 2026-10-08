# PCB layout spec and ordering steps

KiCad was not run here; `hw/kicad/cv_interface.net` is a complete KiCad netlist (278 parts,
footprints from the standard KiCad libraries, MPN/LCSC/Group/Assembly fields). Import it into an empty
board (`pcbnew: File > Import > Netlist`), then place and route with the rules below. Nothing has been
ordered; the steps at the end are for Jouni.

## Form factors (one PCB, two mechanical options)

- **Eurorack 14 HP** (70.8 mm wide panel): PCB 66 x 108 mm behind a 3U panel. Jacks in a 5 x 6 grid on
  14 mm pitch (30 jacks: 8 audio out, 8 CV out, 6 audio in, 8 CV in), the T-Display-S3 vertical at the top
  behind a 26 x 52 mm window, USB-C reachable from the top edge. Depth ~35 mm with the DC-DC module.
- **Desktop**: the same PCB in a 3D-printed or folded-aluminium case (STEP of the T-Display-S3 is in the
  LilyGO repo), with the 10-pin header unpopulated.

## Stack-up and placement

- 4 layers: signal / **solid GND** / power (+-11 V, +5V_SYS, 3.3/4.5 V pours) / signal. One ground; no split.
- Three zones, left to right: power (USB 5 V in from the S3 socket, LM66100s, DC-DC module, pi filters,
  LDOs) - digital (S3 sockets, SPI/I2S, crystal) - analog (converters, op-amps, jacks).
- DC-DC module (U22) at the far corner from the converters, input/output pi filters next to it, with its
  return currents kept on top-layer copper to the module pins. Keep its loop area small.
- PCM3168A: thermal pad to GND with a via array; 10 uF on VCOMAD/VCOMDA/VREFAD1/2 within 3 mm.
  I2S lines (MCLK 16.4 MHz, BCLK 8.2 MHz) short, series 33 R at the S3 end, away from the analog inputs.
- DAC8568 VREF cap (220 nF; datasheet asks for >= 150 nF) at the pin; the Vb divider (R2/R3) next to the four output op-amps it feeds;
  **no capacitor larger than 100 pF on VB** (see SPICE: 10 nF gives a 5.8 V power-up blip).
- ADS131M08: 8.192 MHz crystal within 5 mm, 1 uF AVDD/DVDD, 220 nF CAP; input dividers next to the pins.
- Each output: op-amp -> BAT54S -> 1 k (1206) -> jack, the 1 k at the jack end.
- Rail sense divider (R26/R27) at the +11 V LDO output; rack sense (R20/R21) at J1.

## Before layout, verify (these are the uncertain mappings)

- T-Display-S3 socket pin order (`J2`, `J3` in the netlist use logical order; take the physical order from
  LilyGO's pinmap image).
- A0515S-2WR3 pinout (SIP) and that the A-series is the dual-output (+-15 V) version.
- HTQFP-64 thermal-pad footprint name and size against the PCM3168A drawing.
- DAC8568**B** (midscale reset) is not stocked at JLC: use JLC Global Sourcing (Mouser/Digi-Key) or consign.
  Do not substitute the A/C grades (zero-scale reset = +10 V at power-up).

## JLCPCB order steps (Jouni does this; nothing has been ordered)

1. In KiCad, after routing: Plot Gerbers + drill (JLC preset), and Fabrication > Footprint position (CPL, CSV,
   mm, both sides). Rename columns to Designator, Mid X, Mid Y, Layer, Rotation if needed.
2. BOM: `hw/jlc/bom_full.csv` (or `bom_precision.csv` / `bom_audio.csv` for a partial population). These list
   only the SMT parts; the hand-soldered through-hole parts are in `hw/bom_<population>.csv`.
3. jlcpcb.com > Order now > upload the Gerber zip. 4 layers, 1.6 mm, HASL lead-free or ENIG, qty 5.
4. Enable PCB Assembly: top side, Economic if every part qualifies (otherwise Standard: the HTQFP-64 and
   TQFP-32 may push it to Standard), "Confirm parts placement".
5. Upload BOM + CPL. Check each line's LCSC match and rotation in the 3D preview (rotate the TQFP/HTQFP,
   SOT-23 and SOIC parts if pin 1 is wrong). Global-source the DAC8568BIPW.
6. Review the price: each "extended" part adds about USD 3 per order (see `hw/results/bom_summary.json`).
7. Hand-solder after delivery: 2 x 1x12 sockets for the T-Display-S3, 30 Thonkiconn jacks, the 2x5 rack
   header, the DC-DC module (SIP), optional LiPo boost header. Then the README safety checklist.
