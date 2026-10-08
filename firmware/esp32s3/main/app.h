// Shared state between the firmware tasks.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cv_config.h"
#include "cal.h"
#include "engine.h"
#include "proto.h"
#include "midi_cv.h"
#include "power.h"

typedef enum { TRANSPORT_USB = 0, TRANSPORT_BLE = 1 } transport_t;

typedef struct {
    cv_cal_t cal;
    out_engine_t out_a, out_p;
    in_measure_t meas;
    cv_glide_t cv;              // host CV targets for P outputs
    proto_ctx_t proto;
    mcv_t mapper;
    pwr_state_t pwr;
    SemaphoreHandle_t lock;     // guards cal, proto, mapper, cv (short critical sections only)

    // live status for the display / GET_INFO
    volatile bool usb_mounted, out_streaming, in_streaming, ble_connected;
    volatile bool codec_ok, precision_ok, cal_from_flash;
    volatile uint32_t usb_underruns, i2s_overruns, rt_late;
    float meter_out[CV_MAX_OUT], meter_in[CV_MAX_IN];   // volts, written by the RT tasks
    uint32_t host_last_ms;      // last SysEx from the host (CV path heartbeat)
} app_t;

extern app_t g_app;

static inline void app_lock(void) { xSemaphoreTake(g_app.lock, portMAX_DELAY); }
static inline void app_unlock(void) { xSemaphoreGive(g_app.lock); }
uint32_t app_ms(void);

// Modules
void storage_init(void);
bool storage_load_cal(cv_cal_t *c);
bool storage_save_cal(const cv_cal_t *c);

bool precision_init(void);       // outputs to 0 V first; returns false if the parts do not answer
void precision_start(void);

bool codec_init(void);
void codec_start(void);

void usb_start(void);
void usb_midi_send(const uint8_t *msg, size_t n);

void ble_midi_start(void);
void ble_midi_send(const uint8_t *msg, size_t n);

// MIDI from any transport (complete messages); SysEx replies go back on the same transport.
void midi_router_init(void);
void midi_router_rx(transport_t t, const uint8_t *msg, int len);
void midi_router_poll(void);     // CV_IN streaming, measurement replies, save/DFU requests

void power_start(void);
void display_start(void);
