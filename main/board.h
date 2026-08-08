#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"

/*
 * Initialisierung der GPIOs. Konfiguriert die Uebernahme-Anforderung als Ausgang
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

/* DIP-Bank als 4-Bit-Wert: Bit0=DIP1 .. Bit3=DIP4, ON (=GND) -> 1. */
uint8_t board_dip_read(void);
