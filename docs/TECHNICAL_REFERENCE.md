# FA_Control — Technical Reference

Firmware internals, hardware bindings, protocol details and build procedure.
For operating the device see [USER_MANUAL.md](USER_MANUAL.md) (English) or
[BEDIENUNGSANLEITUNG.md](BEDIENUNGSANLEITUNG.md) (German); for installing a complete image
over USB without ESP-IDF, [USB_FLASH.md](USB_FLASH.md).

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
| Flash | 4 MB, two OTA partitions of 1856 kB each (1.5 MB before v1.18) |

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
| `main.c` | `app_main`: NVS → config → board → **DIP 1 gate** → LISY → naming files → SternFA rom boot → Wi-Fi → web server → power task |
| `board_pins.h` | central GPIO assignment (single source of truth) |
| `board.c/h` | GPIO setup: control request, button, DIP bank, LED, reserved I2C pins |
| `power_mgr.c/h` | DIP 1 as power switch, deep sleep + wake, blink indicator |
| `fa_connect.c/h` | handshake, info group, holds the hardware inventory |
| `lisy.c/h` | UART1 driver + LISY protocol layer, watchdog timer, lamp/switch bitmaps |
| `app_config.c/h` | NVS blob (namespace `facfg`) — **coil pulse time only** |
| `wifi_mgr.c/h` | STA from NVS, fallback AP + captive portal + mDNS |
| `web_server.c/h` | `esp_http_server`: REST API + embedded page |
| `fw_update.c/h` | OTA from lisy.dev (`esp_https_ota`) |
| `repo.c/h` | shared access to the lisy.dev file store: directory listing, download, file name check |
| `names.c/h` | naming files on the LittleFS partition (management only, no parsing) |
| `rom_boot.c/h` | game roms: own partition, import check, lisy.dev download, answering the boot request |
| `web/index.html` | single-page frontend, gzipped and embedded at build time |

### 6.1 The NVS blob has a version

`app_config.c` used to check only the size of the stored blob. If the struct grew and
happened to keep the same size, old data was misread; if the size changed, the
configuration vanished without a word. Since v1.11 the struct carries `magic` (`0xFA`) and
`version`. **Increment `CFG_VERSION` on every change to the field layout.**

Since v1.11 the blob held only `coil_pulse_ms`. Everything else either comes from the
device or was a setting able to contradict the connection state. v1.17 adds
`names_file[24]` (`CFG_VERSION` 3 → 4) — the selected naming file. That does not break the
rule: it does not describe the inventory, only how its numbers are labelled, and it comes
from the user, not from the device.

---

## 6.2 Naming files

[`main/names.c`](../main/names.c). Speaking names for lamps, coils, switches and sounds,
one INI-style text file per machine on a **LittleFS partition `names`**
(`joltwallet/littlefs`).

**The ESP does not parse these files.** It manages them — store, list, delete, select — and
serves the active one verbatim; the web frontend does the parsing. That keeps the parser
where the names are actually needed and costs the device neither RAM nor code.

- Section format: `[game] [lamps] [coils] [switches] [sounds] [displays]`, entries
  `number=name`. **Coils count from 1, everything else from 0** — the numbers that go over
  the wire. `#` and `;` start a comment.
- Limits: 32 kB per file (`NAMES_MAX_FILE_SIZE`), path `DEVICE/GAME.cfg` up to 31 characters
  in total, each part from `[A-Za-z0-9._-]`. `names_valid_path()` splits at the single `/`
  and checks both halves with `repo_valid_segment()` / `repo_valid_filename()` from
  `repo.c` — the same characters that guard OTA file names, which is why the check lives
  there. Since neither `/` nor `..` survives a segment check, that is also what keeps a
  path from escaping `/names`.
- Uploads are rejected if they contain control characters other than tab and newline. Bytes
  from `0x80` up stay allowed so UTF-8 names work. That is deliberately not a parser; it
  just keeps a binary blob — which practically always carries NUL bytes — from filling the
  partition.
- `names_list_json()` reports at most `NAMES_MAX_LIST` (32) files and the caller sizes its
  buffer with `NAMES_LIST_BUF`.
