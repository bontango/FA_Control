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

**Seit v1.11 ist das der einzige Weg.** Die Handeingabe, der automatische Verbindungsaufbau beim
Start und die Watchdog-Einstellung sind entfallen — sie waren drei Möglichkeiten, denselben
Zustand widersprüchlich zu beschreiben. Die Startseite ist jetzt der Verbindungsaufbau selbst:
sie zeigt Gerät, Firmware und die gemeldete Bestückung, und die Steuerungsmenüs sind gesperrt,
solange die Gegenstelle die Kontrolle nicht gewährt hat. Der Watchdog hängt an der Verbindung,
nicht an einer Einstellung.

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
| I2C SCL **/ blaue LED** | 8 | Open-Drain bzw. Ausgang | **doppelt belegt**, siehe unten. Strapping; ext. Pull-Up hält high → boot-sicher |
| Taster | 9 | Eingang, **interner** Pull-Up | BOOT-Pin; während Reset gedrückt → Download-Modus |
| DIP 1 | 0 | Eingang, interner Pull-Up | ON = GND = **wach**, OFF = Tiefschlaf |
| DIP 2 | 1 | Eingang, interner Pull-Up | ON = Blinkanzeige aus (GPIO8 frei für I2C) |
| DIP 3–4 | 2, 4 | Eingänge, interner Pull-Up | frei |
| Reserve | 2 | — | freigehalten (Strapping) |

**GPIO8 ist doppelt belegt.** Dort sitzt die blaue LED des Moduls, und derselbe Pin ist als
I2C SCL reserviert. Solange I2C unbenutzt ist (kein Treiber-Init), gehört der Pin der
Blinkanzeige. Wird I2C jemals in Betrieb genommen, muss **DIP 2 auf ON** — dann lässt
`power_mgr` GPIO8 unkonfiguriert und hochohmig, und der I2C-Treiber kann ihn übernehmen.

Nicht verfügbar: GPIO11–17 (SPI-Flash), GPIO18/19 (USB-Serial/JTAG), GPIO20/21 (UART0-Konsole).

## Betriebsarten — DIP 1 ist der Ein/Aus-Schalter

FA_Control ist ein Werkzeug für die Fehlersuche, hängt aber dauerhaft im Flipper. Seit **v1.12**
ist der Tiefschlaf deshalb der Normalfall:

| DIP 1 | Verhalten |
|---|---|
| **ON** | wach: WLAN, Webserver, Steuerung. Die blaue LED blinkt. |
| **OFF** | Tiefschlaf. Aufgeweckt wird über denselben Pin — DIP 1 auf ON legen genügt. |

Wird DIP 1 im laufenden Betrieb auf OFF gelegt, gibt FA_Control **erst die Kontrolle zurück**
(GPIO10 zurück, Watchdog aus, das Spiel übernimmt sofort wieder) und schläft dann ein. Ohne
diese Reihenfolge stünde der Flipper bis zum Watchdog-Timeout der Gegenseite.

Das Blinkmuster sagt aus zwei Metern, was los ist — ohne die Weboberfläche aufzumachen:

| Muster | Bedeutung |
|---|---|
| **1 Hz** | wach, keine Kontrolle über die Anlage |
| **5 Hz** | Kontrolle aktiv, Watchdog läuft |
| **10 Hz** | Gnadenfrist nach dem Start, gleich geht es in den Tiefschlaf |
| dunkel | Tiefschlaf — oder DIP 2 steht auf ON |

### Die Flash-Falle

Im Tiefschlaf verschwindet der USB-Serial/JTAG-Port; `idf.py flash` findet dann kein COM7 mehr
und `idf.py monitor` bricht ab. Deshalb bleibt das Gerät nach dem Start **30 s wach**, auch wenn
DIP 1 auf OFF steht (LED blinkt in dieser Zeit mit 10 Hz). In diesem Fenster lässt sich flashen.
Bequemer ist es, zum Flashen einfach DIP 1 auf ON zu legen. Notausgang bleibt der BOOT-Taster,
während des Resets gehalten → Download-Modus.

Die Frist steht als `POWER_BOOT_GRACE_MS` oben in [`main/power_mgr.c`](main/power_mgr.c).

