#pragma once
#include <Arduino.h>
#include <stdint.h>

// ─────────────────────────────────────────────
// Hardware Constants
// ─────────────────────────────────────────────
#define NUM_FANS            6
#define NUM_TEMP_SENSORS    5

#define PIN_PWM_FAN1        1
#define PIN_PWM_FAN2        2
#define PIN_PWM_FAN3        3
#define PIN_PWM_FAN4        4
#define PIN_PWM_FAN5        10
#define PIN_PWM_FAN6        11

#define PIN_TACH_FAN1       5
#define PIN_TACH_FAN2       6
#define PIN_TACH_FAN3       7
#define PIN_TACH_FAN4       8
#define PIN_TACH_FAN5       12
#define PIN_TACH_FAN6       13

#define PIN_ONEWIRE         9
#define PIN_LED             48

// PWM config
#define PWM_FREQ_HZ         25000
#define PWM_RESOLUTION_BITS 8
#define PWM_MIN_PERCENT     20
#define PWM_RAMP_STEP       5
#define TEMP_SAFETY_LIMIT   85.0f

// Tachometer
#define TACH_PULSES_PER_REV 2
#define TACH_CALC_INTERVAL_MS 2000
#define TACH_STALL_TIMEOUT_MS 3000

// Temperature
#define TEMP_READ_INTERVAL_MS  2000
#define TEMP_RESOLUTION_BITS   12

// Fan curve
#define MAX_CURVE_POINTS    8
#define HYSTERESIS_DEGREES  2.0f

// ─────────────────────────────────────────────
// Structs
// ─────────────────────────────────────────────

struct CurvePoint {
    float   tempC;
    uint8_t pwmPercent;
};

struct FanCurve {
    CurvePoint points[MAX_CURVE_POINTS];
    uint8_t    numPoints;
};

enum FanMode : uint8_t {
    FAN_MODE_AUTO   = 0,
    FAN_MODE_MANUAL = 1
};

struct FanState {
    uint8_t  targetPercent;
    uint8_t  currentPercent;
    uint16_t rpm;
    FanMode  mode;
    uint8_t  sensorIndex;
    FanCurve curve;
    bool     stalled;
};

struct SensorState {
    uint8_t rom[8];
    float   tempC;
    bool    valid;
    char    name[32];
};

struct SystemState {
    FanState    fans[NUM_FANS];
    SensorState sensors[NUM_TEMP_SENSORS];
    uint8_t     numSensorsFound;
    bool        hidEnabled;
    uint32_t    bootTime;
};
