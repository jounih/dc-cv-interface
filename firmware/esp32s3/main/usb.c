// TinyUSB device: UAC2 (group A) + USB-MIDI (SysEx control, precision CV path, MIDI->CV).
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/usb_phy.h"
#include "esp_mac.h"
#include "tusb.h"
#include "board.h"
#include "app.h"
#include "usb_desc.h"
#include "ble_midi.h"

static usb_phy_handle_t phy;
static SemaphoreHandle_t tx_lock;

static void usb_task(void *arg) {
    (void)arg;
    usb_phy_config_t pc = {
        .controller = USB_PHY_CTRL_OTG, .target = USB_PHY_TARGET_INT, .otg_mode = USB_OTG_MODE_DEVICE,
    };
    ESP_ERROR_CHECK(usb_new_phy(&pc, &phy));
    tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    tusb_init(0, &init);
    static uint8_t sx[PROTO_MAX_MSG];
    static int sx_len;
    for (;;) {
        tud_task_ext(1, false);                     // 1 ms timeout keeps MIDI responsive
        uint8_t pk[4];
        while (tud_midi_available() && tud_midi_packet_read(pk)) {
            uint8_t cin = pk[0] & 0x0F;
            switch (cin) {
            case 0x4:                                 // SysEx start/continue, 3 bytes
                if (pk[1] == 0xF0) sx_len = 0;
                for (int i = 1; i <= 3; i++) if (sx_len < (int)sizeof sx) sx[sx_len++] = pk[i];
                break;
            case 0x5: case 0x6: case 0x7: {           // SysEx end with 1/2/3 bytes (0x5 also 1-byte system common)
                int n = cin - 0x4;
                if (cin == 0x5 && pk[1] != 0xF7 && pk[1] >= 0xF0 && sx_len == 0) { midi_router_rx(TRANSPORT_USB, &pk[1], 1); break; }
                for (int i = 1; i <= n; i++) if (sx_len < (int)sizeof sx) sx[sx_len++] = pk[i];
                if (sx_len >= 2 && sx[0] == 0xF0 && sx[sx_len - 1] == 0xF7) midi_router_rx(TRANSPORT_USB, sx, sx_len);
                sx_len = 0;
                break;
            }
            case 0x8: case 0x9: case 0xA: case 0xB: case 0xE:
                midi_router_rx(TRANSPORT_USB, &pk[1], 3);
                break;
            case 0xC: case 0xD:
                midi_router_rx(TRANSPORT_USB, &pk[1], 2);
                break;
            case 0xF:
                midi_router_rx(TRANSPORT_USB, &pk[1], 1);
                break;
            default:
                break;
            }
        }
    }
}

void usb_start(void) {
    tx_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(usb_task, "usb", 6144, NULL, configMAX_PRIORITIES - 3, NULL, CORE_RT);
}

void usb_midi_send(const uint8_t *msg, size_t n) {
    if (!n || !tud_midi_mounted()) return;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    tud_midi_stream_write(0, msg, (uint32_t)n);
    xSemaphoreGive(tx_lock);
}

// ------------------------------------------------------------------ device callbacks
void tud_mount_cb(void) { g_app.usb_mounted = true; }
void tud_umount_cb(void) {
    g_app.usb_mounted = g_app.out_streaming = g_app.in_streaming = false;
    for (int i = 0; i < ENG_MAX_CH; i++) g_app.out_a.override_on[i] = g_app.out_p.override_on[i] = false;
}
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; g_app.out_streaming = g_app.in_streaming = false; }
void tud_resume_cb(void) {}

uint8_t const *tud_descriptor_device_cb(void) { return cv_desc_device; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return cv_desc_config; }

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t d[40];
    static char serial[13];
    size_t n;
    if (index == STR_LANG) { d[1] = 0x0409; n = 1; }
    else {
        const char *s = cv_string(index);
        if (index == STR_SERIAL) {
            uint8_t mac[6];
            esp_efuse_mac_get_default(mac);
            for (int i = 0; i < 6; i++) { static const char hx[] = "0123456789ABCDEF"; serial[2 * i] = hx[mac[i] >> 4]; serial[2 * i + 1] = hx[mac[i] & 15]; }
            serial[12] = 0;
            s = serial;
        }
        if (!s) return NULL;
        n = strlen(s);
        if (n > 39) n = 39;
        for (size_t i = 0; i < n; i++) d[1 + i] = (uint16_t)s[i];
    }
    d[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return d;
}

// ------------------------------------------------------------------ UAC2 callbacks
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *r = (audio_control_request_t const *)p_request;
    if (r->bEntityID != UAC2_ENTITY_CLOCK) return false;
    if (r->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
        if (r->bRequest == AUDIO20_CS_REQ_CUR) {
            audio20_control_cur_4_t cur = {(int32_t)tu_htole32(CV_AUDIO_RATE)};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof cur);
        }
        if (r->bRequest == AUDIO20_CS_REQ_RANGE) {
            audio20_control_range_4_n_t(1) range = {.wNumSubRanges = tu_htole16(1)};
            range.subrange[0].bMin = (int32_t)CV_AUDIO_RATE;
            range.subrange[0].bMax = (int32_t)CV_AUDIO_RATE;
            range.subrange[0].bRes = 0;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof range);
        }
    } else if (r->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID && r->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof valid);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport;
    audio_control_request_t const *r = (audio_control_request_t const *)p_request;
    if (r->bEntityID != UAC2_ENTITY_CLOCK || r->bControlSelector != AUDIO20_CS_CTRL_SAM_FREQ ||
        r->bRequest != AUDIO20_CS_REQ_CUR || r->wLength != sizeof(audio20_control_cur_4_t))
        return false;
    return (uint32_t)((audio20_control_cur_4_t const *)buf)->bCur == CV_AUDIO_RATE;
}

static void set_alt(uint8_t itf, uint8_t alt) {
    if (itf == ITF_AS_OUT) g_app.out_streaming = alt != 0;
    if (itf == ITF_AS_IN) g_app.in_streaming = alt != 0;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    set_alt(tu_u16_low(tu_le16toh(p_request->wIndex)), tu_u16_low(tu_le16toh(p_request->wValue)));
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    set_alt(tu_u16_low(tu_le16toh(p_request->wIndex)), 0);
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *fb) {
    (void)func_id; (void)alt_itf;
    fb->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    fb->sample_freq = CV_AUDIO_RATE;
}
