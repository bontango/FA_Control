#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"

/*
 * Sprechende Namen fuer Lampen, Spulen, Schalter und Sounds.
 *
 * Je Anlage eine INI-artige Textdatei auf der LittleFS-Partition "names":
 *
 *     [game]
 *     name=Airborne Avenger
 *     [coils]
 *     1=Left Flipper
 *     [switches]
 *     12=Left Slingshot
 *
 * Spulen zaehlen ab 1, alles andere ab 0 -- genau wie ueber die Leitung.
 *
 * Der ORT der Datei ist die Kennung der Anlage: "<HW>/<GAME>.cfg", GAME
 * dreistellig mit fuehrenden Nullen -- Airborne Avenger auf AtariFA liegt unter
 * "AtariFA/002.cfg". Die Spielnummer allein reicht nicht: sie faengt auf jeder
 * Platine wieder bei 0 an, und FA_Control bedient mehrere (AtariFA, GottFA1,
 * ...). Die Hardware-Kennung aus LISY-Opcode 0x00 macht sie eindeutig, und als
 * Ordner haelt sie die Ablage uebersichtlich, sobald viele Spiele zusammen-
 * kommen. Gross- und Kleinschreibung spielt beim Suchen auf BEIDEN Ebenen keine
 * Rolle.
 *
 * Nach aussen -- REST-API, NVS, Weboberflaeche -- ist eine Namensdatei trotzdem
 * EIN Bezeichner, nur eben mit Schraegstrich darin: "AtariFA/002.cfg". Deshalb
 * braucht weder app_config_t ein zweites Feld noch die API ein zweites
 * Parameterpaar.
 *
 * Bis v1.18 hiessen die Dateien "<HW>_<GAME>.cfg" und lagen flach im Wurzel-
 * verzeichnis. Solche Altbestaende werden NICHT automatisch verschoben; sie
 * erscheinen in names_list_json() als "old" und lassen sich dort nur noch
 * loeschen. Verstecken waere schlechter: sie belegen Flash.
 *
 * Der ESP parst diese Dateien NICHT. Er verwaltet sie nur (ablegen, auflisten,
 * loeschen, auswaehlen) und liefert die aktive Datei roh aus; das Zerlegen macht
 * die Weboberflaeche. Das haelt den Parser dort, wo die Namen gebraucht werden,
 * und spart auf dem Geraet RAM wie Code.
 *
 * Die Partition liegt am freien Flash-Ende und kommt erst durch ein Flashen per
 * USB auf das Geraet -- esp_https_ota schreibt die Partitionstabelle nicht. Auf
 * einem nur per OTA aktualisierten Geraet fehlt sie deshalb. Das ist kein
 * Fehlerfall: names_ready() meldet dann false, alle Funktionen weisen ab, und
 * die Oberflaeche blendet den Bereich aus.
 */

/* Groessengrenze je Datei. 255 Lampen + 127 Spulen + 127 Schalter + 255 Sounds
 * sind ~764 Eintraege; bei 20 Byte je Zeile passt das mit Reserve in 32 KB. */
#define NAMES_MAX_FILE_SIZE (32 * 1024)
/* Platz fuer "<hw>/<game>.cfg": hw darf 15 Zeichen haben (fa_conn_info_t.hw),
 * dazu '/', die Spielkennung (max. 11, fa_conn_info_t.game), ".cfg" und NUL --
 * zusammen genau 32. Ebenso lang war das alte "<hw>_<game>.cfg", die Groesse
 * bleibt deshalb unveraendert und app_config_t braucht keine neue CFG_VERSION. */
#define NAMES_MAX_NAME      32
#define NAMES_SUFFIX        ".cfg"
/* Puffer fuer die Kennung ohne Endung -- so bemessen, dass Kennung + Endung
 * immer in NAMES_MAX_NAME passt (sonst warnt der Compiler zu Recht). */
