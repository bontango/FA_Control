# FA_Control — Technical Reference

Firmware internals, hardware bindings, protocol details and build procedure.
For operating the device see [USER_MANUAL.md](USER_MANUAL.md) (English) or
[BEDIENUNGSANLEITUNG.md](BEDIENUNGSANLEITUNG.md) (German).

---

## 1. Overview

FA_Control is an ESP-IDF application for the ESP32-C3. It acts as a **LISY host**: it
serves a single-page web interface and translates the operator's clicks into binary
[LISY protocol](https://missionpinball.org/latest/hardware/lisy/protocol/) (v0.12)
commands sent over UART to the pinball machine.

The counterpart is the VHDL module `rtl/fa_control/` inside the FA FPGA projects, first
implemented in [AtariFA](https://github.com/bontango/AtariFA) (documentation there:
`docs/FA_Control_Interface.md`). **Both sides implement the same handshake — a change to
the protocol here requires a matching change there, and vice versa.**

Two design decisions shape everything else:

- **Nothing works without a granted connection.** The machine's hardware inventory (how
  many lamps, coils, switches, sounds, displays) is reported by the device during the
  handshake. There is no manual entry, no stored fallback and no auto-connect at boot.
  Until the far side grants control, all counts are 0 and every control endpoint rejects
  by range check alone.
- **Deep sleep is the normal state.** FA_Control is a service tool that lives permanently
  inside the machine. DIP 1 is its power switch.

---

## 2. Hardware

| | |
|---|---|
| Board | ESP32-C3 (USB-Serial/JTAG, development board on **COM7**) |
| LISY link | UART1: **TX = GPIO7**, **RX = GPIO6**, 115200 baud, 8N1 |
| Framework | ESP-IDF **v5.5.1** (`C:\Users\bonta\esp\v5.5.1\esp-idf`) |
| Flash | 4 MB, two OTA partitions of 1.5 MB each |

### 2.1 GPIO map

Single source of truth: [`main/board_pins.h`](../main/board_pins.h). Configured by
[`main/board.c`](../main/board.c).

| Signal | GPIO | Direction / pull | Notes |
|---|---|---|---|
| LISY UART1 TX | 7 | output | net `ESP32_TX` → FPGA PIN_33 (input there) |
| LISY UART1 RX | 6 | input | net `ESP32_RX` → FPGA PIN_44 (output there) |
| Control request → machine | 10 | output, **active low** | net `ESP32_IO10` → FPGA PIN_11; far side has a weak pull-up |
| I2C SDA | 5 | open drain, external pull-up | reserved only, no driver init |
| I2C SCL **/ blue LED** | 8 | open drain resp. output | **dual use**, see 2.2. Strapping pin; external pull-up keeps it high at reset |
| Push button | 9 | input, **internal** pull-up | BOOT pin; held during reset → download mode |
| DIP 1 | 0 | input, internal pull-up | ON = GND = **awake**, OFF = deep sleep |
| DIP 2 | 1 | input, internal pull-up | ON = blink indicator off (frees GPIO8 for I2C) |
| DIP 3 | 2 | input, internal pull-up | unassigned (strapping pin, also kept as reserve) |
| DIP 4 | 4 | input, internal pull-up | unassigned |

Not available on the ESP32-C3: GPIO11–17 (SPI flash), GPIO18/19 (USB-Serial/JTAG),
GPIO20/21 (UART0 console). Strapping pins that must read high at reset: GPIO2, GPIO8,
GPIO9.

> **Corrected in v1.10:** TX and RX were swapped, and DIP 3 was listed on GPIO3 (`<nc>` on
> the connector). The AtariFA schematic (connector X7 ↔ X1P) is authoritative, not the
> header file. With the old assignment both sides would have driven PIN_44 against each
> other; it never showed up because until then there was no counterpart at all.

### 2.2 GPIO8 is used twice

The module's blue LED sits on GPIO8, and the same pin is reserved as I2C SCL. As long as
I2C is unused (no driver init), the pin belongs to the blink indicator. **If I2C is ever
put into service, DIP 2 must be set to ON** — `power_mgr` then leaves GPIO8 unconfigured
and high-impedance so the I2C driver can claim it cleanly. When editing `board_pins.h`,
always consider both entries together.

