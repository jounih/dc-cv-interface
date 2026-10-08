#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#include "cv_config.h"
#include "usb_desc.h"

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif
#define CFG_TUSB_DEBUG        0
#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     OPT_MODE_FULL_SPEED
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_QUIRK_OS_GUESSING 1   // macOS full speed wants a 3-byte (10.14) feedback EP

#define CFG_TUD_AUDIO  1
#define CFG_TUD_MIDI   1
#define CFG_TUD_CDC    0
#define CFG_TUD_MSC    0
#define CFG_TUD_HID    0
#define CFG_TUD_VENDOR 0

// ---------------- UAC2 function
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN       CV_AUDIO_FUNC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT       2
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ    64
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE CV_SAMPLE_RATE

// Host -> DAC (RX on the device)
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                 1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX          CV_N_OUT
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX  CV_OUT_BYTES
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX          CV_EP_OUT_SIZE
// 4 packets of FIFO; the FIFO-count feedback holds it half full (~2 ms).
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ       (4 * CV_EP_OUT_SIZE)
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP            1

// ADC -> host (TX on the device)
#define CFG_TUD_AUDIO_ENABLE_EP_IN                  1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX          CV_N_IN
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX  CV_IN_BYTES
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX           CV_EP_IN_SIZE
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ        (4 * CV_EP_IN_SIZE)
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL            1   // 47/48/49-sample packets from FIFO level

// ---------------- USB-MIDI (SysEx control)
#define CFG_TUD_MIDI_RX_BUFSIZE 128
#define CFG_TUD_MIDI_TX_BUFSIZE 128

#endif
