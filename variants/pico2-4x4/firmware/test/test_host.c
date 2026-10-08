// Host-side unit tests for the pure firmware modules. Build: make -C firmware/test
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cv_config.h"
#include "conv_codec.h"
#include "cal.h"
#include "engine.h"
#include "proto.h"
#include "usb_desc.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

// ------------------------------------------------------------- descriptors
static void test_descriptors(void) {
    const uint8_t *d = cv_desc_config;
    CHECK(d[1] == 0x02);
    CHECK((d[2] | d[3] << 8) == CV_CONFIG_LEN);
    CHECK(d[4] == ITF_COUNT);
    int n_itf_alt0 = 0, iad = 0, total = 0, ac_hdr_at = -1, ms_hdr_at = -1;
    int eps = 0, fb_ok = 0, cur_itf = -1, cur_alt = -1;
    int iso_bytes = 0;
    for (int i = 0; i < CV_CONFIG_LEN;) {
        int len = d[i], type = d[i + 1];
        CHECK(len >= 2 && i + len <= CV_CONFIG_LEN);
        if (len < 2) break;
        if (type == 0x0B) { iad++; CHECK(d[i + 2] == ITF_AC && d[i + 3] == 3 && d[i + 6] == 0x20); }
        if (type == 0x04) {
            CHECK(len == 9);
            cur_itf = d[i + 2]; cur_alt = d[i + 3];
            if (cur_alt == 0) n_itf_alt0++;
            if (cur_itf == ITF_AC) CHECK(d[i + 7] == 0x20);       // UAC2 protocol
            if (cur_itf == ITF_MIDI_AC) CHECK(d[i + 7] == 0x00);  // MIDI uses UAC1 control
        }
        if (type == 0x24 && d[i + 2] == 0x01 && cur_itf == ITF_AC) ac_hdr_at = i;
        if (type == 0x24 && d[i + 2] == 0x01 && cur_itf == ITF_MIDI_MS) ms_hdr_at = i;
        if (type == 0x05) {
            eps++;
            int ep = d[i + 2], attr = d[i + 3], mps = d[i + 4] | d[i + 5] << 8;
            CHECK(mps <= 1023);
            if ((attr & 3) == 1) {               // isochronous
                iso_bytes += mps;
                if (ep == EP_AUDIO_OUT) CHECK(mps == CV_EP_OUT_SIZE && (attr & 0x0C) == 0x04);
                if (ep == EP_AUDIO_IN)  CHECK(mps == CV_EP_IN_SIZE && (attr & 0x0C) == 0x04);
                if (ep == EP_AUDIO_FB) { fb_ok = ((attr & 0x30) == 0x10) && cur_itf == ITF_AS_OUT; }
            }
        }
        total += len;
        i += len;
    }
    CHECK(total == CV_CONFIG_LEN);
    CHECK(iad == 1);
    CHECK(n_itf_alt0 == ITF_COUNT);
    CHECK(eps == 5);
    CHECK(fb_ok);
    // Full-speed periodic budget: 90 % of 1500 bytes minus per-transaction overhead.
    CHECK(iso_bytes + 3 * 13 <= 1350);
    CHECK(ac_hdr_at > 0 && (d[ac_hdr_at + 6] | d[ac_hdr_at + 7] << 8) == CV_AC_CS_LEN);
    CHECK(ms_hdr_at > 0 && (d[ms_hdr_at + 5] | d[ms_hdr_at + 6] << 8) == CV_MIDI_MS_CS_LEN);
    // Audio function length used by TinyUSB (CFG_TUD_AUDIO_FUNC_1_DESC_LEN).
    CHECK(9 + CV_AUDIO_FUNC_LEN + CV_MIDI_LEN == CV_CONFIG_LEN);
    CHECK(CV_EP_OUT_SIZE == 49 * CV_OUT_BYTES * CV_N_OUT);
    for (int s = 1; s < STR_COUNT; s++) CHECK(cv_string((uint8_t)s) != NULL);
    CHECK(strcmp(cv_string(STR_CH_IN1), "CV In 1") == 0);
    CHECK(cv_desc_device[4] == 0xEF && cv_desc_device[0] == 18 && cv_desc_device[2] == 0x01);
    { int diff = 0; for (int i = 0; i < CV_CONFIG_LEN; i++) diff += cv_desc_config[i] != cv_desc_config_fb3[i]; CHECK(diff == 1); }
    CHECK(cv_desc_bos[0] == 5 && cv_desc_bos[2] == 12);
    printf("descriptor: config %d bytes, iso %d bytes/frame\n", CV_CONFIG_LEN, iso_bytes);
}

