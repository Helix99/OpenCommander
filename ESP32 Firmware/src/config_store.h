#pragma once
#include "shared_types.h"

// Wraps Preferences NVS for all persistent configuration.
// Call init() once during setup. All other functions are thread-safe
// via an internal mutex.

void configStore_init();

// Fan curves
void configStore_saveCurve(uint8_t fanIndex, const FanCurve& curve);
bool configStore_loadCurve(uint8_t fanIndex, FanCurve& curve);

// Sensor→fan mapping (which sensor index drives which fan)
void configStore_saveMapping(uint8_t fanIndex, uint8_t sensorIndex);
uint8_t configStore_loadMapping(uint8_t fanIndex);

// Sensor friendly names, keyed by 8-byte ROM stored as hex string
void configStore_saveSensorName(const uint8_t rom[8], const char* name);
bool configStore_loadSensorName(const uint8_t rom[8], char* nameBuf, size_t bufLen);

// HID enable flag
void configStore_saveHidEnabled(bool enabled);
bool configStore_loadHidEnabled();

// Wipe everything and signal a reboot is needed
void configStore_factoryReset();
