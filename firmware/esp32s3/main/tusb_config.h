#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#include "cv_config.h"
#include "usb_desc.h"

#define CFG_TUSB_OS            OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH   freertos/
#define CFG_TUSB_DEBUG         0
#define CFG_TUD_ENABLED        1
#define CFG_TUD_MAX_SPEED      OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_AUDIO  1
#define CFG_TUD_MIDI   1
#define CFG_TUD_CDC    0
#define CFG_TUD_MSC    0
#define CFG_TUD_HID    0
#define CFG_TUD_VENDOR 0

// UAC2 function: group A (PCM3168A), 32 kHz, 8 out / 6 in, 16-bit
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE        CV_AUDIO_RATE
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                 1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX          CV_A_OUT
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX  CV_A_BYTES
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX          CV_EP_OUT_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ       (4 * CV_EP_OUT_SIZE)   // FIFO-count feedback holds it half full
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP            1
#define CFG_TUD_AUDIO_ENABLE_EP_IN                  1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX          CV_A_IN
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX  CV_A_BYTES
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX           CV_EP_IN_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ        (4 * CV_EP_IN_SIZE)
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL            1

// USB-MIDI 1.0: SysEx control, precision CV path, MIDI -> CV
#define CFG_TUD_MIDI_RX_BUFSIZE 256
#define CFG_TUD_MIDI_TX_BUFSIZE 512

#endif
