# ESP32-S3 Fan Controller CLI

Companion Python app for the ESP32-S3 SuperMini fan controller firmware
with Corsair Commander Pro emulation.

---

## Requirements

Python 3.10+ (uses `X | Y` type hints and `match`)

```bash
pip install -r requirements.txt
```

---

## Quick Start

```bash
# Auto-detect the device port
python fan_controller_cli.py

# Specify port explicitly
python fan_controller_cli.py --port /dev/ttyACM0

# Windows
python fan_controller_cli.py --port COM5

# Jump straight to live dashboard
python fan_controller_cli.py --dashboard
```

The last-used port is saved to `~/.fan_controller_config.json` so you won't
need to select it again on subsequent runs.

---

## Features

| Feature | Description |
|---|---|
| **Auto port detection** | Scans for CDC ACM / Corsair VID devices automatically |
| **Live dashboard** | Refreshes every 2 s — all temperatures + fan RPMs with colour coding |
| **Manual fan control** | Set any fan to a fixed PWM % or return to auto/curve mode |
| **Fan curve editor** | Built-in presets (Silent / Balanced / Performance) + custom entry |
| **Sensor naming wizard** | Lists discovered DS18B20 ROM addresses and prompts for friendly names |
| **Sensor mapping** | Choose which temperature sensor controls each fan |
| **HID toggle** | Enable/disable Corsair Commander Pro USB HID emulation (triggers reboot) |
| **Export** | Save current readings to JSON or CSV with timestamp |
| **Device status** | Firmware version, board ID, uptime, HID state |
| **Factory reset** | Double-confirmed wipe of all NVS config |

---

## Live Dashboard

Colour coding:

| Colour | Meaning |
|---|---|
| Green | Normal (temp < 50 °C / fan spinning) |
| Yellow | Warm (temp 50–70 °C) |
| Orange | Hot (temp 70–80 °C) |
| Red | Critical (temp ≥ 80 °C) or fan stalled (0 RPM) |

---

## Fan Curve Presets

| Profile | Breakpoints |
|---|---|
| Silent | 20→20%, 35→25%, 50→40%, 65→60%, 75→85%, 85→100% |
| Balanced | 20→30%, 40→50%, 60→75%, 80→100% |
| Performance | 20→50%, 35→65%, 50→80%, 60→100% |

You can also enter custom breakpoints with up to 8 points. Temperatures must
be strictly ascending.

---

## Linux `sensors` Integration

After enabling HID emulation and rebooting the device:

```bash
# Verify the kernel driver loaded
$ lsmod | grep corsair
corsair_cpro           ...

# Check hwmon
$ sensors
corsaircpro-hid-3-3
Adapter: HID adapter
CPU Temperature:  +45.5°C
GPU Temperature:  +52.2°C
fan1:            1200 RPM
fan2:            1350 RPM
fan3:             980 RPM
fan4:            1100 RPM
```

Add sensor labels in `/etc/sensors.d/fan-controller.conf`:

```conf
chip "corsaircpro-*"
    label temp1 "CPU Temperature"
    label temp2 "GPU Temperature"
    label temp3 "Motherboard"
    label fan1  "CPU Fan"
    label fan2  "GPU Fan"
```

---

## Troubleshooting

**Device not detected**
- Check the ESP32-S3 is powered and the USB-C cable supports data
- Try entering download mode: hold BOOT, press RESET, release BOOT
- On Linux check `dmesg | grep ttyACM` after plugging in
- Add yourself to the `dialout` group: `sudo usermod -aG dialout $USER` (re-login required)

**Timeout errors**
- The firmware may still be booting — wait ~3 s and retry
- Check baud rate matches firmware (default 115200)
- Try unplugging and replugging the USB cable

**HID not loading `corsair-cpro`**
- Requires kernel 5.8+: `uname -r`
- Try: `sudo modprobe corsair-cpro`
- Verify VID/PID: `lsusb | grep -i corsair` should show `1b1c:0c10`

**Fan RPM reading 0**
- Check tachometer wiring (Fan Pin 3) and pull-up
- ISR functions must be marked `IRAM_ATTR` in firmware
- Some fans don't emit tach pulses below ~20% PWM — raise minimum speed

**DS18B20 shows -127 °C**
- Sensor disconnected or wiring fault
- Check 4.7 kΩ pull-up resistor on the data line
- Ensure NOT using parasite mode (VDD connected to 3.3 V, not data line)
