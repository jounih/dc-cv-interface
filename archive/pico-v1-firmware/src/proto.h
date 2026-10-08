// SysEx control protocol (status + calibration). Pure C, host-tested.
//
// Frame:   F0 7D 43 56 <cmd> <payload, 7-bit bytes> F7
//          (0x7D = non-commercial manufacturer ID, 43 56 = "CV")
// Numbers: u32 / f32 (bit pattern) as 5 bytes of 7 bits, least significant first.
// Replies use <cmd | 0x40>; plain acknowledgements use cmd 0x7F: [orig_cmd, status].
//
//  cmd  request payload                 reply
//  0x01 GET_INFO  -                      0x41 fw_major fw_minor n_out n_in flags underruns(u32)
//                                         flags: b0 cal from flash, b1 out stream on, b2 in stream on, b3 adc ok
//  0x02 GET_CAL   kind ch                0x42 kind ch gain(f32) offset(f32) range
//                 kind 0 = output, 1 = input
//  0x03 SET_CAL   kind ch gain offset    ack   (RAM only, applied immediately)
//  0x04 SET_RANGE ch range               ack   range: 0 +-10 V, 1 +-5 V, 2 0..10 V, 3 0..5 V
//  0x05 SAVE      -                      ack   (writes the table to flash)
//  0x06 DEFAULTS  -                      ack   (nominal-component table, RAM)
//  0x07 RAW_OUT   ch code(u32)           ack   code > 65535 releases the channel
//  0x08 MEASURE   n_samples(u32)         ack, then 0x48 mean_raw(f32) x n_in when done
//  0x09 BOOTSEL   -                      ack, then reboot into the USB bootloader
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cal.h"
#include "engine.h"

#define PROTO_MAX_MSG 64
enum {
    P_GET_INFO = 0x01, P_GET_CAL, P_SET_CAL, P_SET_RANGE, P_SAVE, P_DEFAULTS, P_RAW_OUT, P_MEASURE, P_BOOTSEL,
    P_ACK = 0x7F,
};
enum { P_OK = 0, P_BAD_ARGS = 1, P_UNKNOWN = 2 };

typedef struct {
    cv_cal_t *cal;
    out_engine_t *out;
    in_measure_t *meas;
    uint8_t flags;            // reported in GET_INFO (b1..b3 maintained by firmware)
    bool req_save;            // set by SAVE, consumed by firmware
    bool req_bootsel;         // set by BOOTSEL, consumed by firmware
} proto_ctx_t;

size_t proto_put_u32(uint8_t *p, uint32_t v);
uint32_t proto_get_u32(const uint8_t *p);
size_t proto_put_f32(uint8_t *p, float f);
float proto_get_f32(const uint8_t *p);

// Handle one complete SysEx message (F0..F7). Returns reply length (0 = none).
size_t proto_handle(proto_ctx_t *ctx, const uint8_t *msg, size_t len, uint8_t *reply, size_t cap);
// Build the MEASURE result message once ctx->meas->done. Returns length.
size_t proto_measure_reply(proto_ctx_t *ctx, uint8_t *reply, size_t cap);
// Build a request (used by tests and mirrored by tools/cvcal.py).
size_t proto_build(uint8_t cmd, const uint8_t *payload, size_t n, uint8_t *out, size_t cap);
