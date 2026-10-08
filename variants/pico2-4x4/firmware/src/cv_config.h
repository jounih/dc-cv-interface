// Build-time configuration for the CV interface. Pure header: used by the
// firmware and by the host-side unit tests.
#pragma once

// Channel counts. CV_N_OUT is 4 (default board) or 8 (all DAC8568 channels
// populated). CV_N_IN is 4 (ADS131M04).
#ifndef CV_N_OUT
#define CV_N_OUT 4
#endif
#ifndef CV_N_IN
#define CV_N_IN 4
#endif

#if CV_N_OUT != 4 && CV_N_OUT != 8
#error "CV_N_OUT must be 4 or 8"
#endif
#if CV_N_IN != 4
#error "CV_N_IN must be 4 (ADS131M04)"
#endif

#define CV_MAX_OUT 8
#define CV_MAX_IN 4

#define CV_SAMPLE_RATE 48000u

// USB sample formats. OUT (host -> DAC) is 16-bit: the DAC is 16-bit.
// IN (ADC -> host) is 24-bit packed when 4 outputs are used. With 8 outputs
// the full-speed isochronous budget (~1 KB/frame comfortably) forces 16-bit IN.
#define CV_OUT_BYTES 2
#if CV_N_OUT == 4
#define CV_IN_BYTES 3
#else
#define CV_IN_BYTES 2
#endif
#define CV_IN_BITS (CV_IN_BYTES * 8)

// Scale convention shared with Circuit Studio: digital full scale (1.0) = 10 V.
#define CV_VOLTS_FS 10.0f

// Watchdog behaviour when the host stops sending output samples.
#define CV_HOLD_SAMPLES (CV_SAMPLE_RATE / 50)   // 20 ms: ride through USB hiccups
#define CV_RAMP_VOLTS_PER_SAMPLE (10.0f / (CV_SAMPLE_RATE / 200)) // 10 V in 5 ms

// Firmware version reported over SysEx.
#define CV_FW_MAJOR 0
#define CV_FW_MINOR 1
