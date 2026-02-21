#!/usr/bin/env python3
"""
ESP32-S3 SuperMini Fan Controller CLI
Companion app for the Corsair Commander Pro emulation firmware.

Usage:
    python fan_controller_cli.py [--port /dev/ttyACM0] [--baud 115200]

Requirements:
    pip install pyserial colorama rich
"""

import argparse
import json
import os
import sys
import time
import serial
import serial.tools.list_ports
from datetime import datetime
from pathlib import Path

try:
    from colorama import init as colorama_init, Fore, Style
    colorama_init(autoreset=True)
except ImportError:
    print("ERROR: colorama not installed. Run: pip install colorama")
    sys.exit(1)

try:
    from rich.console import Console
    from rich.table import Table
    from rich.live import Live
    from rich.panel import Panel
    from rich.columns import Columns
    from rich.text import Text
    from rich import box
except ImportError:
    print("ERROR: rich not installed. Run: pip install rich")
    sys.exit(1)

# ─────────────────────────────────────────────────────────────────────────────
# Constants
# ─────────────────────────────────────────────────────────────────────────────

CONFIG_FILE = Path.home() / ".fan_controller_config.json"
CORSAIR_VID = 0x1B1C
CORSAIR_PID = 0x0C10
DEFAULT_BAUD = 115200
SERIAL_TIMEOUT = 3.0
DASHBOARD_REFRESH = 2.0

console = Console()


# ─────────────────────────────────────────────────────────────────────────────
# Config persistence
# ─────────────────────────────────────────────────────────────────────────────

def load_config() -> dict:
    if CONFIG_FILE.exists():
        try:
            with open(CONFIG_FILE) as f:
                return json.load(f)
        except Exception:
            pass
    return {}


def save_config(cfg: dict):
    try:
        with open(CONFIG_FILE, "w") as f:
            json.dump(cfg, f, indent=2)
    except Exception as e:
        console.print(f"[yellow]Warning: could not save config: {e}[/yellow]")


# ─────────────────────────────────────────────────────────────────────────────
# Serial connection helpers
# ─────────────────────────────────────────────────────────────────────────────

def list_candidate_ports() -> list[str]:
    """Return ports likely to be the ESP32-S3 CDC ACM device."""
    candidates = []
    for port in serial.tools.list_ports.comports():
        desc = (port.description or "").lower()
        mfg  = (port.manufacturer or "").lower()
        vid  = port.vid
        # Prefer Corsair VID or obvious CDC/ACM devices
        if vid == CORSAIR_VID:
            candidates.insert(0, port.device)
        elif "acm" in desc or "cdc" in desc or "esp32" in desc or "usb serial" in mfg:
            candidates.append(port.device)
    return candidates


def auto_detect_port() -> str | None:
    cfg = load_config()
    last = cfg.get("last_port")

    candidates = list_candidate_ports()
    if not candidates:
        # Fall back to all serial ports
        candidates = [p.device for p in serial.tools.list_ports.comports()]

    if not candidates:
        return None

    # If last used port is still available, prefer it
    if last and last in candidates:
        candidates.insert(0, last)
        candidates = list(dict.fromkeys(candidates))  # deduplicate, keep order

    if len(candidates) == 1:
        return candidates[0]

    console.print("\n[bold]Available serial ports:[/bold]")
    for i, p in enumerate(candidates):
        marker = " [green]← last used[/green]" if p == last else ""
        console.print(f"  {i+1}. {p}{marker}")

    while True:
        try:
            choice = input(f"\nSelect port [1-{len(candidates)}] (Enter for {candidates[0]}): ").strip()
            if choice == "":
                return candidates[0]
            idx = int(choice) - 1
            if 0 <= idx < len(candidates):
                return candidates[idx]
        except (ValueError, KeyboardInterrupt):
            return candidates[0]


def open_serial(port: str, baud: int = DEFAULT_BAUD) -> serial.Serial:
    ser = serial.Serial(port, baud, timeout=SERIAL_TIMEOUT)
    time.sleep(0.5)  # Let CDC enumerate
    ser.reset_input_buffer()
    return ser


# ─────────────────────────────────────────────────────────────────────────────
# Protocol layer
# ─────────────────────────────────────────────────────────────────────────────

class FanControllerError(Exception):
    pass


