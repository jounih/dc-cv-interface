#include "engine.h"
#include <string.h>
#include <math.h>

void out_engine_init(out_engine_t *e, int n, int first_ch, uint32_t rate_hz) {
    memset(e, 0, sizeof *e);
    e->n = n > ENG_MAX_CH ? ENG_MAX_CH : n;
    e->first_ch = first_ch;
    e->hold_samples = rate_hz * CV_HOLD_MS / 1000u;
    e->ramp_per_sample = CV_RAMP_V_PER_MS * 1000.0f / (float)rate_hz;
    e->state = OUT_ZERO;
}

void out_zero_codes(const out_engine_t *e, const cv_cal_t *cal, int32_t *codes) {
    for (int i = 0; i < e->n; i++) codes[i] = cal_volts_to_code(cal, e->first_ch + i, 0.0f);
}

static float toward_zero(float v, float step) {
    if (v > step) return v - step;
    if (v < -step) return v + step;
    return 0.0f;
}

void out_engine_step(out_engine_t *e, const cv_cal_t *cal, const float *volts, int32_t *codes) {
    if (e->force_zero) {
        for (int i = 0; i < e->n; i++) e->last[i] = 0.0f;
        e->state = OUT_ZERO;
    } else if (volts) {
        for (int i = 0; i < e->n; i++) e->last[i] = volts[i];
        e->starve = 0;
        e->state = OUT_PLAY;
    } else {
        if (e->state == OUT_PLAY) { e->state = OUT_HOLD; e->underruns++; }
        if (e->starve < 0xFFFFFFFFu) e->starve++;
        if (e->state == OUT_HOLD && e->starve > e->hold_samples) e->state = OUT_RAMP;
        if (e->state == OUT_RAMP) {
            bool all_zero = true;
            for (int i = 0; i < e->n; i++) {
                e->last[i] = toward_zero(e->last[i], e->ramp_per_sample);
                if (e->last[i] != 0.0f) all_zero = false;
            }
            if (all_zero) e->state = OUT_ZERO;
        }
        if (e->state == OUT_ZERO)
            for (int i = 0; i < e->n; i++) e->last[i] = 0.0f;
    }
    for (int i = 0; i < e->n; i++) {
        codes[i] = (e->override_on[i] && !e->force_zero) ? e->raw_override[i]
                                                         : cal_volts_to_code(cal, e->first_ch + i, e->last[i]);
    }
}

int16_t volts_to_usb16(float v) {
    float s = v * (32768.0f / CV_VOLTS_FS);
    if (!(s >= -32768.0f)) s = -32768.0f;
    if (s > 32767.0f) s = 32767.0f;
    return (int16_t)lrintf(s);
}

void in_a_step(const cv_cal_t *cal, const int32_t *raw, int16_t *usb) {
    for (int i = 0; i < CV_A_IN; i++) usb[i] = volts_to_usb16(cal_raw_to_volts(cal, i, raw[i]));
}

void in_measure_start(in_measure_t *m, uint32_t n_samples) {
    memset(m, 0, sizeof *m);
    m->target = n_samples ? n_samples : 1;
    m->active = true;
}

void in_measure_add(in_measure_t *m, int first_ch, int count, const int32_t *raw) {
    if (!m->active || m->done) return;
    bool all = true;
    for (int i = 0; i < count; i++) {
        int ch = first_ch + i;
        if (m->n[ch] < m->target) { m->sum[ch] += raw[i]; m->n[ch]++; }
    }
    // Done when every input that is being fed has reached the target.
    for (int ch = 0; ch < CV_MAX_IN; ch++)
        if (cal_in_kind(ch) != CH_NONE && m->n[ch] > 0 && m->n[ch] < m->target) all = false;
    if (all) m->done = true;
}

float in_measure_mean(const in_measure_t *m, int ch) {
    return m->n[ch] ? (float)((double)m->sum[ch] / (double)m->n[ch]) : 0.0f;
}

void cv_glide_init(cv_glide_t *g, uint32_t glide_samples) {
    memset(g, 0, sizeof *g);
    g->glide_samples = glide_samples ? glide_samples : 1;
}

void cv_glide_set(cv_glide_t *g, int ch, float volts) {
    g->target[ch] = volts;
    g->step[ch] = (volts - g->cur[ch]) / (float)g->glide_samples;
}

void cv_glide_tick(cv_glide_t *g, int n) {
    for (int i = 0; i < n; i++) {
        float d = g->target[i] - g->cur[i];
        if (fabsf(d) <= fabsf(g->step[i]) || g->step[i] == 0.0f) g->cur[i] = g->target[i];
        else g->cur[i] += g->step[i];
    }
}
