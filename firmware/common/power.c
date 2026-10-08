#include "power.h"
#include <string.h>

// Resting LiPo open-circuit curve (mV -> %), coarse but monotonic.
static const int curve_mv[] = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
static const int curve_pc[] = {   0,    5,   10,   20,   30,   40,   50,   60,   75,   90,  100};

int power_batt_percent(int mv) {
    const int n = (int)(sizeof curve_mv / sizeof curve_mv[0]);
    if (mv <= 0) return -1;
    if (mv <= curve_mv[0]) return 0;
    if (mv >= curve_mv[n - 1]) return 100;
    for (int i = 1; i < n; i++)
        if (mv < curve_mv[i])
            return curve_pc[i - 1] + (curve_pc[i] - curve_pc[i - 1]) * (mv - curve_mv[i - 1]) / (curve_mv[i] - curve_mv[i - 1]);
    return 100;
}

void power_init(pwr_state_t *s) {
    memset(s, 0, sizeof *s);
    s->mute = true;           // until the rails have been stable once
    s->batt_pct = -1;
    s->backlight = 255;
}

void power_update(pwr_state_t *s, const pwr_in_t *in, uint32_t dt_ms) {
    s->src = in->vbus ? PWR_SRC_USB : in->rack ? PWR_SRC_RACK : PWR_SRC_BATTERY;

    // Rail health with hysteresis; any dip resets the stability timer and mutes at once.
    bool above = in->rail_mv >= (s->rails_ok ? PWR_RAIL_OK_MV - PWR_RAIL_HYST_MV : PWR_RAIL_OK_MV);
    if (!above) { s->stable_ms = 0; s->rails_ok = false; }
    else if (!s->rails_ok) {
        s->stable_ms += dt_ms;
        if (s->stable_ms >= PWR_STABLE_MS) s->rails_ok = true;
    }

    // Battery: only relevant when it is the source.
    s->batt_pct = power_batt_percent(in->batt_mv);
    bool on_batt = s->src == PWR_SRC_BATTERY && in->batt_mv > 0;
    s->low_battery = on_batt && in->batt_mv < PWR_BATT_LOW_MV;
    if (on_batt && in->batt_mv < PWR_BATT_CRIT_MV) s->crit_ms += dt_ms; else s->crit_ms = 0;
    if (s->crit_ms >= PWR_CRIT_HOLD_MS) s->shutdown = true;
    if (!on_batt) s->shutdown = false;

    s->mute = !s->rails_ok || s->shutdown;

    // Display: dim after 30 s idle, off after 5 min (on battery only), wake on activity.
    if (in->user_activity) s->idle_ms = 0; else s->idle_ms += dt_ms;
    if (s->idle_ms < PWR_DIM_AFTER_MS) s->backlight = 255;
    else if (on_batt && s->idle_ms >= PWR_OFF_AFTER_MS) s->backlight = 0;
    else s->backlight = 40;
    if (s->low_battery && s->backlight > 40) s->backlight = 120;
}
