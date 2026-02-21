#include "hid_handler.h"
#include "fan_control.h"
#include <USB.h>
#include <USBHID.h>
#include <class/hid/hid_device.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ─────────────────────────────────────────────────────────────────────────────
// Corsair Commander Pro USB HID emulation
//
// KEY INSIGHT from usbmon trace:
//   The kernel pre-submits the IN URB *before* sending the OUT command:
//     S Ii  16 <            ← IN URB already waiting for data
//     S Io  63 = 10 00...   ← OUT command arrives
//     C Io  63 >            ← ESP32 ACKs the OUT  ✓
//     C Ii  -2 = 0 bytes    ← IN URB expired with no data
//
//   Solution: call tud_hid_n_report() synchronously inside _onOutput.
//   _onOutput runs in the TinyUSB task; tud_hid_n_report() just writes to
//   the endpoint TX FIFO without blocking. The response is staged before
//   _onOutput returns, so the very next IN poll (resubmitted after the OUT
//   ACK) will collect our data within the 300ms kernel timeout.
//
//   We do NOT call USBHID::SendReport() — it blocks on a semaphore waiting
//   for tud_hid_report_complete_cb (already defined in USBHID.cpp), which
//   can only be signalled by the TinyUSB task itself → deadlock.
//
//   Note: tud_hid_report_complete_cb is defined in USBHID.cpp and cannot
//   be redefined. The retry task below handles the edge case where the TX
//   FIFO was momentarily busy (tud_hid_n_report returned false).
//
// Kernel driver constants: OUT=63 bytes, IN=16 bytes, timeout=300ms
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t OUT_PKT = 63;
static constexpr uint8_t IN_PKT  = 16;

static const uint8_t CORSAIR_REPORT_DESCRIPTOR[] = {
    0x06, 0x00, 0xFF,   // Usage Page (Vendor 0xFF00)
    0x09, 0x01,         // Usage (0x01)
    0xA1, 0x01,         // Collection (Application)
    // OUT: host→device, 63 bytes, no report ID
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,         // Report Size 8 bits
    0x95, 0x3F,         // Report Count 63
    0x91, 0x02,         // Output (Data, Variable, Absolute)
    // IN: device→host, 16 bytes, no report ID
    0x09, 0x03,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x10,         // Report Count 16
    0x81, 0x02,         // Input (Data, Variable, Absolute)
    0xC0
};

// ── Shared state ─────────────────────────────────────────────────────────────
static SystemState* s_statePtr      = nullptr;
static volatile bool s_pending      = false;   // retry needed
static uint8_t       s_rsp[IN_PKT];            // last built response

// ── Response builder ──────────────────────────────────────────────────────────
static void buildResponse(const uint8_t* cmd, uint16_t len, uint8_t* rsp) {
    if (!s_statePtr || len == 0) return;
    SystemState& st = *s_statePtr;

    switch (cmd[0]) {
        case 0x10: // CTL_GET_TMP_CNCT
            for (uint8_t i = 0; i < 4; i++)
                rsp[1+i] = (i < st.numSensorsFound && st.sensors[i].valid) ? 1 : 0;
            break;
        case 0x11: { // CTL_GET_TMP — centi-°C, big-endian
            uint8_t ch = (len > 1) ? cmd[1] : 0;
            if (ch < st.numSensorsFound && st.sensors[ch].valid) {
                int16_t c = (int16_t)(st.sensors[ch].tempC * 100.0f);
                rsp[1] = (c >> 8) & 0xFF; rsp[2] = c & 0xFF;
            }
            break;
        }
        case 0x12: { // CTL_GET_VOLT — millivolts, big-endian
            static const uint16_t mv[3] = {12000, 5000, 3300};
            uint8_t rail = (len > 1) ? cmd[1] : 0;
            uint16_t v = (rail < 3) ? mv[rail] : 0;
            rsp[1] = (v >> 8) & 0xFF; rsp[2] = v & 0xFF;
            break;
        }
        case 0x20: // CTL_GET_FAN_CNCT — 2=4-pin PWM
            for (uint8_t i = 0; i < 6; i++)
                rsp[1+i] = (i < NUM_FANS) ? 2 : 0;
            break;
        case 0x21: { // CTL_GET_FAN_RPM — big-endian
            uint8_t ch = (len > 1) ? cmd[1] : 0;
            if (ch < NUM_FANS) {
                uint16_t rpm = st.fans[ch].rpm;
                rsp[1] = (rpm >> 8) & 0xFF; rsp[2] = rpm & 0xFF;
            }
            break;
        }
        case 0x23: // CTL_SET_FAN_FPWM
            if (len > 2) {
                uint8_t ch = cmd[1], pct = cmd[2];
                if (ch < NUM_FANS) {
                    st.fans[ch].mode          = FAN_MODE_MANUAL;
                    st.fans[ch].targetPercent = (pct > 100) ? 100 : pct;
                }
            }
            break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

class CorsairHID : public USBHIDDevice {
public:
    USBHID _hid;

    void begin() { _hid.addDevice(this, sizeof(CORSAIR_REPORT_DESCRIPTOR)); }

    uint16_t _onGetDescriptor(uint8_t* dst) override {
        memcpy(dst, CORSAIR_REPORT_DESCRIPTOR, sizeof(CORSAIR_REPORT_DESCRIPTOR));
        return sizeof(CORSAIR_REPORT_DESCRIPTOR);
    }

    // Called in TinyUSB task — safe to call tud_hid_n_report() directly.
    void _onOutput(uint8_t /*id*/, const uint8_t* data, uint16_t len) override {
        memset(s_rsp, 0x00, IN_PKT);
        buildResponse(data, len, s_rsp);

        if (!tud_hid_n_report(0, 0, s_rsp, IN_PKT)) {
            s_pending = true;   // TX FIFO busy — retry task will resend
        }
    }

    bool ready() { return _hid.ready(); }

    bool trySend() {
        if (!_hid.ready()) return false;
        return tud_hid_n_report(0, 0, s_rsp, IN_PKT);
    }
};

static CorsairHID corsairHID;

// ── Retry task — handles the rare case where TX FIFO was busy in _onOutput ──
// Also pre-primes the IN endpoint with a zeroed response at startup so the
// very first IN URB (submitted before the first OUT) doesn't expire empty.
static void retryTask(void* /*pv*/) {
    // Wait for USB ready, then prime the IN endpoint
    while (!corsairHID.ready()) vTaskDelay(pdMS_TO_TICKS(20));
    tud_hid_n_report(0, 0, s_rsp, IN_PKT);   // s_rsp is zeroed at this point

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (s_pending && corsairHID.ready()) {
            if (corsairHID.trySend()) s_pending = false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void hidHandler_init(bool enabled) {
    if (!enabled) return;
    corsairHID.begin();
    xTaskCreatePinnedToCore(retryTask, "CProRetry", 1024, nullptr, 3, nullptr, 0);
}

void hidHandler_setStatePtr(SystemState* state) { s_statePtr = state; }
bool hidHandler_processIncoming(SystemState&) { return false; }
void hidHandler_pushStatusReport(const SystemState&) {}
