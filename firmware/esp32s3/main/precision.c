// Precision CV group: DAC8568 (8 outs) + ADS131M08 (8 ins) on SPI2, 2 kHz, core 1.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gptimer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "board.h"
#include "app.h"
#include "conv_codec.h"
#include "spi_shared.h"

static const char *TAG = "prec";
static spi_device_handle_t dac, adc;
static TaskHandle_t rt_task;

static bool bus_ready;
void spi_shared_bus_init(void) {
    if (bus_ready) return;
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SPI_SCLK, .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    bus_ready = true;
}

static void dac_frame(uint32_t f) {
    uint8_t b[4] = {(uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f};
    spi_transaction_t t = {.length = 32, .tx_buffer = b};
    spi_device_polling_transmit(dac, &t);
}

static void ads_frame(const uint32_t *tx, uint32_t *rx) {
    uint8_t out[3 * ADS_FRAME_WORDS] = {0}, in[3 * ADS_FRAME_WORDS] = {0};
    for (int i = 0; i < ADS_FRAME_WORDS; i++) {
        out[3 * i] = (uint8_t)(tx[i] >> 16); out[3 * i + 1] = (uint8_t)(tx[i] >> 8); out[3 * i + 2] = (uint8_t)tx[i];
    }
    spi_transaction_t t = {.length = 8 * sizeof out, .tx_buffer = out, .rx_buffer = in};
    spi_device_polling_transmit(adc, &t);
    ads_unpack(in, ADS_FRAME_WORDS, rx);
}

static uint16_t ads_cmd(uint16_t cmd, uint16_t data, bool with_data) {
    uint32_t tx[ADS_FRAME_WORDS] = {0}, rx[ADS_FRAME_WORDS], nul[ADS_FRAME_WORDS] = {0};
    tx[0] = ads_word(cmd);
    if (with_data) tx[1] = ads_word(data);
    ads_frame(tx, rx);
    ads_frame(nul, rx);          // the response arrives in the next frame
    return ads_resp16(rx[0]);
}

bool precision_init(void) {
    spi_shared_bus_init();
    spi_device_interface_config_t d = {
        .mode = 1, .clock_speed_hz = 25 * 1000 * 1000, .spics_io_num = PIN_CS_DAC, .queue_size = 2,
        .cs_ena_pretrans = 1, .cs_ena_posttrans = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &d, &dac));
    spi_device_interface_config_t a = {
        .mode = 1, .clock_speed_hz = 8 * 1000 * 1000, .spics_io_num = PIN_CS_ADC, .queue_size = 2,
        .cs_ena_pretrans = 1, .cs_ena_posttrans = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &a, &adc));

    // DAC: soft reset (B grade: midscale, reference off -> every jack 0 V), 0 V codes, reference on.
    dac_frame(dac8568_soft_reset());
    esp_rom_delay_us(50);
    int32_t zero[CV_P_OUT];
    uint32_t frames[CV_P_OUT];
    out_zero_codes(&g_app.out_p, &g_app.cal, zero);
    dac8568_encode_sample(zero, CV_P_OUT, frames);
    for (int i = 0; i < CV_P_OUT; i++) dac_frame(frames[i]);
    dac_frame(dac8568_ref_on());

    // ADC clock: 8.192 MHz crystal on XTAL1/XTAL2 (the P group is read on a 2 kHz timer, so it
    // does not need to be locked to the codec clock).
    vTaskDelay(pdMS_TO_TICKS(2));

    ads_cmd(ADS_CMD_RESET, 0, false);
    vTaskDelay(pdMS_TO_TICKS(2));
    uint16_t id = ads_cmd(ads_rreg(ADS_REG_ID, 1), 0, false);
    if (!ads_m08_id_ok(id)) { ESP_LOGW(TAG, "ADS131M08 not found (id %04x)", id); return false; }
    bool ok = ads_cmd(ads_wreg(ADS_REG_MODE, 1), ADS_MODE_VALUE, true) == ads_wreg_ack(ADS_REG_MODE, 1);
    ok &= ads_cmd(ads_wreg(ADS_REG_CLOCK, 1), ADS_CLOCK_VALUE, true) == ads_wreg_ack(ADS_REG_CLOCK, 1);
    ok &= ads_cmd(ads_rreg(ADS_REG_CLOCK, 1), 0, false) == ADS_CLOCK_VALUE;
    return ok;
}

static bool IRAM_ATTR on_tick(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *u) {
    (void)t; (void)e; (void)u;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(rt_task, &hp);
    return hp == pdTRUE;
}

#define HOST_TIMEOUT_MS 2000

static void precision_task(void *arg) {
    (void)arg;
    uint32_t nul[ADS_FRAME_WORDS] = {0}, rx[ADS_FRAME_WORDS], frames[CV_P_OUT];
    int32_t raw[CV_P_IN], codes[CV_P_OUT];
    float volts[CV_P_OUT];
    uint32_t tick = 0;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) > 1) g_app.rt_late++;
        ads_frame(nul, rx);
        for (int i = 0; i < CV_P_IN; i++) raw[i] = ads_sext24(rx[1 + i]);

        bool host_alive = (app_ms() - g_app.host_last_ms) < HOST_TIMEOUT_MS;
        if (xSemaphoreTake(g_app.lock, 0) == pdTRUE) {      // never block the RT path
            in_measure_add(&g_app.meas, CV_P_IN0, CV_P_IN, raw);
            for (int i = 0; i < CV_P_IN; i++) g_app.meter_in[CV_P_IN0 + i] = cal_raw_to_volts(&g_app.cal, CV_P_IN0 + i, raw[i]);
            if (!host_alive)                                  // host gone: host-driven outputs glide to 0 V
                for (int i = 0; i < CV_P_OUT; i++) if (g_app.cv.target[i] != 0.0f) cv_glide_set(&g_app.cv, i, 0.0f);
            cv_glide_tick(&g_app.cv, CV_P_OUT);
            if ((tick & 1) == 0) mcv_tick_ms(&g_app.mapper, 1);
            for (int i = 0; i < CV_P_OUT; i++) {
                float v = 0.0f;
                switch (g_app.proto.p_src[i]) {
                case SRC_HOST: v = g_app.cv.cur[i]; break;
                case SRC_MAPPER: if (!mcv_out(&g_app.mapper, i, &v)) v = 0.0f; break;
                default: break;
                }
                volts[i] = v;
            }
            out_engine_step(&g_app.out_p, &g_app.cal, volts, codes);
            xSemaphoreGive(g_app.lock);
            for (int i = 0; i < CV_P_OUT; i++) g_app.meter_out[CV_P_OUT0 + i] = g_app.out_p.last[i];
            dac8568_encode_sample(codes, CV_P_OUT, frames);
            for (int i = 0; i < CV_P_OUT; i++) dac_frame(frames[i]);
        }
        tick++;
    }
}

void precision_start(void) {
    xTaskCreatePinnedToCore(precision_task, "prec", 4096, NULL, configMAX_PRIORITIES - 2, &rt_task, CORE_RT);
    gptimer_handle_t tm;
    gptimer_config_t cfg = {.clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000};
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &tm));
    gptimer_alarm_config_t al = {.alarm_count = 1000000 / CV_P_RATE, .reload_count = 0, .flags.auto_reload_on_alarm = true};
    gptimer_event_callbacks_t cb = {.on_alarm = on_tick};
    gptimer_register_event_callbacks(tm, &cb, NULL);
    gptimer_set_alarm_action(tm, &al);
    gptimer_enable(tm);
    gptimer_start(tm);
}
