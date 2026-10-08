#include "midi_cv.h"
#include <string.h>

void mcv_default_cfg(mcv_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->midi_ch = 16;          // omni
    c->pitch_out = 0;         // CV Out P1 = pitch
    c->gate_out = 1;          // P2 = gate
    c->vel_out = 2;           // P3 = velocity
    c->ref_note = 48;         // C3 = 0 V
    c->bend_range = 2;
    c->legato = false;
    c->retrig_ms = 2;
    c->gate_volts = 5.0f;
    c->vel_volts = 8.0f;
    c->cc[0].cc = 1;  c->cc[0].out = 3;  c->cc[0].bipolar = false;   // mod wheel -> P4
    c->cc[1].cc = 74; c->cc[1].out = 4;  c->cc[1].bipolar = false;   // brightness -> P5
    c->cc[2].out = -1;
    c->cc[3].out = -1;
}

void mcv_init(mcv_t *m, const mcv_cfg_t *c) {
    memset(m, 0, sizeof *m);
    m->cfg = *c;
    m->note = -1;
    uint32_t mask = 0;
    if (c->pitch_out >= 0) mask |= 1u << c->pitch_out;
    if (c->gate_out >= 0) mask |= 1u << c->gate_out;
    if (c->vel_out >= 0) mask |= 1u << c->vel_out;
    for (int i = 0; i < MCV_CC_MAPS; i++) if (c->cc[i].out >= 0) mask |= 1u << c->cc[i].out;
    m->drive_mask = mask;
}

static void stack_remove(mcv_t *m, uint8_t note) {
    for (int i = 0; i < m->n_held; i++) {
        if (m->stack[i] == note) {
            memmove(&m->stack[i], &m->stack[i + 1], (size_t)(m->n_held - i - 1));
            m->n_held--;
            return;
        }
    }
}

static void note_on(mcv_t *m, uint8_t note, uint8_t vel) {
    bool was_held = m->n_held > 0;
    stack_remove(m, note);
    if (m->n_held == MCV_STACK) {            // full: drop the oldest
        memmove(&m->stack[0], &m->stack[1], MCV_STACK - 1);
        m->n_held--;
    }
    m->stack[m->n_held++] = note;
    m->note = note;
    m->velocity = vel;
    if (was_held && !m->cfg.legato) m->retrig_left_ms = m->cfg.retrig_ms;
    m->gate = true;
}

static void note_off(mcv_t *m, uint8_t note) {
    stack_remove(m, note);
    if (m->n_held > 0) {
        // Last-note priority: fall back to the most recent still-held note, legato.
        m->note = m->stack[m->n_held - 1];
    } else {
        m->gate = false;
        m->retrig_left_ms = 0;
    }
}

void mcv_midi(mcv_t *m, const uint8_t *msg, int len) {
    if (len < 1 || msg[0] < 0x80 || msg[0] >= 0xF0) return;
    uint8_t type = msg[0] & 0xF0, ch = msg[0] & 0x0F;
    if (m->cfg.midi_ch < 16 && ch != m->cfg.midi_ch) return;
    switch (type) {
    case 0x90:
        if (len < 3) return;
        if (msg[2] == 0) note_off(m, msg[1]); else note_on(m, msg[1], msg[2]);
        break;
    case 0x80:
        if (len < 3) return;
        note_off(m, msg[1]);
        break;
    case 0xE0:
        if (len < 3) return;
        m->bend = (int16_t)(((msg[2] & 0x7F) << 7 | (msg[1] & 0x7F)) - 8192);
        break;
    case 0xB0:
        if (len < 3) return;
        if (msg[1] == 123 || msg[1] == 120) { mcv_all_off(m); break; }   // all notes / sound off
        for (int i = 0; i < MCV_CC_MAPS; i++)
            if (m->cfg.cc[i].out >= 0 && m->cfg.cc[i].cc == msg[1]) m->cc_val[i] = (float)(msg[2] & 0x7F) / 127.0f;
        break;
    default:
        break;
    }
}

void mcv_tick_ms(mcv_t *m, uint32_t ms) {
    m->retrig_left_ms = (m->retrig_left_ms > ms) ? m->retrig_left_ms - ms : 0;
}

void mcv_all_off(mcv_t *m) {
    m->n_held = 0;
    m->gate = false;
    m->retrig_left_ms = 0;
}

float mcv_pitch_volts(const mcv_t *m) {
    if (m->note < 0) return 0.0f;
    float semis = (float)(m->note - (int)m->cfg.ref_note) + (float)m->bend * (float)m->cfg.bend_range / 8192.0f;
    return semis / 12.0f;
}

bool mcv_out(const mcv_t *m, int out, float *volts) {
    if (out < 0 || out >= MCV_OUTS || !(m->drive_mask & (1u << out))) return false;
    if (out == m->cfg.pitch_out) { *volts = mcv_pitch_volts(m); return true; }
    if (out == m->cfg.gate_out) { *volts = (m->gate && m->retrig_left_ms == 0) ? m->cfg.gate_volts : 0.0f; return true; }
    if (out == m->cfg.vel_out) { *volts = (float)m->velocity / 127.0f * m->cfg.vel_volts; return true; }
    for (int i = 0; i < MCV_CC_MAPS; i++) {
        if (m->cfg.cc[i].out == out) {
            float v = m->cc_val[i];
            *volts = m->cfg.cc[i].bipolar ? (v * 2.0f - 1.0f) * 5.0f : v * 10.0f;
            return true;
        }
    }
    return false;
}
