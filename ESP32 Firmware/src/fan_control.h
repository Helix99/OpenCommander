#pragma once
#include "shared_types.h"

// Initialise LEDC PWM channels for all 4 fans.
// Must be called before any other fan_control function.
void fanControl_init();

// Set a fan's target duty cycle (0–100%). In manual mode this takes
// immediate effect (subject to ramping). In auto mode the fan curve
// overrides this each control tick.
void fanControl_setTarget(uint8_t fanIndex, uint8_t percent);

// Apply one ramp step toward each fan's target duty.
// Call every 50ms from the fan control task.
void fanControl_rampTick(FanState fans[NUM_FANS]);

// Force all fans to 100% regardless of mode (safety override).
void fanControl_safetyOverride();

// Convert 0–100% to 0–255 PWM value respecting the minimum floor.
uint8_t fanControl_percentToRaw(uint8_t percent);

// Write raw PWM value (0–255) to a fan's LEDC channel.
void fanControl_writeRaw(uint8_t fanIndex, uint8_t raw);
