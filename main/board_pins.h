#pragma once

#include "sdkconfig.h"

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

#if CONFIG_FA_BOARD_PIGGYBACK_V7

/* ========================================================================== *
 * EP4CE6 dev board v7 (Piggyback). Das Supermini steckt auf X7 und erreicht
 * ausser LISY/CTRL_REQ auch EPCS, nCONFIG, CONF_DONE und JTAG des FPGA.
 * Massgeblich: N:\Projekte\misc\USB_BLASTER_DIY\ESP32-C3_SUPERMINI_PIGGYBACK.md
 * §4, verdrahtet laut Netzliste EP4CE6_dev_board_v7_0.NET:
 *     GPIO7  -> R3 (LISY TX, vorher CH340-TxD)   GPIO6  <- T2 (LISY RX)
 *     GPIO10 -> N3 (CTRL_REQ)
 *
 * Regeln hinter der Belegung:
 *  - Was das FPGA beim Einschalten treibt (DCLK, nCSO, ASDO, DATA0, CONF_DONE,
 *    TDO), liegt NICHT auf den Strapping-Pins 2/8/9.
 *  - Auf 2/8/9 nur ESP-Ausgaenge in Netze mit externem Pull-Up: nCONFIG (R6),
 *    TMS (R3), TDI (R4). nCONFIG nie auf 8 (LED) oder 9 (BOOT-Taster).
 *  - Alle Programmierleitungen sind im Ruhezustand Eingaenge ohne internen
 *    Pull. Getrieben wird nur, solange fpga_prog arbeitet.
 *  - GPIO20/21 sind die UART0-Pins; die Konsole laeuft hier ueber USB-Serial-
 *    JTAG (sdkconfig.defaults.v7). Das Boot-Log des ROM auf GPIO21 trifft TDO
 *    hochohmig an, R11 (100 Ohm) begrenzt den Sonderfall.
 * ========================================================================== */

/* --- EPCS16 per Active Serial (ESP = SPI-Master, nur bei nCONFIG low) ----- */
#define BOARD_PIN_AS_DCLK      0   /* SPI-SCLK -> H1 / EPCS-6 */
#define BOARD_PIN_AS_ASDO      1   /* SPI-MOSI -> C1 / EPCS-5 */
#define BOARD_PIN_AS_DATA0     3   /* SPI-MISO <- H2 / EPCS-2 */
#define BOARD_PIN_AS_NCSO      4   /* SPI-CS   -> D2 / EPCS-1 */

/* --- Konfiguration ------------------------------------------------------- */
/* nCONFIG: open-drain, R6 10k nach 3,3 V, C14 100 nF nach GND (1 ms). Low haelt
 * das FPGA im Reset und schaltet dessen AS-Pins hochohmig. Strapping-Pin -- R6
 * haelt ihn beim Reset des ESP high. */
#define BOARD_PIN_NCONFIG      2
/* CONF_DONE: Eingang, open-drain vom FPGA, R7 10k nach 3,3 V. */
#define BOARD_PIN_CONF_DONE   20

/* --- JTAG (Stufe 2, bis dahin nur hochohmig) ----------------------------- */
#define BOARD_PIN_JTAG_TCK     5   /* R2 zieht nach GND */
#define BOARD_PIN_JTAG_TMS     8   /* = LED, R3 nach 3,3 V */
#define BOARD_PIN_JTAG_TDI     9   /* = BOOT-Taster, R4 nach 3,3 V */
#define BOARD_PIN_JTAG_TDO    21   /* ueber R11 100 Ohm */

/* --- Blaue Onboard-LED --------------------------------------------------- */
/* Doppelt belegt mit TMS. Ohne TCK-Flanke passiert im TAP nichts, das Blinken
 * ist also harmlos, solange keine JTAG-Operation laeuft. */
#define BOARD_PIN_LED          8
#define BOARD_LED_ACTIVE_LEVEL 0

/* Keine DIP-Bank: board_dip_read() meldet fest "wach, LED an". Den Wert
 * braucht power_mgr, der Rest der Firmware kennt die Bank nicht. */
