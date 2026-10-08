// DC-coupled USB Audio Class 2 CV interface for Circuit Studio.
// Pico 2 (RP2350) + DAC8568 (outputs) + ADS131M04 (inputs) + TinyUSB.
//
// Data flow (all on core 0, no per-sample interrupts):
//   USB OUT iso -> TinyUSB FIFO (FIFO-count async feedback) -> out_engine
//     -> DAC frame ring -> DMA (timer-paced, CV_N_OUT x 48 kHz) -> PIO -> DAC8568
//   ADS131M04 DRDY -> PIO frame reader -> DMA ring -> in_engine
//     -> TinyUSB IN FIFO (flow-controlled 47/48/49-sample packets) -> USB IN iso
// Both converter clocks derive from the 12 MHz crystal (150 MHz sysclk), so the
// DAC and ADC run at exactly the same 48 kHz; the host is slaved by feedback.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "tusb.h"
#include "board.h"
#include "cv_config.h"
#include "conv_codec.h"
#include "converters.h"
#include "cal.h"
#include "cal_flash.h"
#include "engine.h"
#include "proto.h"
#include "usb_desc.h"
#include "quirk_os_guessing.h"

#define DAC_TARGET 72u   // samples kept queued ahead of the DMA (1.5 ms)
#define DAC_GUARD  8u
#define DAC_HOLD_AHEAD 96u

static cv_cal_t cal;
static out_engine_t out_eng;
static in_measure_t meas;
static proto_ctx_t proto = {.cal = &cal, .out = &out_eng, .meas = &meas};

static volatile bool out_streaming, in_streaming;
static bool out_primed, adc_ok, cal_from_flash;
static uint32_t dac_wr, adc_rd;

#define OUT_FRAME_BYTES (CV_N_OUT * CV_OUT_BYTES)
#define IN_FRAME_BYTES  (CV_N_IN * CV_IN_BYTES)

// ------------------------------------------------------------------ audio
static void write_dac_sample(uint32_t slot, const uint16_t *codes) {
    dac8568_encode_sample(codes, CV_N_OUT, &dac_ring[slot * CV_N_OUT]);
}

static void dac_service(uint32_t *produced) {
    static int16_t usb_buf[CONV_RING * CV_N_OUT];
    bool underrun;
    uint32_t n = ring_to_write(&dac_wr, conv_dac_hw_sample(), CONV_RING, DAC_TARGET, DAC_GUARD, &underrun);
    *produced = n;
    if (!n) return;

    uint32_t have = 0;
    if (out_streaming) {
        uint32_t avail = tud_audio_available();
        if (!out_primed && avail >= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2) out_primed = true;
        if (out_primed) {
            have = avail / OUT_FRAME_BYTES;
            if (have > n) have = n;
            if (have) tud_audio_read(usb_buf, (uint16_t)(have * OUT_FRAME_BYTES));
        }
    }
    uint16_t codes[CV_MAX_OUT];
    for (uint32_t i = 0; i < n; i++) {
        out_engine_step(&out_eng, &cal, i < have ? &usb_buf[i * CV_N_OUT] : NULL, codes);
        write_dac_sample(dac_wr, codes);
        dac_wr = (dac_wr + 1) % CONV_RING;
    }
    // Fill a stretch beyond the write point with the last value, so a late
    // service call repeats the last sample instead of replaying stale audio.
    for (uint32_t i = 0, s = dac_wr; i < DAC_HOLD_AHEAD; i++, s = (s + 1) % CONV_RING)
        write_dac_sample(s, codes);
}

