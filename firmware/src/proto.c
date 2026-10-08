#include "proto.h"
#include <string.h>

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

size_t proto_handle(proto_ctx_t *ctx, const uint8_t *msg, size_t len, uint8_t *reply, size_t cap) {
    if (len < 6 || memcmp(msg, HDR, 4) != 0 || msg[len - 1] != 0xF7) return 0;  // not ours
    const uint8_t cmd = msg[4];
    const uint8_t *a = msg + 5;
    const size_t n = len - 6;
    uint8_t p[PROTO_MAX_MSG];
    size_t k = 0;

    switch (cmd) {
    case P_GET_INFO:
        p[k++] = CV_FW_MAJOR; p[k++] = CV_FW_MINOR; p[k++] = CV_N_OUT; p[k++] = CV_N_IN;
        p[k++] = ctx->flags & 0x7F;
        k += proto_put_u32(p + k, ctx->out->underruns);
        return proto_build(P_GET_INFO | 0x40, p, k, reply, cap);

    case P_GET_CAL: {
        if (n != 2) return ack(cmd, P_BAD_ARGS, reply, cap);
        uint8_t kind = a[0], ch = a[1];
        const cv_lin_t *l;
        uint8_t range = 0;
        if (kind == 0 && ch < CV_N_OUT) { l = &ctx->cal->out[ch]; range = ctx->cal->out_range[ch]; }
        else if (kind == 1 && ch < CV_N_IN) l = &ctx->cal->in[ch];
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
        if (kind == 0 && ch < CV_N_OUT) trial.out[ch] = l;
        else if (kind == 1 && ch < CV_N_IN) trial.in[ch] = l;
        else return ack(cmd, P_BAD_ARGS, reply, cap);
        cal_seal(&trial);
        if (!cal_valid(&trial)) return ack(cmd, P_BAD_ARGS, reply, cap);
        *ctx->cal = trial;
        return ack(cmd, P_OK, reply, cap);
    }
    case P_SET_RANGE:
        if (n != 2 || a[0] >= CV_N_OUT || a[1] >= CV_RANGE_COUNT) return ack(cmd, P_BAD_ARGS, reply, cap);
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
        if (n != 6 || a[0] >= CV_N_OUT) return ack(cmd, P_BAD_ARGS, reply, cap);
        uint32_t code = proto_get_u32(a + 1);
        ctx->out->raw_override[a[0]] = (code > 65535u) ? -1 : (int32_t)code;
        return ack(cmd, P_OK, reply, cap);
    }
    case P_MEASURE: {
        if (n != 5) return ack(cmd, P_BAD_ARGS, reply, cap);
        uint32_t ns = proto_get_u32(a);
        if (ns == 0 || ns > 10u * CV_SAMPLE_RATE) return ack(cmd, P_BAD_ARGS, reply, cap);
        in_measure_start(ctx->meas, ns);
        return ack(cmd, P_OK, reply, cap);
    }
    case P_BOOTSEL:
        ctx->req_bootsel = true;
        return ack(cmd, P_OK, reply, cap);
    default:
        return ack(cmd, P_UNKNOWN, reply, cap);
    }
}

size_t proto_measure_reply(proto_ctx_t *ctx, uint8_t *reply, size_t cap) {
    uint8_t p[PROTO_MAX_MSG];
    size_t k = 0;
    for (int i = 0; i < CV_N_IN; i++) k += proto_put_f32(p + k, in_measure_mean(ctx->meas, i));
    return proto_build(P_MEASURE | 0x40, p, k, reply, cap);
}
