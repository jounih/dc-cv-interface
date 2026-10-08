// Pure encode/decode helpers for the converters (DAC8568, ADS131M08, PCM3168A) and
// clock maths. No hardware access; unit-tested on the host.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cv_config.h"

// ---------------------------------------------------------------- DAC8568 (precision outs)
// 32-bit frame: DB31 = 0, DB27..24 command, DB23..20 channel (0xF = broadcast),
// DB19..4 data, DB3..0 feature bits. MSB first, latched on SCLK falling edges (SPI mode 1).
enum {
    DAC8568_C_WRITE_INPUT      = 0x0,
    DAC8568_C_UPDATE_DAC       = 0x1,
    DAC8568_C_WRITE_UPDATE_ALL = 0x2,
    DAC8568_C_WRITE_UPDATE     = 0x3,
    DAC8568_C_SOFT_RESET       = 0x7,   // = power-on reset (B grade: midscale, internal ref off)
    DAC8568_C_REF_STATIC       = 0x8,   // DB0 = 1: internal reference on
};
static inline uint32_t dac8568_frame(uint32_t cmd, uint32_t addr, uint32_t data16, uint32_t feat) {
    return ((cmd & 0xFu) << 24) | ((addr & 0xFu) << 20) | ((data16 & 0xFFFFu) << 4) | (feat & 0xFu);
}
static inline uint32_t dac8568_soft_reset(void) { return dac8568_frame(DAC8568_C_SOFT_RESET, 0, 0, 0); }
static inline uint32_t dac8568_ref_on(void)     { return dac8568_frame(DAC8568_C_REF_STATIC, 0, 0, 1); }
// Channels 0..n-2 to input registers, the last one "write + update all": simultaneous update.
static inline int dac8568_encode_sample(const int32_t *codes, int n, uint32_t *frames) {
    for (int i = 0; i < n; i++) {
        uint32_t cmd = (i == n - 1) ? DAC8568_C_WRITE_UPDATE_ALL : DAC8568_C_WRITE_INPUT;
        frames[i] = dac8568_frame(cmd, (uint32_t)i, (uint32_t)codes[i], 0);
    }
    return n;
}

// ---------------------------------------------------------------- ADS131M08 (precision ins)
// 24-bit words, SPI mode 1, frame = response + 8 channels + CRC = 10 words.
enum { ADS_CMD_NULL = 0x0000, ADS_CMD_RESET = 0x0011, ADS_CMD_STBY = 0x0022, ADS_CMD_WAKE = 0x0033 };
enum { ADS_REG_ID = 0x00, ADS_REG_STATUS = 0x01, ADS_REG_MODE = 0x02, ADS_REG_CLOCK = 0x03 };
#define ADS_FRAME_WORDS 10
#define ADS_RESET_ACK   0xFF28u
#define ADS_MODE_VALUE  0x0110u   // clear RESET flag, 24-bit words, SPI timeout, DRDY level
// CLOCK: CH0..7 on, OSR 1024 (011), high-resolution: 8.192 MHz / 2 / 1024 = 4 kSPS.
// The firmware reads at CV_P_RATE (2 kHz), so every read sees fresh data.
#define ADS_CLOCK_VALUE 0xFF0Eu
#define ADS_CLKIN_HZ    8192000u   // 8.192 MHz crystal on XTAL1/XTAL2
#define ADS_OSR         1024u

static inline uint32_t ads_word(uint16_t v16) { return (uint32_t)v16 << 8; }
static inline uint16_t ads_wreg(uint8_t addr, uint8_t n) { return (uint16_t)(0x6000u | ((addr & 0x3Fu) << 7) | ((n - 1u) & 0x7Fu)); }
static inline uint16_t ads_rreg(uint8_t addr, uint8_t n) { return (uint16_t)(0xA000u | ((addr & 0x3Fu) << 7) | ((n - 1u) & 0x7Fu)); }
static inline uint16_t ads_wreg_ack(uint8_t addr, uint8_t n) { return (uint16_t)(0x4000u | ((addr & 0x3Fu) << 7) | ((n - 1u) & 0x7Fu)); }
static inline int32_t ads_sext24(uint32_t w) { return (int32_t)(w << 8) >> 8; }
static inline uint16_t ads_resp16(uint32_t w) { return (uint16_t)((w >> 8) & 0xFFFFu); }
static inline bool ads_m08_id_ok(uint16_t id) { return (id >> 8) == 0x28u; }
// Unpack a big-endian byte stream of 24-bit words (as clocked out of SPI) into words.
static inline void ads_unpack(const uint8_t *b, int words, uint32_t *w) {
    for (int i = 0; i < words; i++) w[i] = (uint32_t)b[3 * i] << 16 | (uint32_t)b[3 * i + 1] << 8 | b[3 * i + 2];
}