### Warum DIP 1 auf GPIO0 liegen muss

Aus dem Tiefschlaf wecken kann der ESP32-C3 nur über **GPIO0–GPIO5**
(`SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK`). DIP 1 liegt auf GPIO0 und ist damit der einzige
Kandidat der Bank — der Taster auf GPIO9 könnte es nicht. Beim Umlegen der DIP-Bank ist das zu
beachten.

## Verbinden

Der Verbindungsaufbau steht auf der **Startseite** und muss immer ausgelöst werden — beim Start
verbindet FA_Control bewusst nicht, solange niemand **VERBINDEN** drückt, gehört der Flipper
sich selbst.

1. Der ESP setzt die **Übernahme-Anforderung** auf GPIO10.
2. Er schickt **0x64 (`LISY_INIT`)**. Die Antwort sagt, ob die Gegenstelle die Kontrolle gibt:
   `0` = gewährt · `1` = Freigabeschalter dort steht auf OFF · `2` = Anforderung kam nicht an ·
   keine Antwort = niemand da.
3. Bei Erfolg fragt er die **Info-Gruppe 0–9** ab. Kennung, Versionen und die Bestückung stehen
   danach unter *GEMELDETE BESTÜCKUNG* auf der Startseite — als Anzeige, nicht als Formular.

Klappt es nicht, steht der Grund im Klartext oben auf der Seite, und die Steuerungsmenüs bleiben
gesperrt: ohne gewährte Kontrolle gibt es keine Bestückung und damit nichts zu steuern. Der
Webserver weist Steuerbefehle dann auch selbst ab, nicht nur die Oberfläche. Bei AtariFA ist der
Freigabeschalter **Options-DIP 4**.

Der **Watchdog** (0x65, alle 500 ms) ist zugleich die Totmannschaltung der Gegenseite: bleibt er
aus, gibt sie die Kontrolle nach kurzer Zeit (dort ~2 s) von selbst zurück. Er ist damit
Bedingung der aktiven Kontrolle und nicht einstellbar — er läuft, solange verbunden ist, und
sonst nie.

## Funktionen

Wie viele es jeweils gibt, sagt die Gegenstelle beim Verbinden; die Maxima setzt das Protokoll.

| Kategorie | Maximum (Protokoll) | LISY-Befehle |
|---|---|---|
| Lampen | 255 | 0x0B Ein, 0x0C Aus |
| Spulen | 127 | 0x17 Puls (kein Dauerstrom), 0x18 Pulszeit |
| Schalter | 127 | 0x28 / 0x29 — **nur Statusanzeige** (Protokoll erlaubt kein Setzen) |
| Sound | 255 | 0x32 Play (Track 1), 0x33 Stop |
| Displays | 7 | 0x1E+d, BCD7-kodiert, rechtsbündig |

Zusätzlich:

- **Info-Gruppe** (0x00–0x09): Kennung, Firmware- und Protokollversion, Spielnummer sowie die
  Anzahlen — das ist der Verbindungsaufbau, siehe oben.
- **Watchdog** (0x65) läuft automatisch alle 500 ms, solange die Kontrolle aktiv ist; Zustand und
  letzte Antwort stehen auf der Startseite.
- **Init/Reset** (0x64) per Button auf der Startseite.
- Die **Spulen-Pulszeit** (Menü *SPULEN*) ist die einzige Einstellung, die FA_Control selbst im
  NVS speichert. Alles andere kommt vom Gerät oder hängt am Verbindungszustand.

## WLAN-Einrichtung (Wi-Fi Manager)

1. Beim ersten Start (oder wenn keine Verbindung möglich ist) öffnet der ESP32 den
   offenen Access Point **„FA-Control"**.
2. Mit dem AP verbinden — das **Captive Portal** öffnet die Steuerseite automatisch
   (sonst manuell `http://192.168.4.1`).
3. Unter **WLAN → NETZ** SSID und Passwort eingeben → Neustart.
4. Danach ist die Seite im Heimnetz erreichbar: **`http://fa-control.local`** (mDNS) oder per IP
   (steht im Boot-Log und im Seitenkopf).

## Firmware-Update über lisy.dev (OTA)

Unter **WLAN → FIRMWARE** kann eine neue Firmware direkt vom Server installiert werden:

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

