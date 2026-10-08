// Host-side unit tests for the pure firmware modules (firmware/common). make -C firmware/test
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cv_config.h"
#include "conv_codec.h"
#include "cal.h"
#include "engine.h"
#include "proto.h"
#include "usb_desc.h"
#include "midi_cv.h"
#include "ble_midi.h"
#include "power.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

// ------------------------------------------------------------- USB descriptors + budget
static void test_descriptors(void) {
    const uint8_t *d = cv_desc_config;
    CHECK(d[1] == 0x02 && (d[2] | d[3] << 8) == CV_CONFIG_LEN && d[4] == ITF_COUNT);
    CHECK(d[8] <= 250);   // bus-powered config may declare at most 500 mA
    int alt0 = 0, iad = 0, total = 0, eps = 0, fb_ok = 0, cur_itf = -1, ac_hdr = -1, ms_hdr = -1;
    double iso_ns = 0;
    int iso_bytes = 0;
    for (int i = 0; i < CV_CONFIG_LEN;) {
        int len = d[i], type = d[i + 1];
        CHECK(len >= 2 && i + len <= CV_CONFIG_LEN);
        if (len < 2) break;
        if (type == 0x0B) { iad++; CHECK(d[i + 2] == ITF_AC && d[i + 3] == 3 && d[i + 6] == 0x20); }
        if (type == 0x04) {
            cur_itf = d[i + 2];
            if (d[i + 3] == 0) alt0++;
            if (cur_itf == ITF_AC) CHECK(d[i + 7] == 0x20);
            if (cur_itf == ITF_MIDI_AC) CHECK(d[i + 7] == 0x00);
        }
        if (type == 0x24 && d[i + 2] == 0x01 && cur_itf == ITF_AC) ac_hdr = i;
        if (type == 0x24 && d[i + 2] == 0x01 && cur_itf == ITF_MIDI_MS) ms_hdr = i;
        if (type == 0x05) {
            eps++;
            int ep = d[i + 2], attr = d[i + 3], mps = d[i + 4] | d[i + 5] << 8;
            CHECK(mps <= 1023);
            if ((attr & 3) == 1) {
                iso_bytes += mps;
                iso_ns += usb_fs_iso_ns((uint32_t)mps);
                if (ep == EP_AUDIO_OUT) CHECK(mps == CV_EP_OUT_SIZE);
                if (ep == EP_AUDIO_IN) CHECK(mps == CV_EP_IN_SIZE);
                if (ep == EP_AUDIO_FB) fb_ok = ((attr & 0x30) == 0x10) && cur_itf == ITF_AS_OUT;
            }
        }
        total += len;
        i += len;
    }
    CHECK(total == CV_CONFIG_LEN && iad == 1 && alt0 == ITF_COUNT && eps == 5 && fb_ok);
    CHECK(ac_hdr > 0 && (d[ac_hdr + 6] | d[ac_hdr + 7] << 8) == CV_AC_CS_LEN);
    CHECK(ms_hdr > 0 && (d[ms_hdr + 5] | d[ms_hdr + 6] << 8) == CV_MIDI_MS_CS_LEN);
    CHECK(CV_EP_OUT_SIZE == 33 * 2 * 8 && CV_EP_IN_SIZE == 33 * 2 * 6);
    // Full-speed periodic budget with worst-case bit stuffing: <= 90 % of the 1 ms frame.
    CHECK(iso_ns <= 900000.0);
    // The variants Jouni asked about do not fit (documented in the README budget table).
    CHECK(usb_fs_iso_ns(784) + usb_fs_iso_ns(784) + usb_fs_iso_ns(4) > 900000.0);   // 8+8 @ 48k/16
    CHECK(usb_fs_iso_ns(800) + usb_fs_iso_ns(800) + usb_fs_iso_ns(4) > 900000.0);   // 16+16 @ 24k/16
    int diff = 0;
    for (int i = 0; i < CV_CONFIG_LEN; i++) diff += cv_desc_config[i] != cv_desc_config_fb3[i];
    CHECK(diff == 1);
    for (int s = 1; s < STR_COUNT; s++) CHECK(cv_string((uint8_t)s) != NULL);
    CHECK(strcmp(cv_string(STR_PRODUCT), "Circuit Studio CV 8x6") == 0);
    CHECK(strcmp(cv_string(STR_CH_IN1 + 5), "Audio In 6") == 0);
    printf("descriptor: %d bytes, iso %d B/frame = %.0f us of 900 us\n", CV_CONFIG_LEN, iso_bytes, iso_ns / 1000);
}