class FanController:
    def __init__(self, ser: serial.Serial):
        self.ser = ser

    def _send(self, command: str) -> dict:
        """Send a command and return parsed JSON response.

        The firmware protocol is strictly line-oriented:
          - One command sent as a single line (terminated with newline)
          - One response returned as a single JSON line via USBSerial.println()
          - No echo, no preamble, no multi-line output

        pyserial readline() blocks until a newline arrives or ser.timeout
        expires, which matches the firmware exactly. Non-JSON lines (stray
        debug output) are skipped; we keep reading until we get parseable
        JSON or the overall deadline passes.
        """
        self.ser.reset_input_buffer()
        self.ser.write((command.strip() + "\n").encode())
        self.ser.flush()

        deadline = time.time() + SERIAL_TIMEOUT
        while time.time() < deadline:
            raw = self.ser.readline()       # blocks up to ser.timeout for \n
            if not raw:
                # readline returned empty bytes — no newline within timeout
                raise FanControllerError(
                    f"Timeout waiting for response to: {command!r}"
                )
            line = raw.decode(errors="replace").strip()
            if not line:
                continue                    # skip blank lines
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                # Not JSON — stray debug print; keep reading
                continue

        raise FanControllerError(f"Timeout waiting for response to: {command!r}")

    def get_temps(self) -> list[dict]:
        r = self._send("GET_TEMPS")
        return r.get("temps", [])

    def get_rpms(self) -> list[int]:
        r = self._send("GET_RPMS")
        return r.get("rpms", [0, 0, 0, 0, 0, 0])

    def set_fan(self, fan: int, percent: int) -> bool:
        r = self._send(f"SET_FAN {fan} {percent}")
        return r.get("ok", False)

    def set_auto(self, fan: int) -> bool:
        r = self._send(f"SET_AUTO {fan}")
        return r.get("ok", False)

    def set_all_auto(self) -> bool:
        r = self._send("SET_ALL_AUTO")
        return r.get("ok", False)

    def get_curve(self, fan: int) -> dict:
        return self._send(f"GET_CURVE {fan}")

    def set_curve(self, fan: int, curve: list[list[int]]) -> bool:
        curve_json = json.dumps(curve)
        r = self._send(f"SET_CURVE {fan} {curve_json}")
        return r.get("ok", False)

    def get_mapping(self) -> list[int]:
        r = self._send("GET_MAPPING")
        return r.get("mapping", [0, 0, 0, 0, 0, 0])

    def set_mapping(self, fan: int, sensor: int) -> bool:
        r = self._send(f"SET_MAPPING {fan} {sensor}")
        return r.get("ok", False)

    def list_sensors(self) -> list[dict]:
        r = self._send("LIST_SENSORS")
        return r.get("sensors", [])

    def set_name(self, rom_hex: str, name: str) -> bool:
        r = self._send(f"SET_NAME {rom_hex} {name}")
        return r.get("ok", False)

    def get_status(self) -> dict:
        return self._send("GET_STATUS")

    def hid_enable(self) -> dict:
        return self._send("HID_ENABLE")

    def hid_disable(self) -> dict:
        return self._send("HID_DISABLE")

    def save_config(self) -> bool:
        r = self._send("SAVE_CONFIG")
        return r.get("ok", False)

    def factory_reset(self) -> dict:
        return self._send("FACTORY_RESET")

    def version(self) -> dict:
        return self._send("VERSION")


# ─────────────────────────────────────────────────────────────────────────────
# Display helpers
# ─────────────────────────────────────────────────────────────────────────────

def temp_color(val: float) -> str:
    if val < 0:
        return "dim"
    if val < 50:
        return "green"
    if val < 70:
        return "yellow"
    if val < 80:
        return "dark_orange"
    return "red"


def rpm_color(val: int) -> str:
    if val == 0:
        return "red"
    if val < 500:
        return "yellow"
    return "cyan"


