# ESP32-S3 SuperMini Fan Controller

A complete 4-channel PWM fan controller with 5× DS18B20 temperature monitoring, emulating a **Corsair Commander Pro** over USB HID. Works natively with Linux `lm-sensors`, `fancontrol`, and Prometheus/Grafana — zero custom drivers.

---

## Features

- 6× 4-pin PWM fans (25 kHz, 8-bit, 0–100%)
- 5× DS18B20 temperature sensors (1-Wire, 12-bit, 0.0625 °C resolution)
- Independent fan curves with hysteresis per fan
- Safety override: all fans → 100% if any sensor exceeds 85 °C
- Smooth PWM ramping (prevents bearing stress)
- Friendly sensor names stored in NVS across reboots
- Composite USB: CDC serial + HID simultaneously on one USB-C cable
- Corsair Commander Pro HID protocol (VID `0x1b1c` / PID `0x0c10`)
- Automatic `corsair-cpro` kernel driver load on Linux ≥ 5.8
- JSON serial protocol for Python CLI / scripting

---

## Hardware

### Bill of Materials

| Component | Qty | UK/EU Source | US Source |
|---|---|---|---|
| ESP32-S3 SuperMini | 1 | AliExpress ~£3 | Amazon/AliExpress ~$4 |
| DS18B20 (TO-92) | 5 | Farnell, AliExpress | Mouser, DigiKey |
| 4.7 kΩ resistor | 1 | Any | Any |
| 1 kΩ resistor | 4 | Any | Any |
| 100 nF ceramic cap | 4 | Any | Any |
| 10 µF electrolytic cap | 2 | Any | Any |
| 4-pin PWM fan header (PCB) | 4 | AliExpress | Amazon |
| 12 V PSU (≥5 A for 4× 120 mm fans) | 1 | Amazon ~£12 | Amazon ~$15 |
| Screw terminals / prototype PCB | 1 set | AliExpress | Amazon |

### Pin Assignments

| Function | GPIO | Notes |
|---|---|---|
| PWM Fan 1–4 | 1, 2, 3, 4 | LEDC output, 3.3 V logic OK |
| Tachometer Fan 1–4 | 5, 6, 7, 8 | INPUT_PULLUP, FALLING edge ISR |
| 1-Wire (DS18B20) | 9 | External 4.7 kΩ pull-up to 3.3 V |
| Status LED | 48 | Built-in on SuperMini |
| USB D−/D+ | 19/20 | **Reserved — do not use** |
| Strapping | 0, 45, 46 | **Avoid for peripherals** |

### Wiring Diagram

```
                  ESP32-S3 SuperMini
                 ┌──────────────────┐
            3.3V │                  │ GPIO1 ──PWM──► Fan1 Pin4
             GND │                  │ GPIO2 ──PWM──► Fan2 Pin4
                 │                  │ GPIO3 ──PWM──► Fan3 Pin4
                 │                  │ GPIO4 ──PWM──► Fan4 Pin4
                 │                  │
                 │                  │ GPIO5 ◄─TACH── Fan1 Pin3
                 │                  │ GPIO6 ◄─TACH── Fan2 Pin3
                 │                  │ GPIO7 ◄─TACH── Fan3 Pin3
                 │                  │ GPIO8 ◄─TACH── Fan4 Pin3
                 │                  │
                 │                  │ GPIO9 ◄──┬──► DS18B20 ×5 (DQ)
                 │             3.3V ├──────────┤ 4.7 kΩ pull-up
                 │                  │
                 │  USB-C (19/20)   │◄──────────────── USB to Linux host
                 └──────────────────┘

Fan 4-pin connector:
  Pin1 GND  ──── Common ground
  Pin2 12V  ──── 12 V PSU
  Pin3 TACH ──── 1 kΩ ──── GPIO (internal pull-up active)
  Pin4 PWM  ──── GPIO output

DS18B20 (×5, all in parallel):
  VDD ──── 3.3 V
  GND ──── GND
  DQ  ──┬─ GPIO 9
        └─ 4.7 kΩ ──── 3.3 V

Power rail (CRITICAL — shared ground):
  12 V PSU(+) ──── Fan Pin2 (×4)
  12 V PSU(−) ──┬─ Fan Pin1 (×4)
                └─ ESP32-S3 GND
```

---

## PlatformIO Setup

### Install

```bash
pip install platformio
# Or use VS Code with the PlatformIO extension
```

### Flash (first time — BOOT mode entry)

1. Hold **BOOT** button on the SuperMini
2. Press and release **RESET** while holding BOOT
3. Release **BOOT** — device enters download mode
4. Run:

