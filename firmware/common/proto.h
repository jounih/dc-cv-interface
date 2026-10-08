// SysEx protocol over USB-MIDI (and BLE-MIDI): status, calibration and the precision
// CV path. Pure C, host-tested; tools/cvcal.py and Circuit Studio speak it via Web MIDI.
//
// Frame:   F0 7D 43 56 <cmd> <payload, 7-bit bytes> F7   (7D = non-commercial ID, "CV")
// Numbers: u32/f32 as 5 bytes of 7 bits, LSB first. CV values: signed 21-bit in units of
//          10 uV (+-10.48 V), 3 bytes of 7 bits, LSB first.
// Replies use <cmd | 0x40>; plain acknowledgements use 0x7F [orig_cmd, status].
//
//  cmd  request                         reply
//  0x01 GET_INFO                        0x41 fw_major fw_minor a_out a_in p_out p_in flags src batt% underruns(u32)
//        flags: b0 cal in flash, b1 audio out streaming, b2 audio in streaming, b3 codec ok,
//               b4 precision ok, b5 BLE connected, b6 MIDI mapper active
//  0x02 GET_CAL   kind ch               0x42 kind ch gain(f32) offset(f32) range    kind 0 out / 1 in; ch 0..15
//  0x03 SET_CAL   kind ch gain offset   ack (RAM; applied at once)
//  0x04 SET_RANGE ch range              ack range 0 +-10 V, 1 +-5 V, 2 0..10 V, 3 0..5 V
//  0x05 SAVE                            ack (writes NVS)
//  0x06 DEFAULTS                        ack
//  0x07 RAW_OUT   ch on code(u32)       ack (on=0 releases; code is two's complement for codec channels)
//  0x08 MEASURE   n_samples(u32)        ack, then 0x48 mean_raw(f32) x 16 input slots
//  0x09 DFU                             ack, then reboot into the ROM USB downloader
//  0x10 CV_OUT    mask_lo mask_hi v0..v7  no reply. Sets P-output targets (host source), 2 ms glide.
//                                         Doubles as the host heartbeat (send at least every 500 ms).
//  0x11 CV_IN     rate_hz(u32)          ack; device then streams 0x51 v0..v7 at that rate (0 = off, max 1000)
//  0x12 CV_SRC    s0..s7                ack; per P output: 0 off, 1 host (CV_OUT), 2 MIDI mapper
//  0x13 MAP_CFG   22 bytes (see proto.c) ack; MIDI->CV mapper settings
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cal.h"
#include "engine.h"
#include "midi_cv.h"

#define PROTO_MAX_MSG 128
enum {
    P_GET_INFO = 0x01, P_GET_CAL, P_SET_CAL, P_SET_RANGE, P_SAVE, P_DEFAULTS, P_RAW_OUT, P_MEASURE, P_DFU,
    P_CV_OUT = 0x10, P_CV_IN, P_CV_SRC, P_MAP_CFG,
    P_ACK = 0x7F,
};
enum { P_OK = 0, P_BAD_ARGS = 1, P_UNKNOWN = 2 };
enum { SRC_OFF = 0, SRC_HOST = 1, SRC_MAPPER = 2 };

typedef struct {
    cv_cal_t *cal;
    out_engine_t *out_a, *out_p;   // raw overrides
    in_measure_t *meas;
    cv_glide_t *cv;                // host CV targets (P outputs)
    uint8_t p_src[CV_P_OUT];
    uint32_t cv_in_rate;
    mcv_cfg_t map_cfg;
    bool map_dirty;
    bool host_seen;                // any valid frame since the firmware last cleared it
    uint8_t flags, src, batt_pct;
    uint32_t underruns;
    bool req_save, req_dfu;
} proto_ctx_t;

size_t proto_put_u32(uint8_t *p, uint32_t v);
uint32_t proto_get_u32(const uint8_t *p);
size_t proto_put_f32(uint8_t *p, float f);
float proto_get_f32(const uint8_t *p);
size_t proto_put_cv(uint8_t *p, float volts);   // 3 bytes
float proto_get_cv(const uint8_t *p);

size_t proto_handle(proto_ctx_t *ctx, const uint8_t *msg, size_t len, uint8_t *reply, size_t cap);
size_t proto_measure_reply(proto_ctx_t *ctx, uint8_t *reply, size_t cap);
size_t proto_cv_in_frame(const float *volts, uint8_t *out, size_t cap);
size_t proto_build(uint8_t cmd, const uint8_t *payload, size_t n, uint8_t *out, size_t cap);
