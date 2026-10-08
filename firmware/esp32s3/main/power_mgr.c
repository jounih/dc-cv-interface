// Power supervision task (core 0, 10 ms): rail health -> output mute, battery, backlight, sleep.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "board.h"
#include "app.h"

static adc_oneshot_unit_handle_t adc1;
static adc_cali_handle_t cali;

static int read_mv(adc_channel_t ch) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc1, ch, &raw) != ESP_OK) return 0;
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3100 / 4095;
}

static void backlight_init(void) {
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                             .timer_num = LEDC_TIMER_1, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    ledc_channel_config_t c = {.gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
                               .timer_sel = LEDC_TIMER_1, .duty = 255};
    ledc_channel_config(&c);
}

static void power_task(void *arg) {
    (void)arg;
    uint8_t last_bl = 255;
    bool b1_prev = true, b2_prev = true;
    for (;;) {
        pwr_in_t in = {0};
#ifdef CV_TIER_A
        in.rail_mv = 11000;   // breadboard: no rail-sense divider; the bench supply is assumed good
#else
        in.rail_mv = (int)(read_mv(ADC_CHANNEL_0) * RAIL_SENSE_RATIO);     // GPIO1
#endif
        int b = read_mv(ADC_CHANNEL_3) * 2;                                // GPIO4, x2 divider on the board
        in.vbus = b > 4400 || g_app.usb_mounted;   // with USB-C present the battery pin reads the charger rail
        in.batt_mv = in.vbus ? 0 : b;
#ifdef CV_TIER_A
        in.rack = false;
#else
        in.rack = gpio_get_level(PIN_RACK_SENSE);   // rack +-12 V diode-ORed into the LDO inputs
#endif
        bool b1 = gpio_get_level(PIN_BUTTON_1), b2 = gpio_get_level(PIN_BUTTON_2);
        in.user_activity = (b1 != b1_prev) || (b2 != b2_prev) || g_app.out_streaming;
        b1_prev = b1; b2_prev = b2;

        power_update(&g_app.pwr, &in, 10);
        g_app.out_a.force_zero = g_app.out_p.force_zero = g_app.pwr.mute;

        if (g_app.pwr.backlight != last_bl) {
            last_bl = g_app.pwr.backlight;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, last_bl);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        }
        if (g_app.pwr.shutdown) {
            // Outputs are already forced to 0 V; give the RT tasks two blocks, then sleep.
            vTaskDelay(pdMS_TO_TICKS(20));
            gpio_set_level(PIN_POWER_ON, 0);
            esp_sleep_enable_ext0_wakeup(PIN_BUTTON_2, 0);
            esp_deep_sleep_start();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void power_start(void) {
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&u, &adc1);
    adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(adc1, ADC_CHANNEL_0, &c);
    adc_oneshot_config_channel(adc1, ADC_CHANNEL_3, &c);
    adc_cali_curve_fitting_config_t cc = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cc, &cali) != ESP_OK) cali = NULL;
    gpio_set_direction(PIN_BUTTON_1, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_BUTTON_2, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_RACK_SENSE, GPIO_MODE_INPUT);
    backlight_init();
    xTaskCreatePinnedToCore(power_task, "power", 3072, NULL, 10, NULL, CORE_APP);
}
