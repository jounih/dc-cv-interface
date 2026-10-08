#include "proto.h"
#include <string.h>
#include <math.h>

static const uint8_t HDR[4] = {0xF0, 0x7D, 0x43, 0x56};

size_t proto_put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 5; i++) p[i] = (uint8_t)((v >> (7 * i)) & 0x7Fu);
    return 5;
}
uint32_t proto_get_u32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) v |= (uint32_t)(p[i] & 0x7Fu) << (7 * i);
    return v;
}
size_t proto_put_f32(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); return proto_put_u32(p, u); }
float proto_get_f32(const uint8_t *p) { uint32_t u = proto_get_u32(p); float f; memcpy(&f, &u, 4); return f; }

size_t proto_put_cv(uint8_t *p, float volts) {
    float u = volts * 100000.0f;
    if (!(u >= -1048576.0f)) u = -1048576.0f;
    if (u > 1048575.0f) u = 1048575.0f;
    uint32_t v = (uint32_t)(int32_t)lrintf(u) & 0x1FFFFFu;
    p[0] = v & 0x7F; p[1] = (v >> 7) & 0x7F; p[2] = (v >> 14) & 0x7F;
    return 3;
}
float proto_get_cv(const uint8_t *p) {
    uint32_t v = (uint32_t)(p[0] & 0x7F) | (uint32_t)(p[1] & 0x7F) << 7 | (uint32_t)(p[2] & 0x7F) << 14;
    int32_t s = (int32_t)(v << 11) >> 11;
    return (float)s / 100000.0f;
}

size_t proto_build(uint8_t cmd, const uint8_t *payload, size_t n, uint8_t *out, size_t cap) {
    if (n + 6 > cap) return 0;
    memcpy(out, HDR, 4);
    out[4] = cmd & 0x7F;
    for (size_t i = 0; i < n; i++) out[5 + i] = payload[i] & 0x7F;
    out[5 + n] = 0xF7;
    return n + 6;
}

static size_t ack(uint8_t cmd, uint8_t status, uint8_t *reply, size_t cap) {
    uint8_t p[2] = {cmd, status};
    return proto_build(P_ACK, p, 2, reply, cap);
}

static out_engine_t *engine_for(proto_ctx_t *c, int ch, int *local) {
    cv_kind_t k = cal_out_kind(ch);
    if (k == CH_A_OUT) { *local = ch; return c->out_a; }
    if (k == CH_P_OUT) { *local = ch - CV_P_OUT0; return c->out_p; }
    return NULL;
}

// MAP_CFG layout: ch, pitch+1, gate+1, vel+1, ref_note, bend, legato, retrig_ms, gate_v, vel_v,
//                 then 4 x (cc, out+1, bipolar)  = 22 bytes
static bool parse_map(const uint8_t *a, size_t n, mcv_cfg_t *c) {
    if (n != 22) return false;
    mcv_cfg_t t;
    memset(&t, 0, sizeof t);
    t.midi_ch = a[0];
    t.pitch_out = (int8_t)(a[1] - 1); t.gate_out = (int8_t)(a[2] - 1); t.vel_out = (int8_t)(a[3] - 1);
    t.ref_note = a[4]; t.bend_range = a[5]; t.legato = a[6] != 0; t.retrig_ms = a[7];
    t.gate_volts = (float)a[8]; t.vel_volts = (float)a[9];
    if (t.midi_ch > 16 || t.pitch_out >= MCV_OUTS || t.gate_out >= MCV_OUTS || t.vel_out >= MCV_OUTS) return false;
    if (t.bend_range > 48 || t.gate_volts > 10.0f || t.vel_volts > 10.0f) return false;
    for (int i = 0; i < MCV_CC_MAPS; i++) {
        t.cc[i].cc = a[10 + 3 * i];
        t.cc[i].out = (int8_t)(a[11 + 3 * i] - 1);
        t.cc[i].bipolar = a[12 + 3 * i] != 0;
        if (t.cc[i].out >= MCV_OUTS) return false;
    }
    *c = t;
    return true;
}

