// Calibration table in the last 4 KB flash sector.
#include "cal_flash.h"
#include <string.h>
#include "pico/flash.h"
#include "hardware/flash.h"

#define CAL_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

bool cal_flash_load(cv_cal_t *c) {
    const cv_cal_t *f = (const cv_cal_t *)(XIP_BASE + CAL_FLASH_OFFSET);
    if (!cal_valid(f)) return false;
    *c = *f;
    return true;
}

static uint8_t page_buf[FLASH_PAGE_SIZE * ((sizeof(cv_cal_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE)];

static void do_write(void *param) {
    (void)param;
    flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CAL_FLASH_OFFSET, page_buf, sizeof page_buf);
}

bool cal_flash_save(const cv_cal_t *c) {
    cv_cal_t t = *c;
    cal_seal(&t);
    memset(page_buf, 0xFF, sizeof page_buf);
    memcpy(page_buf, &t, sizeof t);
    // flash_safe_execute pauses the other core / interrupts. The converter DMA
    // keeps running from SRAM and simply replays the ring (~50 ms erase).
    if (flash_safe_execute(do_write, NULL, 500) != PICO_OK) return false;
    cv_cal_t back;
    return cal_flash_load(&back) && memcmp(&back, &t, sizeof t) == 0;
}