def build_dashboard(fc: FanController) -> Panel:
    try:
        temps = fc.get_temps()
        rpms  = fc.get_rpms()
    except FanControllerError as e:
        return Panel(f"[red]Communication error: {e}[/red]", title="Error")

    # Temperature table
    temp_table = Table(title="🌡  Temperatures", box=box.ROUNDED, border_style="blue")
    temp_table.add_column("Sensor", style="bold")
    temp_table.add_column("ROM", style="dim")
    temp_table.add_column("Temperature", justify="right")
    temp_table.add_column("Status", justify="center")

    for t in temps:
        val   = t.get("value", -127.0)
        name  = t.get("name", "Unknown")
        rom   = t.get("rom", "?")[:12] + "…"
        if val <= -127.0:
            temp_str = "[dim]Disconnected[/dim]"
            status   = "[red]✗[/red]"
        else:
            color    = temp_color(val)
            temp_str = f"[{color}]{val:.1f} °C[/{color}]"
            status   = "[red]⚠ HOT[/red]" if val >= 85 else "[green]✓[/green]"
        temp_table.add_row(name, rom, temp_str, status)

    # Fan RPM table
    fan_table = Table(title="🌀  Fans", box=box.ROUNDED, border_style="cyan")
    fan_table.add_column("Fan", style="bold")
    fan_table.add_column("RPM", justify="right")
    fan_table.add_column("Status", justify="center")

    for i, rpm in enumerate(rpms, 1):
        color  = rpm_color(rpm)
        status = "[red]STALL[/red]" if rpm == 0 else "[green]OK[/green]"
        fan_table.add_row(f"Fan {i}", f"[{color}]{rpm:,}[/{color}]", status)

    now = datetime.now().strftime("%H:%M:%S")
    return Panel(
        Columns([temp_table, fan_table], equal=False, expand=True),
        title=f"[bold]ESP32-S3 Fan Controller[/bold]  [dim]— {now}[/dim]",
        border_style="bright_blue",
        padding=(0, 1),
    )


# ─────────────────────────────────────────────────────────────────────────────
# Interactive menus
# ─────────────────────────────────────────────────────────────────────────────

def prompt_int(prompt: str, lo: int, hi: int, default: int | None = None) -> int:
    hint = f" [{lo}-{hi}]"
    if default is not None:
        hint += f" (Enter={default})"
    while True:
        raw = input(f"{prompt}{hint}: ").strip()
        if raw == "" and default is not None:
            return default
        try:
            val = int(raw)
            if lo <= val <= hi:
                return val
            print(f"  Please enter a value between {lo} and {hi}.")
        except ValueError:
            print("  Invalid input, please enter an integer.")


def confirm(prompt: str, default: bool = False) -> bool:
    hint = " [Y/n]" if default else " [y/N]"
    raw = input(f"{prompt}{hint}: ").strip().lower()
    if raw == "":
        return default
    return raw in ("y", "yes")


# ── Fan curve editor ──────────────────────────────────────────────────────────

def fan_curve_editor(fc: FanController):
    console.print("\n[bold cyan]Fan Curve Editor[/bold cyan]")

    fan = prompt_int("Fan number", 1, 6)

    try:
        result = fc.get_curve(fan)
        current = result.get("curve", [])
    except FanControllerError as e:
        console.print(f"[red]Error fetching curve: {e}[/red]")
        return

    console.print(f"\nCurrent curve for Fan {fan}:")
    t = Table(box=box.SIMPLE)
    t.add_column("Point", style="dim")
    t.add_column("Temp (°C)", justify="right")
    t.add_column("PWM (%)", justify="right")
    for i, (temp, pwm) in enumerate(current):
        t.add_row(str(i + 1), str(temp), str(pwm))
    console.print(t)

    # Preset profiles
    presets = {
        "1": ("Silent",      [[20, 20], [35, 25], [50, 40], [65, 60], [75, 85], [85, 100]]),
        "2": ("Balanced",    [[20, 30], [40, 50], [60, 75], [80, 100]]),
        "3": ("Performance", [[20, 50], [35, 65], [50, 80], [60, 100]]),
        "4": ("Custom",      None),
    }

    console.print("\n  1. Silent profile")
    console.print("  2. Balanced profile")
    console.print("  3. Performance profile")
    console.print("  4. Custom (enter manually)")
    console.print("  5. Cancel")

    choice = input("\nSelect [1-5]: ").strip()
    if choice == "5" or choice == "":
        return

    if choice in presets and presets[choice][1] is not None:
        _, curve = presets[choice]
    elif choice == "4":
        curve = _enter_custom_curve()
        if curve is None:
            return
    else:
        console.print("[yellow]Invalid choice.[/yellow]")
        return

    try:
        ok = fc.set_curve(fan, curve)
        if ok:
            console.print(f"[green]✓ Fan {fan} curve updated.[/green]")
        else:
            console.print("[red]Device returned error.[/red]")
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")


