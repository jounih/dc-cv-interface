// Calibration table: per-channel linear maps, stored in NVS (flash) with a CRC.
// Pure C (no SDK), unit-tested on the host.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cv_config.h"

// Nominal analog front-end values. These MUST match hw/ (gen_hw.py, BOM, SPICE).
//
// P outputs (DAC8568, inverting stage ratiometric to the DAC's 2.5 V reference):
//   Vop = Vb*(1+G) - G*Vdac,  G = RF/R1,  Vb = VREF*RB/(RT+RB),  Vjack = Vop*RL/(RL+RS)
#define HW_VREF   2.5f
#define HW_OUT_R1 10.0e3f
#define HW_OUT_RF 82.5e3f
#define HW_OUT_RT 10.0e3f
#define HW_OUT_RB 8.06e3f
#define HW_OUT_RS 1.0e3f
#define HW_OUT_RL 100.0e3f
// P inputs (ADS131M08, gain 1, +-1.2 V): 2 x 49.9 k series, 9.09 k shunt, 330 k ADC input.
#define HW_IN_RS   99.8e3f
#define HW_IN_RSH  9.09e3f
#define HW_IN_RADC 330.0e3f
#define HW_ADC_FS  1.2f
// A outputs (PCM3168A DAC, differential, full scale 1.6 x VCC Vpp): difference amplifier
//   Vjack = kl * (AO_RF/AO_RI) * (VOUT+ - VOUT-)
#define HW_CODEC_VCC 4.5f
#define HW_AO_RI 28.7e3f
#define HW_AO_RF 82.5e3f
// A inputs (PCM3168A ADC, single-ended, full scale 0.2 x VCC Vrms around VCOM):
//   inverting stage on the codec's 4.5 V supply, Vadc - VCOM = -(AI_RF/AI_RI) * Vjack
//   (non-inverting input at VCC * AI_RB/(AI_RT+AI_RB) = VCOM * AI_RI/(AI_RI+AI_RF), so 0 V maps to VCOM)
#define HW_AI_RI 100.0e3f
#define HW_AI_RF 11.0e3f
#define HW_AI_RT 10.0e3f
#define HW_AI_RB 8.2e3f

// Tier A (breadboard, pre-soldered modules): 2x MCP4728 (12-bit, 0..4.096 V) + 2x ADS1115
// (16-bit, single-ended 0..4.096 V) take the place of the precision group (build with CV_TIER_A).
//   out: Vjack = kl*(VbA*(1+GA) - GA*Vdac), GA = 49.9k/10k, VbA = 3.3 V * 10.7k/(10k+10.7k)
//   in:  Vadc = 1.65 V * R1/(R1+Rp) + Vjack * Rp/(R1+Rp), R1 = 100 k, Rp = 22 k || 22 k from 3.3 V/GND
#define HW_TA_R1 10.0e3f
#define HW_TA_RF 49.9e3f
#define HW_TA_RT 10.0e3f
#define HW_TA_RB 10.7e3f
#define HW_TA_VDD 3.3f
#define HW_TA_IN_R1 100.0e3f
#define HW_TA_IN_RP 11.0e3f

typedef enum { CH_A_OUT, CH_P_OUT, CH_A_IN, CH_P_IN, CH_NONE } cv_kind_t;
cv_kind_t cal_out_kind(int ch);   // CH_A_OUT, CH_P_OUT or CH_NONE
cv_kind_t cal_in_kind(int ch);    // CH_A_IN, CH_P_IN or CH_NONE

// Output range limiter (applied before calibration, in volts).
enum { CV_RANGE_BI10 = 0, CV_RANGE_BI5 = 1, CV_RANGE_UNI10 = 2, CV_RANGE_UNI5 = 3, CV_RANGE_COUNT };

typedef struct {
    float gain;    // out: codes per volt; in: volts per count
    float offset;  // out: code at 0 V; in: counts at 0 V
} cv_lin_t;

#define CV_CAL_MAGIC   0x43564341u  // "ACVC"
#define CV_CAL_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    cv_lin_t out[CV_MAX_OUT];
    cv_lin_t in[CV_MAX_IN];
    uint8_t  out_range[CV_MAX_OUT];
    uint8_t  low_power;        // 1: outputs limited to +-5 V (battery saving, see README)
    uint8_t  pad[3];
    uint32_t crc;              // CRC-32 over everything before this field
} cv_cal_t;

void     cal_defaults(cv_cal_t *c);
uint32_t cal_crc32(const void *data, uint32_t len);
void     cal_seal(cv_cal_t *c);
bool     cal_valid(const cv_cal_t *c);

void     cal_code_limits(int ch, int32_t *lo, int32_t *hi);       // output code range
void     cal_range_limits(uint8_t range, float *lo, float *hi);
int32_t  cal_volts_to_code(const cv_cal_t *c, int ch, float volts); // limiter + cal + clamp
float    cal_code_to_volts(const cv_cal_t *c, int ch, int32_t code);
float    cal_raw_to_volts(const cv_cal_t *c, int ch, int32_t raw);

bool cal_solve_out(float c1, float v1, float c2, float v2, cv_lin_t *out);
bool cal_solve_in(float r1, float v1, float r2, float v2, cv_lin_t *out);
