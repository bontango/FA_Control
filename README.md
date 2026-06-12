# FA_Control — ESP32-C3 LISY Pinball-Webcontroller

Webbasierte Steuerung eines Flippers über das [LISY-Protokoll](https://missionpinball.org/latest/hardware/lisy/protocol/) (v0.08).
Der ESP32-C3 agiert als LISY-Host: Er stellt eine Webseite im Retro-80er-Stil bereit und sendet
die Steuerbefehle binär über UART an die Empfängerseite (Flipper), auf der bereits ein
LISY-kompatibles Programm läuft.

## Hardware

| | |
|---|---|
| Board | ESP32-C3 (an **COM6**, USB-Serial/JTAG) |
| UART zum Flipper | UART1: **TX = GPIO6**, **RX = GPIO7**, 115200 Baud, 8N1 |
| Framework | ESP-IDF **v5.5.1** (`C:\Users\bonta\esp\v5.5.1\esp-idf`) |

## Funktionen

| Kategorie | Default | Maximum (Protokoll) | LISY-Befehle |
|---|---|---|---|
| Lampen | 40 | 255 | 0x0B Ein, 0x0C Aus |
| Spulen | 20 | 127 | 0x17 Puls (kein Dauerstrom), 0x18 Pulszeit |
| Schalter | 40 | 127 | 0x28 / 0x29 — **nur Statusanzeige** (Protokoll erlaubt kein Setzen) |
| Sound | 16 | 255 | 0x32 Play (Track 1), 0x33 Stop |
| Displays | 5 (1×4 + 4×6 Stellen) | 7 | 0x1E+d, BCD7-kodiert, rechtsbündig |

Zusätzlich:

- **Watchdog** (0x65) wird automatisch alle 500 ms gesendet (in der Konfiguration abschaltbar) —
  die Empfängerseite schaltet sonst nach 1 s alle Ausgänge ab.
- **Init/Reset** (0x64) per Button in der Konfiguration.
- Alle Anzahlen, Display-Stellenbreiten, Spulen-Pulszeit und Watchdog sind über den
  Menüpunkt **KONFIG** einstellbar und werden im NVS gespeichert.

## WLAN-Einrichtung (Wi-Fi Manager)

1. Beim ersten Start (oder wenn keine Verbindung möglich ist) öffnet der ESP32 den
   offenen Access Point **„FA-Control"**.
2. Mit dem AP verbinden — das **Captive Portal** öffnet die Steuerseite automatisch
   (sonst manuell `http://192.168.4.1`).
3. Unter **KONFIG → WLAN** SSID und Passwort eingeben → Neustart.
4. Danach ist die Seite im Heimnetz erreichbar: **`http://fa-control.local`** (mDNS) oder per IP
   (steht im Boot-Log und im Seitenkopf).

## Bauen & Flashen

**Wichtig:** `N:` ist ein Netzlaufwerk (UNC-Pfad `\\synnew\th\...`). Der Build muss in ein
**lokales** Verzeichnis erfolgen, da `cmd.exe`/Linker keine UNC-Arbeitsverzeichnisse unterstützen.

```powershell
. C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
idf.py -B C:\Users\bonta\esp\build\fa build
idf.py -B C:\Users\bonta\esp\build\fa -p COM6 flash monitor
```

Die VS-Code-Extension (ESP-IDF) ist über `.vscode/settings.json` bereits konfiguriert:
Port COM6, Target esp32c3, lokales Build-Verzeichnis (`idf.buildPathWin`).

## Projektstruktur

```
main/
├── main.c          # app_main: NVS → Konfig → LISY → WLAN → Webserver
├── app_config.c/h  # Konfiguration (Anzahlen, Pulszeit, Watchdog) im NVS
├── lisy.c/h        # UART1-Treiber + LISY-Protokollschicht (Mutex-geschützt)
├── wifi_mgr.c/h    # STA mit NVS-Credentials, Fallback AP + Captive Portal + mDNS
├── web_server.c/h  # esp_http_server: REST-API + eingebettete Webseite
└── web/index.html  # Single-Page-Frontend (wird beim Build gegzippt eingebettet)
```

## REST-API (Query-Parameter)

| Endpoint | Funktion |
|---|---|
| `GET /` | Webseite (gzip) |
| `GET /api/config` | Konfiguration als JSON (inkl. Maxima) |
| `POST /api/config?lamps=&coils=&switches=&sounds=&displays=&dw=4,6,6,6,6&wd=1&pulse=50` | Konfiguration speichern |
| `POST /api/lamp?id=5&on=1` | Lampe ein/aus |
| `POST /api/coil?id=3` | Spule pulsen |
| `POST /api/sound?id=7&on=1` | Sound abspielen / stoppen |
| `POST /api/display?id=0&text=1234` | Displaytext senden |
| `GET /api/state` | Lampen-/Schalter-Bitmaps (hex) + Displaytexte |
| `POST /api/reset` | LISY Init/Reset (0x64) |
| `POST /api/wifi?ssid=&pass=` | WLAN-Credentials speichern + Neustart |
| `GET /api/status` | Modus (ap/sta), IP, Watchdog-Status |