```bash
cd esp32s3-fan-controller
pio run --target upload
```

After the first flash, subsequent uploads work automatically over CDC serial if ARDUINO_USB_CDC_ON_BOOT is handled correctly, or repeat the BOOT+RESET sequence.

### Monitor

```bash
pio device monitor -b 115200 -p /dev/ttyACM0
```

---

## First-Run Guide

### 1. Flash firmware

Follow the BOOT mode entry steps above.

### 2. Open serial terminal

```bash
screen /dev/ttyACM0 115200
# or: pio device monitor
```

### 3. Discover sensors

```
LIST_SENSORS
{"sensors":[{"index":0,"rom":"28FF3A2B0C000042","name":"Sensor 1"},…]}
```

### 4. Assign friendly names

```
SET_NAME 28FF3A2B0C000042 CPU Temperature
SET_NAME 28FF... GPU Temperature
```

### 5. Verify temperatures

```
GET_TEMPS
{"temps":[{"name":"CPU Temperature","rom":"28FF3A2B0C000042","value":42.5,"valid":true},…]}
```

### 6. Set fan curves

```
SET_CURVE 1 [[20,30],[40,50],[60,75],[80,100]]
SET_CURVE 2 [[20,30],[40,50],[60,75],[80,100]]
```

### 7. Map sensors to fans

```
SET_MAPPING 1 0   # Fan 1 follows sensor 0
SET_MAPPING 2 1   # Fan 2 follows sensor 1
```

### 8. Enable Corsair HID (device will reboot)

```
HID_ENABLE
```

### 9. Verify Linux detection

```bash
sensors
# Should show corsaircpro-hid-* with temperatures and RPMs
```

---

## Serial Protocol Reference

All commands are plain text. All responses are JSON terminated with `\n`.

| Command | Arguments | Response |
|---|---|---|
| `GET_TEMPS` | — | `{"temps":[{"name":"CPU","rom":"28FF…","value":45.5,"valid":true},…]}` |
| `GET_RPMS` | — | `{"rpms":[1200,1350,980,1100]}` |
| `SET_FAN` | `<fan 1–4> <pct 0–100>` | `{"ok":true}` |
| `SET_AUTO` | `<fan 1–4>` | `{"ok":true}` |
| `SET_ALL_AUTO` | — | `{"ok":true}` |
| `GET_CURVE` | `<fan 1–4>` | `{"fan":1,"curve":[[20,30],[40,50],…]}` |
| `SET_CURVE` | `<fan> [[t,p],…]` | `{"ok":true}` |
| `GET_MAPPING` | — | `{"mapping":[0,0,1,1]}` |
| `SET_MAPPING` | `<fan 1–4> <sensor 0–4>` | `{"ok":true}` |
| `LIST_SENSORS` | — | `{"sensors":[{"index":0,"rom":"28FF…","name":"CPU"},…]}` |
| `SET_NAME` | `<rom_hex> <name>` | `{"ok":true}` |
| `GET_STATUS` | — | Full system status JSON |
| `HID_ENABLE` | — | Reboots with HID active |
| `HID_DISABLE` | — | Reboots without HID |
| `SAVE_CONFIG` | — | `{"ok":true}` (no-op, saved on change) |
| `FACTORY_RESET` | — | Clears NVS, reboots |
| `VERSION` | — | `{"fw":"1.0.0","board":"ESP32-S3-SuperMini"}` |

---

## Corsair Commander Pro Protocol

HID reports: 16 bytes IN (device→host) and 16 bytes OUT (host→device).

| Cmd byte | Function | Request | Response |
|---|---|---|---|
| `0x01` | Firmware version | `[0x01, 0x00, …]` | `[0x01, 0x00, 0x09, 0x45]` |
| `0x02` | Product name | `[0x02, …]` | `[0x02, "Commander Pro"…]` |
| `0x08` | Sensor count | `[0x08, …]` | `[0x08, 0x05]` |
| `0x09` | Fan count | `[0x09, …]` | `[0x09, 0x04]` |
| `0x10` | Get temperature | `[0x10, sensor_idx, …]` | `[0x10, hi, lo]` (hundredths °C) |
| `0x21` | Get fan RPM | `[0x21, fan_idx, …]` | `[0x21, hi, lo]` |
| `0x23` | Set fan PWM % | `[0x23, fan_idx, pct, …]` | `[0x23, 0x00]` |
| `0x60` | Set fan mode | `[0x60, fan_idx, mode, …]` | `[0x60, 0x00]` (0=manual, 1=auto) |

---

## Fan Curve Profiles