// ------------------------------------------------------------- converters
static void test_codec(void) {
    // DAC8568 frame layout (datasheet Table 4).
    CHECK(dac8568_frame(DAC8568_C_WRITE_INPUT, 2, 0xABCD, 0) == 0x002ABCD0u);
    CHECK(dac8568_soft_reset() == 0x07000000u);
    CHECK(dac8568_ref_on() == 0x08000001u);
    uint16_t codes[4] = {1, 2, 3, 0xFFFF};
    uint32_t fr[4];
    CHECK(dac8568_encode_sample(codes, 4, fr) == 4);
    CHECK((fr[0] >> 24) == 0x0 && (fr[3] >> 24) == 0x2);      // last frame updates all
    CHECK(((fr[3] >> 20) & 0xF) == 3 && ((fr[3] >> 4) & 0xFFFF) == 0xFFFF);
    // ADS131M04 command words.
    CHECK(ads_wreg(ADS_REG_CLOCK, 1) == 0x6180);
    CHECK(ads_rreg(ADS_REG_ID, 1) == 0xA000);
    CHECK(ads_wreg_ack(ADS_REG_CLOCK, 1) == 0x4180);
    CHECK(ads_word(0x6180) == 0x618000u);
    CHECK(ads_sext24(0x800000u) == -8388608 && ads_sext24(0x7FFFFFu) == 8388607 && ads_sext24(0xFFFFFFu) == -1);
    CHECK(ads_id_ok(0x2401) && !ads_id_ok(0x0000) && !ads_id_ok(0xFFFF));
    CHECK(ADS_CLKIN_HZ == 6144000u);
    // Clocks.
    uint16_t x, y;
    CHECK(cv_dma_timer_frac(150000000u, 48000u * 4, &x, &y) && x == 4 && y == 3125);
    CHECK(cv_dma_timer_frac(150000000u, 48000u * 8, &x, &y) && x == 8 && y == 3125);
    CHECK(cv_dma_timer_frac(125000000u, 48000u * 4, &x, &y) && (uint64_t)125000000u * x / y == 192000u);
    uint32_t di; uint16_t df;
    CHECK(cv_gpout_div(150000000u, ADS_CLKIN_HZ, &di, &df) && di == 24 && df == 27136);
    NEAR(150e6 / (di + df / 65536.0) / 2 / ADS_OSR, 48000.0, 1e-6);
}

