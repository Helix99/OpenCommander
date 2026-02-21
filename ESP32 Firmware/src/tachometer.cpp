#include "tachometer.h"

static const uint8_t TACH_PINS[NUM_FANS] = {
    PIN_TACH_FAN1, PIN_TACH_FAN2, PIN_TACH_FAN3,
    PIN_TACH_FAN4, PIN_TACH_FAN5, PIN_TACH_FAN6
};

// Volatile pulse counters written by ISRs
static volatile uint32_t pulseCount[NUM_FANS] = {0, 0, 0, 0, 0, 0};
// Timestamp of last pulse for stall detection
static volatile uint32_t lastPulseMs[NUM_FANS] = {0, 0, 0, 0, 0, 0};

// ISR for each fan — must live in IRAM
static void IRAM_ATTR isr_fan0() { pulseCount[0]++; lastPulseMs[0] = millis(); }
static void IRAM_ATTR isr_fan1() { pulseCount[1]++; lastPulseMs[1] = millis(); }
static void IRAM_ATTR isr_fan2() { pulseCount[2]++; lastPulseMs[2] = millis(); }
static void IRAM_ATTR isr_fan3() { pulseCount[3]++; lastPulseMs[3] = millis(); }
static void IRAM_ATTR isr_fan4() { pulseCount[4]++; lastPulseMs[4] = millis(); }
static void IRAM_ATTR isr_fan5() { pulseCount[5]++; lastPulseMs[5] = millis(); }

static void (*ISR_TABLE[NUM_FANS])() = {
    isr_fan0, isr_fan1, isr_fan2,
    isr_fan3, isr_fan4, isr_fan5
};

void tachometer_init() {
    for (uint8_t i = 0; i < NUM_FANS; i++) {
        pinMode(TACH_PINS[i], INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(TACH_PINS[i]), ISR_TABLE[i], FALLING);
    }
}

void tachometer_update(FanState fans[NUM_FANS]) {
    uint32_t now = millis();

    for (uint8_t i = 0; i < NUM_FANS; i++) {
        // Atomically snapshot and reset the pulse counter
        portDISABLE_INTERRUPTS();
        uint32_t count = pulseCount[i];
        pulseCount[i]  = 0;
        uint32_t lastMs = lastPulseMs[i];
        portENABLE_INTERRUPTS();

        // RPM = (pulses / pulses_per_rev) * (60s / interval_s)
        float intervalS = TACH_CALC_INTERVAL_MS / 1000.0f;
        if (count > 0) {
            fans[i].rpm     = (uint16_t)((count * 60.0f) / (intervalS * TACH_PULSES_PER_REV));
            fans[i].stalled = false;
        } else {
            // No pulses — check stall timeout
            fans[i].rpm = 0;
            fans[i].stalled = (now - lastMs) > TACH_STALL_TIMEOUT_MS;
        }
    }
}
