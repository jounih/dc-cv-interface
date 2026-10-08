// BLE-MIDI packet codec (Apple / MMA "MIDI over Bluetooth LE" 1.0). Pure C, host-tested.
// Service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BLEMIDI_SYSEX_MAX 128

typedef void (*blemidi_emit_fn)(void *user, const uint8_t *msg, int len, uint16_t ts13);

typedef struct {
    uint8_t running;            // running status (channel messages)
    uint8_t msg[3];
    int msg_len, msg_need;
    bool pending_ts;            // last byte was a timestamp
    uint16_t ts;                // 13-bit ms timestamp of the current message
    uint8_t ts_high;
    bool in_sysex;
    uint8_t sysex[BLEMIDI_SYSEX_MAX];
    int sysex_len;
    bool sysex_overflow;
    uint32_t bad_packets;
} blemidi_rx_t;

void blemidi_rx_init(blemidi_rx_t *r);
// Parse one characteristic write; emits complete MIDI messages (channel, system, SysEx F0..F7).
void blemidi_rx_packet(blemidi_rx_t *r, const uint8_t *p, size_t n, blemidi_emit_fn emit, void *user);

// Encode one MIDI message (channel/system, or a complete SysEx) into BLE packets of at most
// `mtu_payload` bytes each. Calls `out` per packet. Returns the number of packets.
typedef void (*blemidi_out_fn)(void *user, const uint8_t *pkt, size_t n);
int blemidi_encode(const uint8_t *msg, size_t len, uint16_t ts13, size_t mtu_payload, blemidi_out_fn out, void *user);

int midi_msg_len(uint8_t status);   // total length incl. status, 0 = variable (SysEx), -1 = invalid
