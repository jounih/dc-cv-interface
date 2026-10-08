#include "engine.h"
#include <string.h>

void out_engine_init(out_engine_t *e) {
    memset(e, 0, sizeof *e);
    e->state = OUT_ZERO;
    for (int i = 0; i < CV_MAX_OUT; i++) e->raw_override[i] = -1;
}

void out_zero_codes(const cv_cal_t *cal, uint16_t *codes) {
    for (int i = 0; i < CV_N_OUT; i++) codes[i] = cal_volts_to_code(cal, i, 0.0f);
}

static float step_toward_zero(float v, float step) {
    if (v > step) return v - step;
    if (v < -step) return v + step;
    return 0.0f;
}

void out_engine_step(out_engine_t *e, const cv_cal_t *cal, const int16_t *usb, uint16_t *codes) {
    if (usb) {
        for (int i = 0; i < CV_N_OUT; i++) e->last[i] = (float)usb[i] * (CV_VOLTS_FS / 32768.0f);
        e->starve = 0;
        e->state = OUT_PLAY;
    } else {
        if (e->state == OUT_PLAY) { e->state = OUT_HOLD; e->underruns++; }
        if (e->starve < 0xFFFFFFFFu) e->starve++;
        if (e->state == OUT_HOLD && e->starve > CV_HOLD_SAMPLES) e->state = OUT_RAMP;
        if (e->state == OUT_RAMP) {
            bool all_zero = true;
            for (int i = 0; i < CV_N_OUT; i++) {
                e->last[i] = step_toward_zero(e->last[i], CV_RAMP_VOLTS_PER_SAMPLE);
                if (e->last[i] != 0.0f) all_zero = false;
            }
            if (all_zero) e->state = OUT_ZERO;
        }
        if (e->state == OUT_ZERO)
            for (int i = 0; i < CV_N_OUT; i++) e->last[i] = 0.0f;
    }
    for (int i = 0; i < CV_N_OUT; i++) {
        codes[i] = (e->raw_override[i] >= 0) ? (uint16_t)e->raw_override[i]
                                              : cal_volts_to_code(cal, i, e->last[i]);
    }
}

int32_t in_volts_to_sample(float volts) {
    const float fs = (float)((1L << (CV_IN_BITS - 1)) - 1);
    float s = volts * (fs / CV_VOLTS_FS);
    if (!(s >= -fs - 1.0f)) s = -fs - 1.0f;
    if (s > fs) s = fs;
    return (int32_t)(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

void in_engine_step(const cv_cal_t *cal, const int32_t *raw, uint8_t *usb_frame) {
    for (int i = 0; i < CV_N_IN; i++) {
        int32_t s = in_volts_to_sample(cal_raw_to_volts(cal, i, raw[i]));
        for (int b = 0; b < CV_IN_BYTES; b++) *usb_frame++ = (uint8_t)((uint32_t)s >> (8 * b));
    }
}

void in_measure_start(in_measure_t *m, uint32_t n_samples) {
    memset(m, 0, sizeof *m);
    m->target = n_samples ? n_samples : 1;
}

void in_measure_add(in_measure_t *m, const int32_t *raw) {
    if (m->target == 0 || m->done) return;
    for (int i = 0; i < CV_N_IN; i++) m->sum[i] += raw[i];
    if (++m->n >= m->target) m->done = true;
}

float in_measure_mean(const in_measure_t *m, int ch) {
    return m->n ? (float)((double)m->sum[ch] / (double)m->n) : 0.0f;
}

uint32_t ring_to_write(uint32_t *wr, uint32_t hw, uint32_t n, uint32_t target, uint32_t guard, bool *underrun) {
    uint32_t ahead = (*wr + n - hw) % n;
    *underrun = false;
    if (ahead > n / 2) {               // the DMA overtook the writer
        *underrun = true;
        *wr = (hw + guard) % n;
        ahead = guard;
    }
    return target > ahead ? target - ahead : 0;
}
