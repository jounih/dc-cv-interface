// Per-sample signal path between the host and the converters. Pure C.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cv_config.h"
#include "cal.h"

typedef enum { OUT_PLAY = 0, OUT_HOLD, OUT_RAMP, OUT_ZERO } out_state_t;

#define ENG_MAX_CH 8

// One output group (A: codec at 32 kHz, P: DAC8568 at 2 kHz).
typedef struct {
    int n;                       // channels in the group
    int first_ch;                // calibration index of channel 0
    uint32_t hold_samples;       // starvation hold before ramping
    float ramp_per_sample;       // volts per sample towards 0 V
    float last[ENG_MAX_CH];      // last commanded volts
    uint32_t starve;
    out_state_t state;
    int32_t raw_override[ENG_MAX_CH];  // calibration: hold this code
    bool override_on[ENG_MAX_CH];
    bool force_zero;             // power/rail fault, low battery: outputs to 0 V now
    uint32_t underruns;
} out_engine_t;

void out_engine_init(out_engine_t *e, int n, int first_ch, uint32_t rate_hz);
// volts: n values, or NULL when the host supplied nothing for this sample period.
void out_engine_step(out_engine_t *e, const cv_cal_t *cal, const float *volts, int32_t *codes);
void out_zero_codes(const out_engine_t *e, const cv_cal_t *cal, int32_t *codes);

// USB int16 (full scale = CV_VOLTS_FS) <-> volts
static inline float usb16_to_volts(int16_t s) { return (float)s * (CV_VOLTS_FS / 32768.0f); }
int16_t volts_to_usb16(float v);

// Group A input: raw codec counts -> calibrated volts -> USB int16.
void in_a_step(const cv_cal_t *cal, const int32_t *raw, int16_t *usb);

// Averaging accumulator for input calibration (all 16 input slots).
typedef struct {
    int64_t sum[CV_MAX_IN];
    uint32_t n[CV_MAX_IN];
    uint32_t target;
    bool active, done;
} in_measure_t;
void in_measure_start(in_measure_t *m, uint32_t n_samples);
void in_measure_add(in_measure_t *m, int first_ch, int count, const int32_t *raw);
float in_measure_mean(const in_measure_t *m, int ch);

// Precision CV targets with glide (removes zipper steps from ~1 kHz host frames).
typedef struct {
    float cur[ENG_MAX_CH], target[ENG_MAX_CH], step[ENG_MAX_CH];
    uint32_t glide_samples;
} cv_glide_t;
void cv_glide_init(cv_glide_t *g, uint32_t glide_samples);
void cv_glide_set(cv_glide_t *g, int ch, float volts);
void cv_glide_tick(cv_glide_t *g, int n);   // advance one sample period
