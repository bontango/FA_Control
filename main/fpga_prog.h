#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * FPGA-Programmierung auf dem EP4CE6 dev board v7 -- nur in dieser Variante
 * gebaut (CONFIG_FA_BOARD_PIGGYBACK_V7).
 *
 * WEG. Active Serial direkt: der ESP zieht nCONFIG auf low, das FPGA geht in den
 * Reset und schaltet seine AS-Pins hochohmig, der ESP beschreibt das EPCS als
 * gewoehnlicher SPI-Master und laesst nCONFIG wieder los. Das FPGA bootet dann
 * aus dem neuen Image; CONF_DONE muss innerhalb von FPGA_CONF_DONE_MS high werden.
 *
 * KEIN STAGING. Seit x.30 legen SternFA, BallyFA und WillFA7 ihre Spiel-ROMs ab
 * 0x080000 im EPCS ab -- genau dort, wo die Evaluierung Staging und Rollback
 * vorgesehen hatte. Geschrieben wird deshalb direkt ab 0, geloescht nur die
 * 64-kB-Sektoren, die das Image beruehrt, und ein Image ueber FPGA_IMAGE_MAX
 * wird abgewiesen. Alles ab 0x080000 bleibt unberuehrt.
 * Bricht ein Upload ab, bootet das FPGA nicht -- ein Ziegelstein ist es trotzdem
 * nicht, der ESP laeuft unabhaengig davon und brennt beim naechsten Versuch neu.
 *
 * BITREIHENFOLGE. Ob die Bytes einer .rbf fuer das EPCS gedreht werden muessen,
 * klaert Stufe 0 am Geraet (Dump eines mit Quartus gebrannten EPCS gegen die
 * .rbf). Bis dahin ist es ein Parameter: rev = true dreht jedes Byte (Bit 0 <->
 * Bit 7), bevor es geschrieben wird.
 *
 * Jeder Zugriff auf das EPCS haelt das FPGA im Reset und beendet damit das
 * laufende Spiel samt einer FA-Control-Sitzung. fpga_prog_info() liest deshalb
 * nur CONF_DONE und das zuletzt Ermittelte; die Kennung holt fpga_prog_probe().
 *
 * Alle Funktionen laufen im Task des Aufrufers (httpd) und blockieren.
 */

/* Obergrenze fuer ein Image: Beginn des ROM-Bereichs der FA-Designs. */
#define FPGA_IMAGE_MAX      0x80000
#define FPGA_CONF_DONE_MS   2000
#define FPGA_MSG_LEN        96

typedef struct {
    bool conf_done;          /* live gelesen */
    bool busy;               /* ein Schreibvorgang ist offen */
    int id_ab;               /* 0xAB Silicon ID (EPCS16 = 0x14), -1 = unbekannt */
    uint32_t jedec;          /* 0x9F, 3 Byte; 0 = unbekannt, 0xFFFFFF = keine Antwort */
    uint32_t size;           /* erkannte Groesse in Byte, 0 = unbekannt */
    char last[FPGA_MSG_LEN]; /* letztes Ergebnis, englisch (geht in die Oberflaeche) */
} fpga_info_t;

void fpga_prog_init(void);
void fpga_prog_info(fpga_info_t *out);

/* nCONFIG halten, Kennung lesen, loslassen, auf CONF_DONE warten.
 * ESP_ERR_NOT_FOUND = kein EPCS erkannt. */
esp_err_t fpga_prog_probe(void);

/* Nur ein nCONFIG-Puls: das FPGA laedt sein Image neu. ESP_ERR_TIMEOUT, wenn
 * CONF_DONE nicht kommt. */
esp_err_t fpga_prog_reconfig(void);

/* EPCS roh lesen (ohne Bitdrehung), stueckweise an sink. Das FPGA steht dabei
 * im Reset und startet danach neu. */
typedef esp_err_t (*fpga_sink_t)(void *ctx, const uint8_t *data, size_t len);
esp_err_t fpga_prog_read(uint32_t addr, uint32_t len, fpga_sink_t sink, void *ctx);

/* Schreiben in drei Schritten. begin prueft die Groesse (ESP_ERR_INVALID_SIZE),
 * haelt nCONFIG und liest die Kennung; chunk nimmt beliebig grosse Stuecke und
 * loescht jeden Sektor beim ersten Betreten; end prueft die Laenge, liest das
 * Geschriebene zurueck (SHA-256, ESP_ERR_INVALID_CRC bei Abweichung), laesst
 * nCONFIG los und wartet auf CONF_DONE (ESP_ERR_TIMEOUT).
 * abort gibt nach einem abgebrochenen Upload alles frei. Nach einem Fehler aus
 * chunk ist abort Pflicht, end und abort raeumen in jedem Fall auf. */
esp_err_t fpga_prog_write_begin(uint32_t size, bool rev);
esp_err_t fpga_prog_write_chunk(const uint8_t *data, size_t len);
esp_err_t fpga_prog_write_end(void);
void fpga_prog_write_abort(void);

/* Stufe-0-Diagnose als JSON nach out (~600 Byte): Pegel/Flanken der AS-Leitungen
 * im Ruhezustand, bei gehaltenem nCONFIG und danach, dazu 0xAB/0x05 per Bit-Bang.
 * Setzt das FPGA zurueck. */
esp_err_t fpga_prog_diag(char *out, size_t len);