### 6.2.1 The machine ID is where the file lives

`names_game_id()` builds `<HW>/<GAME>` from `fa_conn_info_t` — `hw` (LISY opcode `0x00`)
and `game` (opcode `0x08`). Both are stripped to `[A-Za-z0-9_-]`, **case is preserved**
because the ID is displayed exactly as it is built. A purely numeric GAME below 1000 is
padded to three digits; anything else is taken verbatim, because what a future counterpart
reports as its game key is not ours to guess.

| reported | ID | path |
|---|---|---|
| `AtariFA` + `2` | `AtariFA/002` | `AtariFA/002.cfg` |
| `GottFA1` + `4` | `GottFA1/004` | `GottFA1/004.cfg` |
| `GottFA1` + `superman` | `GottFA1/superman` | `GottFA1/superman.cfg` |

**Up to v1.18 the two parts formed one flat file name** (`AtariFA_002.cfg`). The device name
became a folder in v1.19 because a flat directory stops being readable once several boards
and many games share it. Nothing else changed: outward — REST API, NVS, web frontend — a
naming file is still **one** identifier, just with a slash in it. So `app_config_t` needs no
second field and the API no second parameter, and because `<hw>/<game>.cfg` is exactly as
long as the old `<hw>_<game>.cfg`, `NAMES_MAX_NAME` stayed at 32 and `CFG_VERSION` at 5.

Case is ignored on **both** levels: `names_find()` walks the folder first, then the file,
comparing with `strcasecmp()` each time, and returns the spelling that is actually on the
partition. Uploading `atarifa/002.cfg` therefore replaces an existing `AtariFA/002.cfg`
instead of sitting next to it — two spellings would both match one ID and which one won
would be luck. Deleting the last file of a device removes the folder with it
(`rmdir_if_empty()`), otherwise empty folders would pile up with no way to remove them.

**Why the hardware name has to be in there:** AtariFA answers opcode 8 with a *single ASCII
digit* — `fa_control.vhd` sends `ascii_digit(game_info)`, fed from `'0' & (not
game_select)`, the 3-bit game select DIP bank (0 Atarians, 1 Time 2000, 2 Airborne
Avenger, 3 Middle Earth, 4 Space Riders; 5–7 fall back to Middle Earth in `game_sel`, but
opcode 8 still reports the raw value). That numbering restarts at 0 on every board, so the
game number alone cannot tell an AtariFA machine from a GottFA1 one. `HW_NAME` is already
a generic of the `fa_control` module and exists for exactly this purpose.

The ID carries no readable name — that is what the `[game] name=` line is for; the frontend
shows it in brackets behind `GAME …`, and the raw ID in menu 07 NAMES (`"gameid"` in
`/api/config`).

`names_select_for_id()` runs after `query_counts()` on a successful handshake:

- match → that file becomes active
- **no match → the selection is cleared**

The clearing is the point. A unique ID is what makes *wrong* labelling possible in the
first place: switch the DIP from Airborne Avenger to Space Riders without a file for the
latter, and the old game's names would otherwise stay on the tiles unnoticed. No names beat
wrong names. A manual pick via **USE** still works and holds until the next connect.

`set_active()` writes to NVS **only when the name actually changes** — otherwise every
connect would cost a flash write.

**`names_select_for_id()` searches locally and nothing else.** It never reaches out to
lisy.dev, so a handshake never waits on the network — and never waits for a file that may
not exist on the server at all. The convenience sits in the frontend instead: when menu 07
finds the expected file missing, it asks `/api/namefetchlist?dev=…` once and, on a hit,
offers **GET FROM LISY.DEV**, which chains `namefetch` + `namesel`. The check is skipped in
AP mode and fails silently otherwise, so the button simply stays hidden when there is no
answer.

`names_init()` checks a stored selection against **both** the form and the file system.
Form matters because after the v1.19 switch NVS may still hold a flat old-style name;
`names_find()` would find that file in the root directory, but `names_open()` rejects it, so
the selection would look valid and deliver nothing.

