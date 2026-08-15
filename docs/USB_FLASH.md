# FA_Control — Full Installation over USB

How to write the complete firmware package to the ESP32-C3 straight from your browser,
using the web installer at
[lisy.dev/swrep/misc/FA_Control/flasher/](https://lisy.dev/swrep/misc/FA_Control/flasher/FA_Control_flasher.html).

This is the exceptional route. The everyday way to get a new version onto the device is
the over-the-air update described in
[§ 9 of the User Manual](https://github.com/bontango/FA_Control/blob/main/docs/USER_MANUAL.md#9-updating-the-firmware),
which needs nothing but the device's own web page. Read on when that route is not enough.

> The cross-references below point at GitHub rather than at neighbouring files: this page
> is also published on its own next to the installer, where the other documents do not
> exist.

**Contents**

1. [When you need this](#1-when-you-need-this)
2. [What you need](#2-what-you-need)
3. [Before you start](#3-before-you-start)
4. [Step by step](#4-step-by-step)
5. [What is kept and what is not](#5-what-is-kept-and-what-is-not)
6. [Troubleshooting](#6-troubleshooting)
7. [Reference](#7-reference)

---

## 1. When you need this

**The NAMES menu is missing.** Naming files live on their own storage area of the flash
chip, and the map that describes that area — the partition table — is never touched by an
over-the-air update. A device that has only ever been updated over the air therefore does
not know the area exists: tile **07 · NAMES** simply does not appear. That is not a fault,
it is the designed fallback; everything else works. The only cure is a full installation,
because only that writes the partition table.

**A new or blank board.** A fresh ESP32-C3 has no bootloader and no firmware. It cannot
serve a web page, so it cannot update itself. Something has to write the first complete
image, and this is that something.

**A rescue.** If the device no longer boots — an interrupted flash, a corrupted partition,
a bad experiment — a full installation puts it back into a known state.

If none of these apply, use the over-the-air update instead. It is quicker, needs no cable
and no computer.

---

## 2. What you need

| | |
|---|---|
| Browser | **Chrome or Edge on a desktop computer.** The installer talks to the USB port through the Web Serial API, which Firefox and Safari do not have. |
| Cable | A USB-C cable **with data wires**. Many charging cables carry power only; with one of those the device lights up and no port ever appears. |
| Internet | The page downloads the firmware files and the flashing tool while you use it. |
| Access | Physical access to the ESP32-C3 board and its DIP switches. |

Nothing has to be installed — no ESP-IDF, no driver package, no Python. The ESP32-C3
appears to the computer as a serial device on its own.

---

## 3. Before you start

**Set DIP switch 1 to ON.** DIP 1 is the power switch. With it OFF the chip is in deep
sleep, and in deep sleep it drops its USB port — the browser dialog will then list no
device at all, however good the cable is. This is the single most common reason the
installer appears not to work.

**Make sure the machine is not under FA_Control's control.** Press **RELEASE CONTROL** on
the device's own page first, or simply switch the pinball machine off. Flashing resets the
ESP32-C3 in the middle of whatever it was doing, and you do not want that to be a
half-finished coil pulse.

**Leave the device powered throughout.** The USB cable supplies it; do not unplug while
the log is still running.

---

## 4. Step by step

1. Connect the ESP32-C3 to your computer with the USB-C cable.

2. Open
   [FA_Control_flasher.html](https://lisy.dev/swrep/misc/FA_Control/flasher/FA_Control_flasher.html)
   in Chrome or Edge. If a red banner reads *WEB SERIAL NOT AVAILABLE*, you are in the
   wrong browser.

3. Check the **PACKAGE** box. **VERSION** shows which firmware version is about to be
   installed; below it are the four files and the addresses they go to.

4. Press **⚡ FLASH VIA USB**. The four files are downloaded first — you can watch this in
   the log — and then the browser asks you to pick a device.

5. **Pick the ESP32-C3 in the dialog** and confirm. It usually shows up as *USB JTAG/serial
   debug unit* or under the name of the serial chip. If the list is empty, see
   [Troubleshooting](#6-troubleshooting).

6. Watch the progress bar. Each file is reported by name and percentage; the whole package
   is roughly 1.2 MB and takes about a minute. **Do not unplug the cable and do not close
   the tab.**

7. When the status reads **DONE — DEVICE IS RESTARTING**, the device reboots on its own.
   You may unplug it.

8. Open the device's page again. If it had Wi-Fi credentials before, it rejoins your
   network and is reachable at `http://fa-control.local`. If not, it opens its own
   `FA-Control` network — see
   [§ 4 of the User Manual](https://github.com/bontango/FA_Control/blob/main/docs/USER_MANUAL.md#4-setting-up-wi-fi).
   Tile **07 · NAMES** should now be there.

---

## 5. What is kept and what is not

The installer writes to four addresses and erases nothing else, so most of what the device
knows survives:

| | |
|---|---|
| Wi-Fi credentials | **kept** — they live in a separate area (NVS) that is not written |
| Uploaded naming files | **kept**, if the device had the naming area already *and* is running v1.18 or newer. Coming from an older version the area moves (see below), so it starts out empty — as it does on a device that is getting the partition table for the first time. |
| Coil pulse time and the selected naming file | **kept** — same area as the Wi-Fi credentials |
| The choice of which firmware slot to boot | **reset** to the first slot, which is the one just written |

If you want a genuinely blank device, that is a job for `esptool.py erase_flash` on the
command line — the web installer deliberately does not offer it.

**Coming from a version before 1.18.** That release enlarged the two firmware slots, which
moved the naming area to a different place on the chip. The files that were there are not
carried over — the new area starts empty and you upload them again through **07 · NAMES**.
Wi-Fi credentials, pulse time and the selected naming file are unaffected. This is a
one-time cost of the new layout; installations from 1.18 onwards keep the files.

---

## 6. Troubleshooting

**The device dialog is empty.**
In order of likelihood: DIP 1 is OFF and the chip is asleep; the cable has no data wires;
the cable or port is loose. Flip DIP 1 to ON, replug, and press the button again.

**"Failed to connect" or the log stalls at *Connecting*.**
The chip is not answering in time. Hold the **BOOT** button down, press and release
**RESET**, then let BOOT go — that forces the download mode — and start the flash again.

**The download of the firmware files fails.**
The page fetches them from lisy.dev; check the internet connection. If you opened the page
from a local file rather than from lisy.dev, this can also be the browser refusing the
cross-origin request — use the online page.

**The flash was interrupted.**
Nothing is broken that a second attempt cannot fix. The device may not boot in between;
simply run the installer again from the start.

**No progress at all and no error.**
Close other programs that might be holding the serial port — a terminal, the ESP-IDF
monitor, an IDE. Only one program can own the port at a time.

---

## 7. Reference

The package consists of these four files, written to these addresses:

| File | Address | Purpose |
|---|---|---|
| `bootloader.bin` | `0x0` | Second-stage bootloader |
| `partition-table.bin` | `0x8000` | Map of the flash — this is the file that brings the naming storage |
| `ota_data_initial.bin` | `0xf000` | Which firmware slot to boot; reset to the first one |
| `FA_Control.bin` | `0x20000` | The firmware itself |

They are built and published by `build_and_deploy_full.ps1` and live in
[lisy.dev/swrep/misc/FA_Control/esptool/](https://lisy.dev/swrep/misc/FA_Control/esptool/),
alongside a `version.txt` that the installer page reads to show the version.

With ESP-IDF installed, the same thing happens locally with

```powershell
idf.py -B C:\Users\bonta\esp\build\fa -p COM7 flash
```

Note `flash`, not `app-flash` — only the former writes the partition table.

Why the partition table matters, and why an over-the-air update can never supply it, is
explained in
[Technical Reference § 6.2.2](https://github.com/bontango/FA_Control/blob/main/docs/TECHNICAL_REFERENCE.md#622-the-partition-needs-a-usb-flash).
