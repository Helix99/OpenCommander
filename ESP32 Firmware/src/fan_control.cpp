#include "fan_control.h"

static const uint8_t FAN_PINS[NUM_FANS] = {
    PIN_PWM_FAN1, PIN_PWM_FAN2, PIN_PWM_FAN3,
    PIN_PWM_FAN4, PIN_PWM_FAN5, PIN_PWM_FAN6
};

void fanControl_init() {
    for (uint8_t i = 0; i < NUM_FANS; i++) {
        // arduino-esp32 v3.x: ledcAttach(pin, freq, resolution)
        ledcAttach(FAN_PINS[i], PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
        ledcWrite(FAN_PINS[i], fanControl_percentToRaw(PWM_MIN_PERCENT));
    }
}

uint8_t fanControl_percentToRaw(uint8_t percent) {
    // Enforce floor when duty > 0
    if (percent == 0) return 0;
    if (percent < PWM_MIN_PERCENT) percent = PWM_MIN_PERCENT;
    if (percent > 100) percent = 100;
    return (uint8_t)((percent * 255UL) / 100UL);
}

void fanControl_writeRaw(uint8_t fanIndex, uint8_t raw) {
    if (fanIndex >= NUM_FANS) return;
    ledcWrite(FAN_PINS[fanIndex], raw);
}

void fanControl_setTarget(uint8_t fanIndex, uint8_t percent) {
    // Caller is responsible for updating FanState.targetPercent.
    // This just writes PWM directly for immediate manual overrides
    // called outside the ramp loop (e.g. safety override).
    if (fanIndex >= NUM_FANS) return;
    if (percent > 100) percent = 100;
    fanControl_writeRaw(fanIndex, fanControl_percentToRaw(percent));
}

void fanControl_rampTick(FanState fans[NUM_FANS]) {
    for (uint8_t i = 0; i < NUM_FANS; i++) {
        uint8_t target  = fans[i].targetPercent;
        uint8_t current = fans[i].currentPercent;

        if (current < target) {
            uint8_t next = current + PWM_RAMP_STEP;
            fans[i].currentPercent = (next > target) ? target : next;
        } else if (current > target) {
            uint8_t next = (current > PWM_RAMP_STEP) ? current - PWM_RAMP_STEP : 0;
            fans[i].currentPercent = (next < target) ? target : next;
        }

        fanControl_writeRaw(i, fanControl_percentToRaw(fans[i].currentPercent));
    }
}

void fanControl_safetyOverride() {
    for (uint8_t i = 0; i < NUM_FANS; i++) {
        fanControl_writeRaw(i, 255);
    }
}
