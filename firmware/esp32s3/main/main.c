// Circuit Studio CV interface, ESP32-S3 (LilyGO T-Display-S3) firmware.
//
//  Group A (audio-rate, DC-coupled): PCM3168A over I2S TDM <-> USB Audio Class 2, 32 kHz, 8 out / 6 in.
//  Group P (precision CV): DAC8568 + ADS131M08 over SPI at 2 kHz <-> SysEx CV frames (Web MIDI)
//                          and the MIDI->CV mapper (USB-MIDI + BLE-MIDI).
//  Core 1: TinyUSB, codec I/O, precision CV.  Core 0: BLE, display, power, MIDI router.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "board.h"
#include "app.h"

app_t g_app;
static const char *TAG = "cv";

uint32_t app_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void router_task(void *arg) {
    (void)arg;
    for (;;) {
        midi_router_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void app_main(void) {
    // Board peripheral power (LCD) and the carrier's 3.3 V parts come up with the board.
    gpio_set_direction(PIN_POWER_ON, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_POWER_ON, 1);

    g_app.lock = xSemaphoreCreateMutex();
    storage_init();
    g_app.cal_from_flash = storage_load_cal(&g_app.cal);
    if (!g_app.cal_from_flash) cal_defaults(&g_app.cal);

    out_engine_init(&g_app.out_a, CV_A_OUT, 0, CV_AUDIO_RATE);
    out_engine_init(&g_app.out_p, CV_P_OUT, CV_P_OUT0, CV_P_RATE);
    g_app.out_a.force_zero = g_app.out_p.force_zero = true;   // until the rails are confirmed
    cv_glide_init(&g_app.cv, CV_P_RATE / 500);                 // 2 ms glide

    mcv_cfg_t mc;
    mcv_default_cfg(&mc);
    mcv_init(&g_app.mapper, &mc);
    proto_ctx_t *p = &g_app.proto;
    p->cal = &g_app.cal;
    p->out_a = &g_app.out_a;
    p->out_p = &g_app.out_p;
    p->meas = &g_app.meas;
    p->cv = &g_app.cv;
    p->map_cfg = mc;
    // Default: P1..P5 follow MIDI (pitch, gate, velocity, CC1, CC74); P6..P8 follow the host.
    for (int i = 0; i < CV_P_OUT; i++) p->p_src[i] = (i < 5) ? SRC_MAPPER : SRC_HOST;

    power_init(&g_app.pwr);

    // 1) Precision outputs to 0 V before anything else (DAC soft reset -> 0 V codes -> reference on).
    g_app.precision_ok = precision_init();
    // 2) Codec: held in reset by its RC until here; outputs sit at VCOM = 0 V differential.
    g_app.codec_ok = codec_init();
    ESP_LOGI(TAG, "precision %d codec %d cal %s", g_app.precision_ok, g_app.codec_ok,
             g_app.cal_from_flash ? "flash" : "defaults");

    midi_router_init();
    power_start();                 // core 0, 10 ms: rails, battery, backlight, mute
    if (g_app.precision_ok) precision_start();   // core 1, 2 kHz
    if (g_app.codec_ok) codec_start();           // core 1, 1 ms blocks
    usb_start();                   // core 1
    ble_midi_start();              // NimBLE host on core 0
    display_start();               // core 0, 10 fps
    xTaskCreatePinnedToCore(router_task, "router", 4096, NULL, 5, NULL, CORE_APP);
}
