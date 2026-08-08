# FA_Control — ESP32-C3 LISY Pinball-Webcontroller

Webbasierte Steuerung eines Flippers über das [LISY-Protokoll](https://missionpinball.org/latest/hardware/lisy/protocol/) (v0.12).
Der ESP32-C3 agiert als LISY-Host: Er stellt eine Webseite im Retro-80er-Stil bereit und sendet
die Steuerbefehle binär über UART an die Empfängerseite (Flipper), auf der ein LISY-kompatibles
Programm läuft.

**Seit v1.10 gibt es einen richtigen Verbindungsaufbau.** Vorher hat FA_Control blind gesendet:
die Anzahl von Lampen, Spulen, Schaltern und Displays musste von Hand eingetragen werden, und
ob überhaupt jemand zuhört, war nicht feststellbar. Jetzt fragt FA_Control die Bestückung beim
Verbinden ab (LISY-Opcodes 0–9) und konfiguriert sich selbst. Gegenstelle auf FPGA-Seite ist das
VHDL-Modul `rtl/fa_control/` — implementiert in
[AtariFA](https://github.com/bontango/AtariFA), von dort in andere FA-Projekte übernehmbar.

## Hardware

| | |
|---|---|
| Board | ESP32-C3 (an **COM7**, USB-Serial/JTAG) |
| UART zum Flipper | UART1: **TX = GPIO7**, **RX = GPIO6**, 115200 Baud, 8N1 |
| Framework | ESP-IDF **v5.5.1** (`C:\Users\bonta\esp\v5.5.1\esp-idf`) |

### GPIO-Belegung

Zentrale Pin-Zuordnung in [`main/board_pins.h`](main/board_pins.h).

> **In v1.10 korrigiert:** TX und RX waren vertauscht, und DIP3 stand auf GPIO3. Maßgeblich ist
> der AtariFA-Schaltplan (Steckverbinder X7 ↔ X1P): GPIO7 führt das Netz `ESP32_TX` an den
> FPGA-Eingang, GPIO6 das Netz `ESP32_RX` an den FPGA-Ausgang. Mit der alten Belegung hätten
> beide Seiten auf derselben Leitung gegeneinander gesendet — aufgefallen ist es nie, weil es
> bis dahin gar keine Gegenstelle gab.

| Signal | GPIO | Richtung / Pull | Bemerkung |
|---|---|---|---|
| LISY UART1 TX | 7 | Ausgang | Netz `ESP32_TX` |
| LISY UART1 RX | 6 | Eingang | Netz `ESP32_RX` |
| Übernahmewunsch → Flipper | 10 | Ausgang | active-low; Gegenseite hat Pull-Up |
| I2C SDA | 5 | Open-Drain, externer Pull-Up | nur reserviert |
| I2C SCL | 8 | Open-Drain, externer Pull-Up | Strapping; ext. Pull-Up hält high → boot-sicher; nur reserviert |
| Taster | 9 | Eingang, **interner** Pull-Up | BOOT-Pin; während Reset gedrückt → Download-Modus |
| DIP 1–4 | 0, 1, 2, 4 | Eingänge, interner Pull-Up | ON = GND; ohne Funktion |
| Reserve | 2 | — | freigehalten (Strapping) |

Nicht verfügbar: GPIO11–17 (SPI-Flash), GPIO18/19 (USB-Serial/JTAG), GPIO20/21 (UART0-Konsole).

## Verbinden

Beim Start (abschaltbar) und über **KONFIG → VERBINDEN**:

1. Der ESP setzt die **Übernahme-Anforderung** auf GPIO10.
2. Er schickt **0x64 (`LISY_INIT`)**. Die Antwort sagt, ob die Gegenstelle die Kontrolle gibt:
   `0` = gewährt · `1` = Freigabeschalter dort steht auf OFF · `2` = Anforderung kam nicht an ·
   keine Antwort = niemand da.
3. Bei Erfolg fragt er die **Info-Gruppe 0–9** ab und setzt Kennung, Version und alle Anzahlen
   daraus. Die Felder in der Weboberfläche sind dann nicht mehr von Hand änderbar — sie
   beschreiben die tatsächliche Bestückung.

Klappt es nicht, steht der Grund im Klartext oben auf der Seite, und es gelten weiter die
gespeicherten Werte. Bei AtariFA ist der Freigabeschalter **Options-DIP 4**.

Der **Watchdog** (0x65, alle 500 ms) ist zugleich die Totmannschaltung der Gegenseite: bleibt er
aus, gibt sie die Kontrolle nach kurzer Zeit von selbst zurück. Solange FA_Control steuert, läuft
er deshalb unabhängig von der Einstellung in der Oberfläche.

## Funktionen

| Kategorie | Default (Rückfall) | Maximum (Protokoll) | LISY-Befehle |
|---|---|---|---|
| Lampen | 40 | 255 | 0x0B Ein, 0x0C Aus |
| Spulen | 20 | 127 | 0x17 Puls (kein Dauerstrom), 0x18 Pulszeit |
| Schalter | 40 | 127 | 0x28 / 0x29 — **nur Statusanzeige** (Protokoll erlaubt kein Setzen) |
| Sound | 16 | 255 | 0x32 Play (Track 1), 0x33 Stop |
| Displays | 5 (1×4 + 4×6 Stellen) | 7 | 0x1E+d, BCD7-kodiert, rechtsbündig |

Zusätzlich:

- **Info-Gruppe** (0x00–0x09): Kennung, Firmware- und Protokollversion, Spielnummer sowie die
  Anzahlen — das ist der Verbindungsaufbau, siehe oben.
- **Watchdog** (0x65) wird automatisch alle 500 ms gesendet (in der Konfiguration abschaltbar) —
  die Empfängerseite schaltet sonst nach kurzer Zeit alle Ausgänge ab.
- **Init/Reset** (0x64) per Button in der Konfiguration.
- Die Werte in der Tabelle sind nur der **Rückfall**, wenn keine Gegenstelle antwortet. Sie sind
  über den Menüpunkt **KONFIG** einstellbar und werden im NVS gespeichert.

## WLAN-Einrichtung (Wi-Fi Manager)

1. Beim ersten Start (oder wenn keine Verbindung möglich ist) öffnet der ESP32 den
   offenen Access Point **„FA-Control"**.
2. Mit dem AP verbinden — das **Captive Portal** öffnet die Steuerseite automatisch
   (sonst manuell `http://192.168.4.1`).
3. Unter **KONFIG → WLAN** SSID und Passwort eingeben → Neustart.
4. Danach ist die Seite im Heimnetz erreichbar: **`http://fa-control.local`** (mDNS) oder per IP
   (steht im Boot-Log und im Seitenkopf).

## Firmware-Update über lisy.dev (OTA)

Unter **KONFIG → FIRMWARE** kann eine neue Firmware direkt vom Server installiert werden:

1. **VERSIONEN LADEN** — der ESP32 holt das Verzeichnislisting von
   `https://lisy.dev/swrep/misc/FA_Control/bin/` und zeigt alle `.bin`-Dateien an
   (Namenskonvention: `FA_Control_vX.Y.bin`).
2. Version auswählen → **UPDATE INSTALLIEREN** — der ESP32 lädt die Datei selbst per
   HTTPS herunter, schreibt sie in die inaktive OTA-Partition, validiert das Image und
   startet neu. Fortschritt wird live angezeigt.

Voraussetzungen: Gerät im STA-Modus (im AP-Modus kein Internet); zwei OTA-Partitionen
à 1,5 MB (`partitions.csv`, Flash 4 MB). Die laufende Versionsnummer stammt aus
`version.txt` (PROJECT_VER) und wird im Seitenkopf angezeigt.

**Hinweis:** Die Umstellung von Single-App auf OTA-Partitionen erforderte ein einmaliges
komplettes Neuflashen über USB; WLAN-Credentials und Konfiguration im NVS blieben erhalten.

### Release bauen & hochladen (`build_and_deploy.ps1`)

Neue Releases werden per Script erzeugt und hochgeladen:

```powershell
.\build_and_deploy.ps1
```

Das Script baut die Firmware (lokales Build-Verzeichnis, siehe unten), kopiert das Binary
als `FA_Control_v<version>.bin` nach `releases\` (Version aus `version.txt`) und lädt es
per SFTP (WinSCP) nach `lisy.dev/swrep/misc/FA_Control/bin/` hoch. Zugangsdaten kommen
aus einer `.env`-Datei (Vorlage: `.env.example`), das Passwort wird interaktiv abgefragt —
Enter ohne Passwort überspringt den Upload (nur lokale Kopie). Vor einem Release
`version.txt` hochzählen.

## Bauen & Flashen

**Wichtig:** `N:` ist ein Netzlaufwerk (UNC-Pfad `\\synnew\th\...`). Der Build muss in ein
**lokales** Verzeichnis erfolgen, da `cmd.exe`/Linker keine UNC-Arbeitsverzeichnisse unterstützen.

```powershell
. C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
idf.py -B C:\Users\bonta\esp\build\fa build
idf.py -B C:\Users\bonta\esp\build\fa -p COM7 flash monitor
```

Die VS-Code-Extension (ESP-IDF) ist über `.vscode/settings.json` bereits konfiguriert:
Port COM7, Target esp32c3, lokales Build-Verzeichnis (`idf.buildPathWin`).

## Projektstruktur

```
main/
├── main.c          # app_main: NVS → Konfig → Board → LISY → WLAN → Webserver
├── board.c/h       # GPIO-Reservierung (Status, Taster, DIP-Bank, I2C-Pins)
├── board_pins.h    # zentrale Pin-Zuordnung (single source of truth)
├── app_config.c/h  # Konfiguration (Anzahlen, Pulszeit, Watchdog) im NVS
├── fa_connect.c/h  # Verbindungsaufbau: Anforderung, 0x64, Info-Gruppe 0-9 abfragen
├── lisy.c/h        # UART1-Treiber + LISY-Protokollschicht (Mutex-geschützt)
├── wifi_mgr.c/h    # STA mit NVS-Credentials, Fallback AP + Captive Portal + mDNS
├── web_server.c/h  # esp_http_server: REST-API + eingebettete Webseite
├── fw_update.c/h   # OTA-Update von lisy.dev (esp_https_ota, Verzeichnislisting)
└── web/index.html  # Single-Page-Frontend (wird beim Build gegzippt eingebettet)
```

## REST-API (Query-Parameter)

| Endpoint | Funktion |
|---|---|
| `GET /` | Webseite (gzip) |
| `GET /api/config` | Konfiguration als JSON (inkl. Maxima, Verbindungszustand, Geräteinfo) |
| `POST /api/config?lamps=&coils=&switches=&sounds=&displays=&dw=4,6,6,6,6&wd=1&pulse=50&auto=1` | Konfiguration speichern |
| `POST /api/connect` | Verbinden und Bestückung abfragen; antwortet wie `GET /api/config` |
| `POST /api/disconnect` | Kontrolle zurückgeben; antwortet wie `GET /api/config` |
| `POST /api/lamp?id=5&on=1` | Lampe ein/aus |
| `POST /api/coil?id=3` | Spule pulsen |
| `POST /api/sound?id=7&on=1` | Sound abspielen / stoppen |
| `POST /api/display?id=0&text=1234` | Displaytext senden |
| `GET /api/state` | Lampen-/Schalter-Bitmaps (hex) + Displaytexte |
| `POST /api/reset` | LISY Init/Reset (0x64) |
| `POST /api/wifi?ssid=&pass=` | WLAN-Credentials speichern + Neustart |
| `GET /api/status` | Modus (ap/sta), IP, Watchdog-Status, Firmware-Version |
| `GET /api/fwlist` | .bin-Dateien auf lisy.dev als JSON |
| `POST /api/fwupdate?file=FA_Control_v1.01.bin` | OTA-Update starten |
| `GET /api/fwstatus` | Update-Fortschritt (`idle/running/ok/error`, Prozent) |