// ------------------------------------------------------------- calibration
static void test_cal(void) {
    cv_cal_t c;
    cal_defaults(&c);
    CHECK(cal_valid(&c));
    // Nominal zero code close to midscale; full code range covers +-10 V.
    uint16_t z = cal_volts_to_code(&c, 0, 0.0f);
    CHECK(z > 31500 && z < 34000);
    NEAR(cal_code_to_volts(&c, 0, 0), 10.22, 0.02);
    NEAR(cal_code_to_volts(&c, 0, 65535), -10.20, 0.02);
    CHECK(cal_code_to_volts(&c, 0, 0) > 10.0f && cal_code_to_volts(&c, 0, 65535) < -10.0f);
    // Round trip within half an LSB (~0.16 mV).
    for (float v = -10.0f; v <= 10.0f; v += 0.37f)
        NEAR(cal_code_to_volts(&c, 1, cal_volts_to_code(&c, 1, v)), v, 0.00025);
    // Limiter.
    c.out_range[2] = CV_RANGE_BI5; cal_seal(&c);
    NEAR(cal_code_to_volts(&c, 2, cal_volts_to_code(&c, 2, 9.0f)), 5.0, 0.0003);
    c.out_range[2] = CV_RANGE_UNI10; cal_seal(&c);
    NEAR(cal_code_to_volts(&c, 2, cal_volts_to_code(&c, 2, -3.0f)), 0.0, 0.0003);
    CHECK(cal_volts_to_code(&c, 0, NAN) == cal_volts_to_code(&c, 0, -10.0f));
    // CRC catches corruption.
    cv_cal_t bad = c; bad.out[1].offset += 1.0f; CHECK(!cal_valid(&bad));
    // Input: +10 V at the jack -> ADC counts -> volts.
    const float rpar = HW_IN_RSH * HW_IN_RADC / (HW_IN_RSH + HW_IN_RADC);
    const float k = rpar / (HW_IN_RS + rpar);
    int32_t raw10 = (int32_t)lrintf(10.0f * k / HW_ADC_FS * 8388608.0f);
    NEAR(cal_raw_to_volts(&c, 0, raw10), 10.0, 1e-4);
    CHECK(15.0f * k < 1.3f);   // +-15 V fault stays inside the ADC's -1.3 V input limit
    // Two-point solve recovers a synthetic channel.
    cv_lin_t truth = {-3200.0f, 32900.0f}, got;
    float c1 = 8000, c2 = 57000;
    float v1 = (c1 - truth.offset) / truth.gain, v2 = (c2 - truth.offset) / truth.gain;
    CHECK(cal_solve_out(c1, v1, c2, v2, &got));
    NEAR(got.gain, truth.gain, 0.01); NEAR(got.offset, truth.offset, 0.05);
    CHECK(!cal_solve_out(1, 1, 1, 1, &got));
    cv_lin_t tin = {1.7e-6f, 120.0f};
    float r1 = -2.0e6f, r2 = 3.0e6f;
    CHECK(cal_solve_in(r1, (r1 - tin.offset) * tin.gain, r2, (r2 - tin.offset) * tin.gain, &got));
    NEAR(got.gain / tin.gain, 1.0, 1e-4); NEAR(got.offset, tin.offset, 1.0);
}

