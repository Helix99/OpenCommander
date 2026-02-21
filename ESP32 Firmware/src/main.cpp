/*
 * ESP32-S3 SuperMini Fan Controller
 * Corsair Commander Pro Emulation + CDC Serial Control
 *
 * Core 0: USB HID task, Serial handler task
 * Core 1: Temperature task, Fan control task, Tachometer task
 *
 * All shared state in g_state, protected by g_stateMutex.
 */

#include <Arduino.h>
#include <USB.h>
#include <USBCDC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "shared_types.h"
#include "config_store.h"
#include "fan_control.h"
#include "tachometer.h"
#include "temperature.h"
#include "fan_curve.h"
#include "hid_handler.h"
#include "serial_handler.h"

// ─── Global State ────────────────────────────────────────────────────────────

SystemState         g_state;
SemaphoreHandle_t   g_stateMutex;
USBCDC              USBSerial;

// ─── Task Handles ────────────────────────────────────────────────────────────

TaskHandle_t taskHID     = nullptr;
TaskHandle_t taskSerial  = nullptr;
TaskHandle_t taskTemp    = nullptr;
TaskHandle_t taskControl = nullptr;
TaskHandle_t taskTacho   = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Core 0 Tasks — USB Protocol
// ─────────────────────────────────────────────────────────────────────────────

// HID Task: push Commander Pro status report every 2s,
// and process any incoming HID commands from the host.
void taskHID_fn(void* /*pvParam*/) {
    const TickType_t period = pdMS_TO_TICKS(2000);
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50))) {
            hidHandler_processIncoming(g_state);
            hidHandler_pushStatusReport(g_state);
            xSemaphoreGive(g_stateMutex);
        }
        vTaskDelayUntil(&lastWake, period);
    }
}

// Serial Task: process CDC serial commands (event-driven).
void taskSerial_fn(void* /*pvParam*/) {
    for (;;) {
        if (USBSerial && USBSerial.available()) {
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100))) {
                serialHandler_tick(g_state);
                xSemaphoreGive(g_stateMutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Yield; avoid busy-loop
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Core 1 Tasks — Sensors & Control
// ─────────────────────────────────────────────────────────────────────────────

// Temperature Task: async DS18B20 conversion + read every 2s.
void taskTemp_fn(void* /*pvParam*/) {
    for (;;) {
        // Start conversion — non-blocking command
        temperature_startConversion();

        // Wait for 12-bit conversion to complete (~800ms to be safe)
        vTaskDelay(pdMS_TO_TICKS(800));

        // Read all sensors into local copies, then update shared state
        SensorState localSensors[NUM_TEMP_SENSORS];
        uint8_t numFound;

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100))) {
            numFound = g_state.numSensorsFound;
            memcpy(localSensors, g_state.sensors, sizeof(localSensors));
            xSemaphoreGive(g_stateMutex);
        }

        temperature_readAll(localSensors, numFound);

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100))) {
            memcpy(g_state.sensors, localSensors, sizeof(localSensors));
            xSemaphoreGive(g_stateMutex);
        }

        // Wait out the rest of the 2s interval
        vTaskDelay(pdMS_TO_TICKS(TEMP_READ_INTERVAL_MS - 800));
    }
}

// Fan Control Task: apply curves and ramp PWM every 100ms (50ms ramp tick × 2).
void taskControl_fn(void* /*pvParam*/) {
    const TickType_t period = pdMS_TO_TICKS(50);
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(30))) {
            // Apply fan curves (auto-mode only)
            fanCurve_applyAll(g_state.fans, g_state.sensors, g_state.numSensorsFound);

            // Ramp each fan one step toward its target
            fanControl_rampTick(g_state.fans);

            xSemaphoreGive(g_stateMutex);
        }
        vTaskDelayUntil(&lastWake, period);
    }
}

