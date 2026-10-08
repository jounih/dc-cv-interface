#pragma once
#include <stdbool.h>
#include "cal.h"

bool cal_flash_load(cv_cal_t *c);        // false if the sector holds no valid table
bool cal_flash_save(const cv_cal_t *c);  // erase + program + verify
