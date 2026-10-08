// LilyGO T-Display-S3 on the CV interface carrier. Must match hw/netlist.csv.
// The board's own pins (LCD 5-9/38-42/45-48, power-on 15, buttons 0/14, battery 4) are fixed;
// the carrier uses all 13 header GPIOs: 1 2 3 10 11 12 13 16 17 18 21 43 44.
#pragma once

// PCM3168A, I2S0 TDM (S3 is master; codec in slave mode)
#define PIN_I2S_MCLK   16   // SCKI = 512 fs = 16.384 MHz
#define PIN_I2S_BCLK   17   // BCKAD + BCKDA = 256 fs
#define PIN_I2S_WS     18   // LRCKAD + LRCKDA
#define PIN_I2S_DOUT   21   // -> DIN1 (DAC TDM, 8 channels)
#define PIN_I2S_DIN    43   // <- DOUT1 (ADC TDM, 6 channels). Was UART0 TX: logs go to the display/SysEx.

// SPI2 (precision converters + codec control), mode 1
#define PIN_SPI_SCLK   12
#define PIN_SPI_MOSI   11
#define PIN_SPI_MISO   13
#define PIN_CS_DAC     44   // DAC8568 SYNC (10 k pull-up)
#define PIN_CS_ADC     10   // ADS131M08 CS (10 k pull-up)
#define PIN_CS_CODEC   3    // PCM3168A MS (10 k pull-up). GPIO3 is a strapping pin: the pull-up keeps it benign.

#define PIN_RACK_SENSE 2    // Eurorack +12 V present (33 k / 10 k divider), digital input
#define PIN_RAIL_SENSE 1    // ADC1_CH0: +11 V rail through 100 k / 10 k

// T-Display-S3 fixed pins
#define PIN_POWER_ON   15
#define PIN_BATT_ADC   4    // ADC1_CH3, x2 divider on the board (valid only without USB-C)
#define PIN_BUTTON_1   0
#define PIN_BUTTON_2   14
#define PIN_LCD_BL     38
#define PIN_LCD_D0     39
#define PIN_LCD_D1     40
#define PIN_LCD_D2     41
#define PIN_LCD_D3     42
#define PIN_LCD_D4     45
#define PIN_LCD_D5     46
#define PIN_LCD_D6     47
#define PIN_LCD_D7     48
#define PIN_LCD_RES    5
#define PIN_LCD_CS     6
#define PIN_LCD_DC     7
#define PIN_LCD_WR     8
#define PIN_LCD_RD     9
#define LCD_W 320
#define LCD_H 170

// Rail sense divider: 100 k / 10 k -> 11 V reads 1.0 V.
#define RAIL_SENSE_RATIO 11.0f

// Core plan: core 1 = USB + audio I/O + precision CV (real time); core 0 = BLE, display, power.
#define CORE_RT  1
#define CORE_APP 0
