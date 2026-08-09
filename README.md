# FA_Control — ESP32-C3 LISY Pinball Web Controller

Web-based control of a pinball machine over the
[LISY protocol](https://missionpinball.org/latest/hardware/lisy/protocol/). The ESP32-C3
acts as a LISY host: it serves a retro-styled single-page interface and sends the control
commands as binary LISY frames over UART to the machine. Its counterpart is the VHDL module
`rtl/fa_control/`, first implemented in [AtariFA](https://github.com/bontango/AtariFA) and
portable to the other FA FPGA projects.

Lamps, coils, switches, sounds and displays — one click each, no meter and no
disassembly. The machine reports its own hardware inventory during the handshake, so
nothing has to be configured by hand. **Until you press CONNECT, the machine belongs to
itself:** FA_Control never announces itself at power-up, and the control menus stay locked
until the far side actually grants control.

| | |
|---|---|
| Board | ESP32-C3, USB-Serial/JTAG |
| Link to the machine | UART1: TX = GPIO7, RX = GPIO6, 115200 8N1 |
| Framework | ESP-IDF v5.5.1 |
| Firmware version | see `version.txt` |
| Releases | [lisy.dev/swrep/misc/FA_Control/bin/](https://lisy.dev/swrep/misc/FA_Control/bin/) — installable over the air from the device itself |

## Documentation

| Document | For whom |
|---|---|
| [docs/USER_MANUAL.md](docs/USER_MANUAL.md) | Operating the device: DIP switches, LED patterns, Wi-Fi setup, connecting, the menus, firmware updates. No technical detail. |
| [docs/BEDIENUNGSANLEITUNG.md](docs/BEDIENUNGSANLEITUNG.md) | The same manual in German. |
| [docs/TECHNICAL_REFERENCE.md](docs/TECHNICAL_REFERENCE.md) | Firmware internals: GPIO map, power management, connection handshake, LISY command surface, REST API, OTA, build and release procedure. |

## Quick start

1. Set **DIP 1 to ON** — it is the power switch; OFF means deep sleep.
2. Join the open Wi-Fi network **`FA-Control`**; the captive portal opens the page
   (otherwise `http://192.168.4.1`).
3. Under **WI-FI → NETWORK** enter your SSID and password, then **SAVE + REBOOT**.
4. Reach the device at **`http://fa-control.local`** and press **CONNECT**.

## Building

```powershell
. C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
idf.py -B C:\Users\bonta\esp\build\fa build
idf.py -B C:\Users\bonta\esp\build\fa -p COM7 flash monitor
```

Two traps are worth knowing before the first build — the mandatory **local** build
directory (`-B`, because `N:` is a UNC network drive) and the **`IDF_PYTHON_ENV_PATH`**
user variable that pins the ESP-IDF Python to 3.11.2. Both are explained in
[docs/TECHNICAL_REFERENCE.md § 11](docs/TECHNICAL_REFERENCE.md#11-build--flash).

Releases are built and uploaded with `.\build_and_deploy.ps1` (see § 12 there).

---

Part of the FA project family — <https://github.com/bontango/> · <https://lisy.dev/>
