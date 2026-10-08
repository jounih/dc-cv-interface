// Pico 2 pin map. Must match hw/netlist.csv and hw/breadboard.md.
#pragma once

// DAC8568 (PIO0 SM0). SYNC must be PIN_DAC_SCLK + 1 (side-set pair).
#define PIN_DAC_SCLK 2   // Pico pin 4
#define PIN_DAC_SYNC 3   // Pico pin 5
#define PIN_DAC_DIN  4   // Pico pin 6

// ADS131M04 (PIO0 SM1). DRDY must be PIN_ADC_DOUT + 1 (IN pin pair).
#define PIN_ADC_SCLK  10  // Pico pin 14
#define PIN_ADC_CS    11  // Pico pin 15
#define PIN_ADC_DOUT  12  // Pico pin 16
#define PIN_ADC_DRDY  13  // Pico pin 17
#define PIN_ADC_DIN   14  // Pico pin 19 (CPU-held low while streaming)
#define PIN_ADC_RESET 15  // Pico pin 20 (SYNC/RESET, active low)
#define PIN_ADC_CLKIN 21  // Pico pin 27 (GPOUT0 -> 6.144 MHz)

#define PIN_LED 25        // on-board LED

#define CV_SYS_HZ 150000000u   // RP2350 default system clock
