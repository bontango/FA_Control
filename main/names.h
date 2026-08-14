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
 * Der Dateiname ist die Kennung der Anlage: "<HW>_<GAME>.cfg", GAME dreistellig
 * mit fuehrenden Nullen -- Airborne Avenger auf AtariFA ist "AtariFA_002.cfg".
 * Die Spielnummer allein reicht nicht: sie faengt auf jeder Platine wieder bei 0
 * an, und FA_Control bedient mehrere (AtariFA, GottFA1, ...). Die
 * Hardware-Kennung aus LISY-Opcode 0x00 macht sie eindeutig. Gross- und
 * Kleinschreibung spielt beim Suchen keine Rolle.
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
/* Platz fuer "<hw>_<game>.cfg": hw darf 15 Zeichen haben (fa_conn_info_t.hw),
 * dazu '_', die Spielkennung, ".cfg" und NUL. Muss zu app_config_t passen. */
#define NAMES_MAX_NAME      32
#define NAMES_SUFFIX        ".cfg"
/* Puffer fuer die Kennung ohne Endung -- so bemessen, dass Kennung + Endung
 * immer in NAMES_MAX_NAME passt (sonst warnt der Compiler zu Recht). */
#define NAMES_MAX_ID        (NAMES_MAX_NAME - 4)
#define NAMES_MAX_LIST      32   /* Eintraege je names_list_json() */
/* Puffer, den names_list_json() braucht: ~45 Byte je Eintrag plus Rahmen. */
#define NAMES_LIST_BUF      (NAMES_MAX_LIST * 48 + 128)

/* Mountet die Partition (formatiert sie beim ersten Mal). Rueckgabe
 * ESP_ERR_NOT_FOUND, wenn keine Partition "names" in der Tabelle steht. */
esp_err_t names_init(void);

/* false = keine Partition; jede andere Funktion hier weist dann ab. */
bool names_ready(void);

/* Aktive Datei, "" wenn keine gewaehlt ist. */
const char *names_active(void);

/* Aktive Datei setzen und in NVS ablegen. Leerer/NULL-Name loescht die Auswahl. */
esp_err_t names_select(const char *file);

/* Kennung der angeschlossenen Anlage, "<HW>_<GAME>", z.B. "AtariFA_002".
 * Leer, wenn keine Verbindung besteht oder die Gegenstelle nichts meldet.
 * Schreibweise bleibt wie gemeldet -- sie wird so angezeigt. */
void names_game_id(char *out, size_t len);

/* Waehlt die zur Kennung passende Datei. Gibt es sie nicht, wird die Auswahl
 * GELEERT statt die alte stehenzulassen: sonst zeigte die Oberflaeche nach einem
 * Spielwechsel weiter die Namen des vorigen Spiels, ohne dass es auffaellt.
 * Falsche Beschriftung ist schlimmer als gar keine. */
void names_select_for_id(void);

esp_err_t names_delete(const char *file);

/* JSON: {"fs":1,"active":"…","files":[{"n":"…","s":123},…]} */
esp_err_t names_list_json(char *out, size_t out_len);

/* Datei zum Lesen oeffnen; der Aufrufer schliesst sie. */
esp_err_t names_open(const char *file, FILE **fp);

/* Datei anlegen/ersetzen. Prueft Groesse und Zeichenvorrat. */
esp_err_t names_write(const char *file, const char *data, size_t len);

/* Dateinamen pruefen: < NAMES_MAX_NAME, Endung NAMES_SUFFIX, nur [A-Za-z0-9._-]. */
bool names_valid_filename(const char *file);

/* ---- Nachladen von lisy.dev ---------------------------------------------- */
/* Fester Pfad, analog zu den Firmware-Images. Nur im STA-Modus sinnvoll -- der
 * Aufrufer weist im AP-Modus vorher ab. */

esp_err_t names_fetch_list_json(char *out, size_t out_len);
esp_err_t names_fetch(const char *file);
