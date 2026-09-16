#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

/*
 * Spiel-ROMs fuer die FA-Boards -- das FPGA laedt sie beim Booten von hier statt
 * von seiner SD-Karte. Zuerst umgesetzt in SternFA (PCB v2.00, 5.0.6), gedacht
 * fuer jedes FA-Board mit ESP32-Sockel.
 *
 * ABLAGE. Eigene LittleFS-Partition "roms", gemountet unter /roms, geordnet wie
 * die Namensdateien und wie die Ablage auf lisy.dev:
 *
 *     /roms/<HW>/<nnn>.bin        lisy.dev: roms/<HW>/<nnn>[_<Titel>].bin
 *
 * <HW> ist die Kennung, die das Board meldet (LISY-Opcode 0, im FPGA HW_NAME),
 * <nnn> die Spielnummer dreistellig -- der SD-Index, die Zahl im Bootbild. Ein
 * Titel im Dateinamen auf lisy.dev ist nur fuer Menschen; abgelegt wird ohne.
 *
 * Hochgeladen bzw. geladen wird der Spielplatz genau so, wie er auf die SD-Karte
 * geht: ein Vielfaches von 512 Byte, hoechstens 64 kB. Hat er genau 64 kB,
 * steht an 0xFFFE/0xFFFF (big endian) eine CRC16 ueber die ersten 32 kB, und die
 * muss stimmen (SternFA, WillFA7S). Kleinere Plaetze haben kein CRC-Feld
 * (WillFA7, 12 kB) und werden ungeprueft angenommen.
 *
 * Gespeichert wird ohne das Fuellbyte am Ende: 8 Byte Kopf ('F','A','R',1,
 * Fuellbyte, 0, Datenlaenge big endian) und die Daten bis zum letzten Byte, das
 * nicht Fuellbyte ist. Ein SternFA-Spiel belegt so ~8 kB statt 64.
 *
 * BOOT-PROTOKOLL (115200 8N1 auf der LISY-UART, AUSSERHALB von LISY).
 * Gegenstelle: rtl/fa_control/esp_rom_loader.vhd in den FA-Projekten.
 *
 *   FPGA -> ESP   A5 5A 52 <len> <hw, len Byte> <spiel> <sektoren>
 *                 wiederholt alle 250 ms, hoechstens 3 s
 *   ESP -> FPGA   A5 5A 4E                kein ROM -> das Board nimmt die SD-Karte
 *                 A5 5A 44 <sektoren*512 Byte> <crc_hi> <crc_lo>
 *
 * Gesendet werden die gespeicherten Daten, ab dort das Fuellbyte bis zur
 * angefragten Laenge. Die CRC16 (CCITT-FALSE, wie rtl/common/crc16_ccitt.vhd)
 * laeuft ueber genau diese Bytes und schuetzt nur die Uebertragung.
 *
 * Die Antwort geht nur, solange der ESP wach ist (DIP1 = ON); sonst wartet das
 * FPGA 3 s und nimmt die SD-Karte. Fehlt die Partition (Geraet nur per OTA
 * aktualisiert), antwortet der ESP sofort mit 'N'.
 */

#define ROM_BOOT_MAX_IMAGE   65536
#define ROM_BOOT_CRC_SPAN    32768
#define ROM_BOOT_MAX_HW      15      /* wie fa_conn_info_t.hw ohne NUL */
#define ROM_BOOT_MAX_ID      24      /* "<hw>/<nnn>" + NUL */
#define ROM_BOOT_MAX_LIST    64
#define ROM_BOOT_LIST_BUF    (ROM_BOOT_MAX_LIST * 48 + 160)

/* Partition mounten (formatiert sie beim ersten Mal). ESP_ERR_NOT_FOUND, wenn es
 * keine Partition "roms" gibt -- dann bleibt alles hier abgeschaltet. */
esp_err_t rom_boot_init(void);
bool rom_boot_ready(void);

/* Startet die Boot-Antwort. Nach lisy_init() und rom_boot_init(), und moeglichst
 * frueh -- das FPGA fragt nur die ersten 3 s. */
esp_err_t rom_boot_start(void);

/* CRC16-CCITT, Polynom 0x1021, nicht reflektiert; crc = 0xFFFF fuer den Anfang. */
uint16_t rom_boot_crc16(uint16_t crc, const uint8_t *data, size_t len);

/* "<hw>/<nnn>" zerlegen und pruefen. game = 0..255. */
bool rom_parse_id(const char *id, char *hw, size_t hw_len, int *game);

/* Hochladen und Nachladen laufen ueber eine Zwischendatei, damit kein
 * 64-kB-Puffer im RAM stehen muss: oeffnen, roh hineinschreiben, schliessen,
 * dann rom_import_tmp(). Die Zwischendatei verschwindet in jedem Fall. */
FILE *rom_tmp_open(void);
void rom_tmp_discard(void);

/* Prueft die Zwischendatei und legt sie als <hw>/<game> ab.
 * ESP_ERR_INVALID_SIZE  keine ganze Zahl von Sektoren oder mehr als 64 kB
 * ESP_ERR_INVALID_CRC   64-kB-Platz mit falscher Pruefsumme
 * ESP_ERR_INVALID_ARG   ungueltige Kennung
 * ESP_ERR_INVALID_STATE keine Partition
 * ESP_FAIL              Schreiben fehlgeschlagen (voll?) */
esp_err_t rom_import_tmp(const char *hw, int game);

esp_err_t rom_delete(const char *id);

/* {"fs":1,"roms":[{"n":"SternFA/012","s":8192},…],"total":…,"used":…,
 *  "last":{"hw":"SternFA","g":12,"r":"D","ms":1234}}
 * "last" = letzte Boot-Anfrage seit dem Start des ESP ("D" geliefert, "N" keins). */
esp_err_t rom_list_json(char *out, size_t len);

/* ---- Nachladen von lisy.dev (nur STA-Modus, der Aufrufer prueft) ---------- */
esp_err_t rom_fetch_dev_json(char *out, size_t len);                   /* Geraeteordner */
esp_err_t rom_fetch_list_json(const char *dev, char *out, size_t len); /* .bin darin */
/* "<dev>/<nnn>[_Titel].bin" laden und als <dev>/<nnn> ablegen. Fehler wie
 * rom_import_tmp(); ESP_ERR_INVALID_ARG auch fuer einen Namen ohne Spielnummer. */
esp_err_t rom_fetch(const char *path);
