// Converter drivers: DAC8568 and ADS131M04 on PIO, streamed by DMA rings.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cv_config.h"

#define CONV_RING 256u   // samples per ring (~5.3 ms at 48 kHz)

extern uint32_t dac_ring[CONV_RING * CV_N_OUT];   // one 32-bit DAC frame per channel per sample
extern uint32_t adc_ring[CONV_RING * 6];          // ADS131M04 frames (status, ch0..3, crc)

// Safe power-on: software reset (DAC8568B: midscale, internal reference off,
// so the ratiometric output stage sits at 0 V), write the 0 V codes, then
// enable the reference. Blocking; call before anything else.
void conv_dac_init(const uint16_t *zero_codes);
// Start the DMA ring (contents must be valid). Paced at CV_N_OUT x 48 kHz by a DMA timer.
void conv_dac_start(void);
uint32_t conv_dac_hw_sample(void);   // ring sample index the DMA is reading

// Bring up the ADC (clock, reset, registers). Returns false if no ADS131M04 answers.
bool conv_adc_init(uint16_t *id_out);
void conv_adc_start(void);
uint32_t conv_adc_hw_sample(void);   // number of complete samples in the ring (mod CONV_RING)
