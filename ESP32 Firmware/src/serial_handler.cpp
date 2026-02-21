#include "serial_handler.h"
#include "config_store.h"
#include "temperature.h"
#include <ArduinoJson.h>
#include <USB.h>
#include <USBCDC.h>

extern USBCDC USBSerial;

#define CMD_BUF_SIZE 256

static char   cmdBuf[CMD_BUF_SIZE];
static uint16_t cmdLen = 0;

void serialHandler_init() {
    cmdLen = 0;
    memset(cmdBuf, 0, CMD_BUF_SIZE);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static void sendJson(const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    USBSerial.println(out);
}

static void sendOk() {
    USBSerial.println("{\"ok\":true}");
}

static void sendError(const char* msg) {
    JsonDocument doc;
    doc["ok"]    = false;
    doc["error"] = msg;
    sendJson(doc);
}

// Parse a hex string (16 chars) into 8-byte ROM array.
// Returns true on success.
static bool hexToRom(const char* hex, uint8_t rom[8]) {
    if (strlen(hex) < 16) return false;
    for (int i = 0; i < 8; i++) {
        char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        rom[i] = (uint8_t)strtol(byte_str, nullptr, 16);
    }
    return true;
}

// ─── Command Dispatch ────────────────────────────────────────────────────────

static void handleCommand(const char* line, SystemState& state) {
    // Tokenise by spaces
    char buf[CMD_BUF_SIZE];
    strlcpy(buf, line, CMD_BUF_SIZE);

    char* token = strtok(buf, " \t\r\n");
    if (!token) return;

    // ── GET_TEMPS ────────────────────────────────────────────────────────────
    if (strcmp(token, "GET_TEMPS") == 0) {
        JsonDocument doc;
        JsonArray arr = doc["temps"].to<JsonArray>();
        for (uint8_t i = 0; i < state.numSensorsFound; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["name"]  = state.sensors[i].name;
            char hex[17];
            temperature_romToHexStr(state.sensors[i].rom, hex);
            o["rom"]   = hex;
            o["value"] = state.sensors[i].valid ? state.sensors[i].tempC : -127.0f;
            o["valid"] = state.sensors[i].valid;
        }
        sendJson(doc);
    }

    // ── GET_RPMS ─────────────────────────────────────────────────────────────
    else if (strcmp(token, "GET_RPMS") == 0) {
        JsonDocument doc;
        JsonArray arr = doc["rpms"].to<JsonArray>();
        for (uint8_t i = 0; i < NUM_FANS; i++) arr.add(state.fans[i].rpm);
        sendJson(doc);
    }

    // ── SET_FAN <fan 1-4> <percent 0-100> ────────────────────────────────────
    else if (strcmp(token, "SET_FAN") == 0) {
        char* s_fan = strtok(nullptr, " ");
        char* s_pct = strtok(nullptr, " ");
        if (!s_fan || !s_pct) { sendError("Usage: SET_FAN <1-4> <0-100>"); return; }
        int fan = atoi(s_fan) - 1;
        int pct = atoi(s_pct);
        if (fan < 0 || fan >= NUM_FANS) { sendError("Invalid fan index"); return; }
        if (pct < 0 || pct > 100)       { sendError("Percent out of range"); return; }
        state.fans[fan].mode          = FAN_MODE_MANUAL;
        state.fans[fan].targetPercent = (uint8_t)pct;
        sendOk();
    }

    // ── SET_AUTO <fan 1-4> ───────────────────────────────────────────────────
    else if (strcmp(token, "SET_AUTO") == 0) {
        char* s_fan = strtok(nullptr, " ");
        if (!s_fan) { sendError("Usage: SET_AUTO <1-4>"); return; }
        int fan = atoi(s_fan) - 1;
        if (fan < 0 || fan >= NUM_FANS) { sendError("Invalid fan index"); return; }
        state.fans[fan].mode = FAN_MODE_AUTO;
        sendOk();
    }

    // ── SET_ALL_AUTO ─────────────────────────────────────────────────────────
    else if (strcmp(token, "SET_ALL_AUTO") == 0) {
        for (uint8_t i = 0; i < NUM_FANS; i++) state.fans[i].mode = FAN_MODE_AUTO;
        sendOk();
    }

    // ── GET_CURVE <fan 1-4> ──────────────────────────────────────────────────
    else if (strcmp(token, "GET_CURVE") == 0) {
        char* s_fan = strtok(nullptr, " ");
        if (!s_fan) { sendError("Usage: GET_CURVE <1-4>"); return; }
        int fan = atoi(s_fan) - 1;
        if (fan < 0 || fan >= NUM_FANS) { sendError("Invalid fan index"); return; }

        JsonDocument doc;
        doc["fan"] = fan + 1;
        JsonArray pts = doc["curve"].to<JsonArray>();
        const FanCurve& c = state.fans[fan].curve;
        for (uint8_t i = 0; i < c.numPoints; i++) {
            JsonArray pt = pts.add<JsonArray>();
            pt.add(c.points[i].tempC);
            pt.add(c.points[i].pwmPercent);
        }
        sendJson(doc);
    }

    // ── SET_CURVE <fan 1-4> <json_array> ────────────────────────────────────
    // Example: SET_CURVE 1 [[20,30],[40,50],[60,75],[80,100]]
    else if (strcmp(token, "SET_CURVE") == 0) {
        char* s_fan  = strtok(nullptr, " ");
        char* s_json = strtok(nullptr, "\r\n"); // Rest of line is JSON
        if (!s_fan || !s_json) { sendError("Usage: SET_CURVE <1-4> <json_array>"); return; }
        int fan = atoi(s_fan) - 1;
        if (fan < 0 || fan >= NUM_FANS) { sendError("Invalid fan index"); return; }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, s_json);
        if (err || !doc.is<JsonArray>()) { sendError("Invalid JSON array"); return; }

        JsonArray arr = doc.as<JsonArray>();
        if (arr.size() < 2 || arr.size() > MAX_CURVE_POINTS) {
            sendError("Curve must have 2–8 points"); return;
        }

        FanCurve newCurve;
        newCurve.numPoints = 0;
        float lastTemp = -999.0f;
        bool valid = true;
        for (JsonVariant pt : arr) {
            if (!pt.is<JsonArray>() || pt.as<JsonArray>().size() < 2) { valid = false; break; }
            float  t   = pt[0].as<float>();
            uint8_t p  = pt[1].as<uint8_t>();
            if (t <= lastTemp || p > 100) { valid = false; break; }
            newCurve.points[newCurve.numPoints++] = {t, p};
            lastTemp = t;
        }
        if (!valid) { sendError("Invalid curve (temps must ascend, PWM 0-100)"); return; }

        state.fans[fan].curve = newCurve;
        configStore_saveCurve(fan, newCurve);
        sendOk();
    }

    // ── GET_MAPPING ──────────────────────────────────────────────────────────
    else if (strcmp(token, "GET_MAPPING") == 0) {
        JsonDocument doc;
        JsonArray arr = doc["mapping"].to<JsonArray>();
        for (uint8_t i = 0; i < NUM_FANS; i++) arr.add(state.fans[i].sensorIndex);
        sendJson(doc);
    }

    // ── SET_MAPPING <fan 1-4> <sensor 0-4> ──────────────────────────────────
    else if (strcmp(token, "SET_MAPPING") == 0) {
        char* s_fan = strtok(nullptr, " ");
        char* s_sen = strtok(nullptr, " ");
        if (!s_fan || !s_sen) { sendError("Usage: SET_MAPPING <1-4> <0-4>"); return; }
        int fan = atoi(s_fan) - 1;
        int sen = atoi(s_sen);
        if (fan < 0 || fan >= NUM_FANS)          { sendError("Invalid fan index");    return; }
        if (sen < 0 || sen >= NUM_TEMP_SENSORS)  { sendError("Invalid sensor index"); return; }
        state.fans[fan].sensorIndex = (uint8_t)sen;
        configStore_saveMapping(fan, (uint8_t)sen);
        sendOk();
    }

    // ── LIST_SENSORS ─────────────────────────────────────────────────────────
    else if (strcmp(token, "LIST_SENSORS") == 0) {
        JsonDocument doc;
        JsonArray arr = doc["sensors"].to<JsonArray>();
        for (uint8_t i = 0; i < state.numSensorsFound; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["index"] = i;
            char hex[17];
            temperature_romToHexStr(state.sensors[i].rom, hex);
            o["rom"]  = hex;
            o["name"] = state.sensors[i].name;
        }
        sendJson(doc);
    }

    // ── SET_NAME <rom_hex> <name> ─────────────────────────────────────────────
    else if (strcmp(token, "SET_NAME") == 0) {
        char* s_rom  = strtok(nullptr, " ");
        char* s_name = strtok(nullptr, "\r\n");
        if (!s_rom || !s_name) { sendError("Usage: SET_NAME <rom_hex> <name>"); return; }
        uint8_t rom[8];
        if (!hexToRom(s_rom, rom)) { sendError("Invalid ROM hex"); return; }
        configStore_saveSensorName(rom, s_name);

        // Update live state if sensor is known
        for (uint8_t i = 0; i < state.numSensorsFound; i++) {
            if (memcmp(state.sensors[i].rom, rom, 8) == 0) {
                strlcpy(state.sensors[i].name, s_name, sizeof(state.sensors[i].name));
                break;
            }
        }
        sendOk();
    }

    // ── GET_STATUS ───────────────────────────────────────────────────────────
    else if (strcmp(token, "GET_STATUS") == 0) {
        JsonDocument doc;
        doc["uptime"]      = millis() - state.bootTime;
        doc["hid_enabled"] = state.hidEnabled;

        JsonArray fans = doc["fans"].to<JsonArray>();
        for (uint8_t i = 0; i < NUM_FANS; i++) {
            JsonObject f = fans.add<JsonObject>();
            f["fan"]     = i + 1;
            f["rpm"]     = state.fans[i].rpm;
            f["pwm"]     = state.fans[i].currentPercent;
            f["target"]  = state.fans[i].targetPercent;
            f["mode"]    = (state.fans[i].mode == FAN_MODE_AUTO) ? "auto" : "manual";
            f["stalled"] = state.fans[i].stalled;
            f["sensor"]  = state.fans[i].sensorIndex;
        }

        JsonArray temps = doc["temps"].to<JsonArray>();
        for (uint8_t i = 0; i < state.numSensorsFound; i++) {
            JsonObject t = temps.add<JsonObject>();
            t["index"] = i;
            t["name"]  = state.sensors[i].name;
            t["value"] = state.sensors[i].valid ? state.sensors[i].tempC : -127.0f;
            t["valid"] = state.sensors[i].valid;
        }
        sendJson(doc);
    }

    // ── HID_ENABLE ───────────────────────────────────────────────────────────
    else if (strcmp(token, "HID_ENABLE") == 0) {
        configStore_saveHidEnabled(true);
        state.hidEnabled = true;
        JsonDocument doc;
        doc["ok"]     = true;
        doc["reboot"] = true;
        sendJson(doc);
        delay(200);
        ESP.restart();
    }

    // ── HID_DISABLE ──────────────────────────────────────────────────────────
    else if (strcmp(token, "HID_DISABLE") == 0) {
        configStore_saveHidEnabled(false);
        state.hidEnabled = false;
        JsonDocument doc;
        doc["ok"]     = true;
        doc["reboot"] = true;
        sendJson(doc);
        delay(200);
        ESP.restart();
    }

    // ── SAVE_CONFIG ──────────────────────────────────────────────────────────
    else if (strcmp(token, "SAVE_CONFIG") == 0) {
        // Curves and mappings are saved immediately on change;
        // this is a no-op kept for CLI compatibility.
        sendOk();
    }

    // ── FACTORY_RESET ─────────────────────────────────────────────────────────
    else if (strcmp(token, "FACTORY_RESET") == 0) {
        configStore_factoryReset();
        JsonDocument doc;
        doc["ok"]     = true;
        doc["reboot"] = true;
        sendJson(doc);
        delay(200);
        ESP.restart();
    }

    // ── VERSION ──────────────────────────────────────────────────────────────
    else if (strcmp(token, "VERSION") == 0) {
        JsonDocument doc;
        doc["fw"]    = "1.0.0";
        doc["board"] = "ESP32-S3-SuperMini";
        sendJson(doc);
    }

    else {
        sendError("Unknown command");
    }
}

// ─── Main Tick ───────────────────────────────────────────────────────────────

void serialHandler_tick(SystemState& state) {
    while (USBSerial.available()) {
        char c = (char)USBSerial.read();
        if (c == '\n' || c == '\r') {
            if (cmdLen > 0) {
                cmdBuf[cmdLen] = '\0';
                handleCommand(cmdBuf, state);
                cmdLen = 0;
            }
        } else if (cmdLen < CMD_BUF_SIZE - 1) {
            cmdBuf[cmdLen++] = c;
        }
        // Process one full command then return to avoid starving other tasks
        if (cmdLen == 0) break;
    }
}
