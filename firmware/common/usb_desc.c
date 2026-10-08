#include "usb_desc.h"
#include <stddef.h>

#define U16(x) (uint8_t)((x) & 0xFF), (uint8_t)(((x) >> 8) & 0xFF)
#define U32(x) U16((x) & 0xFFFF), U16(((x) >> 16) & 0xFFFF)

const uint8_t cv_desc_device[18] = {
    18, 0x01, U16(0x0201),            // 2.01: BOS present (OS guessing quirk)
    0xEF, 0x02, 0x01,            // misc / IAD composite
    64,                          // EP0 size
    U16(CV_USB_VID), U16(CV_USB_PID), U16(0x0100 | CV_FW_MINOR),
    STR_MANUF, STR_PRODUCT, STR_SERIAL,
    1,
};

#define CV_CONFIG_BYTES(FBSZ) \
 \
    9, 0x02, U16(CV_CONFIG_LEN), ITF_COUNT, 1, 0, 0x80, 250, \
 \
 \
 \
    8, 0x0B, ITF_AC, 3, 0x01, 0x00, 0x20, STR_AUDIO, \
 \
    9, 0x04, ITF_AC, 0, 0, 0x01, 0x01, 0x20, STR_AUDIO, \
 \
    9, 0x24, 0x01, U16(0x0200), 0x08, U16(CV_AC_CS_LEN), 0x00, \
 \
    8, 0x24, 0x0A, UAC2_ENTITY_CLOCK, 0x01, 0x05, 0x00, 0x00, \
 \
    17, 0x24, 0x02, UAC2_ENTITY_USB_IT, U16(0x0101), 0x00, UAC2_ENTITY_CLOCK, \
        CV_A_OUT, U32(0), STR_CH_OUT1, U16(0), 0, \
 \
    12, 0x24, 0x03, UAC2_ENTITY_LINE_OT, U16(0x0603), 0x00, UAC2_ENTITY_USB_IT, UAC2_ENTITY_CLOCK, U16(0), 0, \
 \
    17, 0x24, 0x02, UAC2_ENTITY_LINE_IT, U16(0x0603), 0x00, UAC2_ENTITY_CLOCK, \
        CV_A_IN, U32(0), STR_CH_IN1, U16(0), 0, \
 \
    12, 0x24, 0x03, UAC2_ENTITY_USB_OT, U16(0x0101), 0x00, UAC2_ENTITY_LINE_IT, UAC2_ENTITY_CLOCK, U16(0), 0, \
 \
 \
    9, 0x04, ITF_AS_OUT, 0, 0, 0x01, 0x02, 0x20, 0, \
    9, 0x04, ITF_AS_OUT, 1, 2, 0x01, 0x02, 0x20, 0, \
    16, 0x24, 0x01, UAC2_ENTITY_USB_IT, 0x00, 0x01, U32(1), CV_A_OUT, U32(0), 0, \
    6, 0x24, 0x02, 0x01, CV_A_BYTES, CV_A_BYTES * 8, \
    7, 0x05, EP_AUDIO_OUT, 0x05, U16(CV_EP_OUT_SIZE), 1, \
    8, 0x25, 0x01, 0x00, 0x00, 0x00, U16(0), \
    7, 0x05, EP_AUDIO_FB, 0x11, U16(FBSZ), 1, \
 \
 \
    9, 0x04, ITF_AS_IN, 0, 0, 0x01, 0x02, 0x20, 0, \
    9, 0x04, ITF_AS_IN, 1, 1, 0x01, 0x02, 0x20, 0, \
    16, 0x24, 0x01, UAC2_ENTITY_USB_OT, 0x00, 0x01, U32(1), CV_A_IN, U32(0), 0, \
    6, 0x24, 0x02, 0x01, CV_A_BYTES, CV_A_BYTES * 8, \
    7, 0x05, EP_AUDIO_IN, 0x05, U16(CV_EP_IN_SIZE), 1, \
    8, 0x25, 0x01, 0x00, 0x00, 0x00, U16(0), \
 \
 \
    9, 0x04, ITF_MIDI_AC, 0, 0, 0x01, 0x01, 0x00, 0, \
    9, 0x24, 0x01, U16(0x0100), U16(9), 1, ITF_MIDI_MS, \
    9, 0x04, ITF_MIDI_MS, 0, 2, 0x01, 0x03, 0x00, STR_MIDI, \
    7, 0x24, 0x01, U16(0x0100), U16(CV_MIDI_MS_CS_LEN), \
    6, 0x24, 0x02, 0x01, 0x01, 0, \
    6, 0x24, 0x02, 0x02, 0x02, 0, \
    9, 0x24, 0x03, 0x01, 0x03, 1, 0x02, 0x01, 0, \
    9, 0x24, 0x03, 0x02, 0x04, 1, 0x01, 0x01, 0, \
    9, 0x05, EP_MIDI_OUT, 0x02, U16(CV_EP_MIDI_SIZE), 0, 0, 0, \
    5, 0x25, 0x01, 1, 0x01, \
    9, 0x05, EP_MIDI_IN, 0x02, U16(CV_EP_MIDI_SIZE), 0, 0, 0, \
    5, 0x25, 0x01, 1, 0x03,

// Feedback EP: 4 bytes (16.16, Windows/Linux) by default; macOS on full speed
// wants the spec's 3-byte 10.14 form (selected at enumeration by the OS-guessing quirk).
const uint8_t cv_desc_config[CV_CONFIG_LEN] = { CV_CONFIG_BYTES(4) };
const uint8_t cv_desc_config_fb3[CV_CONFIG_LEN] = { CV_CONFIG_BYTES(3) };

const uint8_t cv_desc_bos[12] = { 5, 0x0F, U16(12), 1, 7, 0x10, 0x02, 0, 0, 0, 0 };

static const char *const strings[STR_COUNT] = {
    [STR_MANUF] = "DIY Circuit Studio",
    [STR_PRODUCT] = CV_PRODUCT,
    [STR_SERIAL] = "000000000000",
    [STR_AUDIO] = "CV Interface Audio",
    [STR_MIDI] = "CV Control",
    [STR_CH_OUT1 + 0] = "Audio Out 1", [STR_CH_OUT1 + 1] = "Audio Out 2",
    [STR_CH_OUT1 + 2] = "Audio Out 3", [STR_CH_OUT1 + 3] = "Audio Out 4",
    [STR_CH_OUT1 + 4] = "Audio Out 5", [STR_CH_OUT1 + 5] = "Audio Out 6",
    [STR_CH_OUT1 + 6] = "Audio Out 7", [STR_CH_OUT1 + 7] = "Audio Out 8",
    [STR_CH_IN1 + 0] = "Audio In 1", [STR_CH_IN1 + 1] = "Audio In 2",
    [STR_CH_IN1 + 2] = "Audio In 3", [STR_CH_IN1 + 3] = "Audio In 4",
    [STR_CH_IN1 + 4] = "Audio In 5", [STR_CH_IN1 + 5] = "Audio In 6",
};

const char *cv_string(uint8_t index) {
    return (index < STR_COUNT) ? strings[index] : NULL;
}
