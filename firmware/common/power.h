// Power supervision: supply source, analog rail health, battery state, display dimming.
// Pure C, host-tested. The firmware calls power_update() every 10 ms.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum { PWR_SRC_USB = 0, PWR_SRC_RACK, PWR_SRC_BATTERY } pwr_src_t;

typedef struct {
    bool vbus;          // USB 5 V present (TinyUSB / VBUS sense)
    bool rack;          // Eurorack +-12 V header powered (rail above the DC-DC's diode-OR point)
    int rail_mv;        // sensed +11 V analog rail (after LDO), mV
    int batt_mv;        // LiPo voltage, mV (0 = no battery)
    bool user_activity; // knob/button/MIDI/host traffic this tick
} pwr_in_t;

typedef struct {
    pwr_src_t src;
    bool rails_ok;          // rails above threshold and stable for PWR_STABLE_MS
    bool mute;              // outputs forced to 0 V
    bool low_battery;       // warn
    bool shutdown;          // critical: outputs 0 V, then deep sleep
    int batt_pct;           // 0..100, -1 = no battery
    uint8_t backlight;      // 0..255
    uint32_t stable_ms, idle_ms, crit_ms;
} pwr_state_t;

#define PWR_RAIL_OK_MV    10500   // +11 V nominal rail; op-amps need >10.55 V for +-10.2 V
#define PWR_RAIL_HYST_MV  300
#define PWR_STABLE_MS     100
#define PWR_BATT_LOW_MV   3450
#define PWR_BATT_CRIT_MV  3300
#define PWR_CRIT_HOLD_MS  2000
#define PWR_DIM_AFTER_MS  30000
#define PWR_OFF_AFTER_MS  300000

int  power_batt_percent(int mv);
void power_init(pwr_state_t *s);
void power_update(pwr_state_t *s, const pwr_in_t *in, uint32_t dt_ms);
