# CLAUDE.md

ESP-IDF-Projekt (C) für ESP32-C3: Webserver zur Flippersteuerung über das LISY-Protokoll
(seriell, UART1: **TX=GPIO7, RX=GPIO6**, 115200 8N1). Details siehe README.md.

**Gegenstelle:** das VHDL-Modul `rtl/fa_control/` in den FA-FPGA-Projekten, zuerst umgesetzt in
`N:\Projekte\FPGA Atari\FPGA_source` (Doku dort: `docs/FA_Control_Interface.md`). Wer hier am
Protokoll etwas ändert, muss dort mitziehen — und umgekehrt.

## Bauen & Flashen

`N:` ist ein UNC-Netzlaufwerk — **niemals** das Standard-`build/`-Verzeichnis verwenden
(cmd.exe/ld scheitern an UNC-Arbeitsverzeichnissen). Immer lokales Build-Verzeichnis per `-B`:

```powershell
. C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
idf.py -B C:\Users\bonta\esp\build\fa build
idf.py -B C:\Users\bonta\esp\build\fa -p COM7 flash
```

- ESP-IDF v5.5.1: `C:\Users\bonta\esp\v5.5.1\esp-idf`
- Board: ESP32-C3 an COM7 (USB-Serial/JTAG, Boot-Log mit 115200 Baud lesbar)

## Release & Deploy

`.\build_and_deploy.ps1` — baut (lokales Build-Verzeichnis), kopiert das Binary als
`FA_Control_v<version>.bin` nach `releases\` (Version aus `version.txt` = PROJECT_VER)
und lädt es per SFTP/WinSCP auf den lisy.dev-Server (`.env` mit Zugangsdaten nötig,
Vorlage `.env.example`; Enter statt Passwort = nur lokal kopieren). Vor einem Release
`version.txt` hochzählen.

## Architektur (alles unter `main/`)

- `board_pins.h` — zentrale GPIO-Zuordnung (single source of truth); `board.c` konfiguriert die
  Übernahme-Anforderung (GPIO10, `board_ctrl_request()`), Taster (GPIO9), 4er-DIP-Bank
  (GPIO0/1/2/4) und reserviert I2C-Pins (SDA=GPIO5, SCL=GPIO8). ESP32-C3-Strapping (GPIO2/8/9)
  beachten. **In v1.10 korrigiert:** TX/RX waren vertauscht und DIP3 stand auf GPIO3 (dort `<nc>`).
  Maßgeblich ist der AtariFA-Schaltplan, nicht diese Datei.
- `fa_connect.c` — Verbindungsaufbau: GPIO10 setzen → 0x64 → bei Erfolg Info-Gruppe 0..9 abfragen
  und `g_cfg` daraus füllen. Bei Ablehnung GPIO10 wieder freigeben; der Grund kommt als Klartext
  in die Weboberfläche. Die im NVS gespeicherten Anzahlen sind nur noch der Rückfall.
- `lisy.c` — LISY-Befehle über UART1; jeder Zugriff Mutex-geschützt; Watchdog (0x65)
  per esp_timer alle 500 ms; Lampen-/Schalter-Zustand als Bitmaps gespiegelt
- `app_config.c` — Konfiguration als NVS-Blob (Namespace `facfg`), mit `magic`+`version` im
  Struct: früher wurde nur die Größe geprüft, eine Struct-Änderung hat die Konfiguration also
  entweder still verworfen oder falsch gelesen. `CFG_VERSION` bei jeder Feldänderung hochzählen.
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
- **Anzahlen kommen vom Gerät, nicht aus dem Formular.** Kam die Verbindung zustande, sind die
  Felder in der Oberfläche schreibgeschützt (`cfg.src == "fpga"`) — sie beschreiben dann die
  tatsächliche Bestückung. Von Hand eingetragene Werte gelten nur ohne Gegenstelle.
- Solange FA_Control die Kontrolle hat, läuft der Watchdog **immer**, unabhängig von der
  Einstellung: er ist die Totmannschaltung der Gegenseite (dort ~2 s), nicht nur ein Lebenszeichen.
- Log-/UI-Texte auf Deutsch
