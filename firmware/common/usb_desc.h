// USB descriptors: UAC2 audio function (N out / M in, 48 kHz, async with
// feedback) + USB-MIDI (SysEx control). Pure byte arrays, host-tested.
#pragma once
#include <stdint.h>
#include "cv_config.h"

#define CV_USB_VID 0x1209   // pid.codes
#define CV_USB_PID 0x0001   // pid.codes test PID: private use only; apply for a PID before sharing builds
#define CV_PRODUCT "Circuit Studio CV 8x6"   // UAC2 channel counts; Circuit Studio matches this

enum { ITF_AC = 0, ITF_AS_OUT = 1, ITF_AS_IN = 2, ITF_MIDI_AC = 3, ITF_MIDI_MS = 4, ITF_COUNT = 5 };

#define EP_AUDIO_OUT 0x01
#define EP_AUDIO_FB  0x81
#define EP_AUDIO_IN  0x82
#define EP_MIDI_OUT  0x03
#define EP_MIDI_IN   0x83

// UAC2 entity IDs (group A, PCM3168A).
#define UAC2_ENTITY_CLOCK   0x10
#define UAC2_ENTITY_USB_IT  0x01   // host -> device stream
#define UAC2_ENTITY_LINE_OT 0x02   // DAC jacks
#define UAC2_ENTITY_LINE_IT 0x03   // ADC jacks
#define UAC2_ENTITY_USB_OT  0x04   // device -> host stream

// String indices.
enum { STR_LANG = 0, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_AUDIO, STR_MIDI, STR_CH_OUT1 };
#define STR_CH_IN1 (STR_CH_OUT1 + CV_A_OUT)
#define STR_COUNT  (STR_CH_IN1 + CV_A_IN)

// Max packet sizes on full speed: (32 + 1) samples per 1 ms frame.
#define CV_EP_OUT_SIZE ((CV_AUDIO_RATE / 1000 + 1) * CV_A_BYTES * CV_A_OUT)
#define CV_EP_IN_SIZE  ((CV_AUDIO_RATE / 1000 + 1) * CV_A_BYTES * CV_A_IN)
#define CV_EP_FB_SIZE  4
#define CV_EP_MIDI_SIZE 64

// Lengths.
#define CV_AC_CS_LEN     (9 + 8 + 17 + 12 + 17 + 12)
#define CV_AS_OUT_LEN    (9 + 9 + 16 + 6 + 7 + 8 + 7)
#define CV_AS_IN_LEN     (9 + 9 + 16 + 6 + 7 + 8)
#define CV_AUDIO_FUNC_LEN (8 + 9 + CV_AC_CS_LEN + CV_AS_OUT_LEN + CV_AS_IN_LEN)
#define CV_MIDI_MS_CS_LEN (7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5)
#define CV_MIDI_LEN      (9 + 9 + 9 + CV_MIDI_MS_CS_LEN)
#define CV_CONFIG_LEN    (9 + CV_AUDIO_FUNC_LEN + CV_MIDI_LEN)

extern const uint8_t cv_desc_device[18];
extern const uint8_t cv_desc_config[CV_CONFIG_LEN];      // feedback EP 4 bytes
extern const uint8_t cv_desc_config_fb3[CV_CONFIG_LEN];  // feedback EP 3 bytes (macOS FS)
extern const uint8_t cv_desc_bos[12];
// ASCII strings by index (index 0 = language, STR_SERIAL filled at runtime).
const char *cv_string(uint8_t index);