### Welcher Python — `IDF_PYTHON_ENV_PATH`

Maßgeblich ist der **von ESP-IDF mitgelieferte Python 3.11.2**
(`.espressif\tools\idf-python\3.11.2`), venv `idf5.5_py3.11_env`. Damit das überall gilt, ist

```
IDF_PYTHON_ENV_PATH = C:\Users\bonta\.espressif\python_env\idf5.5_py3.11_env
```

als **Windows-Benutzervariable** gesetzt. Ohne sie wählt ESP-IDF die Umgebung nach der Version
des Interpreters, den `export.ps1` gerade aufruft — und das ist das erste `python` im PATH, hier
der Microsoft-Store-Alias auf 3.14. Dann entsteht ein zweites venv, und sobald ein
Build-Verzeichnis mit dem einen und die Shell mit dem anderen läuft, bricht CMake ab mit
*„… is currently active in the environment while the project was configured with …"*.

In einer Shell ohne die Benutzervariable (fremder Rechner, frisch aufgesetzt):

```powershell
$env:IDF_PYTHON_ENV_PATH = "$env:USERPROFILE\.espressif\python_env\idf5.5_py3.11_env"
. C:\Users\bonta\esp\v5.5.1\esp-idf\export.ps1
```

Prüfen lässt es sich nach dem `export.ps1` mit `python -c "import sys; print(sys.version)"` —
muss **3.11.2** sagen.

**Beim Umstieg auf eine neuere ESP-IDF-Version** muss die Variable mit: der Pfad enthält `5.5`.
Das fällt aber auf, weil `idf_tools.py` das venv gegen die IDF-Version prüft und im Klartext
meldet, dass es für eine andere Version erzeugt wurde.

## Projektstruktur

```
main/
├── main.c          # app_main: NVS → Konfig → Board → DIP1-Gate → LISY → WLAN → Webserver
├── board.c/h       # GPIO-Reservierung (Status, Taster, DIP-Bank, I2C-Pins, LED)
├── power_mgr.c/h   # DIP1 = Ein/Aus, Tiefschlaf + Aufwecken, Blinkanzeige
├── board_pins.h    # zentrale Pin-Zuordnung (single source of truth)
├── app_config.c/h  # NVS-Konfiguration — nur noch die Spulen-Pulszeit
├── fa_connect.c/h  # Verbindungsaufbau: Anforderung, 0x64, Info-Gruppe 0-9; hält die Bestückung
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
| `GET /api/config` | Bestückung, Verbindungszustand, Geräteinfo, Watchdog und Pulszeit als JSON |
| `POST /api/config?pulse=50` | Spulen-Pulszeit speichern (einzige speicherbare Einstellung) |
| `POST /api/connect` | Verbinden und Bestückung abfragen; antwortet wie `GET /api/config` |
| `POST /api/disconnect` | Kontrolle zurückgeben; antwortet wie `GET /api/config` |
| `POST /api/lamp?id=5&on=1` | Lampe ein/aus |
| `POST /api/coil?id=3` | Spule pulsen |
| `POST /api/sound?id=7&on=1` | Sound abspielen / stoppen |
| `POST /api/display?id=0&text=1234` | Displaytext senden |
| `GET /api/state` | Lampen-/Schalter-Bitmaps (hex) + Displaytexte |
| `POST /api/reset` | LISY Init/Reset (0x64) |
| `POST /api/wifi?ssid=&pass=` | WLAN-Credentials speichern + Neustart |
| `GET /api/status` | WLAN-Modus (ap/sta), IP, Firmware-Version |
| `GET /api/fwlist` | .bin-Dateien auf lisy.dev als JSON |
| `POST /api/fwupdate?file=FA_Control_v1.01.bin` | OTA-Update starten |
| `GET /api/fwstatus` | Update-Fortschritt (`idle/running/ok/error`, Prozent) |

Die Steuer-Endpunkte (`lamp`, `coil`, `sound`, `display`) prüfen gegen die vom Gerät gemeldete
Bestückung. Ohne gewährte Kontrolle steht die auf 0 — jeder Befehl endet dann mit
`400 Parameter`, unabhängig davon, was die Oberfläche anbietet.
