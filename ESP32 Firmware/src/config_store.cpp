#include "config_store.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static Preferences prefs;
static SemaphoreHandle_t nvsMutex = nullptr;

static const char* NS = "fanctrl";

// Default fan curve: [(20,30),(40,50),(60,75),(80,100)]
static const FanCurve DEFAULT_CURVE = {
    { {20.0f, 30}, {40.0f, 50}, {60.0f, 75}, {80.0f, 100}, {}, {}, {}, {} },
    4
};

void configStore_init() {
    nvsMutex = xSemaphoreCreateMutex();
    prefs.begin(NS, false);
    prefs.end();
}

static void lock()   { xSemaphoreTake(nvsMutex, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(nvsMutex); }

// ─── Fan Curves ──────────────────────────────────────────────────────────────

void configStore_saveCurve(uint8_t fanIndex, const FanCurve& curve) {
    if (fanIndex >= NUM_FANS) return;
    char key[12];
    snprintf(key, sizeof(key), "curve%u", fanIndex);
    lock();
    prefs.begin(NS, false);
    prefs.putBytes(key, &curve, sizeof(FanCurve));
    prefs.end();
    unlock();
}

bool configStore_loadCurve(uint8_t fanIndex, FanCurve& curve) {
    if (fanIndex >= NUM_FANS) { curve = DEFAULT_CURVE; return false; }
    char key[12];
    snprintf(key, sizeof(key), "curve%u", fanIndex);
    lock();
    prefs.begin(NS, true);
    bool ok = false;
    // Use isKey() first to avoid Preferences logging a spurious NOT_FOUND error
    // on every boot before curves have ever been saved.
    if (prefs.isKey(key)) {
        size_t len = prefs.getBytesLength(key);
        if (len == sizeof(FanCurve)) {
            prefs.getBytes(key, &curve, sizeof(FanCurve));
            ok = true;
        }
    }
    prefs.end();
    unlock();
    if (!ok) curve = DEFAULT_CURVE;
    return ok;
}

// ─── Sensor Mapping ──────────────────────────────────────────────────────────

void configStore_saveMapping(uint8_t fanIndex, uint8_t sensorIndex) {
    if (fanIndex >= NUM_FANS) return;
    char key[12];
    snprintf(key, sizeof(key), "map%u", fanIndex);
    lock();
    prefs.begin(NS, false);
    prefs.putUChar(key, sensorIndex);
    prefs.end();
    unlock();
}

uint8_t configStore_loadMapping(uint8_t fanIndex) {
    if (fanIndex >= NUM_FANS) return 0;
    char key[12];
    snprintf(key, sizeof(key), "map%u", fanIndex);
    lock();
    prefs.begin(NS, true);
    uint8_t val = prefs.getUChar(key, 0);
    prefs.end();
    unlock();
    return val;
}

// ─── Sensor Names ─────────────────────────────────────────────────────────────

// NVS keys must be ≤15 chars. Use "sn_" + first 6 ROM bytes as 12 hex chars = 15 total.
static void romToKey(const uint8_t rom[8], char* out) {
    snprintf(out, 16, "sn_%02X%02X%02X%02X%02X%02X",
             rom[0], rom[1], rom[2], rom[3], rom[4], rom[5]);
}

void configStore_saveSensorName(const uint8_t rom[8], const char* name) {
    char key[16];
    romToKey(rom, key);
    lock();
    prefs.begin(NS, false);
    prefs.putString(key, name);
    prefs.end();
    unlock();
}

bool configStore_loadSensorName(const uint8_t rom[8], char* nameBuf, size_t bufLen) {
    char key[16];
    romToKey(rom, key);
    lock();
    prefs.begin(NS, true);
    bool exists = prefs.isKey(key);
    String val = exists ? prefs.getString(key, "") : "";
    prefs.end();
    unlock();
    if (!exists || val.length() == 0) return false;
    strlcpy(nameBuf, val.c_str(), bufLen);
    return true;
}

// ─── HID Enable Flag ─────────────────────────────────────────────────────────

void configStore_saveHidEnabled(bool enabled) {
    lock();
    prefs.begin(NS, false);
    prefs.putBool("hidEn", enabled);
    prefs.end();
    unlock();
}

bool configStore_loadHidEnabled() {
    lock();
    prefs.begin(NS, true);
    bool val = prefs.getBool("hidEn", true);
    prefs.end();
    unlock();
    return val;
}

// ─── Factory Reset ───────────────────────────────────────────────────────────

void configStore_factoryReset() {
    lock();
    prefs.begin(NS, false);
    prefs.clear();
    prefs.end();
    unlock();
}
