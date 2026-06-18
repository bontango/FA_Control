#pragma once

/*
 * Zentrale GPIO-Zuordnung (ESP32-C3) — "single source of truth".
 *
 * ESP32-C3 Randbedingungen:
 *  - Nicht nutzbar: GPIO11-17 (SPI-Flash), GPIO18/19 (USB-Serial/JTAG),
 *    GPIO20/21 (UART0-Konsole).
 *  - Strapping-Pins (beim Reset nur "high" zulaessig): GPIO2, GPIO8, GPIO9.
 *    Dort nur Signale, die per Pull-Up beim Boot sicher high liegen.
 *
 * Logikpegel:
 *  - Taster/DIP mit internem Pull-Up -> gedrueckt/ON = GND (low).
 *  - Status-Ausgang mit externem Pull-Up -> Ruhe = high, "aktiv" = treiben
 *    (siehe BOARD_STATUS_ACTIVE_LEVEL).
 */

/* --- LISY UART1 (unveraendert) ------------------------------------------- */
#define BOARD_PIN_LISY_TX     6   /* Ausgang */
#define BOARD_PIN_LISY_RX     7   /* Eingang */

/* --- Statussignal zum Flipper -------------------------------------------- */
/* Ausgang mit EXTERNEM Pull-Up (non-strapping). */
#define BOARD_PIN_STATUS      10
/* Aktiv-Pegel des Status-Ausgangs (0 = active-low, passend zu ext. Pull-Up). */
#define BOARD_STATUS_ACTIVE_LEVEL 0

/* --- I2C (nur reserviert, kein Treiber-Init) ----------------------------- */
/* Open-Drain mit EXTERNEN Pull-Ups. SCL liegt auf Strapping-Pin GPIO8 —
 * der I2C-Pull-Up haelt ihn beim Boot high, daher boot-sicher. */
#define BOARD_PIN_I2C_SDA     5
#define BOARD_PIN_I2C_SCL     8

/* --- Taster -------------------------------------------------------------- */
/* Eingang mit INTERNEM Pull-Up. GPIO9 = BOOT-Pin (high beim Boot).
 * Hinweis: Taster WAEHREND eines Resets gehalten -> Download-Modus. */
#define BOARD_PIN_BUTTON      9

/* --- 4er-DIP-Bank (Optionen) --------------------------------------------- */
/* Vier Eingaenge mit INTERNEM Pull-Up (alle non-strapping). */
#define BOARD_PIN_DIP1        0
#define BOARD_PIN_DIP2        1
#define BOARD_PIN_DIP3        3
#define BOARD_PIN_DIP4        4

/* --- Reserve ------------------------------------------------------------- */
/* GPIO2 (Strapping) bewusst freigehalten fuer kuenftige Erweiterungen. */
#define BOARD_PIN_RESERVED    2