// ------------------------------------------------------------- converters + clocks
static void test_codec(void) {
    CHECK(dac8568_frame(DAC8568_C_WRITE_INPUT, 2, 0xABCD, 0) == 0x002ABCD0u);
    CHECK(dac8568_soft_reset() == 0x07000000u && dac8568_ref_on() == 0x08000001u);
    int32_t codes[8] = {1, 2, 3, 4, 5, 6, 7, 0xFFFF};
    uint32_t fr[8];
    CHECK(dac8568_encode_sample(codes, 8, fr) == 8);
    CHECK((fr[0] >> 24) == 0 && (fr[7] >> 24) == 2 && ((fr[7] >> 20) & 0xF) == 7);
    CHECK(ads_wreg(ADS_REG_CLOCK, 1) == 0x6180 && ads_rreg(ADS_REG_ID, 1) == 0xA000);
    CHECK(ads_sext24(0x800000u) == -8388608 && ads_sext24(0xFFFFFFu) == -1);
    CHECK(ads_m08_id_ok(0x2811) && !ads_m08_id_ok(0x2401));
    CHECK(ADS_CLKIN_HZ / 2 / ADS_OSR == 2 * CV_P_RATE);          // ADC 2x faster than the read rate
    uint8_t b[6] = {0x12, 0x34, 0x56, 0xFF, 0xFF, 0xFE}; uint32_t w[2];
    ads_unpack(b, 2, w); CHECK(w[0] == 0x123456 && ads_sext24(w[1]) == -2);
    CHECK(pcm3168_write(PCM_REG_ADC_HPF, PCM_ADC_HPF_OFF) == 0x5207);
    CHECK(pcm3168_write(PCM_REG_DAC_FMT, PCM_DAC_TDM_LJ) == 0x4107);
    CHECK(tdm_to_s24(s24_to_tdm(-123456)) == -123456 && tdm_to_s24(s24_to_tdm(8388607)) == 8388607);
    // Clocks: codec SCKI = 512 fs from the S3's 160 MHz PLL with a 9-bit fractional divider.
    uint32_t n, num, den;
    CHECK(cv_frac_div(160000000u, 512u * CV_AUDIO_RATE, 512, &n, &num, &den) && n == 9 && num == 49 && den == 64);
    NEAR(160e6 / (n + (double)num / den) / 512, 32000.0, 1e-6);
    // 48 kHz would not be exact with a 6-bit divider (the original ESP32) - documents why S3 matters.
    CHECK(!cv_frac_div(160000000u, 512u * 48000u, 63, &n, &num, &den));
}

// ------------------------------------------------------------- calibration
#ifdef CV_TIER_A
// Tier A build: only the precision-group maths change (12-bit MCP4728, 16-bit ADS1115).
static void test_tiera(void) {
    cv_cal_t c; cal_defaults(&c);
    CHECK(cal_valid(&c));
    int32_t lo, hi; cal_code_limits(8, &lo, &hi); CHECK(lo == 0 && hi == 4095);
    int32_t z = cal_volts_to_code(&c, 8, 0.0f);
    CHECK(z > 1950 && z < 2150);
    CHECK(cal_code_to_volts(&c, 8, 0) > 10.0f && cal_code_to_volts(&c, 8, 4095) < -10.0f);
    for (float v = -10.0f; v <= 10.0f; v += 0.37f) NEAR(cal_code_to_volts(&c, 8, cal_volts_to_code(&c, 8, v)), v, 0.0026);
    const float kt = HW_TA_IN_RP / (HW_TA_IN_R1 + HW_TA_IN_RP), voff = 1.65f * HW_TA_IN_R1 / (HW_TA_IN_R1 + HW_TA_IN_RP);
    for (float v = -10.0f; v <= 10.0f; v += 2.5f) {
        float vadc = voff + kt * v;
        CHECK(vadc > 0.2f && vadc < 3.1f);   // inside the ADS1115's 0..VDD input range
        NEAR(cal_raw_to_volts(&c, 8, (int32_t)lrintf(vadc / 4.096f * 32768.0f)), v, 0.002);
    }
    int32_t codes[4] = {0, 4095, 0x123, 0x800}; uint8_t b[8];
    CHECK(mcp4728_fast_write(codes, b) == 8 && b[2] == 0x0F && b[3] == 0xFF && b[4] == 0x01 && b[5] == 0x23);
    CHECK(ads1115_config(0) == 0xC3E3 && ads1115_config(3) == 0xF3E3);
    uint8_t q[9], rb[24] = {0};
    CHECK(mcp4728_seq_write_eeprom(codes, q) == 9 && q[0] == 0x50 && q[3] == 0x9F && q[4] == 0xFF);
    rb[6 * 2 + 3 + 1] = 0x91; rb[6 * 2 + 3 + 2] = 0x23;     // channel C EEPROM = 0x123
    CHECK(mcp4728_readback_eeprom_code(rb, 2) == 0x123);
}
#endif