static void adc_service(uint32_t produced_if_absent) {
    static uint8_t in_buf[64 * IN_FRAME_BYTES];
    uint32_t count = 0;
    int32_t raw[CV_MAX_IN] = {0};

    if (adc_ok) {
        uint32_t hw = conv_adc_hw_sample();
        while (adc_rd != hw) {
            const uint32_t *f = &adc_ring[adc_rd * 6];
            for (int c = 0; c < CV_N_IN; c++) raw[c] = ads_sext24(f[1 + c]);
            in_measure_add(&meas, raw);
            in_engine_step(&cal, raw, &in_buf[count * IN_FRAME_BYTES]);
            adc_rd = (adc_rd + 1) % CONV_RING;
            if (++count == 64) {
                if (in_streaming) tud_audio_write(in_buf, (uint16_t)(count * IN_FRAME_BYTES));
                count = 0;
            }
        }
    } else {
        // No ADC fitted: keep the IN stream alive with silence at the DAC rate.
        for (uint32_t i = 0; i < produced_if_absent; i++) {
            in_engine_step(&cal, raw, &in_buf[count * IN_FRAME_BYTES]);
            if (++count == 64) {
                if (in_streaming) tud_audio_write(in_buf, (uint16_t)(count * IN_FRAME_BYTES));
                count = 0;
            }
        }
    }
    if (count && in_streaming) tud_audio_write(in_buf, (uint16_t)(count * IN_FRAME_BYTES));
}

// ------------------------------------------------------------------ MIDI / SysEx
static void midi_send(const uint8_t *msg, size_t n) {
    if (n && tud_midi_mounted()) tud_midi_stream_write(0, msg, (uint32_t)n);
}

static void midi_service(void) {
    static uint8_t sx[PROTO_MAX_MSG];
    static size_t sx_len;
    static bool in_sx;
    uint8_t buf[64], reply[PROTO_MAX_MSG];

    while (tud_midi_available()) {
        uint32_t n = tud_midi_stream_read(buf, sizeof buf);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (b == 0xF0) { in_sx = true; sx_len = 0; }
            if (!in_sx) continue;
            if (sx_len < sizeof sx) sx[sx_len++] = b; else in_sx = false;   // overlong: drop
            if (b == 0xF7) {
                in_sx = false;
                proto.flags = (uint8_t)((cal_from_flash ? 1 : 0) | (out_streaming ? 2 : 0) |
                                        (in_streaming ? 4 : 0) | (adc_ok ? 8 : 0));
                midi_send(reply, proto_handle(&proto, sx, sx_len, reply, sizeof reply));
            }
        }
    }
    if (meas.done && meas.target) {
        midi_send(reply, proto_measure_reply(&proto, reply, sizeof reply));
        meas.target = 0;
    }
    if (proto.req_save) {
        proto.req_save = false;
        watchdog_update();
        cal_from_flash = cal_flash_save(&cal);
    }
    if (proto.req_bootsel) {
        // The bootloader leaves the DAC alone, so park every output at 0 V first.
        uint16_t zero[CV_MAX_OUT];
        out_zero_codes(&cal, zero);
        for (uint32_t s = 0; s < CONV_RING; s++) write_dac_sample(s, zero);
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        while (to_ms_since_boot(get_absolute_time()) - t0 < 20) tud_task();   // ring replays 0 V, ack goes out
        reset_usb_boot(0, 0);
    }
}

// ------------------------------------------------------------------ main
static void led_service(void) {
    static uint32_t last;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t period = !tud_mounted() ? 1000 : (out_streaming || in_streaming) ? 0 : 250;
    if (!adc_ok && tud_mounted()) period = 100;   // fast blink: ADC missing
    if (period == 0) { gpio_put(PIN_LED, 1); return; }
    if (now - last >= period) { last = now; gpio_xor_mask(1u << PIN_LED); }
}

int main(void) {
    // 1) Calibration first: the 0 V codes depend on it.
    cal_from_flash = cal_flash_load(&cal);
    if (!cal_from_flash) cal_defaults(&cal);
    out_engine_init(&out_eng);

    // 2) Outputs to 0 V before anything else (reset -> codes -> reference on).
    uint16_t zero[CV_MAX_OUT];
    out_zero_codes(&cal, zero);
    conv_dac_init(zero);
    for (uint32_t s = 0; s < CONV_RING; s++) write_dac_sample(s, zero);
    dac_wr = DAC_TARGET;
    conv_dac_start();

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);

    // 3) Inputs.
    uint16_t id = 0;
    adc_ok = conv_adc_init(&id);
    if (adc_ok) conv_adc_start();

    // 4) USB.
    tud_init(BOARD_TUD_RHPORT);

    // 5) Watchdog: a hung loop reboots, and the boot path above returns the
    //    outputs to 0 V (the DAC is soft-reset with its reference off).
    watchdog_enable(200, true);

    for (;;) {
        tud_task();
        uint32_t produced;
        dac_service(&produced);
        adc_service(produced);
        midi_service();
        led_service();
        watchdog_update();
    }
}