// ------------------------------------------------------------- engine
static void test_engine(void) {
    cv_cal_t c; cal_defaults(&c);
    out_engine_t e; out_engine_init(&e);
    uint16_t codes[CV_MAX_OUT], zero[CV_MAX_OUT];
    out_zero_codes(&c, zero);
    // Power-on: no data -> 0 V.
    out_engine_step(&e, &c, NULL, codes);
    CHECK(memcmp(codes, zero, sizeof(uint16_t) * CV_N_OUT) == 0);
    // Play +5 V on ch0, -10 V on ch1 (int16 full scale = 10 V).
    int16_t s[CV_MAX_OUT] = {16384, -32768, 0, 3277};
    out_engine_step(&e, &c, s, codes);
    NEAR(cal_code_to_volts(&c, 0, codes[0]), 5.0, 0.001);
    NEAR(cal_code_to_volts(&c, 1, codes[1]), -10.0, 0.001);
    NEAR(cal_code_to_volts(&c, 3, codes[3]), 1.0, 0.001);
    CHECK(e.state == OUT_PLAY);
    // Starvation: hold exactly CV_HOLD_SAMPLES, then ramp, then zero.
    uint32_t n = 0;
    do { out_engine_step(&e, &c, NULL, codes); n++; } while (e.state == OUT_HOLD && n < 100000);
    CHECK(n == CV_HOLD_SAMPLES + 1);
    CHECK(e.underruns == 1);
    float prev = 10.0f; int monotonic = 1; uint32_t ramp = 0;
    while (e.state != OUT_ZERO && ramp < 100000) {
        out_engine_step(&e, &c, NULL, codes); ramp++;
        float v = fabsf(e.last[1]);
        if (v > prev + 1e-6f) monotonic = 0;
        prev = v;
    }
    CHECK(monotonic);
    CHECK(ramp <= CV_SAMPLE_RATE / 200 + 2);   // 10 V ramps out within ~5 ms
    out_engine_step(&e, &c, NULL, codes);
    CHECK(memcmp(codes, zero, sizeof(uint16_t) * CV_N_OUT) == 0);
    // Resume is immediate.
    out_engine_step(&e, &c, s, codes);
    NEAR(cal_code_to_volts(&c, 0, codes[0]), 5.0, 0.001);
    // Raw override wins (calibration), release restores.
    e.raw_override[2] = 1234;
    out_engine_step(&e, &c, s, codes); CHECK(codes[2] == 1234);
    e.raw_override[2] = -1;
    out_engine_step(&e, &c, s, codes); CHECK(codes[2] == zero[2]);

    // Input path: raw counts for +7.5 V -> USB sample of 0.75 FS.
    const float rpar = HW_IN_RSH * HW_IN_RADC / (HW_IN_RSH + HW_IN_RADC);
    const float k = rpar / (HW_IN_RS + rpar);
    int32_t raw[CV_MAX_IN] = {(int32_t)lrintf(7.5f * k / 1.2f * 8388608.0f), 0, -8388608, 8388607};
    uint8_t f[CV_N_IN * CV_IN_BYTES];
    in_engine_step(&c, raw, f);
    int32_t s0 = 0;
    for (int b = 0; b < CV_IN_BYTES; b++) s0 |= (int32_t)f[b] << (8 * b);
    s0 = (s0 << (32 - CV_IN_BITS)) >> (32 - CV_IN_BITS);
    const double fs = (double)((1L << (CV_IN_BITS - 1)) - 1);
    NEAR(s0 / fs, 0.75, 2.0 / fs);
    CHECK(in_volts_to_sample(100.0f) == (int32_t)fs);         // clamps
    CHECK(in_volts_to_sample(-100.0f) == -(int32_t)fs - 1);
    // Measurement accumulator.
    in_measure_t m; in_measure_start(&m, 3);
    int32_t r1[4] = {10, 20, 30, 40}, r2[4] = {20, 40, 60, 80};
    in_measure_add(&m, r1); in_measure_add(&m, r2); in_measure_add(&m, r2); in_measure_add(&m, r2);
    CHECK(m.done && m.n == 3); NEAR(in_measure_mean(&m, 1), 100.0 / 3.0, 1e-4);
}

// ------------------------------------------------------------- ring maths
static void test_ring(void) {
    const uint32_t N = 256;
    bool ur;
    uint32_t wr = 0;
    // Startup: fill to target.
    CHECK(ring_to_write(&wr, 0, N, 72, 8, &ur) == 72 && !ur);
    wr = 72;
    // Steady state: DMA consumed 48 samples -> write 48.
    CHECK(ring_to_write(&wr, 48, N, 72, 8, &ur) == 48 && !ur);
    wr = (72 + 48) % N;
    // Wrap-around arithmetic.
    wr = 10; CHECK(ring_to_write(&wr, 250, N, 72, 8, &ur) == 56 && !ur);
    // Underrun: hw passed wr.
    wr = 100; uint32_t k = ring_to_write(&wr, 110, N, 72, 8, &ur);
    CHECK(ur && wr == 118 && k == 64);
    // Simulate 10 s of 1 ms service with random-ish jitter: never underruns.
    uint32_t hw = 0, w = 72, underruns = 0;   // firmware pre-fills to target
    for (int ms = 0; ms < 10000; ms++) {
        uint32_t step = 48 + (ms % 3 == 0) - (ms % 5 == 0);   // 47..49 per ms
        hw = (hw + step) % N;
        uint32_t todo = ring_to_write(&w, hw, N, 72, 8, &ur);
        underruns += ur;
        w = (w + todo) % N;
    }
    CHECK(underruns == 0);
}