#define BOARD_DIP_ACTIVE      (1 << 0)
#define BOARD_DIP_NO_LED      (1 << 1)

#else /* Sockel auf dem FA-Mainboard */

/* --- I2C (nur reserviert, kein Treiber-Init) ----------------------------- */
/* Open-Drain mit EXTERNEN Pull-Ups. SCL liegt auf Strapping-Pin GPIO8 —
 * der I2C-Pull-Up haelt ihn beim Boot high, daher boot-sicher. */
#define BOARD_PIN_I2C_SDA     5
#define BOARD_PIN_I2C_SCL     8

/* --- Blaue Onboard-LED (ESP32-C3-Modul) ---------------------------------- */
/* Ausgang, ACTIVE LOW: low = LED an.
 *
 * ACHTUNG, DOPPELBELEGUNG: das ist derselbe Pin wie BOARD_PIN_I2C_SCL. Beides
 * steht hier bewusst nebeneinander -- I2C ist reserviert, aber unbenutzt (kein
 * Treiber-Init), die LED wird dagegen wirklich getrieben. Wird I2C eines Tages
 * in Betrieb genommen, muss die Blinkanzeige ueber Options-DIP 2 (auf ON)
 * abgeschaltet werden; power_mgr laesst GPIO8 dann unkonfiguriert und hochohmig,
 * sodass der I2C-Treiber den Pin sauber uebernehmen kann.
 *
 * GPIO8 ist ausserdem Strapping-Pin und muss beim Reset high liegen. Das ist
 * gegeben: der externe I2C-Pull-Up haelt ihn, und getrieben wird er erst nach
 * dem Boot. Vor dem Tiefschlaf wird er high gehalten (gpio_hold_en). */
#define BOARD_PIN_LED          8
#define BOARD_LED_ACTIVE_LEVEL 0

/* --- Taster -------------------------------------------------------------- */
/* Eingang mit INTERNEM Pull-Up. GPIO9 = BOOT-Pin (high beim Boot).
 * Hinweis: Taster WAEHREND eines Resets gehalten -> Download-Modus. */
#define BOARD_PIN_BUTTON      9

/* --- 4er-DIP-Bank (Optionen) --------------------------------------------- */
/* Vier Eingaenge mit INTERNEM Pull-Up, ON = GND = low.
 * DIP3 war auf GPIO3 eingetragen; laut Schaltplan fuehrt die Stiftleiste K11
 * GPIO0/1/2/4, GPIO3 ist dort <nc>. Korrigiert auf GPIO2 (Strapping-Pin, wird
 * vom internen Pull-Up beim Boot high gehalten).
 *
 * Belegung seit v1.12 (vorher hatte die Bank keine Funktion):
 *   DIP1 = Ein/Aus. ON  -> wach: WLAN, Webserver, Steuerung.
 *                   OFF -> Tiefschlaf, Aufwecken durch Umlegen auf ON.
 *   DIP2 = Blinkanzeige. ON -> aus, GPIO8 bleibt frei fuer I2C (siehe oben).
 *   DIP3, DIP4 = frei.
 *
 * DIP1 muss auf GPIO0..GPIO5 liegen: nur diese Pins koennen den ESP32-C3 aus dem
 * Tiefschlaf wecken (SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK). Beim Umlegen der
 * Bank also darauf achten. */
#define BOARD_PIN_DIP1        0
#define BOARD_PIN_DIP2        1
#define BOARD_PIN_DIP3        2
#define BOARD_PIN_DIP4        4

/* Bitmasken zu board_dip_read() -- Bit0 = DIP1. */
#define BOARD_DIP_ACTIVE      (1 << 0)   /* DIP1 ON: wach statt Tiefschlaf */
#define BOARD_DIP_NO_LED      (1 << 1)   /* DIP2 ON: Blinkanzeige abgeschaltet */

/* --- Reserve ------------------------------------------------------------- */
/* GPIO2 (Strapping) bewusst freigehalten fuer kuenftige Erweiterungen. */
#define BOARD_PIN_RESERVED    2

#endif /* CONFIG_FA_BOARD_PIGGYBACK_V7 */