### Silent
`[[20,20],[35,25],[50,40],[65,60],[75,85],[85,100]]`

### Balanced (default)
`[[20,30],[40,50],[60,75],[80,100]]`

### Performance
`[[20,50],[35,65],[50,80],[60,100]]`

---

## Linux Integration

### Automatic with corsair-cpro kernel module (Linux ≥ 5.8)

```bash
# Plug in with HID enabled — driver loads automatically
cat /sys/class/hwmon/hwmon*/name | grep corsair
# corsaircpro

sensors
# Shows temps and fan RPMs
```

### Sensor labels — /etc/sensors.d/fan-controller.conf

```conf
chip "corsaircpro-*"
    label temp1 "CPU Temperature"
    label temp2 "GPU Temperature"
    label temp3 "Motherboard"
    label temp4 "Intake Air"
    label temp5 "Exhaust Air"
    label fan1  "CPU Fan"
    label fan2  "GPU Fan"
    label fan3  "Front Intake"
    label fan4  "Rear Exhaust"
```

### fancontrol

```bash
sudo pwmconfig   # Auto-generates /etc/fancontrol
sudo systemctl enable --now fancontrol
```

### Prometheus (node_exporter)

```bash
node_exporter --collector.hwmon
# node_hwmon_temp_celsius{chip="corsaircpro",sensor="temp1"} 45.5
# node_hwmon_fan_rpm{chip="corsaircpro",sensor="fan1"} 1200
```

---

## Troubleshooting

### SuperMini not detected / upload fails
- Enter BOOT mode: hold BOOT, press RESET, release BOOT
- Check USB cable supports data (not charge-only)
- Try `lsusb` to confirm device appears

### HID not enumerating
- Verify `USB_VID=0x1b1c`, `USB_PID=0x0c10` build flags are present
- Check `ARDUINO_USB_MODE=0` is set (native USB, not UART bridge)
- Inspect `lsusb -v` output

### corsair-cpro module not loading
```bash
uname -r        # Kernel must be ≥ 5.8
modprobe corsair-cpro
dmesg | tail    # Check for binding errors
```

### DS18B20 not found (GET_TEMPS returns empty)
- Confirm 4.7 kΩ pull-up is on DQ line to 3.3 V
- Confirm sensors are NOT in parasite mode (VDD tied to 3.3 V, not left floating)
- Try with a single sensor first
- Check GPIO 9 is free (no other peripheral assigned)

### Fan tachometer reads 0 RPM
- Confirm `IRAM_ATTR` on ISR functions (already present in source)
- Check 1 kΩ series resistor and internal pull-up on GPIO 5–8
- Verify fan Pin 3 tach wire is connected
- Some fans emit 1 pulse/rev — adjust `TACH_PULSES_PER_REV` in `shared_types.h`

### CDC serial and HID not both present
- Confirm composite USB setup: `hidHandler_init()` called before `USB.begin()`
- Check `ARDUINO_USB_CDC_ON_BOOT=0` — CDC must be managed manually (done in `main.cpp`)
- If only one interface appears, try a powered USB hub (some ports limit current during enumeration)

---

## Code Structure

```
esp32s3-fan-controller/
├── platformio.ini           Board, framework, build flags, lib deps
├── include/
│   └── shared_types.h       All structs, enums, pin/constant defines
└── src/
    ├── main.cpp             Setup, USB init, FreeRTOS task creation, heartbeat
    ├── config_store.cpp/h   Preferences NVS wrapper (curves, mapping, names, HID flag)
    ├── fan_control.cpp/h    LEDC PWM init, duty write, ramping
    ├── tachometer.cpp/h     ISR pulse counting, RPM calculation, stall detection
    ├── temperature.cpp/h    OneWire/DallasTemperature async read, ROM→hex helpers
    ├── fan_curve.cpp/h      Breakpoint interpolation, hysteresis, safety override
    ├── hid_handler.cpp/h    USBHIDGeneric subclass, Commander Pro protocol
    └── serial_handler.cpp/h CDC command parser, JSON responses (ArduinoJson)
```

### FreeRTOS Tasks

| Task | Core | Period | Responsibility |
|---|---|---|---|
| `HID` | 0 | 2000 ms | Push Commander Pro HID report, process host commands |
| `Serial` | 0 | Event | Parse CDC serial commands, return JSON |
| `Temp` | 1 | 2000 ms | Async DS18B20 conversion + read |
| `Control` | 1 | 50 ms | Apply fan curves, ramp PWM |
| `Tacho` | 1 | 2000 ms | Calculate RPM from ISR counters |
