#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* LISY-Protokoll — Befehlsbytes.
 * Referenz ist N:\Projekte\lisy_5_28\src\lisy\lisy_api.h (dort dezimal notiert).
 * Die Info-Gruppe 0..9 ist der Connect-Handshake: damit erfaehrt FA_Control die
 * tatsaechliche Bestueckung der Anlage, statt sie vom Benutzer eingetragen zu
 * bekommen. Gegenstelle: rtl/fa_control/fa_control.vhd im jeweiligen FPGA-Projekt. */
#define LISY_CMD_G_HW           0x00  /* -> String  Hardware-Kennung, z.B. "AtariFA" */
#define LISY_CMD_G_LISY_VER     0x01  /* -> String  Firmware-Version der Gegenstelle */
#define LISY_CMD_G_API_VER      0x02  /* -> String  Protokoll-Version */
#define LISY_CMD_G_NO_LAMPS     0x03  /* -> Byte */
#define LISY_CMD_G_NO_SOL       0x04  /* -> Byte */
#define LISY_CMD_G_NO_SOUNDS    0x05  /* -> Byte */
#define LISY_CMD_G_NO_DISP      0x06  /* -> Byte */
#define LISY_CMD_G_DISP_DETAIL  0x07  /* Nr -> 2 Bytes: Typ, Stellen */
#define LISY_CMD_G_GAME_INFO    0x08  /* -> String */
#define LISY_CMD_G_NO_SW        0x09  /* -> Byte */

#define LISY_CMD_LAMP_ON        0x0B
#define LISY_CMD_LAMP_OFF       0x0C
#define LISY_CMD_COIL_PULSE     0x17
#define LISY_CMD_COIL_PULSETIME 0x18
#define LISY_CMD_DISPLAY_BASE   0x1E  /* 0x1E + Display-Index (0-6) */
#define LISY_CMD_SWITCH_GET     0x28
#define LISY_CMD_SWITCH_CHANGED 0x29
#define LISY_CMD_SOUND_PLAY     0x32
#define LISY_CMD_SOUND_STOP     0x33
#define LISY_CMD_INIT_RESET     0x64
#define LISY_CMD_WATCHDOG       0x65

/* Antwortcodes auf LISY_CMD_INIT_RESET. 0 = OK ist Protokoll; die Fehlernummern
 * legt die Gegenstelle fest (rtl/fa_control/fa_control.vhd). */
#define LISY_INIT_OK            0   /* Kontrolle gewaehrt */
#define LISY_INIT_NO_ALLOW      1   /* verweigert: Freigabeschalter steht auf OFF */
#define LISY_INIT_NO_REQUEST    2   /* verweigert: Anforderungsleitung nicht gesetzt */

#define LISY_SWITCH_BITMAP_LEN  16  /* 127 Schalter -> 16 Byte */
#define LISY_LAMP_BITMAP_LEN    32  /* 255 Lampen  -> 32 Byte */

esp_err_t lisy_init(void);

/* Lampen (Zustand wird lokal gespiegelt) */
void lisy_lamp_set(uint8_t idx, bool on);
const uint8_t *lisy_lamp_bitmap(void);
void lisy_lamp_bitmap_clear(void);

/* Spulen -- Nummern 1-basiert (LISY-Konvention), anders als Lampen/Sounds */
void lisy_coil_pulse(uint8_t no);
void lisy_coil_apply_pulse_time(uint8_t count, uint8_t ms);

/* Sound */
void lisy_sound_play(uint8_t track, uint8_t idx);
void lisy_sound_stop(uint8_t track);

/* Displays: Text wird BCD7-kodiert, rechtsbuendig, blank = 0x0F */
void lisy_display_set(uint8_t d, const char *text, uint8_t width);

/* Schalter: Cache per 0x28 fuellen, per 0x29 aktualisieren */
void lisy_switches_refresh_all(uint8_t count);
void lisy_switches_drain_changes(void);
const uint8_t *lisy_switch_bitmap(void);

/* Init/Reset: Rueckgabe 0=OK, sonst Fehlercode, -1 = keine Antwort */
int lisy_init_reset(void);

/* ---- Abfragen (Info-Gruppe 0..9) ---------------------------------------- */
/* Alle drei geben bei ausbleibender Antwort einen Fehler zurueck; die Gegenstelle
 * darf dann als "nicht vorhanden" gelten. */

/* Ein Antwortbyte holen. Rueckgabe: 0..255 oder -1 bei Timeout. */
int lisy_get_byte(uint8_t cmd);

/* Ein Antwortbyte zu einem Befehl MIT Parameter. -1 bei Timeout. */
int lisy_get_byte_p(uint8_t cmd, uint8_t param);

/* Zwei Antwortbytes zu einem Befehl mit Parameter (z.B. Display-Details).
 * Rueckgabe 0 = OK, -1 = Timeout. */
int lisy_get_2bytes(uint8_t cmd, uint8_t param, uint8_t *b1, uint8_t *b2);

/* NUL-terminierten String holen. Rueckgabe = Laenge ohne NUL, -1 bei Timeout.
 * Nicht druckbare Zeichen werden verworfen, out ist immer terminiert. */
int lisy_get_string(uint8_t cmd, char *out, size_t len);

/* ---- Direkter Buszugriff (rom_boot.c) ------------------------------------ */
/* Fuer den SternFA-Boot-Lader, der AUSSERHALB von LISY auf derselben UART
 * spricht. Nehmen, lesen/schreiben, zurueckgeben -- dazwischen sendet keine
 * LISY-Funktion. lisy_bus_take() gibt false zurueck, wenn der Bus innerhalb
 * von timeout_ms nicht frei wurde. */
bool lisy_bus_take(uint32_t timeout_ms);
void lisy_bus_give(void);
/* Bytes im Empfangspuffer -- ohne den Bus zu nehmen, zum Nachsehen. */
size_t lisy_bus_available(void);
/* Rueckgabe: Anzahl gelesener Bytes (0 bei Timeout). */
int lisy_bus_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
void lisy_bus_write(const uint8_t *buf, size_t len);
/* Wartet, bis alles Gesendete draussen ist, und verwirft dann den Empfang. */
void lisy_bus_drain(void);

/* Watchdog (0x65 alle 500 ms per esp_timer) */
void lisy_watchdog_enable(bool en);
bool lisy_watchdog_enabled(void);
int lisy_watchdog_last_result(void);
