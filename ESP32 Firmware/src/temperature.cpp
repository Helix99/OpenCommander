#include "temperature.h"
#include "config_store.h"
#include <OneWire.h>
#include <DallasTemperature.h>

static OneWire            oneWire(PIN_ONEWIRE);
static DallasTemperature  sensors(&oneWire);

void temperature_romToHexStr(const uint8_t rom[8], char* out16) {
    for (int i = 0; i < 8; i++) {
        snprintf(out16 + (i * 2), 3, "%02X", rom[i]);
    }
    out16[16] = '\0';
}

uint8_t temperature_init(SensorState sensorStates[NUM_TEMP_SENSORS]) {
    sensors.begin();
    sensors.setResolution(TEMP_RESOLUTION_BITS);
    sensors.setWaitForConversion(false); // Non-blocking

    uint8_t found = sensors.getDeviceCount();
    if (found > NUM_TEMP_SENSORS) found = NUM_TEMP_SENSORS;

    for (uint8_t i = 0; i < found; i++) {
        DeviceAddress addr;
        if (sensors.getAddress(addr, i)) {
            memcpy(sensorStates[i].rom, addr, 8);
            sensorStates[i].tempC = 0.0f;
            sensorStates[i].valid = true;

            // Default name: "Sensor N"
            snprintf(sensorStates[i].name, sizeof(sensorStates[i].name), "Sensor %u", i + 1);
            // Override with stored name if available
            configStore_loadSensorName(sensorStates[i].rom,
                                       sensorStates[i].name,
                                       sizeof(sensorStates[i].name));
        } else {
            sensorStates[i].valid = false;
        }
    }

    // Mark any slots beyond found count as invalid
    for (uint8_t i = found; i < NUM_TEMP_SENSORS; i++) {
        memset(&sensorStates[i], 0, sizeof(SensorState));
        sensorStates[i].valid = false;
        snprintf(sensorStates[i].name, sizeof(sensorStates[i].name), "Not found");
    }

    return found;
}

void temperature_startConversion() {
    sensors.requestTemperatures();
}

void temperature_readAll(SensorState sensorStates[NUM_TEMP_SENSORS], uint8_t numSensors) {
    for (uint8_t i = 0; i < numSensors; i++) {
        if (!sensorStates[i].valid) continue;

        float t = sensors.getTempC(sensorStates[i].rom);

        if (t == DEVICE_DISCONNECTED_C || t < -55.0f) {
            // Sensor error — keep last known value, mark invalid
            sensorStates[i].valid = false;
            char hexStr[17];
            temperature_romToHexStr(sensorStates[i].rom, hexStr);
            Serial.printf("[TEMP] Sensor %s disconnected or error\n", hexStr);
        } else {
            sensorStates[i].tempC  = t;
            sensorStates[i].valid  = true;
        }
    }
}