static void test_cal(void) {
    cv_cal_t c;
    cal_defaults(&c);
    CHECK(cal_valid(&c));
    CHECK(cal_out_kind(0) == CH_A_OUT && cal_out_kind(8) == CH_P_OUT && cal_out_kind(16) == CH_NONE);
    CHECK(cal_in_kind(5) == CH_A_IN && cal_in_kind(6) == CH_NONE && cal_in_kind(15) == CH_P_IN);
    // Precision outputs: 0 V near midscale, both rails beyond +-10 V, round trip < 0.25 mV.
    int32_t z = cal_volts_to_code(&c, 8, 0.0f);
    CHECK(z > 31500 && z < 34000);
    CHECK(cal_code_to_volts(&c, 8, 0) > 10.0f && cal_code_to_volts(&c, 8, 65535) < -10.0f);
    for (float v = -10.0f; v <= 10.0f; v += 0.37f) NEAR(cal_code_to_volts(&c, 9, cal_volts_to_code(&c, 9, v)), v, 0.00025);
    // Codec outputs: signed 24-bit, full scale beyond 10 V, round trip < 10 uV.
    CHECK(cal_volts_to_code(&c, 0, 0.0f) == 0);
    CHECK(cal_code_to_volts(&c, 0, 8388607) > 10.0f);
    for (float v = -10.0f; v <= 10.0f; v += 0.37f) NEAR(cal_code_to_volts(&c, 3, cal_volts_to_code(&c, 3, v)), v, 0.00001);
    // Limiter and low-power (+-5 V) mode.
    c.out_range[2] = CV_RANGE_UNI5; cal_seal(&c);
    NEAR(cal_code_to_volts(&c, 2, cal_volts_to_code(&c, 2, 9.0f)), 5.0, 0.0003);
    c.low_power = 1; cal_seal(&c);
    NEAR(cal_code_to_volts(&c, 9, cal_volts_to_code(&c, 9, -9.0f)), -5.0, 0.0003);
    c.low_power = 0; cal_seal(&c);
    CHECK(cal_volts_to_code(&c, 8, NAN) == cal_volts_to_code(&c, 8, -10.0f));
    cv_cal_t bad = c; bad.out[1].offset += 1.0f; CHECK(!cal_valid(&bad));
    // Inputs: precision divider and inverting codec stage both map 10 V back to 10 V.
    const float rpar = HW_IN_RSH * HW_IN_RADC / (HW_IN_RSH + HW_IN_RADC), k = rpar / (HW_IN_RS + rpar);
    NEAR(cal_raw_to_volts(&c, 8, (int32_t)lrintf(10.0f * k / HW_ADC_FS * 8388608.0f)), 10.0, 1e-4);
    const float afs = 0.2f * HW_CODEC_VCC * 1.41421356f, ag = HW_AI_RF / HW_AI_RI;
    NEAR(cal_raw_to_volts(&c, 0, (int32_t)lrintf(-10.0f * ag / afs * 8388608.0f)), 10.0, 1e-4);
    CHECK(10.0f * ag < afs);                     // codec input headroom at +-10 V
    CHECK(15.0f * k < 1.3f);                     // +-15 V fault inside the ADS131M08 input range
    cv_lin_t got, truth = {-3200.0f, 32900.0f};
    float c1 = 8000, c2 = 57000;
    CHECK(cal_solve_out(c1, (c1 - truth.offset) / truth.gain, c2, (c2 - truth.offset) / truth.gain, &got));
    NEAR(got.gain, truth.gain, 0.01); NEAR(got.offset, truth.offset, 0.05);
    CHECK(!cal_solve_in(1, 1, 1, 1, &got));
}

