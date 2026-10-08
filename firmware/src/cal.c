#include "cal.h"
#include <stddef.h>
#include <string.h>
#include <math.h>

static float out_load_factor(void) { return HW_OUT_RL / (HW_OUT_RL + HW_OUT_RS); }

void cal_defaults(cv_cal_t *c) {
    memset(c, 0, sizeof *c);
    const float G = HW_OUT_RF / HW_OUT_R1;
    const float vb = HW_VREF * HW_OUT_RB / (HW_OUT_RT + HW_OUT_RB);
    const float kl = out_load_factor();
    const float volts_per_code = HW_VREF / 65536.0f;
    // Vjack = kl*(vb*(1+G) - G*code*volts_per_code)
    const float gain = -1.0f / (kl * G * volts_per_code);       // codes per volt
    const float offset = vb * (1.0f + G) / (G * volts_per_code); // code at 0 V
    for (int i = 0; i < CV_MAX_OUT; i++) {
        c->out[i].gain = gain;
        c->out[i].offset = offset;
        c->out_range[i] = CV_RANGE_BI10;
    }
    const float rpar = HW_IN_RSH * HW_IN_RADC / (HW_IN_RSH + HW_IN_RADC);
    const float k = rpar / (HW_IN_RS + rpar);
    for (int i = 0; i < CV_MAX_IN; i++) {
        c->in[i].gain = HW_ADC_FS / 8388608.0f / k;
        c->in[i].offset = 0.0f;
    }
    c->n_out = CV_N_OUT;
    c->n_in = CV_N_IN;
    cal_seal(c);
}

uint32_t cal_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void cal_seal(cv_cal_t *c) {
    c->magic = CV_CAL_MAGIC;
    c->version = CV_CAL_VERSION;
    c->crc = cal_crc32(c, (uint32_t)offsetof(cv_cal_t, crc));
}

static bool finite_nonzero(float x) { return isfinite(x) && fabsf(x) > 1e-12f; }

bool cal_valid(const cv_cal_t *c) {
    if (c->magic != CV_CAL_MAGIC || c->version != CV_CAL_VERSION) return false;
    if (c->crc != cal_crc32(c, (uint32_t)offsetof(cv_cal_t, crc))) return false;
    for (int i = 0; i < CV_MAX_OUT; i++) {
        if (!finite_nonzero(c->out[i].gain) || !isfinite(c->out[i].offset)) return false;
        if (c->out_range[i] >= CV_RANGE_COUNT) return false;
    }
    for (int i = 0; i < CV_MAX_IN; i++)
        if (!finite_nonzero(c->in[i].gain) || !isfinite(c->in[i].offset)) return false;
    return true;
}

void cal_range_limits(uint8_t range, float *lo, float *hi) {
    switch (range) {
    case CV_RANGE_BI5:   *lo = -5.0f;  *hi = 5.0f;  break;
    case CV_RANGE_UNI10: *lo = 0.0f;   *hi = 10.0f; break;
    case CV_RANGE_UNI5:  *lo = 0.0f;   *hi = 5.0f;  break;
    default:             *lo = -10.0f; *hi = 10.0f; break;
    }
}

uint16_t cal_volts_to_code(const cv_cal_t *c, int ch, float volts) {
    float lo, hi;
    cal_range_limits(c->out_range[ch], &lo, &hi);
    if (!(volts >= lo)) volts = lo;   // also catches NaN
    if (volts > hi) volts = hi;
    float code = c->out[ch].offset + c->out[ch].gain * volts;
    if (code < 0.0f) code = 0.0f;
    if (code > 65535.0f) code = 65535.0f;
    return (uint16_t)(code + 0.5f);
}

float cal_code_to_volts(const cv_cal_t *c, int ch, uint16_t code) {
    return ((float)code - c->out[ch].offset) / c->out[ch].gain;
}

float cal_raw_to_volts(const cv_cal_t *c, int ch, int32_t raw) {
    return ((float)raw - c->in[ch].offset) * c->in[ch].gain;
}

bool cal_solve_out(float c1, float v1, float c2, float v2, cv_lin_t *out) {
    if (fabsf(v2 - v1) < 0.5f || fabsf(c2 - c1) < 1.0f) return false;
    out->gain = (c2 - c1) / (v2 - v1);
    out->offset = c1 - out->gain * v1;
    return true;
}

bool cal_solve_in(float r1, float v1, float r2, float v2, cv_lin_t *out) {
    if (fabsf(v2 - v1) < 0.5f || fabsf(r2 - r1) < 1.0f) return false;
    out->gain = (v2 - v1) / (r2 - r1);
    out->offset = r1 - v1 / out->gain;
    return true;
}
