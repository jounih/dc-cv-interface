// Pure encode/decode helpers for the DAC8568 and ADS131M04 serial protocols,
// plus clock-divider maths. No hardware access; unit-tested on the host.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cv_config.h"

// ---------------------------------------------------------------- DAC8568
// 32-bit frame: DB31 = 0, DB30..28 don't care, DB27..24 = C3..C0 (command),
// DB23..20 = A3..A0 (channel, 0..7, 0xF = broadcast), DB19..4 = 16-bit data,
// DB3..0 = feature bits. Shifted MSB first, latched on SCLK falling edges.
enum {
    DAC8568_C_WRITE_INPUT        = 0x0, // write input register n
    DAC8568_C_UPDATE_DAC         = 0x1, // update DAC register n
    DAC8568_C_WRITE_UPDATE_ALL   = 0x2, // write input n, update all DACs
    DAC8568_C_WRITE_UPDATE       = 0x3, // write input n, update DAC n
    DAC8568_C_POWER              = 0x4,
    DAC8568_C_CLEAR_CODE         = 0x5,
    DAC8568_C_LDAC_REG           = 0x6,
    DAC8568_C_SOFT_RESET         = 0x7, // = power-on reset (midscale on B/D grades, ref off)
    DAC8568_C_REF_STATIC         = 0x8, // DB0 = 1: internal ref on (static mode)
    DAC8568_C_REF_FLEX           = 0x9,
};
#define DAC8568_ADDR_ALL 0xFu

static inline uint32_t dac8568_frame(uint32_t cmd, uint32_t addr, uint32_t data16, uint32_t feat) {
    return ((cmd & 0xFu) << 24) | ((addr & 0xFu) << 20) | ((data16 & 0xFFFFu) << 4) | (feat & 0xFu);
}
static inline uint32_t dac8568_soft_reset(void) { return dac8568_frame(DAC8568_C_SOFT_RESET, 0, 0, 0); }
static inline uint32_t dac8568_ref_on(void)     { return dac8568_frame(DAC8568_C_REF_STATIC, 0, 0, 1); }

// Encode one sample period: write input registers for channels 0..n-2 and
// "write + update all" for the last one, so every output changes at the
// same instant. Returns the number of 32-bit frames written (= n).
static inline int dac8568_encode_sample(const uint16_t *codes, int n, uint32_t *frames) {
    for (int i = 0; i < n; i++) {
        uint32_t cmd = (i == n - 1) ? DAC8568_C_WRITE_UPDATE_ALL : DAC8568_C_WRITE_INPUT;
        frames[i] = dac8568_frame(cmd, (uint32_t)i, codes[i], 0);
    }
    return n;
}

// ---------------------------------------------------------------- ADS131M04
// 24-bit words (default WLENGTH), SPI mode 1. A frame is 6 words:
// command/response, CH0..CH3, CRC. Commands are 16 bits left-aligned in a word.
enum {
    ADS_CMD_NULL   = 0x0000,
    ADS_CMD_RESET  = 0x0011,
    ADS_CMD_STBY   = 0x0022,
    ADS_CMD_WAKE   = 0x0033,
    ADS_CMD_LOCK   = 0x0555,
    ADS_CMD_UNLOCK = 0x0655,
};
enum { ADS_REG_ID = 0x00, ADS_REG_STATUS = 0x01, ADS_REG_MODE = 0x02, ADS_REG_CLOCK = 0x03, ADS_REG_GAIN = 0x04 };
#define ADS_FRAME_WORDS 6
#define ADS_RESET_ACK 0xFF24u
// MODE: clear RESET flag, 24-bit words, SPI timeout on, DRDY = most lagging channel, level mode.
#define ADS_MODE_VALUE  0x0110u
// CLOCK: CH0..3 enabled, TBM=1 (OSR 64), PWR=10 (high resolution).
#define ADS_CLOCK_VALUE 0x0F22u

static inline uint32_t ads_word(uint16_t v16) { return (uint32_t)v16 << 8; }
static inline uint16_t ads_wreg(uint8_t addr, uint8_t n) { return (uint16_t)(0x6000u | ((addr & 0x3Fu) << 7) | ((n - 1u) & 0x7Fu)); }
static inline uint16_t ads_rreg(uint8_t addr, uint8_t n) { return (uint16_t)(0xA000u | ((addr & 0x3Fu) << 7) | ((n - 1u) & 0x7Fu)); }
static inline uint16_t ads_wreg_ack(uint8_t addr, uint8_t n) { return (uint16_t)(0x4000u | ((addr & 0x3Fu) << 7) | ((n - 1u) & 0x7Fu)); }
static inline int32_t ads_sext24(uint32_t w) { return (int32_t)(w << 8) >> 8; }
static inline uint16_t ads_resp16(uint32_t w) { return (uint16_t)((w >> 8) & 0xFFFFu); }
static inline bool ads_id_ok(uint16_t id) { return (id >> 8) == 0x24u; }

// ADS131M04 output data rate: fDATA = fCLKIN / 2 / OSR (OSR 64 in turbo mode).
#define ADS_OSR 64u
#define ADS_CLKIN_HZ (CV_SAMPLE_RATE * 2u * ADS_OSR)   // 6.144 MHz for 48 kHz

// ---------------------------------------------------------------- clocks
static inline uint32_t cv_gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

// DMA pacing timer: DREQ rate = sys_hz * x / y, x and y are 16-bit.
// Returns true when the rate is exact.
static inline bool cv_dma_timer_frac(uint32_t sys_hz, uint32_t rate_hz, uint16_t *x, uint16_t *y) {
    uint32_t g = cv_gcd(sys_hz, rate_hz);
    uint32_t nx = rate_hz / g, ny = sys_hz / g;
    if (nx <= 0xFFFFu && ny <= 0xFFFFu) { *x = (uint16_t)nx; *y = (uint16_t)ny; return true; }
    // Best approximation with y = 65535.
    *y = 0xFFFFu;
    *x = (uint16_t)(((uint64_t)rate_hz * 0xFFFFu + sys_hz / 2) / sys_hz);
    return false;
}

// Clock GPOUT divider (16.16 on RP2350). Returns true when exact.
static inline bool cv_gpout_div(uint32_t sys_hz, uint32_t out_hz, uint32_t *div_int, uint16_t *div_frac16) {
    uint64_t q = ((uint64_t)sys_hz << 16);
    uint64_t d = (q + out_hz / 2) / out_hz;
    *div_int = (uint32_t)(d >> 16);
    *div_frac16 = (uint16_t)(d & 0xFFFFu);
    return (q % out_hz) == 0;
}
