// Calibration table in NVS (wear-levelled, power-fail safe).
#include "nvs_flash.h"
#include "nvs.h"
#include "app.h"

void storage_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool storage_load_cal(cv_cal_t *c) {
    nvs_handle_t h;
    if (nvs_open("cv", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof *c;
    cv_cal_t t;
    bool ok = nvs_get_blob(h, "cal", &t, &n) == ESP_OK && n == sizeof t && cal_valid(&t);
    nvs_close(h);
    if (ok) *c = t;
    return ok;
}

bool storage_save_cal(const cv_cal_t *c) {
    cv_cal_t t = *c;
    cal_seal(&t);
    nvs_handle_t h;
    if (nvs_open("cv", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "cal", &t, sizeof t) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    cv_cal_t back;
    return ok && storage_load_cal(&back) && back.crc == t.crc;
}
