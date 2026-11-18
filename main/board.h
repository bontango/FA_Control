#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"

/*
 * Traeger des Supermini. Seit v1.31 gibt es ein Image fuer alle; board_init()
 * stellt das Board beim Start fest (board_detect() in board.c).
 */
typedef enum {
    BOARD_SOCKET = 0,   /* Sockel auf dem FA-Mainboard: LISY-UART, CTRL_REQ, DIP-Bank */
    BOARD_V7,           /* EP4CE6 dev board v7/v7.1: zusaetzlich EPCS (AS) und JTAG */
    BOARD_JTAG,         /* JTAG-Piggyback an P5: nur JTAG, LISY ueber Virtual JTAG */
} board_type_t;

/* Ergebnis der Erkennung; vor board_init() BOARD_SOCKET. */
board_type_t board_type(void);
/* "socket", "v7" oder "jtag" -- so heisst es auch in /api/status. */
const char *board_name(void);
/* Rohwerte der Erkennung als Klartext (Log, /api/status). */
const char *board_detect_info(void);
/* Das Board erreicht den JTAG-Port des FPGA (v7, JTAG-Piggyback). */
static inline bool board_has_jtag(void)
{
    return board_type() != BOARD_SOCKET;
}

/*
 * Initialisierung der GPIOs. Erkennt zuerst das Board (nur Eingaenge mit
 * internem Pull, nichts wird getrieben), danach je Board: Konfiguriert die Uebernahme-Anforderung als Ausgang
 * (zuerst inaktiv) sowie Taster und DIP-Bank als Eingaenge.
 * Die I2C-Pins werden nur reserviert (kein Treiber-Init).
 */
void board_init(void);

/*
 * Uebernahme-Anforderung an die Flipper-Seite setzen oder zuruecknehmen.
 * true = "ich moechte die Anlage steuern". Die Gegenseite gewaehrt das nur, wenn
 * der Betreiber es dort freigegeben hat, und gibt die Kontrolle sofort zurueck,
 * sobald diese Leitung wieder inaktiv wird (vgl. BOARD_CTRL_ACTIVE_LEVEL).
 */
void board_ctrl_request(bool active);

/* Taster: true = gedrueckt (GPIO liegt auf GND). */
bool board_button_pressed(void);

/* DIP-Bank als 4-Bit-Wert: Bit0=DIP1 .. Bit3=DIP4, ON (=GND) -> 1.
 * Masken dazu: BOARD_DIP_ACTIVE, BOARD_DIP_NO_LED (board_pins.h). */
uint8_t board_dip_read(void);

/*
 * Blaue Onboard-LED (GPIO8). Absichtlich NICHT Teil von board_init(): der Pin
 * ist mit I2C SCL doppelt belegt, deshalb entscheidet power_mgr anhand von DIP2,
 * ob er ueberhaupt als Ausgang konfiguriert wird. Ohne board_led_init() bleibt
 * GPIO8 unangetastet und hochohmig.
 */
void board_led_init(void);
void board_led_set(bool on);

/*
 * Board v7: GPIO8 ist zugleich TMS. Waehrend fpga_jtag den TAP bedient, sperrt
 * es die LED; board_led_set() laesst den Pin dann in Ruhe. Beim Entsperren wird
 * die LED ausgeschaltet (high = TMS-Ruhepegel).
 */
void board_led_lock(bool locked);
