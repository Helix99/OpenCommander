#pragma once
#include "shared_types.h"

// Attach interrupts and start tracking pulse counts for all 4 fans.
void tachometer_init();

// Recalculate RPM for all fans from accumulated pulse counts since last call.
// Call every TACH_CALC_INTERVAL_MS (2000ms) from the tachometer task.
void tachometer_update(FanState fans[NUM_FANS]);
