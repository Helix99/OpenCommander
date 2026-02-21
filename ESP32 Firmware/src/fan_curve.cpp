#include "fan_curve.h"
#include "fan_control.h"
#include <math.h>

uint8_t fanCurve_evaluate(const FanCurve& curve, float tempC) {
    if (curve.numPoints == 0) return PWM_MIN_PERCENT;

    // Below first breakpoint — return first point's PWM
    if (tempC <= curve.points[0].tempC) return curve.points[0].pwmPercent;

    // Above last breakpoint — return last point's PWM
    if (tempC >= curve.points[curve.numPoints - 1].tempC) {
        return curve.points[curve.numPoints - 1].pwmPercent;
    }

    // Find surrounding breakpoints and interpolate
    for (uint8_t i = 0; i < curve.numPoints - 1; i++) {
        const CurvePoint& lo = curve.points[i];
        const CurvePoint& hi = curve.points[i + 1];

        if (tempC >= lo.tempC && tempC <= hi.tempC) {
            float ratio = (tempC - lo.tempC) / (hi.tempC - lo.tempC);
            float pwm   = lo.pwmPercent + ratio * (hi.pwmPercent - lo.pwmPercent);
            return (uint8_t)(pwm + 0.5f); // round
        }
    }

    return PWM_MIN_PERCENT;
}

void fanCurve_applyAll(FanState fans[NUM_FANS],
                       const SensorState sensors[NUM_TEMP_SENSORS],
                       uint8_t numSensors)
{
    // Check global safety override first
    bool safetyTriggered = false;
    for (uint8_t s = 0; s < numSensors; s++) {
        if (sensors[s].valid && sensors[s].tempC >= TEMP_SAFETY_LIMIT) {
            safetyTriggered = true;
            break;
        }
    }

    if (safetyTriggered) {
        for (uint8_t i = 0; i < NUM_FANS; i++) {
            fans[i].targetPercent = 100;
        }
        return;
    }

    for (uint8_t i = 0; i < NUM_FANS; i++) {
        if (fans[i].mode != FAN_MODE_AUTO) continue;

        uint8_t si = fans[i].sensorIndex;
        if (si >= numSensors || !sensors[si].valid) {
            // No valid sensor — run at minimum safe speed
            fans[i].targetPercent = PWM_MIN_PERCENT;
            continue;
        }

        uint8_t desiredPercent = fanCurve_evaluate(fans[i].curve, sensors[si].tempC);

        // Hysteresis: only update if change exceeds threshold
        float currentTemp = sensors[si].tempC;
        // We track the last temperature at which we last changed speed by
        // checking if the new target is meaningfully different from current target.
        // Simplified implementation: evaluate at ±HYSTERESIS_DEGREES and only
        // change if current target falls outside that band's output.
        uint8_t loTarget = fanCurve_evaluate(fans[i].curve, currentTemp - HYSTERESIS_DEGREES);
        uint8_t hiTarget = fanCurve_evaluate(fans[i].curve, currentTemp + HYSTERESIS_DEGREES);

        if (fans[i].targetPercent < loTarget || fans[i].targetPercent > hiTarget) {
            fans[i].targetPercent = desiredPercent;
        }
    }
}
