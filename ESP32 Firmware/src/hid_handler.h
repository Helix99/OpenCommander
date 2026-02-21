#pragma once
#include "shared_types.h"

// Call before USB.begin(). Registers the HID interface and starts the
// response task if enabled.
void hidHandler_init(bool enabled);

// Provide a pointer to the live SystemState so the response task can read
// sensor and fan data when building responses to the kernel driver.
// Call after hidHandler_init() and before USB.begin().
void hidHandler_setStatePtr(SystemState* state);

// No-op — command processing happens in the internal response task.
bool hidHandler_processIncoming(SystemState& state);

// No-op — corsair-cpro is purely request/response, no unsolicited IN reports.
void hidHandler_pushStatusReport(const SystemState& state);
