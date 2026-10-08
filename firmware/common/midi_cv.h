// MIDI -> CV/gate mapper for the precision outputs. USB-MIDI and BLE-MIDI both feed it.
// Pure C, host-tested. Monophonic voice with last-note priority plus CC -> CV maps.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MCV_OUTS 8          // precision outputs (P group, local index 0..7)
#define MCV_STACK 16
#define MCV_CC_MAPS 4

typedef struct {
    uint8_t midi_ch;        // 0..15, 16 = omni
    int8_t pitch_out;       // P-group output for 1 V/oct pitch, -1 = none
    int8_t gate_out;
    int8_t vel_out;         // velocity 0..127 -> 0..vel_volts
    uint8_t ref_note;       // MIDI note that gives 0 V (default 48 = C3)
    uint8_t bend_range;     // semitones for full pitch-bend deflection
    bool legato;            // true: overlapping notes slide without a new gate
    uint8_t retrig_ms;      // gate-low gap on retrigger (legato off)
    float gate_volts;       // 5 or 10
    float vel_volts;        // full-scale velocity voltage
    struct { uint8_t cc; int8_t out; bool bipolar; } cc[MCV_CC_MAPS];  // out -1 = unused
} mcv_cfg_t;

typedef struct {
    mcv_cfg_t cfg;
    uint8_t stack[MCV_STACK];   // held notes, oldest first
    int n_held;
    int note;                   // sounding note, -1 = none yet
    int16_t bend;               // -8192..8191
    uint8_t velocity;
    bool gate;
    uint32_t retrig_left_ms;    // gate forced low while > 0
    float cc_val[MCV_CC_MAPS];
    uint32_t drive_mask;        // outputs the mapper owns (from cfg)
} mcv_t;

void mcv_default_cfg(mcv_cfg_t *c);
void mcv_init(mcv_t *m, const mcv_cfg_t *c);
// One complete channel message (status + data). Real-time and SysEx are ignored here.
void mcv_midi(mcv_t *m, const uint8_t *msg, int len);
void mcv_tick_ms(mcv_t *m, uint32_t ms);      // retrigger timing
void mcv_all_off(mcv_t *m);                   // transport loss: gate low, pitch held
// Voltage the mapper wants on P output `out`; false if it does not drive that output.
bool mcv_out(const mcv_t *m, int out, float *volts);
float mcv_pitch_volts(const mcv_t *m);