// ------------------------------------------------------------- SysEx
static void test_proto(void) {
    cv_cal_t c; cal_defaults(&c);
    out_engine_t e; out_engine_init(&e);
    in_measure_t m; memset(&m, 0, sizeof m);
    proto_ctx_t ctx = {.cal = &c, .out = &e, .meas = &m, .flags = 0x0A};
    uint8_t msg[PROTO_MAX_MSG], rep[PROTO_MAX_MSG], p[32];
    size_t n, r;

    uint8_t b[5];
    proto_put_u32(b, 0xDEADBEEFu); CHECK(proto_get_u32(b) == 0xDEADBEEFu);
    for (int i = 0; i < 5; i++) CHECK(b[i] < 0x80);
    proto_put_f32(b, -3252.5f); CHECK(proto_get_f32(b) == -3252.5f);

    n = proto_build(P_GET_INFO, NULL, 0, msg, sizeof msg);
    r = proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(r == 6 + 10 && rep[4] == 0x41 && rep[7] == CV_N_OUT && rep[8] == CV_N_IN && rep[9] == 0x0A);
    for (size_t i = 1; i < r - 1; i++) CHECK(rep[i] < 0x80);

    // SET_CAL out ch1 then GET_CAL reads it back.
    size_t k = 0; p[k++] = 0; p[k++] = 1;
    k += proto_put_f32(p + k, -3300.0f); k += proto_put_f32(p + k, 32800.0f);
    n = proto_build(P_SET_CAL, p, k, msg, sizeof msg);
    r = proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[4] == P_ACK && rep[5] == P_SET_CAL && rep[6] == P_OK);
    CHECK(c.out[1].gain == -3300.0f && cal_valid(&c));
    uint8_t q[2] = {0, 1};
    n = proto_build(P_GET_CAL, q, 2, msg, sizeof msg);
    r = proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[4] == 0x42 && proto_get_f32(rep + 7) == -3300.0f && proto_get_f32(rep + 12) == 32800.0f);
    // Rejects zero gain and bad channel.
    k = 0; p[k++] = 0; p[k++] = 1; k += proto_put_f32(p + k, 0.0f); k += proto_put_f32(p + k, 1.0f);
    n = proto_build(P_SET_CAL, p, k, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[6] == P_BAD_ARGS && c.out[1].gain == -3300.0f);
    q[0] = 0; q[1] = 9;
    n = proto_build(P_GET_CAL, q, 2, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(rep[4] == P_ACK && rep[6] == P_BAD_ARGS);
    // Range, raw out, measure, save, unknown, foreign SysEx ignored.
    q[0] = 3; q[1] = CV_RANGE_BI5;
    n = proto_build(P_SET_RANGE, q, 2, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(c.out_range[3] == CV_RANGE_BI5 && cal_valid(&c));
    k = 0; p[k++] = 2; k += proto_put_u32(p + k, 40000);
    n = proto_build(P_RAW_OUT, p, k, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(e.raw_override[2] == 40000);
    k = 0; p[k++] = 2; k += proto_put_u32(p + k, 70000);
    n = proto_build(P_RAW_OUT, p, k, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(e.raw_override[2] == -1);
    k = proto_put_u32(p, 4800);
    n = proto_build(P_MEASURE, p, k, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(m.target == 4800 && !m.done);
    int32_t raw[4] = {100, -200, 300, -400};
    for (int i = 0; i < 4800; i++) in_measure_add(&m, raw);
    r = proto_measure_reply(&ctx, rep, sizeof rep);
    CHECK(r == 6 + 5 * CV_N_IN && rep[4] == 0x48 && proto_get_f32(rep + 5 + 5) == -200.0f);
    n = proto_build(P_SAVE, NULL, 0, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(ctx.req_save);
    n = proto_build(0x33, NULL, 0, msg, sizeof msg);
    proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(rep[6] == P_UNKNOWN);
    uint8_t foreign[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};   // identity request
    CHECK(proto_handle(&ctx, foreign, sizeof foreign, rep, sizeof rep) == 0);
}

int main(void) {
    test_descriptors();
    test_codec();
    test_cal();
    test_engine();
    test_ring();
    test_proto();
    printf("host tests (N_OUT=%d): %d checks, %d failures\n", CV_N_OUT, checks, fails);
    return fails ? 1 : 0;
}
