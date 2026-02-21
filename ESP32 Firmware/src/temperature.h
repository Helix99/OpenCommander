#pragma once
#include "shared_types.h"

// Initialise the OneWire bus and enumerate connected DS18B20 sensors.
// Loads friendly names from NVS. Returns the number of sensors found.
uint8_t temperature_init(SensorState sensors[NUM_TEMP_SENSORS]);

// Non-blocking async temperature cycle.
// Call from a looping task:
//   1. temperature_startConversion() — issues bus-wide requestTemperatures()
//   2. vTaskDelay(800ms) to let sensors convert
//   3. temperature_readAll(sensors) — reads all sensor values
void temperature_startConversion();
void temperature_readAll(SensorState sensors[NUM_TEMP_SENSORS], uint8_t numSensors);

// Convert an 8-byte ROM address to a 16-character hex string (no separators).
void temperature_romToHexStr(const uint8_t rom[8], char* out16);
