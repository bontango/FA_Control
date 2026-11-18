#pragma once

#include "sdkconfig.h"

/*
 * Zentrale GPIO-Zuordnung (ESP32-C3) — "single source of truth".
 *
 * Seit v1.31 gibt es EIN Image fuer alle Traeger. Welches Board es ist, stellt
 * board_detect() beim Start fest (board.c, Signaturen dort beschrieben); die
 * drei Belegungen stehen deshalb nebeneinander. Was nur auf einem Board
 * existiert, traegt dessen Praefix oder steht in dessen Abschnitt; der Code
 * fasst es nur an, wenn board_type() passt.
 *
 * ESP32-C3 Randbedingungen:
 *  - Nicht nutzbar: GPIO11-17 (SPI-Flash), GPIO18/19 (USB-Serial/JTAG).
 *  - Strapping-Pins (beim Reset nur "high" zulaessig): GPIO2, GPIO8, GPIO9.
 *    Dort nur Signale, die per Pull-Up beim Boot sicher high liegen.
 *  - GPIO20/21 (UART0) sind auf keinem Board die Konsole: die laeuft seit v1.31
 *    ueberall ueber USB-Serial-JTAG (sdkconfig.defaults).
 *
 * Logikpegel:
 *  - Taster/DIP mit internem Pull-Up -> gedrueckt/ON = GND (low).
 */

/* ========================================================================== *
 * Gemeinsam
 * ========================================================================== */

/* Blaue Onboard-LED des Supermini, ACTIVE LOW. Sockel: zugleich I2C SCL
 * (reserviert). v7: zugleich TMS (fpga_jtag sperrt die LED waehrend eines
 * Zugriffs, board_led_lock). */
#define BOARD_PIN_LED          8
#define BOARD_LED_ACTIVE_LEVEL 0

/* BOOT-Taster des Supermini (Sockel, JTAG-Piggyback). Auf v7 ist GPIO9 TDI,
 * board_button_pressed() meldet dort immer false. */
#define BOARD_PIN_BUTTON       9

/* Bitmasken zu board_dip_read() -- Bit0 = DIP1. Boards ohne DIP-Bank melden
 * fest BOARD_DIP_ACTIVE ("wach, LED an"). */
#define BOARD_DIP_ACTIVE      (1 << 0)   /* DIP1 ON: wach statt Tiefschlaf */
#define BOARD_DIP_NO_LED      (1 << 1)   /* DIP2 ON: Blinkanzeige abgeschaltet */

/* ========================================================================== *
 * Sockel und Board v7: LISY-UART und CTRL_REQ als eigene Leitungen
 * (JTAG-Piggyback: beides ueber Virtual JTAG, GPIO6/7/10 sind dort TCK/TDI/TMS)
 * ========================================================================== */

/* Korrigiert 08.2026: TX und RX waren vertauscht. Massgeblich ist der
 * AtariFA-Schaltplan (N:\Projekte\FPGA Atari\doc\AtariFA_07_Final_Main_SCH.PDF),
 * Steckverbinder X7 <-> X1P:
 *     GPIO7 = Netz ESP32_TX    -> FPGA PIN_33 (dort ein Eingang) => hier TX
 *     GPIO6 = Netz ESP32_RX    -> FPGA PIN_44 (dort ein Ausgang) => hier RX
 * v7: GPIO7 -> R3 (vorher CH340-TxD), GPIO6 <- T2. */
#define BOARD_PIN_LISY_TX     7   /* Ausgang */
#define BOARD_PIN_LISY_RX     6   /* Eingang */

/* Uebernahmewunsch an den Flipper. Ausgang, active low. Netz ESP32_IO10 ->
 * FPGA PIN_11 (v7: N3). Solange dieser Pin aktiv (= low) ist, darf FA_Control
 * die Anlage steuern -- sofern der Betreiber es dort freigegeben hat. Das FPGA
 * hat einen Weak-Pull-Up: kein ESP gesteckt = keine Anforderung. */
#define BOARD_PIN_CTRL_REQ    10
#define BOARD_CTRL_ACTIVE_LEVEL 0

/* ========================================================================== *
 * Sockel auf dem FA-Mainboard (AtariFA, SternFA 2.0 u. a.)
 * ========================================================================== */

/* I2C (nur reserviert, kein Treiber-Init). Open-Drain mit EXTERNEN Pull-Ups
 * 4,7k (SternFA 2.0: R102/R103). SCL = GPIO8 = LED: solange I2C keinen
 * Treiber-Init hat, gehoert der Pin der Blinkanzeige; wer I2C in Betrieb nimmt,
 * legt DIP2 auf ON, dann fasst power_mgr GPIO8 nicht an.
 * Der Pull-up an SDA ist zugleich ein Merkmal der Board-Erkennung (nur Log). */
#define BOARD_PIN_I2C_SDA     5
#define BOARD_PIN_I2C_SCL     8

/* 4er-DIP-Bank: Eingaenge mit INTERNEM Pull-Up, ON = GND = low.
 * DIP3 war auf GPIO3 eingetragen; laut Schaltplan fuehrt die Stiftleiste K11
 * GPIO0/1/2/4, GPIO3 ist dort <nc>. Korrigiert auf GPIO2.
 *   DIP1 = Ein/Aus. ON -> wach, OFF -> Tiefschlaf (Aufwecken durch ON).
 *   DIP2 = Blinkanzeige. ON -> aus, GPIO8 bleibt frei fuer I2C.
 *   DIP3, DIP4 = frei.
 * DIP1 muss auf GPIO0..GPIO5 liegen: nur diese Pins wecken den C3 aus dem
 * Tiefschlaf (SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK). */
