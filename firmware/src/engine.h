// Per-sample signal path between USB and the converters. Pure C.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cv_config.h"
#include "cal.h"

typedef enum { OUT_PLAY = 0, OUT_HOLD, OUT_RAMP, OUT_ZERO } out_state_t;

typedef struct {
    float last[CV_MAX_OUT];      // last commanded volts (pre-calibration)
    uint32_t starve;             // consecutive samples without host data
    out_state_t state;
    int32_t raw_override[CV_MAX_OUT]; // >= 0: hold this DAC code (calibration)
    uint32_t underruns;          // count of PLAY -> HOLD transitions
} out_engine_t;

void out_engine_init(out_engine_t *e);
// usb: CV_N_OUT little-endian int16 samples, or NULL when no host data is
// available for this sample period. Writes CV_N_OUT DAC codes.
void out_engine_step(out_engine_t *e, const cv_cal_t *cal, const int16_t *usb, uint16_t *codes);
// Codes for 0 V on every channel (power-on / mute value).
void out_zero_codes(const cv_cal_t *cal, uint16_t *codes);

// ADC -> USB. raw: CV_N_IN sign-extended 24-bit ADC counts. Writes the packed
// little-endian USB frame (CV_N_IN * CV_IN_BYTES bytes).
void in_engine_step(const cv_cal_t *cal, const int32_t *raw, uint8_t *usb_frame);
int32_t in_volts_to_sample(float volts); // full scale = CV_VOLTS_FS, CV_IN_BITS wide

// Averaging accumulator for input calibration.
typedef struct {
    int64_t sum[CV_MAX_IN];
    uint32_t n, target;
    bool done;
} in_measure_t;
void in_measure_start(in_measure_t *m, uint32_t n_samples);
void in_measure_add(in_measure_t *m, const int32_t *raw);   // no-op when idle/done
float in_measure_mean(const in_measure_t *m, int ch);

// Ring writer maths (DMA ring of n samples, hw = sample index the DMA reads next).
// Returns how many samples to write at *wr to reach `target` samples ahead of hw.
// On underrun (hw passed wr) resyncs *wr to hw + guard and sets *underrun.
uint32_t ring_to_write(uint32_t *wr, uint32_t hw, uint32_t n, uint32_t target, uint32_t guard, bool *underrun);