**Old files are not migrated.** A device that got 1.18 over the air keeps its `names`
partition at the old offset including its contents, so flat `<HW>_<GAME>.cfg` files can
still be lying in the root directory. They are not moved automatically — but they are not
hidden either: `names_list_json()` lists them with `"old":1`, the frontend strikes them
through and disables **USE**, and `names_delete()` is the one function that still accepts a
flat name so they can be cleared out. Hiding them would be worse than showing them; they
occupy flash either way.

### 6.2.2 The partition needs a USB flash

```
names,    data, littlefs, 0x3C0000, 0x40000,
```

256 kB at the end of the 4 MB flash = 64 LittleFS blocks of 4 kB. Measured cost on the
device: the empty file system takes 2 blocks, **each device folder costs 2 more**, and every
`.cfg` costs 1 (a naming file is about 2.5 kB, so it never fills a second block). Three
boards with fifteen games each therefore come to 2 + 6 + 45 = 53 blocks, and after garbage
collection headroom the practical ceiling is around 50 files. Where the partition sits is
covered in [§ 6.2.3](#623-the-v118-layout-change).

The other direction is the one that matters: **`esp_https_ota` never writes the partition
table** (it lives at `0x8000`). A device updated only over the air therefore does not have
the partition (nor, since v1.21, `roms` - § 6.3.2). That is not an error case — `names_init()` returns `ESP_ERR_NOT_FOUND`,
`names_ready()` stays false, every naming endpoint answers `No name storage on this
device`, `/api/config` reports `"namesfs":0` and the frontend hides tile 07. Everything
else works unchanged. To actually get the partition, flash over USB
(`idf.py -B … -p COM7 flash`, not `app-flash`) — or, without a toolchain, from the browser
with the web installer of [§ 13](#13-web-installer).

### 6.2.3 The v1.18 layout change

Until v1.17 the table ended at `0x360000` and the last 640 kB of the flash lay unused —
reserve left over from adding `names`. Meanwhile the application had grown to 78 % of its
1536 kB slot. v1.18 hands that reserve to the two app slots:

| Partition | v1.17 | v1.18 |
|---|---|---|
| `ota_0` | `0x20000`, 1536 kB | `0x20000`, **1856 kB** (`0x1D0000`) |
| `ota_1` | `0x1A0000`, 1536 kB | `0x1F0000`, **1856 kB** (`0x1D0000`) |
| `names` | `0x320000`, 256 kB | `0x3C0000`, 256 kB |

`0x20000 + 2 × 0x1D0000 + 0x40000 = 0x400000` — the flash comes out even, every app offset
stays 64 kB aligned, and `ota_0` deliberately keeps `0x20000` because the web installer
([§ 13](#13-web-installer)) carries that offset hard-coded.

Three consequences:

- **Only a USB flash brings the new layout**, for the reason given in § 6.2.2. Nothing new —
  the same rule, applied to the table itself.
- **Mixed operation holds up to the old slot size.** An ESP-IDF app image is slot
  independent, so new firmware still runs on a device with the old table — as long as
  `FA_Control.bin` stays below 1536 kB. Past that point `esp_ota_write()` on such a device
  fails with `ESP_ERR_INVALID_SIZE` and the update status reports the error. That is the
  release where the notes have to demand a USB re-install.
- **Naming files do not survive the move.** The partition changes offset, the installer does
  not erase (`eraseAll` stays off), so the old content simply sits orphaned at `0x320000`
  while the new area is formatted on first mount (`format_if_mount_failed`). NVS is
  untouched at `0x9000`: Wi-Fi credentials, pulse time and the selected naming file survive.

### 6.2.4 The v1.21 layout change

v1.21 takes 512 kB of the v1.18 reserve back for the game roms (§ 6.3):

| Partition | v1.18 | v1.21 |
|---|---|---|
| `ota_0` | `0x20000`, 1856 kB | `0x20000`, **1600 kB** (`0x190000`) |
| `ota_1` | `0x1F0000`, 1856 kB | `0x1B0000`, **1600 kB** (`0x190000`) |
| `roms` | - | `0x340000`, **512 kB** (`0x80000`) |
| `names` | `0x3C0000`, 256 kB | `0x3C0000`, 256 kB - unchanged |

`0x20000 + 2 × 0x190000 + 0x80000 + 0x40000 = 0x400000`. The app stood at 1231 kB when this
was set, which leaves about 25 %.

- **`names` stays where it is, so a USB installation keeps the naming files** - unlike the
  v1.18 change. `roms` is inserted in front of it on purpose.
- The mixed-operation limit is unchanged: a device with a v1.17 table has 1536 kB slots, so
  `FA_Control.bin` has to stay below that for OTA to keep working everywhere. The new, smaller
  1600 kB slot is not the tighter limit.
- A device with an older table has no `roms` partition; it answers every boot request with
  `N` and hides tile 08.
- **v1.20 is not compatible.** It carried a first, SternFA-only version of the boot loader:
  request `A5 5A 52 <game>` without device ID and length, roms flat in `/names/rom/`. The
  FPGA side of that version never left the bench. A v1.20 device receiving the current request
  reads the length byte as the game number and may answer with the wrong game - update such a
  device to v1.21 before using the rom boot. Roms uploaded with v1.20 are not carried over.

---

## 6.3 Game roms

FA boards with an ESP32 socket can boot without their SD card: right after reading its DIP
switches the FPGA asks this device for its game. First implemented in SternFA (PCB v2.00,
5.0.6). Counterpart in every FA project: `rtl/fa_control/esp_rom_loader.vhd`.

**Deliberately not LISY.** `fa_control.vhd` keeps speaking plain LISY API 0.12 so a real LISY
host could still log on. The boot request happens before any LISY session, on the same UART;
the FPGA keeps `fa_control` deaf and off the TX pin while its loader runs.

```
FPGA -> ESP   A5 5A 52 <len> <hw, len bytes> <game> <sectors>   every 250 ms, for at most 3 s
ESP -> FPGA   A5 5A 4E                                          no rom -> SD card
              A5 5A 44 <sectors*512 bytes> <crc_hi> <crc_lo>
```

- `hw` is the board's ID as reported by LISY opcode 0 (`HW_NAME` in the FPGA), without NUL,
  1-15 characters. `game` is the SD card index, the number on the boot display. `sectors` is
  what the board wants, in 512-byte units (SternFA 16 = 8 kB, WillFA7S would ask 64 = 32 kB).
- The ESP sends the stored data and pads with the stored fill byte up to the requested
  length. The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, not reflected, check value
  0x29B1) over exactly the bytes sent - the same `crc16_ccitt.vhd` the SD card readers use. It
  protects the transfer only. That the VHDL equations and `rom_boot_crc16()` agree was checked
  in Python.
- Without the `roms` partition (device only ever updated over the air) the ESP answers `N`
  at once, so the board does not wait.

### 6.3.1 Naming and storage

| where | path |
|---|---|
| lisy.dev | `roms/<HW>/<nnn>.bin` or `roms/<HW>/<nnn>_<title>.bin` |
| device | `/roms/<HW>/<nnn>.bin` |
| REST, UI | `<HW>/<nnn>`, e.g. `SternFA/012` |

The same two levels as the naming files (`names/<HW>/<nnn>.cfg`), and the same key: `<HW>` is
the device ID, `<nnn>` the three-digit game number. A title on lisy.dev is for people browsing
the folder; only the leading three digits count, and the device stores without it. A title for
the list comes, if wanted, from the naming file of the same key. The HW folder is matched
without regard to case, as with the naming files.

That the keys line up needs the board to report the **full** game number at opcode 8.
SternFA did not until 5.0.6 (one digit, so games 5, 15 and 105 shared a naming file);
`fa_control.vhd` has had a `GAME_DIGITS` generic since then.

**File content** is the game slot exactly as it goes onto the SD card: a multiple of 512
bytes, at most 64 kB. A full 64 kB slot carries a CRC16 over its first 32 kB at
0xFFFE/0xFFFF, big endian (SternFA, WillFA7S), and is rejected if it does not match. Smaller
slots have no CRC field (WillFA7, 12 kB) and are taken as they are. Checked against
`SternFA_SD_098_CRC.img`: 204 games pass; the filler slots (CRC `FFFF`), the empty slots and
the text block at game 238 are rejected - the SD readers would reject them too.

**Stored format:** 8-byte header `'F' 'A' 'R' 1 <fill> 0 <len_hi> <len_lo>` plus the data up to
the last byte that is not the fill byte. The fill byte is the last data byte of the slot (the
one before the CRC field). A SternFA game comes down from 64 kB to about 8 kB this way.

**Import** (upload and download alike) goes through `/roms/.tmp`, so no 64 kB buffer is ever
held in RAM: the body or the download is streamed into it, `rom_import_tmp()` checks size and
CRC in one pass that also finds the last non-fill byte, then copies the data into place and
removes the temp file. A leftover temp file is removed at mount.

### 6.3.2 The `roms` partition

```
roms,     data, littlefs, 0x340000, 0x80000,
```

512 kB of its own, mounted at `/roms` by `rom_boot_init()` with the same pattern as
`names_init()` (`format_if_mount_failed`, `rom_boot_ready()`, `/api/config` reports
`"romfs"`, tile 08 is hidden without it). It is a partition of its own rather than a folder in
`names` because LittleFS mounts a partition at exactly one place, and `/roms/<HW>` next to
`/names/<HW>` mirrors lisy.dev. About 60 SternFA games fit. Like `names` it only arrives with a
USB installation, see [§ 6.2.2](#622-the-partition-needs-a-usb-flash) and
[§ 6.2.4](#624-the-v121-layout-change).

### 6.3.3 Answering

- `boot_task` polls the UART receive buffer every 10 ms **without** taking the bus; only
  when bytes are waiting does it take the LISY mutex (`lisy_bus_take()`), so the web
  interface's commands are not held up.
- After an answer `lisy_bus_drain()` waits for the TX to finish and throws away what arrived
  meanwhile: the FPGA repeats its request until it sees the header, and a second answer would
  end up in `fa_control`.
- The file length is checked against the header **before** `D` goes out. A read error after
  that can only be reported one way: the ESP sends the inverted CRC, the FPGA discards and
  falls back to its SD card, and `last.r` says `E`.
- **Started before Wi-Fi** in `app_main` - the FPGA only asks for 3 s after power on. In deep
  sleep (DIP 1 OFF) nothing answers and the board falls back to its SD card after those 3 s.
- **Why the header:** on a board without a module the FPGA's receive line is an open mux
  input. Noise must never pass as an answer; the header plus the CRC make sure of that.
- `GET /api/romlist` also reports the last request since boot
  (`"last":{"hw","g","r","ms"}`) - the only way to see from the browser what the board asked
  for and got, and the source of the *LAST BOOT REQUEST* block in tile 08.

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
| `GET /api/names` | the active naming file, raw `text/plain`, chunked |
| `GET /api/namelist` | files on the partition + active one + bytes used |
| `POST /api/namesel?file=AtariFA%2F002.cfg` | select the active file (empty = none) |
| `POST /api/namedel?file=AtariFA%2F002.cfg` | delete a file (also takes a flat old-style name) |
| `POST /api/nameup?file=AtariFA%2F002.cfg` | upload — **the only endpoint with a request body** |
| `GET /api/namefetchlist` | device folders on lisy.dev as JSON |
| `GET /api/namefetchlist?dev=AtariFA` | `.cfg` files inside one folder |
| `POST /api/namefetch?file=AtariFA%2F002.cfg` | download a naming file from lisy.dev |
| `GET /api/romlist` | stored game roms (`n` = `<HW>/<nnn>`, `s` = stored bytes), bytes used, last boot request |
| `POST /api/romup?file=SternFA%2F012` | upload a game slot - body like `nameup` |
| `POST /api/romdel?file=SternFA%2F012` | delete a stored game rom |
| `GET /api/romfetchlist` | device folders below `roms/` on lisy.dev |
| `GET /api/romfetchlist?dev=SternFA` | `.bin` files inside one folder |
| `POST /api/romfetch?file=SternFA%2F012_Stars.bin` | download a game slot from lisy.dev and store it as `SternFA/012` |

The `/` inside a path arrives URL-encoded as `%2F`; `get_param()` decodes it before the
handler sees it.
| `GET /*` | captive portal redirect to `/` resp. `http://192.168.4.1/` |

`cfg.max_uri_handlers` in `web_server_start()` must be at least as large as the `uris[]`
array — otherwise the last entries are silently not registered and fall through to the
captive-portal handler. It sat at exactly 16 with 16 entries; v1.17 raised it to 24, the
game rom endpoints to 32 (29 entries). **Count along when adding an endpoint.**

`POST /api/nameup` and `POST /api/romup` carry the file content in the body (a file does not
fit sensibly into a URL); the name resp. game number stays a query parameter. Everything else
keeps to query parameters.

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

[`main/web/index.html`](../main/web/index.html) — a single page, vanilla JS, **no external
assets** (the CSP of the embedded server aside, the device has no internet in AP mode). At
build time `main/CMakeLists.txt` runs `web/gzip_file.py` over it and embeds the result with
`target_add_binary_data`; `web_server.c` serves it with `Content-Encoding: gzip`.

### 8.1 Visual identity

Since v1.17 the page carries the colour world of **lisy.dev**, so device and website read as
one project. The palette is derived from `lisy.dev/assets/bundle.css` and then darkened one
step — the site's ground is `#FCF7F0` at 96 % lightness, practically white, which glares on
a service screen inside an opened cabinet. Hue kept, lightness down to 90 %.

All colours live in `:root`; every rule takes them from there, so a later dark mode is a
matter of redefining tokens. The page is deliberately **single-theme** and paints every
colour explicitly rather than inheriting anything from the browser.

| token | value | role |
|---|---|---|
| `--bg` / `--panel` | `#F2E9DC` / `#FBF6EF` | ground, tiles |
| `--dark` / `--dark-2` | `#33201A` / `#281713` | dark surfaces, header bar |
| `--cu` / `--cu-dk` / `--cu-hi` | `#A96A47` / `#6B3A20` / `#BE8360` | accent, headings, highlight |
| `--txt` / `--dim` | `#2E2622` / `#6F6053` | body, secondary |
| `--ok` / `--ok-dk` | `#2A6E2C` / `#1D4E1F` | switch closed |
| `--warn` | `#8F2A0F` | control lost, destructive |
| `--seg-bg` | `#211A15` | seven-segment mock-up |

Two consequences of moving off the black ground:

- **State is a filled area, not a glow.** `.cell.on` (lamp) fills copper, `.cell.sw.on`
  (switch closed) fills green, both with a darker border and white text — the same method
  in two colours. On black the glow carried the information; on a light ground a merely
  tinted border is invisible at arm's length. The two fills sit in separate menus and carry
  different glyphs (◉ / ▣), so they never have to be told apart by hue.
- **The seven-segment mock-up stays dark.** A real display is, and the ghost digits `8888`
  only work on a dark ground. It is the one deliberate dark island.

The CRT scanlines and vignette (`body::before` / `body::after`) are gone, and the base font
is the lisy.dev stack `'Helvetica Neue',Helvetica,Arial,sans-serif`. Monospace remains only
where characters must line up: the seven-segment mock-up and the file-format example.

Labels stayed in upper case on purpose — the manuals quote them verbatim, and changing them
would mean a full documentation pass for no functional gain.

The logo sits at the right of the header, served from `GET /logo.png` as embedded binary
data. It is **not** a data URI: a PNG is already compressed, base64 would inflate it by a
third, and the page's gzip could not win that back (14,677 B versus 19,569 B).

Structure: the home screen **is** the handshake — it shows the connection banner, the
buttons **CONNECT** / **RELEASE CONTROL** / **RE-INITIALIZE (0x64)**, the *REPORTED
HARDWARE* panel, and a tile menu. Tiles 01–05 (`LAMPS`, `COILS`, `SWITCHES`, `SOUND`,
`DISPLAYS`) stay disabled while `conn !== 1`; tiles 06 `WI-FI` (network + firmware) and
07 `NAMES` are always reachable — setting up the network and managing files works without
control. Tile 07 is hidden entirely when `namesfs === 0`. The home screen polls
`/api/config` every 2 s, because the far side can take control back at any time and that
has to be visible.

`parseNames()` reads the active naming file and `mkgrid()` puts the name under the number
on each tile; the grid switches to the wider `.named` layout. **The number stays the
leading label** — it is what goes over the wire. Names are inserted with `textContent`
(or through `esc()` where `innerHTML` is unavoidable), because they come from an uploaded
file. Without a file everything looks exactly as before.

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

- `repo_list_json()` ([`main/repo.c`](../main/repo.c)) fetches the Apache directory index
  and scans it for `href="*<suffix>"` (max `REPO_MAX_FILES` = 256 entries), sorted
  descending so the newest is first. Naming convention: `FA_Control_vX.YZ.bin`. The naming
  files under `…/FA_Control/names/<DEVICE>/` and the game roms under
  `…/FA_Control/roms/<DEVICE>/` (§ 6.3.1) are found the same way, only with `.cfg` or `.bin`
  — which is why the scanner sits in `repo.c` instead of three times in the callers.
  **Since v1.22 the index is scanned while it is read**, through a 1 kB window plus a carry
  of `REPO_MAX_NAME + 8` bytes for an entry cut in half at a chunk boundary. Nothing holds
  the whole page any more: with 204 SternFA roms the HTML is about 25 kB, and up to v1.21 a
  16 kB buffer (plus a 64 × 64-byte name array) sat next to the TLS context — so a folder
  stopped at roughly 125 entries by the buffer and at 64 by the array. Now the heap holds the
  window, the names themselves (a pool grown in 1 kB steps, ~3 kB for 204 short names) and
  256 offsets. Beyond 256 matches the rest is ignored with a log warning; an answer that does
  not fit the caller's buffer is cut off as valid JSON, also with a warning. The caller for
  the rom folders (`/api/romfetchlist?dev=…`) therefore answers from 5 kB, enough for 256
  names of the `nnn_xxxxx.bin` form.
  **`suffix = "/"` lists the sub-directories instead of the files**, which is how the device
  folders are enumerated. The one rule that makes both work from a single loop: slashes are
  allowed *inside* the suffix only. For `.cfg` that means none at all; for `/` it means the
  single trailing one — so Apache's parent link `href="/swrep/misc/FA_Control/"` drops out
  over its remaining slashes and the sort links `href="?C=N;O=D"` over the missing suffix.
  Folder names come back with the slash still attached, exactly as the listing has them.
- `fw_update_start()` runs `esp_https_ota` in its own task against the certificate bundle,
  writes to the inactive OTA partition, validates the image, sets the boot partition and
  reboots. Progress is polled via `/api/fwstatus`.
- Requires STA mode — there is no internet in AP mode, and both endpoints reject with
  `No internet in AP mode`.
- Partitions: `partitions.csv`, `ota_0` and `ota_1` at 0x20000 / 0x1B0000, 1600 kB each,
  `roms` (LittleFS) at 0x340000, 512 kB, `names` (LittleFS) at 0x3C0000, 256 kB, flash size
  4 MB — see [§ 6.2.3](#623-the-v118-layout-change) and
  [§ 6.2.4](#624-the-v121-layout-change) for what changed and what it means for devices still
  carrying an old table. The running version comes from
  `version.txt` (PROJECT_VER) via `esp_app_get_description()`. **An OTA update writes the
  app partition only** — never the partition table, the bootloader, `names` or `roms`.
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

There are **three** deploy scripts, and a version bump wants all three. They differ in what
they publish, not in how they work — WinSCP lookup, `.env` parsing, interactive password
(Enter = local copy only) and the `IDF_PYTHON_ENV_PATH` pin are identical in all of them.

| Script | Publishes | `.env` variable | Server folder |
|---|---|---|---|
| `build_and_deploy.ps1` | `FA_Control_v<version>.bin` — the app alone | `SFTP_PATH` | `…/FA_Control/bin/` |
| `build_and_deploy_full.ps1` | the four flash files + `version.txt` | `SFTP_PATH_FULL` | `…/FA_Control/esptool/` |
| `flasher\deploy_flasher.ps1` | the installer page + its manual | `SFTP_PATH_WEB` | `…/FA_Control/flasher/` |

### 12.1 App only — `build_and_deploy.ps1`

```powershell
.\build_and_deploy.ps1
```

Builds the firmware into the local build directory, copies the binary to
`releases\FA_Control_v<version>.bin` (version from `version.txt`) and uploads it by
SFTP/WinSCP to `lisy.dev/swrep/misc/FA_Control/bin/`. Credentials come from a `.env` file
(template `.env.example`). That folder is what `fw_update.c` scans, so this is the source
of every over-the-air update.

**Increment `version.txt` before a release.**

### 12.2 Full package — `build_and_deploy_full.ps1`

```powershell
.\build_and_deploy_full.ps1
```

Same build, but it collects everything an `esptool` run writes and puts it in
`releases\full\`:

| Copied from the build directory | Published as | Address |
|---|---|---|
| `bootloader\bootloader.bin` | `bootloader.bin` | `0x0` |
| `partition_table\partition-table.bin` | `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `ota_data_initial.bin` | `0xf000` |
| `FA_Control.bin` | `FA_Control.bin` | `0x20000` |

The addresses are not invented here — they are what `flash_args` in the build directory
says, and the ESP32-C3 puts the bootloader at `0x0` rather than the `0x1000` of the
classic ESP32.

Two files travel with the binaries. `version.txt` carries the plain version number and is
written through `[System.IO.File]::WriteAllText` with `UTF8Encoding($false)` — with a BOM
the installer page would show an invisible `U+FEFF` in front of the version. And
`flasher\esptool.htaccess` is uploaded as `.htaccess`, for the CORS reason in § 13.

### 12.3 Installer page — `flasher\deploy_flasher.ps1`

```powershell
.\flasher\deploy_flasher.ps1
```

No build. Converts `docs\USB_FLASH.md` to `USB_FLASH.html` with Pandoc and uploads that
together with `FA_Control_flasher.html`, `logo.png` and `.htaccess`. It reads the `.env`
from its own folder or from the project root, so the existing one is found. Separate from
§ 12.2 on purpose: a typo in the manual should not trigger a rebuild.

---

## 13. Web installer

`flasher\FA_Control_flasher.html` is a standalone page — not part of the firmware, not
served by the device. It writes the § 12.2 package to a bare ESP32-C3 straight from
Chrome or Edge. Its ancestor is the LISYclock config editor, from which only the USB
flash was taken; the Bootstrap scaffolding around it was not.

**Mechanism.** `navigator.serial.requestPort()` (Web Serial) hands a port to `esptool-js`,
pulled in at runtime by `import('https://esm.sh/esptool-js@0.4.1')` — esm.sh bundles its
dependencies inline, so there is nothing else to host. The four binaries are fetched as
`ArrayBuffer` and converted to the Latin-1 binary strings esptool-js expects, in 32 kB
chunks because `String.fromCharCode.apply` blows the stack on a megabyte.

**`eraseAll` stays off, `flashSize` stays `'keep'`.** Only the four addresses are written,
so NVS (Wi-Fi credentials, pulse time, selected naming file) and the `names` LittleFS
partition survive an installation. `'keep'` leaves the flash-mode/frequency/size header of
the bootloader as built — mode, clock and size are already correct in the file.

**Why the CORS header.** The page normally runs on lisy.dev and fetches the binaries from
a sibling folder — same origin, nothing needed. But opened from disk it has origin `null`,
and the browser then refuses the download. `flasher\esptool.htaccess` sets
`Access-Control-Allow-Origin "*"` on the `esptool/` folder for exactly that case, plus
`no-store` on `.bin`/`.txt` so a cache cannot serve the previous version while
`version.txt` already announces the new one. In the repository the file cannot be named
`.htaccess`, because the flasher folder already has its own.

**Visual identity.** The page repeats the `:root` palette of `main/web/index.html`
verbatim rather than importing it — it lives outside the device and can fetch nothing from
it. Whoever changes the palette there has to change it here. The log window is, next to
the seven-segment mock-up of § 8.1, the second deliberately dark island: it is a console
showing tool output, and it should look like one.

The user-facing side of all this is [USB_FLASH.md](USB_FLASH.md).
