// Status display (T-Display-S3: ST7789 170x320 on the 8-bit i80 bus), core 0, ~10 fps.
// Shows per-channel voltage meters, USB/host state, BLE state, calibration, power and battery.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "board.h"
#include "app.h"
#include "font6x10.h"

#define STRIP 10
static esp_lcd_panel_handle_t panel;
static uint16_t *strips[2];       // LCD_W x STRIP, RGB565 big-endian, double-buffered
static uint16_t *strip;
static SemaphoreHandle_t free_bufs;  // given back by the i80 DMA-done callback
static char lines[4][54];         // text rows: 0 header, 1 out labels, 2 in labels, 3 footer

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define SWAP(c) (uint16_t)(((c) >> 8) | ((c) << 8))
static const uint16_t C_BG = RGB(12, 14, 18), C_FG = RGB(220, 224, 230), C_DIM = RGB(90, 96, 108),
                      C_POS = RGB(80, 200, 140), C_NEG = RGB(230, 120, 80), C_MID = RGB(50, 54, 62),
                      C_WARN = RGB(240, 190, 60), C_BAD = RGB(240, 70, 70);

// Layout (landscape 320 x 170)
#define Y_HDR 0
#define Y_OUT 14          // output meters: 16 bars
#define H_BAR 56
#define Y_OUTL (Y_OUT + H_BAR + 1)
#define Y_IN (Y_OUTL + 12)
#define Y_INL (Y_IN + H_BAR + 1)
#define Y_FOOT 158

static bool text_px(const char *s, int x0, int y0, int x, int y) {
    if (y < y0 || y >= y0 + FONT_H || x < x0) return false;
    int ci = (x - x0) / FONT_W, cx = (x - x0) % FONT_W;
    if (ci >= (int)strlen(s)) return false;
    unsigned char c = (unsigned char)s[ci];
    if (c < 32 || c > 126) return false;
    return (font6x10[c - 32][y - y0] >> (7 - cx)) & 1;
}

static uint16_t bar_px(const float *v, int n, int x, int y, int y0, bool group_gap) {
    // n bars across the width, bipolar around the centre line, full scale +-10 V.
    const int bw = 19, gap = group_gap ? 8 : 0;
    int idx = x / bw;
    int xx = x;
    if (group_gap && idx >= 8) { xx -= gap; idx = xx / bw; }
    if (idx < 0 || idx >= n || xx % bw >= bw - 3) return C_BG;
    int mid = y0 + H_BAR / 2, h = (int)(v[idx] / 10.0f * (H_BAR / 2));
    if (h > H_BAR / 2) h = H_BAR / 2;
    if (h < -H_BAR / 2) h = -H_BAR / 2;
    if (y == mid) return C_DIM;
    if (h > 0 && y < mid && y >= mid - h) return C_POS;
    if (h < 0 && y > mid && y <= mid - h) return C_NEG;
    return C_MID;
}

static void render(int y0) {
    float out[16], in[16];
    for (int i = 0; i < 8; i++) { out[i] = g_app.meter_out[i]; out[8 + i] = g_app.meter_out[CV_P_OUT0 + i]; }
    for (int i = 0; i < 6; i++) in[i] = g_app.meter_in[i];
    for (int i = 0; i < 8; i++) in[6 + i] = g_app.meter_in[CV_P_IN0 + i];
    for (int y = y0; y < y0 + STRIP; y++) {
        for (int x = 0; x < LCD_W; x++) {
            uint16_t c = C_BG;
            if (y >= Y_OUT && y < Y_OUT + H_BAR) c = bar_px(out, 16, x, y, Y_OUT, true);
            else if (y >= Y_IN && y < Y_IN + H_BAR) c = bar_px(in, 14, x, y, Y_IN, false);
            else if (text_px(lines[0], 2, Y_HDR + 2, x, y)) c = C_FG;
            else if (text_px(lines[1], 2, Y_OUTL, x, y)) c = C_DIM;
            else if (text_px(lines[2], 2, Y_INL, x, y)) c = C_DIM;
            else if (text_px(lines[3], 2, Y_FOOT, x, y)) c = g_app.pwr.mute ? C_BAD : (g_app.pwr.low_battery ? C_WARN : C_FG);
            strip[(y - y0) * LCD_W + x] = SWAP(c);
        }
    }
}

static void update_text(void) {
    const char *usb = !g_app.usb_mounted ? "USB --" : (g_app.out_streaming || g_app.in_streaming) ? "USB audio" : "USB idle";
    char bat[24];
    if (g_app.pwr.batt_pct < 0) snprintf(bat, sizeof bat, "%s", g_app.pwr.src == PWR_SRC_USB ? "USB pwr" : "rack");
    else snprintf(bat, sizeof bat, "BAT %d%%", g_app.pwr.batt_pct);
    snprintf(lines[0], sizeof lines[0], "%s  BLE %s  CAL %s  %s", usb, g_app.ble_connected ? "on" : "--",
             g_app.cal_from_flash ? "ok" : "nom", bat);
    snprintf(lines[1], sizeof lines[1], "OUT A1..A8 audio      P1..P8 precision CV");
    snprintf(lines[2], sizeof lines[2], "IN  A1..A6 audio  P1..P8 precision CV");
    snprintf(lines[3], sizeof lines[3], "%s rail %.1fV  xrun %lu/%lu  %s%s", g_app.pwr.mute ? "MUTED" : "live",
             g_app.pwr.rails_ok ? 11.0 : 0.0, (unsigned long)g_app.usb_underruns, (unsigned long)g_app.rt_late,
             g_app.codec_ok ? "" : "no codec ", g_app.precision_ok ? "" : "no P");
}

static void display_task(void *arg) {
    (void)arg;
    for (;;) {
        update_text();
        int k = 0;
        for (int y = 0; y < LCD_H; y += STRIP, k ^= 1) {
            xSemaphoreTake(free_bufs, portMAX_DELAY);
            strip = strips[k];
            render(y);
            esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + STRIP, strip);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx) {
    (void)io; (void)ev; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(free_bufs, &hp);
    return hp == pdTRUE;
}

void display_start(void) {
    free_bufs = xSemaphoreCreateCounting(2, 2);
    gpio_set_direction(PIN_LCD_RD, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_RD, 1);
    esp_lcd_i80_bus_handle_t bus;
    esp_lcd_i80_bus_config_t bc = {
        .clk_src = LCD_CLK_SRC_DEFAULT, .dc_gpio_num = PIN_LCD_DC, .wr_gpio_num = PIN_LCD_WR,
        .data_gpio_nums = {PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7},
        .bus_width = 8, .max_transfer_bytes = LCD_W * STRIP * 2, .psram_trans_align = 64, .sram_trans_align = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bc, &bus));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i80_config_t ic = {
        .cs_gpio_num = PIN_LCD_CS, .pclk_hz = 10 * 1000 * 1000, .trans_queue_depth = 4,
        .dc_levels = {.dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1},
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .on_color_trans_done = on_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &ic, &io));
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = PIN_LCD_RES, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, 0, 35);
    esp_lcd_panel_disp_on_off(panel, true);
    for (int i = 0; i < 2; i++) strips[i] = heap_caps_malloc(LCD_W * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 3, NULL, CORE_APP);
}
