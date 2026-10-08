#include "cal.h"
#include <stddef.h>
#include <string.h>
#include <math.h>

cv_kind_t cal_out_kind(int ch) {
    if (ch >= 0 && ch < CV_A_OUT) return CH_A_OUT;
    if (ch >= CV_P_OUT0 && ch < CV_P_OUT0 + CV_P_OUT) return CH_P_OUT;
    return CH_NONE;
}

cv_kind_t cal_in_kind(int ch) {
    if (ch >= 0 && ch < CV_A_IN) return CH_A_IN;
    if (ch >= CV_P_IN0 && ch < CV_P_IN0 + CV_P_IN) return CH_P_IN;
    return CH_NONE;
}

static float load_factor(void) { return HW_OUT_RL / (HW_OUT_RL + HW_OUT_RS); }

void cal_defaults(cv_cal_t *c) {
    memset(c, 0, sizeof *c);
    const float kl = load_factor();
    // P outputs (DAC8568 codes 0..65535)
    const float G = HW_OUT_RF / HW_OUT_R1;
    const float vb = HW_VREF * HW_OUT_RB / (HW_OUT_RT + HW_OUT_RB);
    const float vpc = HW_VREF / 65536.0f;
    // A outputs (signed 24-bit codec samples): full-scale differential peak = 0.8 x VCC
    const float a_gain = 8388608.0f / (kl * (HW_AO_RF / HW_AO_RI) * 0.8f * HW_CODEC_VCC);
    (void)G; (void)vb; (void)vpc;
#ifdef CV_TIER_A
    const float ga = HW_TA_RF / HW_TA_R1, vba = HW_TA_VDD * HW_TA_RB / (HW_TA_RT + HW_TA_RB), vpc_a = 4.096f / 4096.0f;
#endif
    for (int i = 0; i < CV_MAX_OUT; i++) {
        if (cal_out_kind(i) == CH_P_OUT) {
#ifdef CV_TIER_A
            c->out[i].gain = -1.0f / (kl * ga * vpc_a);
            c->out[i].offset = vba * (1.0f + ga) / (ga * vpc_a);
#else
            c->out[i].gain = -1.0f / (kl * G * vpc);
            c->out[i].offset = vb * (1.0f + G) / (G * vpc);
#endif
        } else {
            c->out[i].gain = a_gain;
            c->out[i].offset = 0.0f;
        }
        c->out_range[i] = CV_RANGE_BI10;
    }
    // P inputs (ADS131M08 counts, +-2^23 = +-1.2 V)
    const float rpar = HW_IN_RSH * HW_IN_RADC / (HW_IN_RSH + HW_IN_RADC);
    const float k = rpar / (HW_IN_RS + rpar);
    // A inputs (codec counts, +-2^23 = +-0.2*VCC*sqrt(2) V around VCOM, inverting stage)
    const float a_fs = 0.2f * HW_CODEC_VCC * 1.41421356f;
    const float a_in = -a_fs / ((HW_AI_RF / HW_AI_RI) * 8388608.0f);
    for (int i = 0; i < CV_MAX_IN; i++) {
        c->in[i].gain = (cal_in_kind(i) == CH_A_IN) ? a_in : HW_ADC_FS / 8388608.0f / k;
        c->in[i].offset = 0.0f;
#ifdef CV_TIER_A
        if (cal_in_kind(i) == CH_P_IN) {
            const float kt = HW_TA_IN_RP / (HW_TA_IN_R1 + HW_TA_IN_RP);
            const float voff = (HW_TA_VDD / 2.0f) * HW_TA_IN_R1 / (HW_TA_IN_R1 + HW_TA_IN_RP);
            c->in[i].gain = 4.096f / 32768.0f / kt;
            c->in[i].offset = voff / 4.096f * 32768.0f;
        }
#endif
    }
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
    return c->low_power <= 1;
}

void cal_code_limits(int ch, int32_t *lo, int32_t *hi) {
#ifdef CV_TIER_A
    if (cal_out_kind(ch) == CH_P_OUT) { *lo = 0; *hi = 4095; return; }
#endif
    if (cal_out_kind(ch) == CH_P_OUT) { *lo = 0; *hi = 65535; }
    else { *lo = -8388608; *hi = 8388607; }
}

void cal_range_limits(uint8_t range, float *lo, float *hi) {
    switch (range) {
    case CV_RANGE_BI5:   *lo = -5.0f;  *hi = 5.0f;  break;
    case CV_RANGE_UNI10: *lo = 0.0f;   *hi = 10.0f; break;
    case CV_RANGE_UNI5:  *lo = 0.0f;   *hi = 5.0f;  break;
    default:             *lo = -10.0f; *hi = 10.0f; break;
    }
}

int32_t cal_volts_to_code(const cv_cal_t *c, int ch, float volts) {
    float lo, hi;
    cal_range_limits(c->out_range[ch], &lo, &hi);
    if (c->low_power) { if (lo < -5.0f) lo = -5.0f; if (hi > 5.0f) hi = 5.0f; }
    if (!(volts >= lo)) volts = lo;   // also catches NaN
    if (volts > hi) volts = hi;
    int32_t clo, chi;
    cal_code_limits(ch, &clo, &chi);
    float code = c->out[ch].offset + c->out[ch].gain * volts;
    if (code < (float)clo) code = (float)clo;
    if (code > (float)chi) code = (float)chi;
    return (int32_t)lrintf(code);
}

float cal_code_to_volts(const cv_cal_t *c, int ch, int32_t code) {
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
