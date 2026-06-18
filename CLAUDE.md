# CLAUDE.md

ESP-IDF-Projekt (C) für ESP32-C3: Webserver zur Flippersteuerung über das LISY-Protokoll
(seriell, UART1: TX=GPIO6, RX=GPIO7, 115200 8N1). Details siehe README.md.

## Bauen & Flashen

`N:` ist ein UNC-Netzlaufwerk — **niemals** das Standard-`build/`-Verzeichnis verwenden
(cmd.exe/ld scheitern an UNC-Arbeitsverzeichnissen). Immer lokales Build-Verzeichnis per `-B`:

```powershell
. C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
idf.py -B C:\Users\bonta\esp\build\fa build
idf.py -B C:\Users\bonta\esp\build\fa -p COM6 flash
```

- ESP-IDF v5.5.1: `C:\Users\bonta\esp\v5.5.1\esp-idf`
- Board: ESP32-C3 an COM6 (USB-Serial/JTAG, Boot-Log mit 115200 Baud lesbar)

## Release & Deploy

`.\build_and_deploy.ps1` — baut (lokales Build-Verzeichnis), kopiert das Binary als
`FA_Control_v<version>.bin` nach `releases\` (Version aus `version.txt` = PROJECT_VER)
und lädt es per SFTP/WinSCP auf den lisy.dev-Server (`.env` mit Zugangsdaten nötig,
Vorlage `.env.example`; Enter statt Passwort = nur lokal kopieren). Vor einem Release
`version.txt` hochzählen.

## Architektur (alles unter `main/`)

- `board_pins.h` — zentrale GPIO-Zuordnung (single source of truth); `board.c` reserviert/
  konfiguriert Status-Ausgang (GPIO10), Taster (GPIO9), 4er-DIP-Bank (GPIO0/1/3/4) und
  reserviert I2C-Pins (SDA=GPIO5, SCL=GPIO8). ESP32-C3-Strapping (GPIO2/8/9) beachten.
- `lisy.c` — LISY-Befehle über UART1; jeder Zugriff Mutex-geschützt; Watchdog (0x65)
  per esp_timer alle 500 ms; Lampen-/Schalter-Zustand als Bitmaps gespiegelt
- `app_config.c` — Konfiguration als NVS-Blob (Namespace `facfg`)
- `wifi_mgr.c` — STA mit Credentials aus NVS (Namespace `wifi`), Fallback: offener AP
  „FA-Control" + Mini-DNS (Captive Portal) auf 192.168.4.1; mDNS `fa-control.local`
- `web_server.c` — REST-API nur über Query-Parameter (kein JSON-Parsing);
  JSON-Antworten per snprintf
- `fw_update.c` — OTA-Update von `https://lisy.dev/swrep/misc/FA_Control/bin/`
  (esp_https_ota + Cert-Bundle); Listing = Apache-Index, per `href="*.bin"` gescannt;
  zwei OTA-Partitionen à 1,5 MB (`partitions.csv`, Flash 4 MB); Version aus `version.txt`
- `web/index.html` — Single-Page-Frontend (Vanilla JS, Retro-CRT-Stil); wird beim Build
  durch `web/gzip_file.py` gegzippt und per `target_add_binary_data` eingebettet

## Konventionen

- Schalter sind im LISY-Protokoll nur lesbar (0x28/0x29) — UI zeigt nur Status
- Spulen werden ausschließlich gepulst (0x17), nie dauerhaft eingeschaltet
- Protokoll-Maxima: Lampen 255, Spulen 127, Schalter 127, Sounds 255, Displays 7
- Log-/UI-Texte auf Deutsch
