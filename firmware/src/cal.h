// Calibration table: per-channel linear maps, stored in the last flash sector.
// Pure C (no SDK), unit-tested on the host.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cv_config.h"

// Nominal analog front-end values. These MUST match hw/ (schematic, BOM, SPICE).
// Output stage (inverting, ratiometric to the DAC reference):
//   Vop = Vb*(1+G) - G*Vdac,  G = RF/R1,  Vb = VREF*RB/(RT+RB)
//   Vjack = Vop * RL/(RL+RS)   (1 k series resistor, 100 k nominal load)
#define HW_VREF   2.5f
#define HW_OUT_R1 10.0e3f
#define HW_OUT_RF 82.5e3f
#define HW_OUT_RT 10.0e3f
#define HW_OUT_RB 8.06e3f
#define HW_OUT_RS 1.0e3f
#define HW_OUT_RL 100.0e3f
// Input stage: 2 x 49.9 k series, 9.09 k shunt, ADC input impedance 330 k (gain 1).
#define HW_IN_RS   99.8e3f
#define HW_IN_RSH  9.09e3f
#define HW_IN_RADC 330.0e3f
#define HW_ADC_FS  1.2f   // ADS131M04 internal reference, gain 1: +-1.2 V

// Output range limiter (applied before calibration, in volts).
enum { CV_RANGE_BI10 = 0, CV_RANGE_BI5 = 1, CV_RANGE_UNI10 = 2, CV_RANGE_UNI5 = 3, CV_RANGE_COUNT };

typedef struct {
    float gain;    // out: DAC codes per volt (negative: inverting stage); in: volts per ADC count
    float offset;  // out: DAC code at 0 V; in: ADC counts at 0 V
} cv_lin_t;

#define CV_CAL_MAGIC   0x43564341u  // "ACVC"
#define CV_CAL_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_out, n_in;
    cv_lin_t out[CV_MAX_OUT];
    cv_lin_t in[CV_MAX_IN];
    uint8_t  out_range[CV_MAX_OUT];
    uint32_t crc;   // CRC-32 over everything before this field
} cv_cal_t;

void     cal_defaults(cv_cal_t *c);
uint32_t cal_crc32(const void *data, uint32_t len);
void     cal_seal(cv_cal_t *c);               // fill magic/version/crc
bool     cal_valid(const cv_cal_t *c);

void     cal_range_limits(uint8_t range, float *lo, float *hi);
uint16_t cal_volts_to_code(const cv_cal_t *c, int ch, float volts); // limiter + cal + clamp
float    cal_code_to_volts(const cv_cal_t *c, int ch, uint16_t code); // inverse (for tests/tools)
float    cal_raw_to_volts(const cv_cal_t *c, int ch, int32_t raw);

// Two-point fits. Output: codes c1,c2 measured as v1,v2 volts. Input: raw counts
// r1,r2 measured for known v1,v2 volts. Return false on degenerate input.
bool cal_solve_out(float c1, float v1, float c2, float v2, cv_lin_t *out);
bool cal_solve_in(float r1, float v1, float r2, float v2, cv_lin_t *out);