// ------------------------------------------------------------- engines
static void test_engine(void) {
    cv_cal_t c; cal_defaults(&c);
    out_engine_t a; out_engine_init(&a, CV_A_OUT, 0, CV_AUDIO_RATE);
    out_engine_t p; out_engine_init(&p, CV_P_OUT, CV_P_OUT0, CV_P_RATE);
    int32_t codes[8], zero[8];
    out_zero_codes(&p, &c, zero);
    out_engine_step(&p, &c, NULL, codes);
    CHECK(memcmp(codes, zero, sizeof(int32_t) * 8) == 0);
    float v[8] = {5, -10, 0, 1, 2, 3, 4, 9.99f};
    out_engine_step(&p, &c, v, codes);
    NEAR(cal_code_to_volts(&c, 8, codes[0]), 5.0, 0.0003);
    NEAR(cal_code_to_volts(&c, 9, codes[1]), -10.0, 0.0003);
    // Hold 20 ms then ramp: sample counts scale with each group's rate.
    uint32_t n = 0;
    do { out_engine_step(&a, &c, n ? NULL : v, codes); n++; } while ((a.state == OUT_PLAY || a.state == OUT_HOLD) && n < 100000);
    CHECK(n == CV_AUDIO_RATE / 50 + 2);
    uint32_t r = 0;
    while (a.state != OUT_ZERO && r < 100000) { out_engine_step(&a, &c, NULL, codes); r++; }
    CHECK(r <= CV_AUDIO_RATE / 200 + 2);
    CHECK(codes[1] == 0);
    // Power fault forces 0 V at once, overriding data and calibration overrides.
    p.override_on[3] = true; p.raw_override[3] = 1234;
    out_engine_step(&p, &c, v, codes); CHECK(codes[3] == 1234);
    p.force_zero = true;
    out_engine_step(&p, &c, v, codes);
    CHECK(memcmp(codes, zero, sizeof(int32_t) * 8) == 0);
    p.force_zero = false;
    // Codec input path.
    int32_t raw[CV_A_IN] = {0};
    const float afs = 0.2f * HW_CODEC_VCC * 1.41421356f, ag = HW_AI_RF / HW_AI_RI;
    raw[0] = (int32_t)lrintf(-7.5f * ag / afs * 8388608.0f);
    int16_t usb[CV_A_IN];
    in_a_step(&c, raw, usb);
    NEAR(usb[0] / 32768.0, 0.75, 2.0 / 32768);
    CHECK(volts_to_usb16(100) == 32767 && volts_to_usb16(-100) == -32768);
    // Measurement across both input groups.
    in_measure_t m; in_measure_start(&m, 3);
    int32_t ra[6] = {10, 20, 30, 40, 50, 60}, rp[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int i = 0; i < 3; i++) { CHECK(!m.done); in_measure_add(&m, 0, 6, ra); in_measure_add(&m, CV_P_IN0, 8, rp); }
    CHECK(m.done); NEAR(in_measure_mean(&m, 1), 20, 1e-6); NEAR(in_measure_mean(&m, 15), 8, 1e-6);
    // Glide: 1 kHz host frames -> 2 kHz updates without steps larger than half the jump.
    cv_glide_t g; cv_glide_init(&g, 4);
    cv_glide_set(&g, 0, 1.0f);
    float prev = 0; int monotonic = 1;
    for (int i = 0; i < 4; i++) { cv_glide_tick(&g, 8); if (g.cur[0] < prev) monotonic = 0; prev = g.cur[0]; }
    CHECK(monotonic && g.cur[0] == 1.0f);
}

// ------------------------------------------------------------- MIDI -> CV
static void midi3(mcv_t *m, uint8_t a, uint8_t b, uint8_t c) { uint8_t x[3] = {a, b, c}; mcv_midi(m, x, 3); }

