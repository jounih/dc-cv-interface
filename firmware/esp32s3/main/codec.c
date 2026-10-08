// Group A: PCM3168A codec, I2S0 full-duplex TDM at 32 kHz, bridged to USB Audio Class 2.
// The codec ADC high-pass filter is bypassed (register 82, BYP = 111), so inputs are DC-coupled.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_tdm.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "tusb.h"
#include "board.h"
#include "app.h"
#include "conv_codec.h"
#include "spi_shared.h"

static const char *TAG = "codec";
static i2s_chan_handle_t tx_ch, rx_ch;
static spi_device_handle_t ctl;

#define BLOCK 32   // frames per I/O block = 1 ms at 32 kHz

static void reg_write(uint8_t reg, uint8_t val) {
    uint16_t w = pcm3168_write(reg, val);
    uint8_t b[2] = {(uint8_t)(w >> 8), (uint8_t)w};
    spi_transaction_t t = {.length = 16, .tx_buffer = b};
    spi_device_polling_transmit(ctl, &t);
}

bool codec_init(void) {
    spi_shared_bus_init();
    spi_device_interface_config_t d = {
        .mode = 0, .clock_speed_hz = 1000 * 1000, .spics_io_num = PIN_CS_CODEC, .queue_size = 2,
    };
    if (spi_bus_add_device(SPI2_HOST, &d, &ctl) != ESP_OK) return false;

    // I2S0: S3 master, MCLK 512 fs (160 MHz / 9.765625 exact with the 9-bit fractional divider),
    // 8 x 32-bit TDM slots, left-justified (MSB at the frame edge) to match FMT = 0111.
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = BLOCK;
    if (i2s_new_channel(&cc, &tx_ch, &rx_ch) != ESP_OK) return false;
    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(CV_AUDIO_RATE),
        .slot_cfg = I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, 0xFF),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN,
        },
    };
    tdm.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    tdm.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512;
    tdm.slot_cfg.total_slot = PCM_SLOTS;
    if (i2s_channel_init_tdm_mode(tx_ch, &tdm) != ESP_OK || i2s_channel_init_tdm_mode(rx_ch, &tdm) != ESP_OK) return false;

    // Clocks must run before the codec leaves reset; DAC outputs stay at VCOM (0 V differential).
    i2s_channel_enable(tx_ch);
    i2s_channel_enable(rx_ch);
    vTaskDelay(pdMS_TO_TICKS(5));
    reg_write(PCM_REG_DAC_FMT, PCM_DAC_TDM_LJ);
    reg_write(PCM_REG_ADC_FMT, PCM_ADC_TDM_LJ);
    reg_write(PCM_REG_ADC_HPF, PCM_ADC_HPF_OFF);
    reg_write(PCM_REG_ADC_SE, PCM_ADC_SE_ALL);
    reg_write(PCM_REG_RESET, PCM_RESET_RUN & ~0x40);   // SRST = 0: system reset (resync)
    vTaskDelay(pdMS_TO_TICKS(2));
    reg_write(PCM_REG_RESET, PCM_RESET_RUN);
    ESP_LOGI(TAG, "PCM3168A configured (write-only control port: presence is inferred from ADC data)");
    return true;
}

static void codec_task(void *arg) {
    (void)arg;
    static int32_t rx[BLOCK * PCM_SLOTS], tx[BLOCK * PCM_SLOTS];
    static int16_t usb_out[BLOCK * CV_A_OUT], usb_in[BLOCK * CV_A_IN];
    static uint8_t primed;
    int32_t raw[CV_A_IN], codes[CV_A_OUT];
    float volts[CV_A_OUT];
    size_t got;
    for (;;) {
        // Blocking read paces the loop at the codec clock (1 ms per block).
        if (i2s_channel_read(rx_ch, rx, sizeof rx, &got, portMAX_DELAY) != ESP_OK) continue;

        // ---- inputs -> USB IN
        for (int f = 0; f < BLOCK; f++) {
            for (int c = 0; c < CV_A_IN; c++) raw[c] = tdm_to_s24(rx[f * PCM_SLOTS + c]);
            in_a_step(&g_app.cal, raw, &usb_in[f * CV_A_IN]);
            in_measure_add(&g_app.meas, 0, CV_A_IN, raw);
        }
        for (int c = 0; c < CV_A_IN; c++) g_app.meter_in[c] = usb16_to_volts(usb_in[c]);
        if (g_app.in_streaming) tud_audio_write(usb_in, sizeof usb_in);

        // ---- USB OUT -> outputs (FIFO-count feedback keeps the FIFO half full)
        uint32_t have = 0;
        if (g_app.out_streaming) {
            uint32_t avail = tud_audio_available();
            if (!primed && avail >= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2) primed = 1;
            if (primed) {
                have = avail / (CV_A_OUT * CV_A_BYTES);
                if (have > BLOCK) have = BLOCK;
                if (have) tud_audio_read(usb_out, (uint16_t)(have * CV_A_OUT * CV_A_BYTES));
                if (have < BLOCK) g_app.usb_underruns++;
            }
        } else {
            primed = 0;
        }
        for (uint32_t f = 0; f < BLOCK; f++) {
            if (f < have) for (int c = 0; c < CV_A_OUT; c++) volts[c] = usb16_to_volts(usb_out[f * CV_A_OUT + c]);
            out_engine_step(&g_app.out_a, &g_app.cal, f < have ? volts : NULL, codes);
            for (int c = 0; c < CV_A_OUT; c++) tx[f * PCM_SLOTS + c] = s24_to_tdm(codes[c]);
        }
        for (int c = 0; c < CV_A_OUT; c++) g_app.meter_out[c] = g_app.out_a.last[c];
        size_t wr;
        i2s_channel_write(tx_ch, tx, sizeof tx, &wr, portMAX_DELAY);
    }
}

void codec_start(void) {
    xTaskCreatePinnedToCore(codec_task, "codec", 6144, NULL, configMAX_PRIORITIES - 1, NULL, CORE_RT);
}
