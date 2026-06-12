#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* LISY-Protokoll v0.08 — Befehlsbytes */
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

#define LISY_SWITCH_BITMAP_LEN  16  /* 127 Schalter -> 16 Byte */
#define LISY_LAMP_BITMAP_LEN    32  /* 255 Lampen  -> 32 Byte */

esp_err_t lisy_init(void);

/* Lampen (Zustand wird lokal gespiegelt) */
void lisy_lamp_set(uint8_t idx, bool on);
const uint8_t *lisy_lamp_bitmap(void);
void lisy_lamp_bitmap_clear(void);

/* Spulen */
void lisy_coil_pulse(uint8_t idx);
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

/* Watchdog (0x65 alle 500 ms per esp_timer) */
void lisy_watchdog_enable(bool en);
bool lisy_watchdog_enabled(void);
int lisy_watchdog_last_result(void);