// ------------------------------------------------------------------ USB callbacks
static void stream_stop_all(void) {
    out_streaming = in_streaming = false;
    out_primed = false;
    for (int i = 0; i < CV_MAX_OUT; i++) out_eng.raw_override[i] = -1;
}

void tud_mount_cb(void) {}
void tud_umount_cb(void) { stream_stop_all(); }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; stream_stop_all(); }
void tud_resume_cb(void) {}

uint8_t const *tud_descriptor_device_cb(void) {
    quirk_os_guessing_desc_device_cb();
    return cv_desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    quirk_os_guessing_desc_configuration_cb();
    if (tud_speed_get() == TUSB_SPEED_FULL && quirk_os_guessing_get() == QUIRK_OS_GUESSING_OSX)
        return cv_desc_config_fb3;
    return cv_desc_config;
}

uint8_t const *tud_descriptor_bos_cb(void) {
    quirk_os_guessing_desc_bos_cb();
    return cv_desc_bos;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t d[40];
    static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    size_t n;
    quirk_os_guessing_desc_string_cb();
    if (index == STR_LANG) {
        d[1] = 0x0409;
        n = 1;
    } else {
        const char *s = cv_string(index);
        if (index == STR_SERIAL) { pico_get_unique_board_id_string(serial, sizeof serial); s = serial; }
        if (!s) return NULL;
        n = strlen(s);
        if (n > 39) n = 39;
        for (size_t i = 0; i < n; i++) d[1 + i] = (uint16_t)s[i];
    }
    d[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return d;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *r = (audio_control_request_t const *)p_request;
    if (r->bEntityID != UAC2_ENTITY_CLOCK) return false;
    if (r->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (r->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t cur = {(int32_t)tu_htole32(CV_SAMPLE_RATE)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof cur);
        }
        if (r->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(1) range = {.wNumSubRanges = tu_htole16(1)};
            range.subrange[0].bMin = (int32_t)CV_SAMPLE_RATE;
            range.subrange[0].bMax = (int32_t)CV_SAMPLE_RATE;
            range.subrange[0].bRes = 0;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof range);
        }
    } else if (r->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && r->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof valid);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport;
    audio_control_request_t const *r = (audio_control_request_t const *)p_request;
    if (r->bEntityID != UAC2_ENTITY_CLOCK || r->bControlSelector != AUDIO_CS_CTRL_SAM_FREQ ||
        r->bRequest != AUDIO_CS_REQ_CUR || r->wLength != sizeof(audio_control_cur_4_t))
        return false;
    // Fixed clock: accept only 48 kHz.
    return (uint32_t)((audio_control_cur_4_t const *)buf)->bCur == CV_SAMPLE_RATE;
}

static void set_alt(uint8_t itf, uint8_t alt) {
    if (itf == ITF_AS_OUT) { out_streaming = alt != 0; out_primed = false; }
    if (itf == ITF_AS_IN) in_streaming = alt != 0;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    set_alt(tu_u16_low(tu_le16toh(p_request->wIndex)), tu_u16_low(tu_le16toh(p_request->wValue)));
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    set_alt(tu_u16_low(tu_le16toh(p_request->wIndex)), 0);
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *fb) {
    (void)func_id; (void)alt_itf;
    fb->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    fb->sample_freq = CV_SAMPLE_RATE;
}

bool tud_audio_feedback_format_correction_cb(uint8_t func_id) {
    (void)func_id;
    return tud_speed_get() == TUSB_SPEED_FULL && quirk_os_guessing_get() == QUIRK_OS_GUESSING_OSX;
}