def _enter_custom_curve() -> list[list[int]] | None:
    console.print("\nEnter breakpoints (up to 8). Temperatures must be ascending.")
    console.print("Leave temperature blank to finish.\n")
    curve = []
    prev_temp = -1
    for i in range(8):
        raw_temp = input(f"  Point {i+1} - Temperature (°C): ").strip()
        if raw_temp == "":
            break
        try:
            temp = int(raw_temp)
        except ValueError:
            console.print("[red]Invalid temperature.[/red]")
            return None
        if temp <= prev_temp:
            console.print("[red]Temperatures must be strictly ascending.[/red]")
            return None
        pwm = prompt_int(f"  Point {i+1} - PWM %", 0, 100)
        curve.append([temp, pwm])
        prev_temp = temp

    if len(curve) < 2:
        console.print("[yellow]Need at least 2 breakpoints.[/yellow]")
        return None

    console.print("\nCurve entered:")
    for temp, pwm in curve:
        console.print(f"  {temp}°C → {pwm}%")
    if not confirm("Apply this curve?", default=True):
        return None
    return curve


# ── Sensor naming wizard ──────────────────────────────────────────────────────

def sensor_naming_wizard(fc: FanController):
    console.print("\n[bold cyan]Sensor Naming Wizard[/bold cyan]")

    try:
        sensors = fc.list_sensors()
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")
        return

    if not sensors:
        console.print("[yellow]No sensors discovered.[/yellow]")
        return

    console.print(f"\nFound {len(sensors)} sensor(s):\n")

    for s in sensors:
        idx  = s.get("index", "?")
        rom  = s.get("rom", "?")
        name = s.get("name", "")
        console.print(f"  [{idx}] ROM: [dim]{rom}[/dim]  Current name: [cyan]{name or '(unnamed)'}[/cyan]")

    console.print()
    for s in sensors:
        rom      = s.get("rom", "")
        idx      = s.get("index", "?")
        cur_name = s.get("name", "")
        raw = input(f"  Name for sensor [{idx}] (Enter to skip) [{cur_name}]: ").strip()
        if raw:
            try:
                ok = fc.set_name(rom, raw)
                if ok:
                    console.print(f"  [green]✓ Saved: '{raw}'[/green]")
                else:
                    console.print("  [red]Device returned error.[/red]")
            except FanControllerError as e:
                console.print(f"  [red]Error: {e}[/red]")

    console.print("\n[green]Done.[/green]")


# ── Manual fan control ────────────────────────────────────────────────────────

def manual_fan_control(fc: FanController):
    console.print("\n[bold cyan]Manual Fan Control[/bold cyan]")
    console.print("  1. Set fan speed (manual)")
    console.print("  2. Return fan to auto")
    console.print("  3. Return ALL fans to auto")
    console.print("  4. Back")

    choice = input("\nSelect [1-4]: ").strip()

    if choice == "1":
        fan     = prompt_int("Fan number", 1, 6)
        percent = prompt_int("Speed %", 0, 100)
        try:
            ok = fc.set_fan(fan, percent)
            if ok:
                console.print(f"[green]✓ Fan {fan} set to {percent}%[/green]")
            else:
                console.print("[red]Device returned error.[/red]")
        except FanControllerError as e:
            console.print(f"[red]Error: {e}[/red]")

    elif choice == "2":
        fan = prompt_int("Fan number", 1, 6)
        try:
            ok = fc.set_auto(fan)
            console.print(f"[green]✓ Fan {fan} set to auto[/green]" if ok else "[red]Error.[/red]")
        except FanControllerError as e:
            console.print(f"[red]Error: {e}[/red]")

    elif choice == "3":
        try:
            ok = fc.set_all_auto()
            console.print("[green]✓ All fans set to auto[/green]" if ok else "[red]Error.[/red]")
        except FanControllerError as e:
            console.print(f"[red]Error: {e}[/red]")


# ── Sensor mapping ────────────────────────────────────────────────────────────

def sensor_mapping_menu(fc: FanController):
    console.print("\n[bold cyan]Fan → Sensor Mapping[/bold cyan]")

    try:
        mapping = fc.get_mapping()
        sensors = fc.list_sensors()
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")
        return

    # Build index → name lookup
    sensor_names = {s["index"]: s.get("name") or s.get("rom", "?")[:12] for s in sensors}

    t = Table(box=box.SIMPLE)
    t.add_column("Fan")
    t.add_column("Mapped Sensor", style="cyan")
    for i, si in enumerate(mapping):
        t.add_row(f"Fan {i+1}", f"[{si}] {sensor_names.get(si, '?')}")
    console.print(t)

    if not confirm("\nChange a mapping?"):
        return

    fan    = prompt_int("Fan number", 1, 6)
    sensor = prompt_int("Sensor index", 0, len(sensors) - 1)

    try:
        ok = fc.set_mapping(fan, sensor)
        console.print(f"[green]✓ Fan {fan} now mapped to sensor {sensor}[/green]" if ok else "[red]Error.[/red]")
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")