size_t proto_handle(proto_ctx_t *ctx, const uint8_t *msg, size_t len, uint8_t *reply, size_t cap) {
    if (len < 6 || memcmp(msg, HDR, 4) != 0 || msg[len - 1] != 0xF7) return 0;  // not ours
    const uint8_t cmd = msg[4];
    const uint8_t *a = msg + 5;
    const size_t n = len - 6;
    uint8_t p[PROTO_MAX_MSG];
    size_t k = 0;
    ctx->host_seen = true;

    switch (cmd) {
    case P_GET_INFO:
        p[k++] = CV_FW_MAJOR; p[k++] = CV_FW_MINOR;
        p[k++] = CV_A_OUT; p[k++] = CV_A_IN; p[k++] = CV_P_OUT; p[k++] = CV_P_IN;
        p[k++] = ctx->flags & 0x7F; p[k++] = ctx->src & 0x7F; p[k++] = ctx->batt_pct & 0x7F;
        k += proto_put_u32(p + k, ctx->underruns);
        return proto_build(P_GET_INFO | 0x40, p, k, reply, cap);

    case P_GET_CAL: {
        if (n != 2) return ack(cmd, P_BAD_ARGS, reply, cap);
        uint8_t kind = a[0], ch = a[1];
        const cv_lin_t *l;
        uint8_t range = 0;
        if (kind == 0 && cal_out_kind(ch) != CH_NONE) { l = &ctx->cal->out[ch]; range = ctx->cal->out_range[ch]; }
        else if (kind == 1 && cal_in_kind(ch) != CH_NONE) l = &ctx->cal->in[ch];
        else return ack(cmd, P_BAD_ARGS, reply, cap);
        p[k++] = kind; p[k++] = ch;
        k += proto_put_f32(p + k, l->gain);
        k += proto_put_f32(p + k, l->offset);
        p[k++] = range;
        return proto_build(P_GET_CAL | 0x40, p, k, reply, cap);
    }
    case P_SET_CAL: {
        if (n != 12) return ack(cmd, P_BAD_ARGS, reply, cap);
        uint8_t kind = a[0], ch = a[1];
        cv_lin_t l = {proto_get_f32(a + 2), proto_get_f32(a + 7)};
        cv_cal_t trial = *ctx->cal;
        if (kind == 0 && cal_out_kind(ch) != CH_NONE) trial.out[ch] = l;
        else if (kind == 1 && cal_in_kind(ch) != CH_NONE) trial.in[ch] = l;
        else return ack(cmd, P_BAD_ARGS, reply, cap);
        cal_seal(&trial);
        if (!cal_valid(&trial)) return ack(cmd, P_BAD_ARGS, reply, cap);
        *ctx->cal = trial;
        return ack(cmd, P_OK, reply, cap);
    }
    case P_SET_RANGE:
        if (n != 2 || cal_out_kind(a[0]) == CH_NONE || a[1] >= CV_RANGE_COUNT) return ack(cmd, P_BAD_ARGS, reply, cap);
        ctx->cal->out_range[a[0]] = a[1];
        cal_seal(ctx->cal);
        return ack(cmd, P_OK, reply, cap);
    case P_SAVE:
        ctx->req_save = true;
        return ack(cmd, P_OK, reply, cap);
    case P_DEFAULTS:
        cal_defaults(ctx->cal);
        return ack(cmd, P_OK, reply, cap);
    case P_RAW_OUT: {
        int local;
        out_engine_t *e = (n == 7) ? engine_for(ctx, a[0], &local) : NULL;
        if (!e) return ack(cmd, P_BAD_ARGS, reply, cap);
        int32_t code = (int32_t)proto_get_u32(a + 2);
        int32_t lo, hi;
        cal_code_limits(a[0], &lo, &hi);
        if (a[1] && (code < lo || code > hi)) return ack(cmd, P_BAD_ARGS, reply, cap);
        e->override_on[local] = a[1] != 0;
        e->raw_override[local] = code;
        return ack(cmd, P_OK, reply, cap);
    }
    case P_MEASURE: {
        if (n != 5) return ack(cmd, P_BAD_ARGS, reply, cap);
        uint32_t ns = proto_get_u32(a);
        if (ns == 0 || ns > 10u * CV_AUDIO_RATE) return ack(cmd, P_BAD_ARGS, reply, cap);
        in_measure_start(ctx->meas, ns);
        return ack(cmd, P_OK, reply, cap);
    }
    case P_DFU:
        ctx->req_dfu = true;
        return ack(cmd, P_OK, reply, cap);
    case P_CV_OUT: {
        if (n != 2 + 3 * CV_P_OUT) return 0;   // streaming: no ack, malformed frames dropped
        uint32_t mask = (uint32_t)(a[0] & 0x7F) | (uint32_t)(a[1] & 0x01) << 7;
        for (int i = 0; i < CV_P_OUT; i++)
            if (mask & (1u << i)) cv_glide_set(ctx->cv, i, proto_get_cv(a + 2 + 3 * i));
        return 0;
    }
    case P_CV_IN: {
        if (n != 5) return ack(cmd, P_BAD_ARGS, reply, cap);
        uint32_t r = proto_get_u32(a);
        if (r > 1000) return ack(cmd, P_BAD_ARGS, reply, cap);
        ctx->cv_in_rate = r;
        return ack(cmd, P_OK, reply, cap);
    }
    case P_CV_SRC:
        if (n != CV_P_OUT) return ack(cmd, P_BAD_ARGS, reply, cap);
        for (int i = 0; i < CV_P_OUT; i++) if (a[i] > SRC_MAPPER) return ack(cmd, P_BAD_ARGS, reply, cap);
        for (int i = 0; i < CV_P_OUT; i++) ctx->p_src[i] = a[i];
        return ack(cmd, P_OK, reply, cap);
    case P_MAP_CFG:
        if (!parse_map(a, n, &ctx->map_cfg)) return ack(cmd, P_BAD_ARGS, reply, cap);
        ctx->map_dirty = true;
        return ack(cmd, P_OK, reply, cap);
    default:
        return ack(cmd, P_UNKNOWN, reply, cap);
    }
}

size_t proto_measure_reply(proto_ctx_t *ctx, uint8_t *reply, size_t cap) {
    uint8_t p[PROTO_MAX_MSG];
    size_t k = 0;
    for (int i = 0; i < CV_MAX_IN; i++) k += proto_put_f32(p + k, in_measure_mean(ctx->meas, i));
    return proto_build(P_MEASURE | 0x40, p, k, reply, cap);
}

size_t proto_cv_in_frame(const float *volts, uint8_t *out, size_t cap) {
    uint8_t p[3 * CV_P_IN];
    size_t k = 0;
    for (int i = 0; i < CV_P_IN; i++) k += proto_put_cv(p + k, volts[i]);
    return proto_build(P_CV_IN | 0x40, p, k, out, cap);
}
