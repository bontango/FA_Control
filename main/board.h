#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"

/*
 * Reservierung/Initialisierung der GPIOs fuer kuenftige Erweiterungen.
 * Konfiguriert Status-Ausgang sowie Taster und DIP-Bank als Eingaenge.
 * Die I2C-Pins werden nur reserviert (kein Treiber-Init).
 */
void board_init(void);

/* Status-Ausgang setzen (active = logisch aktiv, vgl. BOARD_STATUS_ACTIVE_LEVEL). */
void board_status_set(bool active);

/* Taster: true = gedrueckt (GPIO liegt auf GND). */
bool board_button_pressed(void);

/* DIP-Bank als 4-Bit-Wert: Bit0=DIP1 .. Bit3=DIP4, ON (=GND) -> 1. */
uint8_t board_dip_read(void);