#define NAMES_MAX_ID        (NAMES_MAX_NAME - 4)
#define NAMES_MAX_LIST      32   /* Eintraege je names_list_json() */
/* Puffer, den names_list_json() braucht: ein Eintrag ist mit Pfad und dem
 * optionalen "old"-Merker ~56 Byte, dazu der Rahmen. */
#define NAMES_LIST_BUF      (NAMES_MAX_LIST * 56 + 128)

/* Mountet die Partition (formatiert sie beim ersten Mal). Rueckgabe
 * ESP_ERR_NOT_FOUND, wenn keine Partition "names" in der Tabelle steht. */
esp_err_t names_init(void);

/* false = keine Partition; jede andere Funktion hier weist dann ab. */
bool names_ready(void);

/* Aktive Datei, "" wenn keine gewaehlt ist. */
const char *names_active(void);

/* Aktive Datei setzen und in NVS ablegen. Leerer/NULL-Name loescht die Auswahl. */
esp_err_t names_select(const char *file);

/* Kennung der angeschlossenen Anlage, "<HW>/<GAME>", z.B. "AtariFA/002" -- der
 * erwartete Ort der Namensdatei ohne Endung. Leer, wenn keine Verbindung besteht
 * oder die Gegenstelle nichts meldet. Schreibweise bleibt wie gemeldet, sie wird
 * so angezeigt. */
void names_game_id(char *out, size_t len);

/* Waehlt die zur Kennung passende Datei. Gibt es sie nicht, wird die Auswahl
 * GELEERT statt die alte stehenzulassen: sonst zeigte die Oberflaeche nach einem
 * Spielwechsel weiter die Namen des vorigen Spiels, ohne dass es auffaellt.
 * Falsche Beschriftung ist schlimmer als gar keine. */
void names_select_for_id(void);

/* Loeschen. Nimmt als einzige Funktion auch einen flachen Altnamen ohne Ordner
 * an -- sonst liessen sich die Altbestaende nicht mehr entfernen. Bleibt der
 * Geraeteordner leer zurueck, verschwindet er mit. */
esp_err_t names_delete(const char *file);

/* JSON: {"fs":1,"active":"…","files":[{"n":"AtariFA/002.cfg","s":123},…]}
 * Altbestaende im Wurzelverzeichnis tragen zusaetzlich "old":1. */
esp_err_t names_list_json(char *out, size_t out_len);

/* Datei zum Lesen oeffnen; der Aufrufer schliesst sie. */
esp_err_t names_open(const char *file, FILE **fp);

/* Datei anlegen/ersetzen. Prueft Groesse und Zeichenvorrat. */
esp_err_t names_write(const char *file, const char *data, size_t len);

/* Pfad pruefen: genau ein '/', beide Teile nicht leer und aus [A-Za-z0-9._-],
 * Dateiteil mit Endung NAMES_SUFFIX, zusammen < NAMES_MAX_NAME. Weil weder '/'
 * noch ".." durch die Segmentpruefung kommen, ist damit auch ausgeschlossen,
 * dass ein Pfad aus /names herausfuehrt. */
bool names_valid_path(const char *path);

/* ---- Nachladen von lisy.dev ---------------------------------------------- */
/* Fester Pfad, analog zu den Firmware-Images. Nur im STA-Modus sinnvoll -- der
 * Aufrufer weist im AP-Modus vorher ab. */

/* Die Geraeteordner: {"files":["AtariFA/","GottFA1/"]}. Die Schraegstriche
 * stehen so im Listing und bleiben drin; die Oberflaeche schneidet sie ab. */
esp_err_t names_fetch_dev_json(char *out, size_t out_len);

/* Die Dateien in einem Geraeteordner. */
esp_err_t names_fetch_list_json(const char *dev, char *out, size_t out_len);

/* Laedt "<dev>/<datei>.cfg" und legt sie unter demselben Pfad ab. */
esp_err_t names_fetch(const char *path);