static void test_mapper(void) {
    cv_cal_t c; cal_defaults(&c);
    mcv_cfg_t cfg; mcv_default_cfg(&cfg);
    mcv_t m; mcv_init(&m, &cfg);
    float v;
    // 1 V/oct through the precision output calibration: every note within 0.2 mV (0.25 cent).
    double worst = 0;
    for (int note = 0; note <= 127; note++) {
        midi3(&m, 0x90, (uint8_t)note, 100);
        CHECK(mcv_out(&m, 0, &v));
        float target = (note - 48) / 12.0f;
        if (target < -10.0f || target > 10.0f) { midi3(&m, 0x80, (uint8_t)note, 0); continue; }
        int32_t code = cal_volts_to_code(&c, CV_P_OUT0 + 0, v);
        double err = fabs(cal_code_to_volts(&c, CV_P_OUT0, code) - target);
        if (err > worst) worst = err;
        midi3(&m, 0x80, (uint8_t)note, 0);
    }
    CHECK(worst < 0.0002);
    printf("mapper: worst 1 V/oct error %.3f mV (%.3f cent)\n", worst * 1e3, worst * 1200);
    // Pitch bend: full up = +bend_range semitones, centre = 0.
    midi3(&m, 0x90, 60, 100);
    midi3(&m, 0xE0, 0x7F, 0x7F); mcv_out(&m, 0, &v); NEAR(v, (12 + 2 * 8191.0 / 8192) / 12.0, 1e-5);
    midi3(&m, 0xE0, 0x00, 0x40); mcv_out(&m, 0, &v); NEAR(v, 1.0, 1e-6);
    midi3(&m, 0xE0, 0x00, 0x00); mcv_out(&m, 0, &v); NEAR(v, (12 - 2) / 12.0, 1e-6);
    midi3(&m, 0xE0, 0x00, 0x40);
    // Last-note priority with fallback to the previous held note.
    midi3(&m, 0x90, 64, 90); mcv_out(&m, 0, &v); NEAR(v, 16 / 12.0, 1e-6);
    midi3(&m, 0x90, 67, 80); mcv_out(&m, 0, &v); NEAR(v, 19 / 12.0, 1e-6);
    midi3(&m, 0x80, 67, 0);  mcv_out(&m, 0, &v); NEAR(v, 16 / 12.0, 1e-6);
    mcv_tick_ms(&m, 5); mcv_out(&m, 1, &v); CHECK(v == 5.0f);   // gate still high (after the retrigger gap)
    midi3(&m, 0x80, 64, 0); midi3(&m, 0x90, 60, 0);             // note-on vel 0 = off
    mcv_out(&m, 1, &v); CHECK(v == 0.0f);
    mcv_out(&m, 0, &v); NEAR(v, 1.0, 1e-6);                     // pitch holds after release
    // Retrigger (legato off): overlapping note drops the gate for retrig_ms.
    midi3(&m, 0x90, 60, 100); midi3(&m, 0x90, 62, 100);
    mcv_out(&m, 1, &v); CHECK(v == 0.0f);
    mcv_tick_ms(&m, 1); mcv_out(&m, 1, &v); CHECK(v == 0.0f);
    mcv_tick_ms(&m, 1); mcv_out(&m, 1, &v); CHECK(v == 5.0f);
    mcv_all_off(&m);
    // Legato on: no gate drop.
    cfg.legato = true; mcv_init(&m, &cfg);
    midi3(&m, 0x90, 60, 100); midi3(&m, 0x90, 62, 100);
    mcv_out(&m, 1, &v); CHECK(v == 5.0f);
    // Velocity and CC maps; channel filter.
    mcv_out(&m, 2, &v); NEAR(v, 100 / 127.0 * 8.0, 1e-5);
    midi3(&m, 0xB0, 1, 127); mcv_out(&m, 3, &v); NEAR(v, 10.0, 1e-6);
    midi3(&m, 0xB0, 74, 0);  mcv_out(&m, 4, &v); NEAR(v, 0.0, 1e-6);
    CHECK(!mcv_out(&m, 7, &v));
    cfg.midi_ch = 2; mcv_init(&m, &cfg);
    midi3(&m, 0x90, 72, 100); CHECK(m.note == -1);
    midi3(&m, 0x92, 72, 100); CHECK(m.note == 72);
    midi3(&m, 0xB2, 123, 0); mcv_out(&m, 1, &v); CHECK(v == 0.0f);   // all notes off
    // Stack overflow keeps the newest notes.
    mcv_default_cfg(&cfg); mcv_init(&m, &cfg);
    for (int i = 0; i < 20; i++) midi3(&m, 0x90, (uint8_t)(40 + i), 100);
    CHECK(m.n_held == MCV_STACK && m.note == 59);
}