# ── HID toggle ────────────────────────────────────────────────────────────────

def hid_toggle_menu(fc: FanController):
    console.print("\n[bold cyan]Corsair HID Emulation[/bold cyan]")

    try:
        status = fc.get_status()
        hid_on = status.get("hid_enabled", False)
    except FanControllerError as e:
        console.print(f"[red]Error reading status: {e}[/red]")
        return

    state_str = "[green]ENABLED[/green]" if hid_on else "[red]DISABLED[/red]"
    console.print(f"\nCurrent state: {state_str}")

    if hid_on:
        console.print("\n  1. Disable HID (CDC only)")
    else:
        console.print("\n  1. Enable HID (Corsair Commander Pro emulation)")
    console.print("  2. Back")

    choice = input("\nSelect [1-2]: ").strip()
    if choice != "1":
        return

    action = "disable" if hid_on else "enable"
    console.print(f"\n[yellow]⚠  This will {action} Corsair HID emulation and reboot the device.[/yellow]")
    if not hid_on:
        console.print("[dim]After reboot, Linux will load the 'corsair-cpro' kernel module automatically.[/dim]")
        console.print("[dim]Run 'sensors' to verify detection.[/dim]")

    if not confirm(f"Confirm {action} HID and reboot?"):
        return

    try:
        if hid_on:
            r = fc.hid_disable()
        else:
            r = fc.hid_enable()
        console.print(f"[green]✓ Command sent. Device is rebooting…[/green]")
        if r.get("reboot"):
            console.print("[dim]Reconnect after ~3 seconds.[/dim]")
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")


# ── Export ────────────────────────────────────────────────────────────────────

def export_readings(fc: FanController):
    console.print("\n[bold cyan]Export Readings[/bold cyan]")
    console.print("  1. JSON")
    console.print("  2. CSV")
    console.print("  3. Back")

    choice = input("\nSelect [1-3]: ").strip()
    if choice not in ("1", "2"):
        return

    try:
        temps = fc.get_temps()
        rpms  = fc.get_rpms()
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")
        return

    timestamp = datetime.now().isoformat(timespec="seconds")
    data = {
        "timestamp": timestamp,
        "temperatures": temps,
        "fan_rpms": [{"fan": i + 1, "rpm": r} for i, r in enumerate(rpms)],
    }

    default_name = f"fan_controller_{datetime.now().strftime('%Y%m%d_%H%M%S')}"

    if choice == "1":
        fname = input(f"Filename [{default_name}.json]: ").strip() or f"{default_name}.json"
        if not fname.endswith(".json"):
            fname += ".json"
        with open(fname, "w") as f:
            json.dump(data, f, indent=2)
        console.print(f"[green]✓ Saved: {fname}[/green]")

    else:
        fname = input(f"Filename [{default_name}.csv]: ").strip() or f"{default_name}.csv"
        if not fname.endswith(".csv"):
            fname += ".csv"
        with open(fname, "w") as f:
            f.write("timestamp,type,index,name,value,unit\n")
            for t in temps:
                val = t.get("value", -127)
                name = t.get("name", t.get("rom", "?"))
                f.write(f"{timestamp},temperature,{t.get('index','')},{name},{val},celsius\n")
            for i, r in enumerate(rpms):
                f.write(f"{timestamp},fan_rpm,{i+1},Fan {i+1},{r},rpm\n")
        console.print(f"[green]✓ Saved: {fname}[/green]")


# ── Status / Version ──────────────────────────────────────────────────────────

def show_status(fc: FanController):
    try:
        ver    = fc.version()
        status = fc.get_status()
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")
        return

    console.print()
    t = Table(title="Device Status", box=box.ROUNDED, border_style="blue")
    t.add_column("Key", style="bold")
    t.add_column("Value", style="cyan")
    t.add_row("Firmware", ver.get("fw", "?"))
    t.add_row("Board", ver.get("board", "?"))
    t.add_row("Uptime (s)", str(status.get("uptime", "?")))
    t.add_row("HID enabled", "[green]Yes[/green]" if status.get("hid_enabled") else "[red]No[/red]")
    console.print(t)


# ── Factory reset ─────────────────────────────────────────────────────────────