---

## 3. Power management

[`main/power_mgr.c`](../main/power_mgr.c). `power_mgr_boot_gate()` runs early in
`app_main()` — before `lisy_init()`, because a sleeping device needs no UART — and only
returns when DIP 1 is ON. `power_mgr_start()` then launches a 50 ms task that drives the
blink indicator and watches DIP 1 during operation.

| DIP 1 | Behaviour |
|---|---|
| **ON** | awake: Wi-Fi, web server, control. Blue LED blinking. |
| **OFF** | deep sleep. Woken through the same pin — setting DIP 1 to ON is enough. |

### 3.1 Blink patterns

| Pattern | Meaning |
|---|---|
| **1 Hz** | awake, no control over the machine |
| **5 Hz** | control granted, watchdog running |
| **10 Hz** | boot grace period, deep sleep imminent |
| dark | deep sleep — or DIP 2 is ON |

### 3.2 Order of operations when falling asleep

`enter_deep_sleep()` ([`power_mgr.c:78`](../main/power_mgr.c)) is deliberately sequenced:

1. `fa_connect_release()` — **release control first.** Without this the far side would
   only recover after its own watchdog timeout (~2 s), and the machine would stand still
   for that long. This also drops GPIO10 and stops the watchdog.
2. LED off, `wifi_mgr_stop()` — the latter unregisters the event handlers before
   `esp_wifi_stop()`; without that the call used to hang and the device simply kept
   running instead of sleeping.
3. `gpio_hold_en()` on GPIO10 and (if enabled) GPIO8, then `gpio_deep_sleep_hold_en()`.
   Unheld pads would float: GPIO10 would hang on the FPGA's weak pull-up alone, and GPIO8
   could make the LED glow faintly.
4. `esp_deep_sleep_enable_gpio_wakeup()` on DIP 1, low level.

DIP 1 must be OFF for `DIP_DEBOUNCE_TICKS` (3 × 50 ms) in a row before sleep is entered —
a single misread must not shut the machine down.

### 3.3 The flash trap

In deep sleep the USB-Serial/JTAG port disappears; `idf.py flash` no longer finds COM7 and
`idf.py monitor` aborts. Therefore the device stays **awake for 30 s after every boot**
even with DIP 1 OFF (`POWER_BOOT_GRACE_MS`, LED at 10 Hz). Flashing is possible in that
window. Simpler: set DIP 1 to ON for flashing. Last resort is the BOOT button held during
reset → download mode.

### 3.4 Why DIP 1 must sit on GPIO0

The ESP32-C3 can only be woken from deep sleep through **GPIO0–GPIO5**
(`SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK`). DIP 1 is on GPIO0 and is therefore the only
candidate on the bank — the button on GPIO9 could not do it. Keep this in mind when
reassigning the DIP bank.

---

## 4. Connection handshake

[`main/fa_connect.c`](../main/fa_connect.c). Triggered exclusively by `POST /api/connect`,
i.e. by the operator pressing **CONNECT**. Never at boot: as long as nobody asks, the
machine belongs to itself.

1. `board_ctrl_request(true)` asserts GPIO10 (active low), then waits `CTRL_SETTLE_MS`
   (20 ms). The far side synchronises the request line through two flip-flops; 20 ms is
   ample and also covers a slow edge on the external pull-up.
2. `lisy_init_reset()` sends **0x64 (`LISY_INIT`)**. The reply decides:

| Reply | `fa_conn_state_t` | Message shown in the banner |
|---|---|---|
| `0` | `FA_CONN_ACTIVE` | `Control granted` |
| `1` | `FA_CONN_DENIED_DIP` | `Control denied - set option DIP 4 to ON` |
| `2` | `FA_CONN_DENIED_REQ` | `Control denied - request line (GPIO10) not seen` |
| other | `FA_CONN_DENIED_OTHER` | `Control denied` |
| timeout (`-1`) | `FA_CONN_NO_ANSWER` | `No answer - check wiring and power` |
| — | `FA_CONN_IDLE` | `Not connected` |

   `0` and the error numbering are defined by the counterpart (`rtl/fa_control/fa_control.vhd`).
   On AtariFA the enable switch of code `1` is **option DIP 4**.