// ------------------------------------------------------------- BLE-MIDI codec
static uint8_t got[16][BLEMIDI_SYSEX_MAX]; static int got_len[16], n_got;
static void emit(void *u, const uint8_t *msg, int len, uint16_t ts) {
    (void)u; (void)ts;
    if (n_got < 16) { memcpy(got[n_got], msg, (size_t)len); got_len[n_got++] = len; }
}
static uint8_t pk[8][64]; static size_t pk_len[8]; static int n_pk;
static void out(void *u, const uint8_t *p, size_t n) { (void)u; if (n_pk < 8) { memcpy(pk[n_pk], p, n); pk_len[n_pk++] = n; } }

static void test_blemidi(void) {
    blemidi_rx_t r; blemidi_rx_init(&r);
    // Note on, running-status note on (no timestamp), timestamped note off, real-time clock.
    const uint8_t p1[] = {0x80, 0x81, 0x90, 60, 100, 62, 90, 0x82, 0x80, 60, 0, 0x83, 0xF8};
    n_got = 0; blemidi_rx_packet(&r, p1, sizeof p1, emit, NULL);
    CHECK(n_got == 4);
    CHECK(got_len[0] == 3 && got[0][0] == 0x90 && got[0][1] == 60);
    CHECK(got_len[1] == 3 && got[1][0] == 0x90 && got[1][1] == 62);
    CHECK(got[2][0] == 0x80 && got_len[3] == 1 && got[3][0] == 0xF8);
    // SysEx across two packets with a real-time byte inside.
    const uint8_t s1[] = {0x80, 0x81, 0xF0, 0x7D, 0x43, 0x56};
    const uint8_t s2[] = {0x80, 0x01, 0x82, 0xF8, 0x02, 0x83, 0xF7};
    n_got = 0;
    blemidi_rx_packet(&r, s1, sizeof s1, emit, NULL);
    blemidi_rx_packet(&r, s2, sizeof s2, emit, NULL);
    CHECK(n_got == 2 && got[0][0] == 0xF8);
    CHECK(got_len[1] == 7 && got[1][0] == 0xF0 && got[1][4] == 0x01 && got[1][5] == 0x02 && got[1][6] == 0xF7);
    // Bad header rejected.
    const uint8_t bad[] = {0x40, 0x90, 1, 2};
    blemidi_rx_packet(&r, bad, sizeof bad, emit, NULL); CHECK(r.bad_packets == 1);
    // Encode -> decode round trip, SysEx split over a 20-byte MTU.
    uint8_t sx[40]; sx[0] = 0xF0; for (int i = 1; i < 39; i++) sx[i] = (uint8_t)i; sx[39] = 0xF7;
    n_pk = 0;
    int np = blemidi_encode(sx, sizeof sx, 1234, 20, out, NULL);
    CHECK(np == n_pk && np >= 3);
    for (int i = 0; i < n_pk; i++) CHECK(pk_len[i] <= 20);
    blemidi_rx_init(&r); n_got = 0;
    for (int i = 0; i < n_pk; i++) blemidi_rx_packet(&r, pk[i], pk_len[i], emit, NULL);
    CHECK(n_got == 1 && got_len[0] == 40 && memcmp(got[0], sx, 40) == 0);
    const uint8_t cc[3] = {0xB3, 74, 64};
    n_pk = 0; blemidi_encode(cc, 3, 5, 20, out, NULL);
    n_got = 0; blemidi_rx_packet(&r, pk[0], pk_len[0], emit, NULL);
    CHECK(n_got == 1 && memcmp(got[0], cc, 3) == 0);
    CHECK(midi_msg_len(0xC0) == 2 && midi_msg_len(0xF0) == 0 && midi_msg_len(0x40) == -1);
}

