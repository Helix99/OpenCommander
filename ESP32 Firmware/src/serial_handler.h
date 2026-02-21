#pragma once
#include "shared_types.h"

// Initialise serial command handler.
// Must be called after USBSerial is ready.
void serialHandler_init();

// Process one pending serial command from the CDC buffer.
// Non-blocking: returns immediately if no complete command available.
// Call from the serial handler task loop.
void serialHandler_tick(SystemState& state);