#define BOARD_PIN_DIP1        0
#define BOARD_PIN_DIP2        1
#define BOARD_PIN_DIP3        2
#define BOARD_PIN_DIP4        4

/* GPIO2 (Strapping) bewusst freigehalten fuer kuenftige Erweiterungen. */
#define BOARD_PIN_RESERVED    2

/* ========================================================================== *
 * EP4CE6 dev board v7 / v7.1 (Supermini fest auf der FPGA-Piggyback)
 * Massgeblich: N:\Projekte\misc\USB_BLASTER_DIY\ESP32-C3_SUPERMINI_PIGGYBACK.md
 * §4, verdrahtet laut Netzliste EP4CE6_dev_board_v7_0.NET.
 *
 * Regeln hinter der Belegung:
 *  - Was das FPGA beim Einschalten treibt (DCLK, nCSO, ASDO, DATA0, CONF_DONE,
 *    TDO), liegt NICHT auf den Strapping-Pins 2/8/9.
 *  - Auf 2/8/9 nur ESP-Ausgaenge in Netze mit externem Pull-Up: nCONFIG (R6),
 *    TMS (R3), TDI (R4). nCONFIG nie auf 8 (LED) oder 9 (BOOT-Taster).
 *  - Alle Programmierleitungen sind im Ruhezustand Eingaenge ohne internen
 *    Pull. Getrieben wird nur, solange fpga_prog/fpga_jtag arbeitet.
 *  - GPIO20/21: das Boot-Log des ROM auf GPIO21 trifft TDO hochohmig an,
 *    R11 (100 Ohm) begrenzt den Sonderfall.
 *  - R2 (TCK nach GND) und R6 (nCONFIG nach 3,3 V) sind die Signatur, an der
 *    board_detect() das Board erkennt. Sie muessen in jeder Revision bleiben.
 * ========================================================================== */

/* EPCS16 per Active Serial (ESP = SPI-Master, nur bei nCONFIG low) */
#define BOARD_PIN_AS_DCLK      0   /* SPI-SCLK -> H1 / EPCS-6 */
#define BOARD_PIN_AS_ASDO      1   /* SPI-MOSI -> C1 / EPCS-5 */
#define BOARD_PIN_AS_DATA0     3   /* SPI-MISO <- H2 / EPCS-2 */
#define BOARD_PIN_AS_NCSO      4   /* SPI-CS   -> D2 / EPCS-1 */

/* nCONFIG: open-drain, R6 10k nach 3,3 V, C14 100 nF nach GND (1 ms). Low haelt
 * das FPGA im Reset. Strapping-Pin -- R6 haelt ihn beim Reset des ESP high. */
#define BOARD_PIN_NCONFIG      2
/* CONF_DONE: Eingang, open-drain vom FPGA, R7 10k nach 3,3 V. */
#define BOARD_PIN_CONF_DONE   20

/* JTAG */
#define V7_PIN_JTAG_TCK        5   /* R2 10k nach GND */
#define V7_PIN_JTAG_TMS        8   /* = LED, R3 nach 3,3 V */
#define V7_PIN_JTAG_TDI        9   /* = BOOT-Taster, R4 nach 3,3 V */
#define V7_PIN_JTAG_TDO       21   /* ueber R11 100 Ohm */

/* ========================================================================== *
 * JTAG-Piggyback an P5 (ESP32_jtag_piggyback v1.00, Flachbandkabel zum
 * 10-poligen JTAG-Header). Massgeblich: N:\Projekte\misc\USB_BLASTER_DIY\
 * ESP32-C3_EVALUIERUNG.md Abschnitt 14.9/14.10 (Platine) und 15 (vJTAG).
 *     P1-1 TCK <- GPIO6  (33 Ohm)      P1-5 TMS <- GPIO10 (33 Ohm)
 *     P1-9 TDI <- GPIO7  (33 Ohm)      P1-3 TDO -> GPIO5  (100 Ohm)
 *     P1-4 3,3 V vom Zielboard = Versorgung, Teiler 10k/10k auf GPIO4 (ADC)
 *
 * Kein LISY-UART und kein CTRL_REQ: beides kommt ueber Virtual JTAG.
 * Keine Strapping-Pins (2/8/9) auf den JTAG-Leitungen. LED (8) und BOOT-Taster
 * (9) wie auf dem Supermini. TMS bleibt nach dem ersten Zugriff dauerhaft high
 * getrieben (TAP bleibt im Test-Logic-Reset); TCK bekommt im Ruhezustand den
 * internen Pull-down, weil das Board v4.x dort keinen eigenen hat.
 * Der Teiler an GPIO4 (~1,65 V) ist die Signatur fuer board_detect() und muss
 * in jeder Revision bleiben.
 * ========================================================================== */

#define JP_PIN_JTAG_TCK        6
#define JP_PIN_JTAG_TMS       10
#define JP_PIN_JTAG_TDI        7
#define JP_PIN_JTAG_TDO        5
#define JP_PIN_VTARGET         4   /* ADC1_CH4, Teiler 1:2 */
