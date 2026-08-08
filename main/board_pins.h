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

/* --- LISY UART1 ----------------------------------------------------------- */
/* Korrigiert 08.2026: TX und RX waren vertauscht. Massgeblich ist der
 * AtariFA-Schaltplan (N:\Projekte\FPGA Atari\doc\AtariFA_07_Final_Main_SCH.PDF),
 * Steckverbinder X7 <-> X1P:
 *     GPIO7 = Netz ESP32_TX    -> FPGA PIN_33 (dort ein Eingang) => hier TX
 *     GPIO6 = Netz ESP32_RX    -> FPGA PIN_44 (dort ein Ausgang) => hier RX
 * Mit der alten Belegung haetten beide Seiten auf PIN_44 gegeneinander gesendet.
 * Die Firmware hatte nie eine Gegenstelle, deshalb ist es nie aufgefallen. */
#define BOARD_PIN_LISY_TX     7   /* Ausgang */
#define BOARD_PIN_LISY_RX     6   /* Eingang */

/* --- Uebernahmewunsch an den Flipper -------------------------------------- */
/* Ausgang, active low. Netz ESP32_IO10 -> FPGA PIN_11. Solange dieser Pin
 * aktiv (= low) ist, darf FA_Control die Anlage steuern -- sofern der Betreiber
 * es dort freigegeben hat (bei AtariFA: Options-DIP 4 auf ON). Wird der Pin
 * inaktiv, gibt die Gegenstelle die Kontrolle sofort zurueck ans Spiel.
 * Das FPGA hat einen Weak-Pull-Up: kein ESP gesteckt = keine Anforderung. */
#define BOARD_PIN_CTRL_REQ    10
/* Aktiv-Pegel des Ausgangs (0 = active-low, passend zum Pull-Up der Gegenseite). */
#define BOARD_CTRL_ACTIVE_LEVEL 0

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
/* Vier Eingaenge mit INTERNEM Pull-Up (alle non-strapping).
 * DIP3 war auf GPIO3 eingetragen; laut Schaltplan fuehrt die Stiftleiste K11
 * GPIO0/1/2/4, GPIO3 ist dort <nc>. Korrigiert auf GPIO2 (Strapping-Pin, wird
 * vom internen Pull-Up beim Boot high gehalten). Die Bank hat ohnehin noch
 * keine Funktion -- board_dip_read() hat bis heute keinen Aufrufer. */
#define BOARD_PIN_DIP1        0
#define BOARD_PIN_DIP2        1
#define BOARD_PIN_DIP3        2
#define BOARD_PIN_DIP4        4

/* --- Reserve ------------------------------------------------------------- */
/* GPIO2 (Strapping) bewusst freigehalten fuer kuenftige Erweiterungen. */
#define BOARD_PIN_RESERVED    2