3. On failure GPIO10 is released again immediately and the watchdog is switched off — a
   permanently asserted request line without granted control would just be a trip wire,
   and a second failed attempt must not leave a heartbeat from the first one running.
4. On success `query_counts()` reads the **info group 0x00–0x09** and fills
   `fa_conn_info_t`. Only plausible answers are accepted (`0 < n ≤ max`); whatever the
   device does not answer stays 0 and is therefore simply absent from the interface.
   `0` sounds is a valid statement ("this machine has no audio") and is accepted.
   For each display, opcode `0x07` is asked per index and yields type + digit count.
5. Finally the watchdog is enabled and the stored coil pulse time is pushed to the device.

`fa_conn_info_t` ([`fa_connect.h`](../main/fa_connect.h)) holds the identification strings
and the hardware inventory. **It belongs to the connection, not to the persistent
configuration** — `fa_connect_release()` zeroes it.

### 4.1 The watchdog is not an option

`0x65` every 500 ms, driven by an `esp_timer` in [`lisy.c`](../main/lisy.c). It is at the
same time the far side's **dead man's switch**: if it stops, the machine returns control to
the game on its own after roughly 2 s. It is therefore a condition of active control, not a
comfort feature — which is exactly why it is coupled to the connection instead of being
configurable. Making it switchable would mean declaring loss of control a user preference.

---

## 5. LISY command surface

Opcodes in [`main/lisy.h`](../main/lisy.h), maxima in
[`main/app_config.h`](../main/app_config.h). All UART access is mutex protected.

| Category | Max | Commands |
|---|---|---|
| Lamps | 255 | `0x0B` on, `0x0C` off — state mirrored locally in a 32-byte bitmap |
| Coils | 127 | `0x17` pulse (**never continuous**), `0x18` pulse time |
| Switches | 127 | `0x28` read all, `0x29` read changes — **read only**, the protocol has no way to set a switch |
| Sound | 255 | `0x32` play (track 1), `0x33` stop |
| Displays | 7 | `0x1E + index`, BCD7 encoded, right-aligned, blank = `0x0F` |

**Coil numbers are 1-based on the wire** (`main/lisy.c`), every other category is 0-based.
That is the LISY convention (`lisy_5_28/src/lisy/lisy_w.c`: "sol number starts with 1")
and it matches the driver names of the schematic, Q1..Qn. Since FA_Control 1.16; the
counterpart has to agree, on AtariFA that means SW 0.2.0 or newer.

Info group:

| Opcode | Returns |
|---|---|
| `0x00` | string — hardware identification, e.g. `AtariFA` |
| `0x01` | string — firmware version of the counterpart |
| `0x02` | string — protocol version |
| `0x03` | byte — number of lamps |
| `0x04` | byte — number of coils |
| `0x05` | byte — number of sounds |
| `0x06` | byte — number of displays |
| `0x07` | index → 2 bytes: type, digits |
| `0x08` | string — game info |
| `0x09` | byte — number of switches |

Control: `0x64` init/reset, `0x65` watchdog.

Switch handling is stateful: the first `GET /api/state` after a connect does a full
`0x28` refresh, every following one drains changes via `0x29`. Without granted control
nothing is polled at all — the bus belongs to the game then, and a switch poll would be
interference.

---

## 6. Firmware modules

All under `main/`.

| File | Responsibility |
|---|---|
| `main.c` | `app_main`: NVS → config → board → **DIP 1 gate** → LISY → Wi-Fi → web server → power task |
| `board_pins.h` | central GPIO assignment (single source of truth) |
| `board.c/h` | GPIO setup: control request, button, DIP bank, LED, reserved I2C pins |
| `power_mgr.c/h` | DIP 1 as power switch, deep sleep + wake, blink indicator |
| `fa_connect.c/h` | handshake, info group, holds the hardware inventory |
| `lisy.c/h` | UART1 driver + LISY protocol layer, watchdog timer, lamp/switch bitmaps |
| `app_config.c/h` | NVS blob (namespace `facfg`) — **coil pulse time only** |
| `wifi_mgr.c/h` | STA from NVS, fallback AP + captive portal + mDNS |
| `web_server.c/h` | `esp_http_server`: REST API + embedded page |
| `fw_update.c/h` | OTA from lisy.dev (`esp_https_ota`, directory listing) |
| `web/index.html` | single-page frontend, gzipped and embedded at build time |