// ------------------------------------------------------------- power supervision
static void test_power(void) {
    CHECK(power_batt_percent(0) == -1 && power_batt_percent(4250) == 100 && power_batt_percent(3200) == 0);
    CHECK(power_batt_percent(3850) == 50);
    pwr_state_t s; power_init(&s);
    pwr_in_t in = {.vbus = true, .rail_mv = 11000, .batt_mv = 4000};
    CHECK(s.mute);
    for (int t = 0; t < 9; t++) power_update(&s, &in, 10);
    CHECK(s.mute);                                  // not yet stable for 100 ms
    power_update(&s, &in, 10); CHECK(!s.mute && s.src == PWR_SRC_USB);
    in.rail_mv = 9000; power_update(&s, &in, 10); CHECK(s.mute);   // dip mutes at once
    in.rail_mv = 10400; for (int t = 0; t < 20; t++) power_update(&s, &in, 10); CHECK(s.mute);  // below threshold
    in.rail_mv = 11000; for (int t = 0; t < 10; t++) power_update(&s, &in, 10); CHECK(!s.mute);
    in.rail_mv = 10300; power_update(&s, &in, 10); CHECK(!s.mute);   // hysteresis
    // Battery source: low warning, then critical shutdown after 2 s.
    in.vbus = false; in.rack = false; in.batt_mv = 3400; in.rail_mv = 11000;
    power_update(&s, &in, 10); CHECK(s.src == PWR_SRC_BATTERY && s.low_battery && !s.shutdown);
    in.batt_mv = 3250;
    for (int t = 0; t < 199; t++) power_update(&s, &in, 10);
    CHECK(!s.shutdown);
    power_update(&s, &in, 10); CHECK(s.shutdown && s.mute);
    // Plugging USB back cancels shutdown; display dims after 30 s idle.
    in.vbus = true; in.batt_mv = 3700; power_update(&s, &in, 10); CHECK(!s.shutdown);
    for (int t = 0; t < 3000; t++) power_update(&s, &in, 10);
    CHECK(s.backlight == 40);
    in.user_activity = true; power_update(&s, &in, 10); CHECK(s.backlight == 255);
}

