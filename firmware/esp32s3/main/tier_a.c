// Tier A (breadboard, no PCB): 2x MCP4728 (outputs P1..P8, 12-bit) + 2x ADS1115 (inputs P1..P8)
// on I2C, replacing the precision group. CV rate only: outputs ~1 kHz, inputs ~125 Hz per channel.
// Build with: idf.py -DCV_TIER_A=1 build. The codec group is absent (UAC2 still enumerates).
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "app.h"
#include "conv_codec.h"
#include "tusb.h"

// Two I2C buses so both module pairs keep their default addresses (MCP4728 0x60, ADS1115 0x48):
// bus 0 = outputs P1-P4 / inputs P1-P4, bus 1 = outputs P5-P8 / inputs P5-P8.
static const int SDA[2] = {18, 16}, SCL[2] = {17, 21};
static const char *TAG = "tierA";
static i2c_master_bus_handle_t bus[2];
static i2c_master_dev_handle_t dac[2], adc[2];

static bool add(int b, uint16_t addr, i2c_master_dev_handle_t *h) {
    i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    return i2c_master_bus_add_device(bus[b], &d, h) == ESP_OK && i2c_master_probe(bus[b], addr, 20) == ESP_OK;
}

static void ads_start(int chip, int ch) {
    uint16_t cfg = ads1115_config(ch);
    uint8_t b[3] = {0x01, (uint8_t)(cfg >> 8), (uint8_t)cfg};
    i2c_master_transmit(adc[chip], b, 3, 5);
}

static int32_t ads_read(int chip) {
    uint8_t reg = 0x00, b[2] = {0};
    i2c_master_transmit_receive(adc[chip], &reg, 1, b, 2, 5);
    return (int16_t)((b[0] << 8) | b[1]);
}

bool precision_init(void) {
    bool ok = true, any = false;
    for (int b = 0; b < 2; b++) {
        i2c_master_bus_config_t bc = {.i2c_port = b, .sda_io_num = SDA[b], .scl_io_num = SCL[b],
                                      .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                      .flags.enable_internal_pullup = true};
        if (i2c_new_master_bus(&bc, &bus[b]) != ESP_OK) return false;
        bool d = add(b, 0x60, &dac[b]);
        ok &= d;
        any |= d;
        ok &= add(b, 0x48, &adc[b]);
    }
    if (!ok) { ESP_LOGW(TAG, "I2C modules missing (each bus: MCP4728 0x60 + ADS1115 0x48)"); }
    // Power-on safety: a fresh MCP4728 starts at code 0, which the inverting bipolar stage turns
    // into +10 V at the jack before any firmware runs. Store the 0 V codes (internal ref, x2) in the
    // MCP4728 EEPROM once, so every later power-up starts at 0 V by itself.
    int32_t zero[CV_P_OUT];
    out_zero_codes(&g_app.out_p, &g_app.cal, zero);
    for (int k = 0; k < 2; k++) {
        uint8_t rb[24] = {0}, b[9];
        bool same = i2c_master_receive(dac[k], rb, sizeof rb, 20) == ESP_OK;
        for (int i = 0; same && i < 4; i++) same = mcp4728_readback_eeprom_code(rb, i) == zero[4 * k + i];
        mcp4728_seq_write_eeprom(&zero[4 * k], b);
        if (!same) { i2c_master_transmit(dac[k], b, sizeof b, 20); vTaskDelay(pdMS_TO_TICKS(60)); }
        else {
            uint8_t f[8], vref = MCP4728_VREF_ALL_INT, gain = MCP4728_GAIN_ALL_X2;
            mcp4728_fast_write(&zero[4 * k], f);
            i2c_master_transmit(dac[k], f, 8, 5);
            i2c_master_transmit(dac[k], &vref, 1, 5);
            i2c_master_transmit(dac[k], &gain, 1, 5);
        }
    }
    return any;   // run with whatever is wired (bus 0 alone is fine for a first test)
}

static void tiera_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    int ch = 0, tick = 0;
    int32_t raw[CV_P_IN] = {0}, codes[CV_P_OUT];
    float volts[CV_P_OUT];
    ads_start(0, 0); ads_start(1, 0);
    for (;;) {
        vTaskDelayUntil(&last, 1);                    // 1 kHz (CONFIG_FREERTOS_HZ=1000)
        app_lock();
        cv_glide_tick(&g_app.cv, CV_P_OUT);
        mcv_tick_ms(&g_app.mapper, 1);
        for (int i = 0; i < CV_P_OUT; i++) {
            float v = 0.0f;
            if (g_app.proto.p_src[i] == SRC_HOST) v = g_app.cv.cur[i];
            else if (g_app.proto.p_src[i] == SRC_MAPPER && !mcv_out(&g_app.mapper, i, &v)) v = 0.0f;
            volts[i] = v;
        }
        out_engine_step(&g_app.out_p, &g_app.cal, volts, codes);
        app_unlock();
        for (int k = 0; k < 2; k++) {
            uint8_t b[8];
            mcp4728_fast_write(&codes[4 * k], b);
            i2c_master_transmit(dac[k], b, 8, 2);
        }
        for (int i = 0; i < CV_P_OUT; i++) g_app.meter_out[CV_P_OUT0 + i] = g_app.out_p.last[i];
        if (++tick == 2) {                            // 860 SPS: one conversion per 2 ms per chip
            tick = 0;
            raw[ch] = ads_read(0);
            raw[4 + ch] = ads_read(1);
            ch = (ch + 1) & 3;
            ads_start(0, ch); ads_start(1, ch);
            for (int i = 0; i < CV_P_IN; i++) g_app.meter_in[CV_P_IN0 + i] = cal_raw_to_volts(&g_app.cal, CV_P_IN0 + i, raw[i]);
            if (ch == 0) in_measure_add(&g_app.meas, CV_P_IN0, CV_P_IN, raw);
        }
    }
}

void precision_start(void) {
    xTaskCreatePinnedToCore(tiera_task, "tierA", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);
}

// No codec on the breadboard: keep the UAC2 streams alive (silence in, data discarded) so the
// browser side of Circuit Studio can be exercised. Paced by the RTOS tick (1 ms = 32 frames).
static void silent_audio_task(void *arg) {
    (void)arg;
    static int16_t zeros[32 * CV_A_IN];
    static int16_t sink[32 * CV_A_OUT];
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, 1);
        if (g_app.in_streaming) tud_audio_write(zeros, sizeof zeros);
        if (g_app.out_streaming && tud_audio_available() >= sizeof sink) tud_audio_read(sink, sizeof sink);
    }
}

bool codec_init(void) { return true; }   // "codec" here = the silent UAC2 bridge
void codec_start(void) { xTaskCreatePinnedToCore(silent_audio_task, "silent", 3072, NULL, configMAX_PRIORITIES - 1, NULL, 1); }