### 6.1 The NVS blob has a version

`app_config.c` used to check only the size of the stored blob. If the struct grew and
happened to keep the same size, old data was misread; if the size changed, the
configuration vanished without a word. Since v1.11 the struct carries `magic` (`0xFA`) and
`version`. **Increment `CFG_VERSION` on every change to the field layout.**

Since v1.11 the blob contains only `coil_pulse_ms`. Everything else either comes from the
device or was a setting able to contradict the connection state.

---

## 7. REST API

Query parameters only — there is no JSON parsing anywhere. Responses are built with
`snprintf`. Registered in `web_server_start()`.

| Endpoint | Function |
|---|---|
| `GET /` | the web page (gzip) |
| `GET /api/config` | inventory, connection state, device info, watchdog and pulse time as JSON |
| `POST /api/config?pulse=50` | store coil pulse time (the only storable setting) |
| `POST /api/connect` | run the handshake; replies like `GET /api/config` |
| `POST /api/disconnect` | release control; replies like `GET /api/config` |
| `POST /api/lamp?id=5&on=1` | lamp on/off |
| `POST /api/coil?id=3` | pulse a coil |
| `POST /api/sound?id=7&on=1` | play / stop sound |
| `POST /api/display?id=0&text=1234` | send display text |
| `GET /api/state` | lamp/switch bitmaps (hex) + display texts |
| `POST /api/reset` | LISY init/reset (`0x64`), returns `{"result":n}` |
| `POST /api/wifi?ssid=&pass=` | store Wi-Fi credentials + reboot after 1 s |
| `GET /api/status` | Wi-Fi mode (`ap`/`sta`), IP, firmware version |
| `GET /api/fwlist` | `.bin` files on lisy.dev as JSON |
| `POST /api/fwupdate?file=FA_Control_v1.13.bin` | start OTA update |
| `GET /api/fwstatus` | progress (`idle`/`running`/`ok`/`error`, percent, message) |
| `GET /*` | captive portal redirect to `/` resp. `http://192.168.4.1/` |

The control endpoints (`lamp`, `coil`, `sound`, `display`) validate against
`fa_connect_info()->…`. Without granted control those counts are 0, so every command ends
in `400 Bad parameter` regardless of what the interface offers. The greyed-out tile in the
browser is only the polite form of the same rule.

**Language convention:** every plain-text message reachable through the API is user
interface and therefore English — this includes `fa_connect_state_str()` and all
`send_err()` strings, even though they live in C files. ESP-IDF log texts
(`ESP_LOGI/W/E`) go to the serial console and stay German.

---

## 8. Web frontend

[`main/web/index.html`](../main/web/index.html) — a single page, vanilla JS, retro CRT
styling, no external assets. At build time `main/CMakeLists.txt` runs `web/gzip_file.py`
over it and embeds the result with `target_add_binary_data`; `web_server.c` serves it with
`Content-Encoding: gzip`.

Structure: the home screen **is** the handshake — it shows the connection banner, the
buttons **CONNECT** / **RELEASE CONTROL** / **RE-INITIALIZE (0x64)**, the *REPORTED
HARDWARE* panel, and a tile menu. Tiles 01–05 (`LAMPS`, `COILS`, `SWITCHES`, `SOUND`,
`DISPLAYS`) stay disabled while `conn !== 1`; tile 06 `WI-FI` (network + firmware) is
always reachable. The home screen polls `/api/config` every 2 s, because the far side can
take control back at any time and that has to be visible.

The interface is in English. The manuals under `docs/` quote its labels verbatim — a
renamed button has to be followed up there.

---

## 9. Wi-Fi manager

[`main/wifi_mgr.c`](../main/wifi_mgr.c).

- **STA mode** with credentials from NVS (namespace `wifi`), `STA_MAX_RETRY` = 5.
- On failure or without credentials: **open access point `FA-Control`** on
  `192.168.4.1`, plus a mini DNS server answering every query with that address — this is
  what makes the captive portal pop up. `web_server.c` redirects all unknown URIs.