// Tachometer Task: calculate RPM every 2s from ISR pulse counts.
void taskTacho_fn(void* /*pvParam*/) {
    const TickType_t period = pdMS_TO_TICKS(TACH_CALC_INTERVAL_MS);
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        FanState localFans[NUM_FANS];

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50))) {
            memcpy(localFans, g_state.fans, sizeof(localFans));
            xSemaphoreGive(g_stateMutex);
        }

        tachometer_update(localFans);

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50))) {
            for (uint8_t i = 0; i < NUM_FANS; i++) {
                g_state.fans[i].rpm     = localFans[i].rpm;
                g_state.fans[i].stalled = localFans[i].stalled;
            }
            xSemaphoreGive(g_stateMutex);
        }

        vTaskDelayUntil(&lastWake, period);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    // Status LED
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    // ── NVS / Preferences ────────────────────────────────────────────────────
    configStore_init();

    // ── Initialise shared state ───────────────────────────────────────────────
    g_stateMutex = xSemaphoreCreateMutex();
    memset(&g_state, 0, sizeof(g_state));
    g_state.bootTime = millis();

    // Load HID preference
    g_state.hidEnabled = configStore_loadHidEnabled();

    // Load per-fan configuration (curves, sensor mapping, start in auto mode)
    for (uint8_t i = 0; i < NUM_FANS; i++) {
        configStore_loadCurve(i, g_state.fans[i].curve);
        g_state.fans[i].sensorIndex   = configStore_loadMapping(i);
        g_state.fans[i].mode          = FAN_MODE_AUTO;
        g_state.fans[i].targetPercent = PWM_MIN_PERCENT;
        g_state.fans[i].currentPercent= PWM_MIN_PERCENT;
    }

    // ── Hardware peripherals ──────────────────────────────────────────────────
    fanControl_init();
    tachometer_init();

    // Discover DS18B20 sensors and populate state
    g_state.numSensorsFound = temperature_init(g_state.sensors);

    // ── USB ───────────────────────────────────────────────────────────────────
    // Order matters for TinyUSB composite device:
    //   1. Set VID/PID/strings before any interface registers
    //   2. hidHandler_init: registers HID interface + spawns high-priority
    //      response task that wakes immediately on OUT report from kernel
    //   3. USBSerial.begin: registers CDC interface
    //   4. USB.begin: starts TinyUSB with all registered interfaces
    USB.VID(0x1b1c);
    USB.PID(0x0c10);
    USB.manufacturerName("OPENCOMMANDER");
    USB.productName("OPENCOMMANDER V0.1");

    hidHandler_init(g_state.hidEnabled);       // registers HID + starts response task
    hidHandler_setStatePtr(&g_state);          // give response task access to live state

    USBSerial.begin(115200);              // registers CDC before stack starts

    USB.begin();                          // starts TinyUSB with all interfaces

    serialHandler_init();

    // Brief wait for USB enumeration
    delay(1000);

    // ── FreeRTOS Tasks ────────────────────────────────────────────────────────
    // Core 0: USB protocol tasks
    xTaskCreatePinnedToCore(taskHID_fn,    "HID",     4096, nullptr, 1, &taskHID,     0);
    xTaskCreatePinnedToCore(taskSerial_fn, "Serial",  4096, nullptr, 2, &taskSerial,  0);

    // Core 1: Sensor & control tasks
    xTaskCreatePinnedToCore(taskTemp_fn,    "Temp",    4096, nullptr, 2, &taskTemp,    1);
    xTaskCreatePinnedToCore(taskControl_fn, "Control", 3072, nullptr, 3, &taskControl, 1);
    xTaskCreatePinnedToCore(taskTacho_fn,   "Tacho",   3072, nullptr, 1, &taskTacho,   1);

    // Blink LED to indicate successful boot
    for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_LED, HIGH); delay(150);
        digitalWrite(PIN_LED, LOW);  delay(150);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop — runs on Core 1 at idle priority, used only for LED heartbeat.
// All real work is done in the FreeRTOS tasks above.
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // Slow heartbeat: ON for 100ms, OFF for 1900ms
    digitalWrite(PIN_LED, HIGH); delay(100);
    digitalWrite(PIN_LED, LOW);  delay(1900);
}
