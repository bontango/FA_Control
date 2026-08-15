# FA_Control — User Manual

For operating the device on the pinball machine. This manual deliberately contains no
technical detail — if you want to know how it works inside, see the
[Technical Reference](TECHNICAL_REFERENCE.md). A German version of this manual is
available as [BEDIENUNGSANLEITUNG.md](BEDIENUNGSANLEITUNG.md).

**Contents**

1. [What FA_Control does](#1-what-fa_control-does)
2. [Switching on and off](#2-switching-on-and-off)
3. [What the blue LED tells you](#3-what-the-blue-led-tells-you)
4. [Setting up Wi-Fi](#4-setting-up-wi-fi)
5. [Opening the page](#5-opening-the-page)
6. [Connecting](#6-connecting)
7. [The menus](#7-the-menus)
8. [Releasing control](#8-releasing-control)
9. [Updating the firmware](#9-updating-the-firmware)
10. [Safety notes](#10-safety-notes)

---

## 1. What FA_Control does

FA_Control is a **service tool for troubleshooting**. It stays permanently inside the
pinball machine and serves a web page over Wi-Fi. From that page you can switch individual
lamps, fire individual coils, watch the switches, play sounds and write to the displays —
no meter, no disassembly, just a phone in your hand.

The important part: **as long as you do not explicitly connect, the machine keeps playing
normally.** FA_Control never interferes on its own and does not announce itself to the
machine at power-up. Only when you press **CONNECT** on the home screen and the game hands
over control does FA_Control take over. Until then it is merely a device showing a web
page.

---

## 2. Switching on and off

**DIP switch 1 on the small four-way switch bank is the power switch.**

| DIP 1 | |
|---|---|
| **ON** | switched on: Wi-Fi, web page, control. The blue LED blinks. |
| **OFF** | switched off: the device sleeps and draws practically no power. |

To switch it back on, flip the same switch to ON — that is all it takes, the device wakes
up from it.

Because FA_Control stays inside the machine permanently, **off is the normal state.** You
switch it on for troubleshooting and off again afterwards.

**If you flip DIP 1 to OFF while it is running**, FA_Control first hands control back to
the game and only then goes to sleep. The machine resumes immediately — you can flip the
switch at any time without risk, even in the middle of a measurement.

DIP 3 and DIP 4 currently have no function. DIP 2 only affects the blue LED (see the next
section) and normally stays OFF.

---

## 3. What the blue LED tells you

The blink pattern tells you what is going on from two metres away, without opening the web
page:

| Pattern | Meaning |
|---|---|
| **slow, once per second** | switched on, but not (yet) connected to the machine |
| **fast, five times per second** | connected — FA_Control is controlling the machine right now |
| **very fast, ten times per second** | just after power-up, the device is about to fall asleep (DIP 1 is OFF) |
| **dark** | switched off |

If the LED stays dark although DIP 1 is ON, check DIP 2: when that one is ON, the blink
indicator is switched off on purpose. The device still works normally.

---

## 4. Setting up Wi-Fi

At first power-up FA_Control does not know any network yet, so it opens one of its own.

1. Set **DIP 1 to ON** and wait a moment.
2. On your phone or laptop, look for Wi-Fi networks and join the open network
   **`FA-Control`**. There is no password.
3. As a rule the control page **opens by itself**. If it does not, enter
   `http://192.168.4.1` in your browser.
4. On the page, click the tile **06 · WI-FI**.
5. Under **NETWORK**, enter the name of your home network in **SSID** and its password in
   **PASSWORD**.
6. Press **SAVE + REBOOT**. The device restarts and joins your network.

The `FA-Control` network is gone afterwards — reconnect your phone or laptop to your usual
home network.

> If the `FA-Control` network reappears later, FA_Control could not log in: usually a typo
> in the password, a renamed network, or poor reception. You can re-enter the credentials
> the same way.

---

## 5. Opening the page

Once FA_Control has joined your home network, you can reach it at

**`http://fa-control.local`**

If your device cannot resolve that name (this happens on some Android devices and in some
corporate networks), use the IP address instead. It is shown in the header of the page
once you have had it open — and you will find it in your router's device list.

The top right of the page also shows the installed version, the mode (`STA` = on your home
network, `AP` = its own network) and the IP address.

---

## 6. Connecting

The **home screen is the connection procedure**. It shows a bar with the connection state,
three buttons, and the tile menu below.

Tiles **01 to 05 are greyed out and cannot be clicked** while there is no connection. That
is intentional: FA_Control only learns after connecting how many lamps, coils, switches,
sounds and displays your machine actually has. Before that there would be nothing to
control.

**How to connect:**

1. Press **CONNECT**.
2. The bar turns green and reads **CONTROL GRANTED**.
3. Below it the panel **REPORTED HARDWARE** appears, showing what your machine reported
   itself: the number of lamps, coils, switches, sounds and displays, plus the digit count
   of each display. The line above shows the device name, its firmware version and the
   detected game.
4. Tiles 01 to 05 are now enabled and the blue LED blinks fast.

**If it does not work**, the reason is stated in plain text in the bar:

| Message | What to do |
|---|---|
| **CONTROL DENIED - SET OPTION DIP 4 TO ON** | The machine is there but will not release control. On AtariFA the enable switch is **option DIP 4** on the FPGA board — set it to ON and press **CONNECT** again. |
| **CONTROL DENIED - REQUEST LINE (GPIO10) NOT SEEN** | The machine answers but did not notice the takeover request. Check that the connector between FA_Control and the board is seated properly. |
| **NO ANSWER - CHECK WIRING AND POWER** | Nobody is answering. Is the machine powered on? Is the cable seated? Is matching firmware actually running on the other side? |
| **CONTROL DENIED** | The machine refuses for some other reason. Restarting the machine usually helps. |

The home screen keeps re-checking the state. If the machine takes control back on its own,
you see it there immediately and the tiles grey out again.

The **RE-INITIALIZE (0x64)** button re-establishes the connection while running. You only
need it when something appears to have gone out of step — for example when the lamp
display and reality disagree.

---

## 7. The menus

All control menus are reached through the tile menu on the home screen. **◀ MENU** at the
top left takes you back.

### 01 · LAMPS

A grid with one tile per lamp, numbered from 0 up. **One click switches the lamp on, the
next one switches it off again.** Lamps that are on are filled copper in the grid.

This is how you find a dead lamp without playing through the game: click through them and
watch which one stays dark in the cabinet.

### 02 · COILS

A grid with one tile per coil, **numbered from 1 up** — unlike lamps, switches and sounds,
which start at 0. That is how the LISY protocol counts coils and how the schematic names
them: tile 1 is driver Q1, tile 20 is driver Q20. What comes after that depends on the
machine; on AtariFA it is the coin counter and the coin door lockout coil.

**One click fires a single pulse** — the tile flashes briefly. Coils are only ever pulsed
and never energized continuously; that protects them from burning out.

> Coils start at 1 only **from FA_Control 1.16 together with AtariFA 0.2.0**. If either
> side is older, the coil one position off will fire — update both together.

Below, **PULSE TIME** holds the pulse duration in **MILLISECONDS**. It applies to all coils
together. Change the value and press **APPLY** — it is stored and survives the next power
cycle.

> This pulse duration is the **only** setting FA_Control stores itself. Everything else
> comes from the machine.

### 03 · SWITCHES

A grid with one tile per switch. The display refreshes **once per second**; an actuated
switch is filled green — the same way an active lamp is filled copper.

This page is **display only**. Switches cannot be set — the protocol between FA_Control and
the machine has no command for it, because a switch reports something rather than doing
something. To test one, actuate it by hand on the machine and watch whether the tile
reacts.

### 04 · SOUND

A grid with one tile per sound. **One click plays it, another click stops it.** Only one
sound plays at a time; a new one replaces the previous.

If your machine reports no sounds, this grid stays empty — it cannot produce audio.

### 05 · DISPLAYS

One row per display, each with a mock seven-segment readout, an input field and a **SEND**
button. The label states how many digits that display has.

Enter digits and press **SEND** (or the Enter key). The text appears **right-aligned** on
the real display of the machine. Digits and spaces are allowed; a space leaves that
position dark.

### 07 · NAMES

Optional. Tiles are numbered — this menu lets you give them **speaking names** as well, so
coil 7 reads *Knocker* instead of just *7*.

A **naming file** is a small text file, one per machine. Once one is in use, the name
appears under the number on every tile in `LAMPS`, `COILS`, `SWITCHES` and `SOUND`, and
hovering a tile shows the full name. **The number always stays the leading label** — it is
the number that actually goes to the machine.

At the top of the menu, **THIS MACHINE** shows the **DEVICE** and the **GAME** of the
machine you are connected to — for example `AtariFA` and `002` — and whether the matching
file is present. Together they say where the file belongs: in the folder `AtariFA`, under
the name **`002.cfg`**. Read it there rather than working it out — connect first, then look.

*FILES ON THIS DEVICE* lists what is stored, as `DEVICE/FILE`. **USE** activates a file (the
active one is marked ●), **DELETE** removes it. The selection survives a restart.

There are two ways to add a file:

- **UPLOAD** — type the **DEVICE**, pick a `.cfg` file from your phone or computer and press
  **UPLOAD**. When you are connected, the device box is already filled in for you. This
  works in the machine's own `FA-Control` network too, so it is the way that always works,
  even in a cellar without reception.
- **LOAD LIST FROM LISY.DEV** — first pick the device, then the file, then **DOWNLOAD**
  copies it onto the device. This needs your home network; the button is disabled in AP mode.

**How the two parts are built:** the folder is the **hardware name** the board reports, the
file is the **game number**, three digits with leading zeros — `AtariFA` and game 2 give
`AtariFA/002.cfg`. On AtariFA the game number is the position of the game select DIP bank:

| Folder and file | Machine |
|---|---|
| `AtariFA/000.cfg` | The Atarians |
| `AtariFA/001.cfg` | Time 2000 |
| `AtariFA/002.cfg` | Airborne Avenger |
| `AtariFA/003.cfg` | Middle Earth |
| `AtariFA/004.cfg` | Space Riders |

The device folder is what keeps them apart, because the game numbers start over on every
board — on a GottFA1 for Gottlieb System 1 the files are `GottFA1/000.cfg` and so on.

**Upper and lower case do not matter**, in the folder or in the file name. `atarifa/002.cfg`
works just as well; if you upload a file that already exists under a different spelling, it
replaces that one instead of sitting next to it.

> **Coming from a version before 1.19?** Back then both parts formed a single file name,
> `AtariFA_002.cfg`. Such files still on the device are shown struck through and can only be
> deleted — pick one for **UPLOAD** and FA_Control will split the name for you, then delete
> the old entry to free the space.

Neither part carries a readable name — that is what the `[game] name=` line inside the
file is for. FA_Control shows it in brackets on the home screen: `GAME 2 (AIRBORNE
AVENGER)`.

**Automatic selection on CONNECT:** the matching file is activated. **If there is no
matching file, the selection is cleared** and the tiles go back to plain numbers. That is
deliberate — otherwise, after switching the machine to a different game, you would keep
seeing the previous game's names without noticing. A file picked by hand with **USE** stays
active until the next **CONNECT**.

**CONNECT only looks at files already on the device** — it never fetches anything from
lisy.dev by itself, so connecting stays as quick as it is. What it does instead: open menu
**07 · NAMES** and, if the expected file is missing but published on lisy.dev, *THIS
MACHINE* says so and offers **GET FROM LISY.DEV**. One click downloads it and puts it
straight to use.

The **FILE FORMAT** box at the bottom of the menu shows the layout. It is plain text, one
`number=name` line per entry, grouped in sections:

```
AtariFA/002.cfg

[game]
name=Airborne Avenger

[coils]
1=Left Flipper
7=Knocker

[switches]
12=Left Slingshot
```

Sections are `[game]`, `[lamps]`, `[coils]`, `[switches]`, `[sounds]` and `[displays]`.
**Coil numbers start at 1, lamps, switches and sounds start at 0** — the same numbering the
tiles use. Lines starting with `#` are comments. You only need the entries you care about;
everything else stays numeric. Maximum 32 kB per file.

> If tile 07 is missing entirely, your device has no storage for naming files. That happens
> when it was only ever updated over the air: the storage area has to be set up once by
> flashing over USB — see [USB_FLASH.md](USB_FLASH.md), which does that from a browser
> without any development tools. Everything else works exactly the same without it.

---

## 8. Releasing control

When you are done, hand the machine back. There are two equivalent ways:

- Press **RELEASE CONTROL** on the home screen. The bar turns grey, tiles 01–05 are
  disabled again, and the LED goes back to slow blinking. The device stays switched on and
  reachable.
- Set **DIP 1 to OFF**. That releases control as well and additionally switches FA_Control
  off.

In both cases the game takes over immediately.

> If you ever do neither — because the phone died or the Wi-Fi dropped out: the machine
> notices by itself that FA_Control has stopped reporting in and takes control back after
> roughly two seconds. The machine never stalls.

---

## 9. Updating the firmware

FA_Control fetches new versions from the internet itself. For that the device must be
joined to your home network — its own `FA-Control` network has no internet connection, and
the button is disabled there.

1. Open tile **06 · WI-FI**, section **FIRMWARE**. **INSTALLED VERSION** shows what is
   running right now.
2. Press **LOAD VERSIONS**. After a moment a dropdown appears with the available versions,
   newest first.
3. Pick a version and press **INSTALL UPDATE**. Confirm the prompt.
4. Progress is shown as a percentage. The device then reboots and the page reloads by
   itself after about ten seconds.

**Do not switch the device off or flip DIP 1 during the update.** An interrupted update is
not dangerous — the old version is kept and boots again — but the procedure then starts
over.

### Full installation over USB

An over-the-air update replaces the firmware and nothing else. Sometimes that is not
enough: a brand-new board has no firmware to start from, a device that has only ever been
updated over the air is missing the storage for naming files, and a device that no longer
boots cannot fetch anything for itself.

For those cases there is a **web installer** that writes the complete image over a USB
cable, straight from Chrome or Edge, with nothing to install on your computer:
<https://lisy.dev/swrep/misc/FA_Control/flasher/FA_Control_flasher.html>

Your Wi-Fi settings and uploaded naming files survive it — with one exception: if the
device is still older than version 1.18, that release moves the naming storage and it
starts out empty, so you upload the files once more. The full procedure, including
what to do when the device does not appear in the browser dialog, is in
[USB_FLASH.md](USB_FLASH.md).

---

## 10. Safety notes

**Do not connect during a game.** As soon as FA_Control takes control, the game program
gives it up: a game in progress ends, and lamps, coils and displays sit wherever
FA_Control puts them. Connect while the machine is idle.

**Fire coils deliberately.** FA_Control only pulses coils briefly and never energizes them
continuously. A coil is still a powerful electromagnet: keep your hands off the playfield
while firing, and do not fire the same coil repeatedly in quick succession — it gets warm.

**Be careful with long pulse times.** The **PULSE TIME** value applies to every coil. Too
high a value stresses the coil beyond what normal play intends. Raise it only if you know
why, and set it back afterwards.

**The machine runs on mains voltage.** This manual describes operating FA_Control only.
Working inside an opened machine requires the usual precautions.
