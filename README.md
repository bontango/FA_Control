# FA_Control — ESP32-C3 LISY Pinball Web Controller

Web-based control of a pinball machine over the
[LISY protocol](https://missionpinball.org/latest/hardware/lisy/protocol/). The ESP32-C3
acts as a LISY host: it serves a single-page interface in the colours of lisy.dev and sends the control
commands as binary LISY frames over UART to the machine. Its counterpart is the VHDL module
`rtl/fa_control/`, first implemented in [AtariFA](https://github.com/bontango/AtariFA) and
portable to the other FA FPGA projects.

Lamps, coils, switches, sounds and displays — one click each, no meter and no
disassembly. The machine reports its own hardware inventory during the handshake, so
nothing has to be configured by hand. **Until you press CONNECT, the machine belongs to
itself:** FA_Control never announces itself at power-up, and the control menus stay locked
until the far side actually grants control.

Optionally a **naming file** per machine turns the numbered tiles into speaking names — coil
7 reads *Knocker* instead of *7*. Where the file lives is the machine ID the board reports:
a folder per device, the game number as the file name — `<hardware>/<game>.cfg`, e.g.
`AtariFA/002.cfg`. It is picked up automatically on connect. See
[`names/example.cfg`](names/example.cfg) for the format.

FA boards with an ESP32 socket (first: **SternFA** PCB v2.00 from 5.0.6) can also boot their
**game rom** from here instead of from their SD card: right after reading its DIP switches
the board asks for its game. Roms are kept per device like the naming files -
`<hardware>/<nnn>.bin`, uploaded from the browser or loaded from
[lisy.dev](https://lisy.dev/swrep/misc/FA_Control/roms/) (`roms/<hardware>/<nnn>[_<title>].bin`).
That is the one exchange FA_Control takes part in without CONNECT - it only answers, it never
starts it. See the technical reference, section 6.3.

| | |
|---|---|
| Board | ESP32-C3, USB-Serial/JTAG |
| Link to the machine | UART1: TX = GPIO7, RX = GPIO6, 115200 8N1 |
| Framework | ESP-IDF v5.5.1 |
| Firmware version | see `version.txt` |
| Releases | [lisy.dev/swrep/misc/FA_Control/bin/](https://lisy.dev/swrep/misc/FA_Control/bin/) — installable over the air from the device itself |
| Full installation | [web installer](https://lisy.dev/swrep/misc/FA_Control/flasher/FA_Control_flasher.html) — writes bootloader, partition table, OTA data and firmware over USB from Chrome or Edge |

## Documentation

| Document | For whom |
|---|---|
| [docs/USER_MANUAL.md](docs/USER_MANUAL.md) | Operating the device: DIP switches, LED patterns, Wi-Fi setup, connecting, the menus, firmware updates. No technical detail. |
| [docs/BEDIENUNGSANLEITUNG.md](docs/BEDIENUNGSANLEITUNG.md) | The same manual in German. |
| [docs/USB_FLASH.md](docs/USB_FLASH.md) | Full installation over USB with the browser-based installer: when you need it, what it keeps, what to do when it goes wrong. |
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

Note that `flash` (not `app-flash`) is what writes the partition table. Naming files live
on their own LittleFS partition, and an over-the-air update never touches the table — a
device that has only seen OTA updates simply reports no naming storage and hides that menu.
The same applies to the enlarged firmware slots of v1.18 (1856 kB instead of 1536 kB): they
too arrive only with a full installation. Without ESP-IDF that is available from a browser:
see [docs/USB_FLASH.md](docs/USB_FLASH.md).

Two traps are worth knowing before the first build — the mandatory **local** build
directory (`-B`, because `N:` is a UNC network drive) and the **`IDF_PYTHON_ENV_PATH`**
user variable that pins the ESP-IDF Python to 3.11.2. Both are explained in
[docs/TECHNICAL_REFERENCE.md § 11](docs/TECHNICAL_REFERENCE.md#11-build--flash).

Releases are built and uploaded with `.\build_and_deploy.ps1` (see § 12 there). The full
flash package that feeds the web installer comes from `.\build_and_deploy_full.ps1`, the
installer page itself from `.\flasher\deploy_flasher.ps1` — a version bump wants all three.

---

Part of the FA project family — <https://github.com/bontango/> · <https://lisy.dev/>
