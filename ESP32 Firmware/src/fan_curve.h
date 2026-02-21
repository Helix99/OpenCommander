#pragma once
#include "shared_types.h"

// Evaluate a fan curve at a given temperature.
// Performs linear interpolation between breakpoints.
// Returns 0–100 (percent).
uint8_t fanCurve_evaluate(const FanCurve& curve, float tempC);

// Apply auto-mode fan curves to all fans based on their mapped sensor.
// Respects hysteresis: only adjusts if change exceeds HYSTERESIS_DEGREES.
// Also checks for the global safety temperature limit.
void fanCurve_applyAll(FanState fans[NUM_FANS],
                       const SensorState sensors[NUM_TEMP_SENSORS],
                       uint8_t numSensors);