// ------------------------------------------------------------- SysEx protocol
static void test_proto(void) {
    cv_cal_t c; cal_defaults(&c);
    out_engine_t a, p; out_engine_init(&a, 8, 0, CV_AUDIO_RATE); out_engine_init(&p, 8, CV_P_OUT0, CV_P_RATE);
    in_measure_t m; memset(&m, 0, sizeof m);
    cv_glide_t g; cv_glide_init(&g, 4);
    proto_ctx_t ctx = {.cal = &c, .out_a = &a, .out_p = &p, .meas = &m, .cv = &g, .flags = 0x1B, .batt_pct = 77};
    uint8_t msg[PROTO_MAX_MSG], rep[PROTO_MAX_MSG], pl[64];
    size_t n, r, k;

    uint8_t b[5];
    proto_put_u32(b, 0xDEADBEEFu); CHECK(proto_get_u32(b) == 0xDEADBEEFu);
    for (float v = -10.48f; v <= 10.48f; v += 0.731f) { proto_put_cv(b, v); NEAR(proto_get_cv(b), v, 6e-6); }
    proto_put_cv(b, 20.0f); NEAR(proto_get_cv(b), 10.48575, 1e-5);

    n = proto_build(P_GET_INFO, NULL, 0, msg, sizeof msg);
    r = proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(r == 6 + 14 && rep[4] == 0x41 && rep[7] == 8 && rep[8] == 6 && rep[9] == 8 && rep[10] == 8 && rep[11] == 0x1B && rep[13] == 77);
    CHECK(ctx.host_seen);
    // SET_CAL on a precision output, GET_CAL reads it back.
    k = 0; pl[k++] = 0; pl[k++] = 9; k += proto_put_f32(pl + k, -3300.0f); k += proto_put_f32(pl + k, 32800.0f);
    n = proto_build(P_SET_CAL, pl, k, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[4] == P_ACK && rep[6] == P_OK && c.out[9].gain == -3300.0f && cal_valid(&c));
    uint8_t q[2] = {0, 9};
    n = proto_build(P_GET_CAL, q, 2, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[4] == 0x42 && proto_get_f32(rep + 7) == -3300.0f);
    q[1] = 7; q[0] = 1;   // input slot 7 does not exist
    n = proto_build(P_GET_CAL, q, 2, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[6] == P_BAD_ARGS);
    // RAW_OUT: negative codec code allowed, out-of-range DAC code refused.
    k = 0; pl[k++] = 2; pl[k++] = 1; k += proto_put_u32(pl + k, (uint32_t)-500000);
    n = proto_build(P_RAW_OUT, pl, k, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(a.override_on[2] && a.raw_override[2] == -500000);
    k = 0; pl[k++] = 10; pl[k++] = 1; k += proto_put_u32(pl + k, 70000);
    n = proto_build(P_RAW_OUT, pl, k, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[6] == P_BAD_ARGS && !p.override_on[2]);
    // CV_OUT frame: masked targets, no reply.
    k = 0; pl[k++] = 0x05; pl[k++] = 0x01;          // ch 0, 2, 7
    for (int i = 0; i < 8; i++) k += proto_put_cv(pl + k, (float)i - 3.5f);
    n = proto_build(P_CV_OUT, pl, k, msg, sizeof msg);
    CHECK(n == 32 && proto_handle(&ctx, msg, n, rep, sizeof rep) == 0);
    NEAR(g.target[0], -3.5, 1e-5); NEAR(g.target[2], -1.5, 1e-5); NEAR(g.target[7], 3.5, 1e-5); CHECK(g.target[1] == 0.0f);
    // CV_IN rate, CV_SRC, MAP_CFG.
    k = proto_put_u32(pl, 500);
    n = proto_build(P_CV_IN, pl, k, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(ctx.cv_in_rate == 500);
    k = proto_put_u32(pl, 5000);
    n = proto_build(P_CV_IN, pl, k, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[6] == P_BAD_ARGS && ctx.cv_in_rate == 500);
    uint8_t src[8] = {2, 2, 2, 1, 1, 1, 0, 0};
    n = proto_build(P_CV_SRC, src, 8, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(ctx.p_src[0] == SRC_MAPPER && ctx.p_src[3] == SRC_HOST);
    uint8_t mc[22] = {16, 1, 2, 3, 48, 12, 1, 3, 10, 8, 1, 4, 0, 74, 5, 1, 0, 0, 0, 0, 0, 0};
    n = proto_build(P_MAP_CFG, mc, 22, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(rep[6] == P_OK && ctx.map_dirty && ctx.map_cfg.bend_range == 12 && ctx.map_cfg.legato &&
          ctx.map_cfg.gate_volts == 10.0f && ctx.map_cfg.cc[1].bipolar && ctx.map_cfg.cc[2].out == -1);
    // CV_IN frame and MEASURE reply sizes fit a single USB-MIDI SysEx and PROTO_MAX_MSG.
    float vin[8] = {1, -1, 2, -2, 3, -3, 10, -10};
    r = proto_cv_in_frame(vin, rep, sizeof rep);
    CHECK(r == 30 && rep[4] == 0x51); NEAR(proto_get_cv(rep + 5 + 18), 10.0, 1e-5);
    k = proto_put_u32(pl, 100);
    n = proto_build(P_MEASURE, pl, k, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep);
    CHECK(m.active && m.target == 100);
    r = proto_measure_reply(&ctx, rep, sizeof rep); CHECK(r == 6 + 5 * CV_MAX_IN && r <= PROTO_MAX_MSG);
    n = proto_build(P_DFU, NULL, 0, msg, sizeof msg); proto_handle(&ctx, msg, n, rep, sizeof rep); CHECK(ctx.req_dfu);
    uint8_t foreign[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    CHECK(proto_handle(&ctx, foreign, sizeof foreign, rep, sizeof rep) == 0);
}

int main(void) {
#ifdef CV_TIER_A
    test_tiera();
    test_blemidi();
    test_power();
    printf("tier A tests: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
#endif
    test_descriptors();
    test_codec();
    test_cal();
    test_engine();
    test_mapper();
    test_blemidi();
    test_power();
    test_proto();
    printf("host tests: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
