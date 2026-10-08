// Build-time configuration, v2 (ESP32-S3 controller). Pure header shared by the
// firmware and the host tests.
//
// Two channel groups:
//  A  audio-rate, DC-coupled: PCM3168A codec, 8 out / 6 in, streamed as USB Audio
//     Class 2 at 32 kHz / 16-bit (what USB full speed can carry; see README "USB budget").
//     Calibrated, good for audio, modulation and envelopes; not specified for 1 V/oct pitch.
//  P  precision CV: DAC8568 (8 out) + ADS131M08 (8 in), 2 kHz update, carried over
//     USB-MIDI SysEx (Web MIDI) and driven by the MIDI->CV mapper (USB-MIDI and BLE-MIDI).
#pragma once

#define CV_AUDIO_RATE 32000u
#define CV_A_OUT 8
#define CV_A_IN 6
#define CV_A_BYTES 2             // USB sample size, both directions

#define CV_P_OUT 8
#define CV_P_IN 8
#define CV_P_RATE 2000u          // precision DAC update / ADC read rate (Hz)

// Channel index spaces used by the calibration table and the UI.
#define CV_MAX_OUT 16            // 0..7 = A outs (codec), 8..15 = P outs (DAC8568)
#define CV_MAX_IN 16             // 0..5 = A ins (codec), 8..15 = P ins (ADS131M08)
#define CV_P_OUT0 8
#define CV_P_IN0 8

// Scale convention shared with Circuit Studio: digital full scale (1.0) = 10 V.
#define CV_VOLTS_FS 10.0f

// Output watchdog: hold, then ramp to 0 V when host data stops.
#define CV_HOLD_MS 20u
#define CV_RAMP_V_PER_MS 2.0f    // 10 V -> 0 V in 5 ms

#define CV_FW_MAJOR 0
#define CV_FW_MINOR 2