- In STA mode mDNS registers `fa-control.local` and an `_http._tcp` service on port 80.

---

## 10. OTA update

[`main/fw_update.c`](../main/fw_update.c). Base URL
`https://lisy.dev/swrep/misc/FA_Control/bin/`.

- `fw_update_list_json()` fetches the Apache directory index and scans it for
  `href="*.bin"` (max 20 entries), sorted descending so the newest is first. Naming
  convention: `FA_Control_vX.YZ.bin`.
- `fw_update_start()` runs `esp_https_ota` in its own task against the certificate bundle,
  writes to the inactive OTA partition, validates the image, sets the boot partition and
  reboots. Progress is polled via `/api/fwstatus`.
- Requires STA mode — there is no internet in AP mode, and both endpoints reject with
  `No internet in AP mode`.
- Partitions: `partitions.csv`, `ota_0` and `ota_1` at 0x20000 / 0x1A0000, 1.5 MB each,
  flash size 4 MB. The running version comes from `version.txt` (PROJECT_VER) via
  `esp_app_get_description()`.
- The httpd task runs with `stack_size = 10240` because the TLS client for the listing
  executes inside it.

> Switching from a single-app layout to OTA partitions once required a full re-flash over
> USB. Wi-Fi credentials and configuration in NVS survived it.

---

## 11. Build & flash

### 11.1 Never build into the default directory

`N:` is a network drive (UNC path `\\synnew\th\…`). `cmd.exe` and the linker do not support
UNC working directories, so the build **must** go to a local directory via `-B`:

```powershell
. C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
idf.py -B C:\Users\bonta\esp\build\fa build
idf.py -B C:\Users\bonta\esp\build\fa -p COM7 flash monitor
```

The VS Code ESP-IDF extension is already configured through `.vscode/settings.json`:
port COM7, target esp32c3, local build path (`idf.buildPathWin`).

### 11.2 Which Python — `IDF_PYTHON_ENV_PATH`

The authoritative interpreter is the **Python 3.11.2 shipped with ESP-IDF**
(`.espressif\tools\idf-python\3.11.2`), venv `idf5.5_py3.11_env`. Since 08.2026 it is the
only venv; a second one, `idf5.5_py3.14_env`, had grown alongside it for months and was
removed. This is enforced through the Windows **user** variable:

```
IDF_PYTHON_ENV_PATH = C:\Users\bonta\.espressif\python_env\idf5.5_py3.11_env
```

Without it, `idf_tools.py:1874` derives the venv name from the `sys.version_info` of the
interpreter that `export.ps1:24` invokes as a bare `python` — which is the Microsoft Store
alias pointing at 3.14, and a second venv appears. If that does not match the one the build
directory was configured with, CMake aborts with *"… is currently active in the environment
while the project was configured with …"*. **The cure is then to delete the build
directory, not to switch venvs.**

- The variable only takes effect in **newly opened shells**. Restart VS Code after setting it.
- `build_and_deploy.ps1` sets it itself and aborts if a different one is already active.
- On a foreign or freshly set up machine:
  ```powershell
  $env:IDF_PYTHON_ENV_PATH = "$env:USERPROFILE\.espressif\python_env\idf5.5_py3.11_env"
  . C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
  ```
  Verify afterwards with `python -c "import sys; print(sys.version)"` — must say **3.11.2**.
- **When moving to a newer ESP-IDF version the variable must follow** (the path contains
  `5.5`). This does get noticed, because `idf_tools.py` checks the venv against the IDF
  version and reports the mismatch in plain text.

---

## 12. Release & deploy

```powershell
.\build_and_deploy.ps1
```

Builds the firmware into the local build directory, copies the binary to
`releases\FA_Control_v<version>.bin` (version from `version.txt`) and uploads it by
SFTP/WinSCP to `lisy.dev/swrep/misc/FA_Control/bin/`. Credentials come from a `.env` file
(template `.env.example`); the password is prompted interactively — pressing Enter without
one skips the upload and only keeps the local copy.

**Increment `version.txt` before a release.**
