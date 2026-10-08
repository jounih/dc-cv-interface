// MIDI from USB and BLE: SysEx -> protocol (status, calibration, precision CV path),
// channel messages -> MIDI->CV mapper. Runs on core 0; the RT tasks only read results.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_private/system_internal.h"
#include "app.h"

static transport_t last_sysex_from = TRANSPORT_USB;
static uint32_t last_cv_in_ms;

static void send(transport_t t, const uint8_t *m, size_t n) {
    if (!n) return;
    if (t == TRANSPORT_USB) usb_midi_send(m, n); else ble_midi_send(m, n);
}

void midi_router_init(void) {}

void midi_router_rx(transport_t t, const uint8_t *msg, int len) {
    if (len <= 0) return;
    if (msg[0] == 0xF0) {
        uint8_t reply[PROTO_MAX_MSG];
        app_lock();
        g_app.proto.flags = (uint8_t)((g_app.cal_from_flash ? 1 : 0) | (g_app.out_streaming ? 2 : 0) |
                                      (g_app.in_streaming ? 4 : 0) | (g_app.codec_ok ? 8 : 0) |
                                      (g_app.precision_ok ? 16 : 0) | (g_app.ble_connected ? 32 : 0) | 64);
        g_app.proto.src = (uint8_t)g_app.pwr.src;
        g_app.proto.batt_pct = (uint8_t)(g_app.pwr.batt_pct < 0 ? 127 : g_app.pwr.batt_pct);
        g_app.proto.underruns = g_app.usb_underruns + g_app.out_a.underruns;
        g_app.proto.host_seen = false;
        size_t n = proto_handle(&g_app.proto, msg, (size_t)len, reply, sizeof reply);
        if (g_app.proto.host_seen) g_app.host_last_ms = app_ms();
        app_unlock();
        last_sysex_from = t;
        send(t, reply, n);
        return;
    }
    app_lock();
    mcv_midi(&g_app.mapper, msg, len);
    app_unlock();
}

static void enter_rom_download(void) {
    // Same mechanism esp_tinyusb uses: force the ROM USB downloader on the next boot.
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void midi_router_poll(void) {
    uint8_t buf[PROTO_MAX_MSG];
    size_t n = 0;
    bool save = false, dfu = false;
    app_lock();
    if (g_app.meas.done && g_app.meas.active) {
        n = proto_measure_reply(&g_app.proto, buf, sizeof buf);
        g_app.meas.active = false;
    }
    if (g_app.proto.map_dirty) {
        mcv_init(&g_app.mapper, &g_app.proto.map_cfg);
        g_app.proto.map_dirty = false;
    }
    save = g_app.proto.req_save; g_app.proto.req_save = false;
    dfu = g_app.proto.req_dfu;
    uint32_t rate = g_app.proto.cv_in_rate;
    app_unlock();
    send(last_sysex_from, buf, n);

    if (rate && g_app.usb_mounted) {
        uint32_t now = app_ms();
        if (now - last_cv_in_ms >= 1000u / rate) {
            last_cv_in_ms = now;
            float v[CV_P_IN];
            for (int i = 0; i < CV_P_IN; i++) v[i] = g_app.meter_in[CV_P_IN0 + i];
            usb_midi_send(buf, proto_cv_in_frame(v, buf, sizeof buf));
        }
    }
    if (save) {
        cv_cal_t c;
        app_lock(); c = g_app.cal; app_unlock();
        g_app.cal_from_flash = storage_save_cal(&c);
    }
    if (dfu) {
        // Park every output at 0 V, let the ack go out, then reboot into the ROM downloader.
        g_app.out_a.force_zero = g_app.out_p.force_zero = true;
        vTaskDelay(pdMS_TO_TICKS(50));
        enter_rom_download();
    }
}