def factory_reset_menu(fc: FanController):
    console.print("\n[bold red]Factory Reset[/bold red]")
    console.print("[yellow]This will erase all stored configuration and reboot the device.[/yellow]")
    console.print("All sensor names, fan curves, and mappings will be lost.\n")

    if not confirm("Are you absolutely sure?"):
        return
    if not confirm("Confirm a second time — this cannot be undone"):
        return

    try:
        fc.factory_reset()
        console.print("[green]✓ Factory reset command sent. Device is rebooting…[/green]")
    except FanControllerError as e:
        console.print(f"[red]Error: {e}[/red]")


# ─────────────────────────────────────────────────────────────────────────────
# Live dashboard
# ─────────────────────────────────────────────────────────────────────────────

def live_dashboard(fc: FanController):
    console.print("\n[dim]Press Ctrl+C to exit dashboard.[/dim]\n")
    try:
        with Live(build_dashboard(fc), refresh_per_second=0.5, console=console) as live:
            while True:
                time.sleep(DASHBOARD_REFRESH)
                live.update(build_dashboard(fc))
    except KeyboardInterrupt:
        pass


# ─────────────────────────────────────────────────────────────────────────────
# Main menu
# ─────────────────────────────────────────────────────────────────────────────

MENU_ITEMS = [
    ("Live dashboard",              live_dashboard),
    ("Manual fan control",          manual_fan_control),
    ("Fan curve editor",            fan_curve_editor),
    ("Sensor → fan mapping",        sensor_mapping_menu),
    ("Sensor naming wizard",        sensor_naming_wizard),
    ("Corsair HID toggle",          hid_toggle_menu),
    ("Export readings",             export_readings),
    ("Device status / version",     show_status),
    ("Factory reset",               factory_reset_menu),
    ("Quit",                        None),
]


def main_menu(fc: FanController):
    while True:
        console.print("\n" + "─" * 50)
        console.print("[bold bright_blue]  ESP32-S3 Fan Controller[/bold bright_blue]")
        console.print("─" * 50)

        for i, (label, _) in enumerate(MENU_ITEMS, 1):
            color = "red" if label == "Factory reset" else "white"
            console.print(f"  [{color}]{i:2}. {label}[/{color}]")

        try:
            raw = input("\nSelect option: ").strip()
            idx = int(raw) - 1
        except (ValueError, EOFError, KeyboardInterrupt):
            break

        if 0 <= idx < len(MENU_ITEMS):
            label, fn = MENU_ITEMS[idx]
            if fn is None:
                break
            try:
                fn(fc)
            except Exception as e:
                console.print(f"\n[red]Unexpected error: {e}[/red]")
        else:
            console.print("[yellow]Invalid choice.[/yellow]")


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="ESP32-S3 Fan Controller CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                        # auto-detect port
  %(prog)s --port /dev/ttyACM0    # specify port
  %(prog)s --port COM5 --baud 115200
        """,
    )
    parser.add_argument("--port",  help="Serial port (e.g. /dev/ttyACM0 or COM5)")
    parser.add_argument("--baud",  type=int, default=DEFAULT_BAUD, help=f"Baud rate (default {DEFAULT_BAUD})")
    parser.add_argument("--dashboard", action="store_true", help="Jump straight to live dashboard")
    args = parser.parse_args()

    console.print("\n[bold bright_blue]ESP32-S3 SuperMini Fan Controller CLI[/bold bright_blue]")
    console.print("[dim]Corsair Commander Pro Emulation Companion[/dim]\n")

    # Resolve port
    port = args.port
    if not port:
        port = auto_detect_port()
    if not port:
        console.print("[red]No serial port found. Connect the device and try again.[/red]")
        sys.exit(1)

    # Connect
    console.print(f"Connecting to [cyan]{port}[/cyan] @ {args.baud} baud…")
    try:
        ser = open_serial(port, args.baud)
    except serial.SerialException as e:
        console.print(f"[red]Failed to open port: {e}[/red]")
        sys.exit(1)

    # Persist port for next run
    cfg = load_config()
    cfg["last_port"] = port
    save_config(cfg)

    fc = FanController(ser)

    # Quick connectivity check
    try:
        ver = fc.version()
        console.print(
            f"[green]✓ Connected:[/green] {ver.get('board', 'unknown')}  "
            f"fw [cyan]{ver.get('fw', '?')}[/cyan]"
        )
    except FanControllerError:
        console.print("[yellow]⚠  Device connected but version check failed. Continuing anyway.[/yellow]")

    try:
        if args.dashboard:
            live_dashboard(fc)
        else:
            main_menu(fc)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    console.print("\n[dim]Goodbye.[/dim]\n")


if __name__ == "__main__":
    main()