// ---------------------------------------------------------------- PCM3168A (audio-rate codec)
// SPI control word: bit15 = R/W (0 write), bits 14..8 register, bits 7..0 data.
static inline uint16_t pcm3168_write(uint8_t reg, uint8_t val) { return (uint16_t)(((reg & 0x7Fu) << 8) | val); }
#define PCM_REG_RESET   64   // MRST SRST - - - - SRDA1 SRDA0
#define PCM_REG_DAC_FMT 65   // PSMDA MSDA2..0 FMTDA3..0
#define PCM_REG_ADC_FMT 81   // - MSAD2..0 - FMTAD2..0
#define PCM_REG_ADC_HPF 82   // - PSVAD2..0 - BYP2..0
#define PCM_REG_ADC_SE  83   // - - SEAD6..1
#define PCM_RESET_RUN   0xC0 // both resets released, auto sampling mode
#define PCM_DAC_TDM_LJ  0x07 // slave, 24-bit left-justified TDM
#define PCM_ADC_TDM_LJ  0x07 // slave, 24-bit left-justified TDM
#define PCM_ADC_HPF_OFF 0x07 // BYP = 111: HPF bypassed on all ADC pairs -> DC-coupled
#define PCM_ADC_SE_ALL  0x3F // all six ADC inputs single-ended
#define PCM_SLOTS 8          // TDM: 8 x 32-bit slots per frame, BCK = 256 fs, SCKI = 512 fs

// 24-bit sample <-> 32-bit TDM slot (left-justified).
static inline int32_t tdm_to_s24(int32_t slot) { return slot >> 8; }
static inline int32_t s24_to_tdm(int32_t s) { return (int32_t)((uint32_t)s << 8); }

// ---------------------------------------------------------------- Tier A modules (I2C)
// MCP4728: select internal 2.048 V reference and x2 gain once, then 8-byte "fast write" updates.
#define MCP4728_VREF_ALL_INT 0x8F
#define MCP4728_GAIN_ALL_X2  0xCF
static inline int mcp4728_fast_write(const int32_t *codes4, uint8_t *b) {
    for (int i = 0; i < 4; i++) {
        uint32_t c = (uint32_t)codes4[i] & 0x0FFFu;
        b[2 * i] = (uint8_t)(c >> 8);          // C2 C1 = 00 (fast write), PD = 00
        b[2 * i + 1] = (uint8_t)c;
    }
    return 8;
}
// Sequential write of channels A..D to the input registers AND the EEPROM (power-on defaults),
// with internal reference and x2 gain: 1 + 4 x 2 bytes.
static inline int mcp4728_seq_write_eeprom(const int32_t *codes4, uint8_t *b) {
    b[0] = 0x50;                                   // C2C1C0 = 010, W1W0 = 10, start channel A
    for (int i = 0; i < 4; i++) {
        uint32_t c = (uint32_t)codes4[i] & 0x0FFFu;
        b[1 + 2 * i] = (uint8_t)(0x80 | 0x10 | (c >> 8));   // VREF = 1 (internal), PD = 00, GAIN = 1 (x2)
        b[2 + 2 * i] = (uint8_t)c;
    }
    return 9;
}
// Read-back (24 bytes): per channel 3 bytes DAC register then 3 bytes EEPROM; code from bytes 1..2.
static inline int32_t mcp4728_readback_eeprom_code(const uint8_t *r24, int ch) {
    const uint8_t *e = r24 + 6 * ch + 3;
    return ((e[1] & 0x0F) << 8) | e[2];
}

// ADS1115 config: start single-shot, AINx vs GND, +-4.096 V, 860 SPS, comparator off.
static inline uint16_t ads1115_config(int ch) {
    return (uint16_t)(0x8000u | ((4u + (uint32_t)ch) << 12) | (1u << 9) | (1u << 8) | (7u << 5) | 3u);
}

// ---------------------------------------------------------------- clocks
static inline uint32_t cv_gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

// Fractional divider src/out = n + num/den with den <= den_max. Returns true if exact.
static inline bool cv_frac_div(uint32_t src_hz, uint32_t out_hz, uint32_t den_max,
                               uint32_t *n, uint32_t *num, uint32_t *den) {
    *n = src_hz / out_hz;
    uint32_t r = src_hz % out_hz;
    uint32_t g = cv_gcd(r, out_hz);
    uint32_t nu = r / (g ? g : 1), de = out_hz / (g ? g : 1);
    if (r == 0) { *num = 0; *den = 1; return true; }
    if (de <= den_max) { *num = nu; *den = de; return true; }
    // best approximation with den_max (not exact)
    *den = den_max;
    *num = (uint32_t)(((uint64_t)r * den_max + out_hz / 2) / out_hz);
    return false;
}

// USB full-speed periodic budget (USB 2.0 5.11.3, worst-case bit stuffing):
// isochronous transaction time in ns for a payload of `bytes`; total must stay <= 900 us.
static inline double usb_fs_iso_ns(uint32_t bytes) {
    return 7268.0 + 83.54 * (3.167 + 7.0 * 8.0 * (double)bytes / 6.0);
}
