#include "ble_midi.h"
#include <string.h>

int midi_msg_len(uint8_t s) {
    if (s < 0x80) return -1;
    switch (s & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;
    case 0xC0: case 0xD0: return 2;
    default: break;
    }
    switch (s) {
    case 0xF0: return 0;
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF7: return -1;
    default: return 1;   // F6, F8..FF
    }
}

void blemidi_rx_init(blemidi_rx_t *r) { memset(r, 0, sizeof *r); }

static void start_msg(blemidi_rx_t *r, uint8_t status) {
    r->msg[0] = status;
    r->msg_len = 1;
    r->msg_need = midi_msg_len(status);
}

void blemidi_rx_packet(blemidi_rx_t *r, const uint8_t *p, size_t n, blemidi_emit_fn emit, void *user) {
    if (n < 2 || !(p[0] & 0x80) || (p[0] & 0x40)) { r->bad_packets++; return; }
    r->ts_high = p[0] & 0x3F;
    r->pending_ts = false;
    for (size_t i = 1; i < n; i++) {
        uint8_t b = p[i];
        if (b & 0x80) {
            if (!r->pending_ts) {           // timestamp byte
                r->pending_ts = true;
                r->ts = (uint16_t)((r->ts_high << 7) | (b & 0x7F));
                continue;
            }
            r->pending_ts = false;          // status byte
            if (b >= 0xF8) { emit(user, &b, 1, r->ts); continue; }        // real-time, may sit inside SysEx
            if (b == 0xF7) {
                if (r->in_sysex) {
                    if (!r->sysex_overflow && r->sysex_len < BLEMIDI_SYSEX_MAX) {
                        r->sysex[r->sysex_len++] = 0xF7;
                        emit(user, r->sysex, r->sysex_len, r->ts);
                    }
                    r->in_sysex = false;
                }
                continue;
            }
            if (r->in_sysex) r->in_sysex = false;   // a new status aborts an unterminated SysEx
            if (b == 0xF0) {
                r->in_sysex = true;
                r->sysex_len = 0;
                r->sysex_overflow = false;
                r->sysex[r->sysex_len++] = 0xF0;
                r->msg_len = 0;
                continue;
            }
            if (b < 0xF0) r->running = b; else r->running = 0;
            start_msg(r, b);
            if (r->msg_need == 1) { emit(user, r->msg, 1, r->ts); r->msg_len = 0; }
            continue;
        }
        // data byte
        r->pending_ts = false;
        if (r->in_sysex) {
            if (r->sysex_len < BLEMIDI_SYSEX_MAX - 1) r->sysex[r->sysex_len++] = b; else r->sysex_overflow = true;
            continue;
        }
        if (r->msg_len == 0) {
            if (!r->running) continue;      // stray data
            start_msg(r, r->running);       // running status
        }
        r->msg[r->msg_len++] = b;
        if (r->msg_len == r->msg_need) {
            emit(user, r->msg, r->msg_len, r->ts);
            r->msg_len = 0;
        }
    }
}

int blemidi_encode(const uint8_t *msg, size_t len, uint16_t ts13, size_t mtu, blemidi_out_fn out, void *user) {
    uint8_t pkt[64];
    if (mtu > sizeof pkt) mtu = sizeof pkt;
    if (mtu < 5 || len == 0) return 0;
    const uint8_t hdr = (uint8_t)(0x80 | ((ts13 >> 7) & 0x3F));
    const uint8_t tsl = (uint8_t)(0x80 | (ts13 & 0x7F));
    int packets = 0;
    if (msg[0] != 0xF0) {
        pkt[0] = hdr; pkt[1] = tsl;
        memcpy(pkt + 2, msg, len);
        out(user, pkt, len + 2);
        return 1;
    }
    // SysEx: header ts F0 data...; continuation packets: header data...; end: ts F7.
    size_t i = 0, k = 0;
    pkt[k++] = hdr; pkt[k++] = tsl; pkt[k++] = 0xF0; i = 1;
    while (i < len) {
        uint8_t b = msg[i];
        size_t need = (b == 0xF7) ? 2 : 1;
        if (k + need > mtu) { out(user, pkt, k); packets++; k = 0; pkt[k++] = hdr; }
        if (b == 0xF7) pkt[k++] = tsl;
        pkt[k++] = b;
        i++;
    }
    out(user, pkt, k);
    return packets + 1;
}
