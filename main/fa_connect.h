#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "app_config.h"   /* CFG_MAX_DISPLAYS */

/*
 * Verbindungsaufbau zur Flipper-Seite ("Connect").
 *
 * Bis Version 1.00 hat FA_Control blind gesendet: die Anzahl von Lampen, Spulen,
 * Schaltern, Sounds und Displays musste der Benutzer von Hand eintragen, und ob
 * ueberhaupt jemand zuhoert, war nicht feststellbar.
 *
 * Jetzt gibt es einen richtigen Verbindungsaufbau:
 *   1. Uebernahme-Anforderung setzen (GPIO10, board_ctrl_request()).
 *   2. LISY_CMD_INIT_RESET (0x64) senden. Die Antwort sagt, ob die Gegenstelle
 *      die Kontrolle gewaehrt -- sie tut das nur, wenn der Betreiber es dort
 *      freigegeben hat (bei AtariFA: Options-DIP 4 auf ON).
 *   3. Bei Erfolg die Info-Gruppe 0..9 abfragen; die gemeldete Bestueckung landet
 *      in fa_conn_info_t.
 *
 * Seit Version 1.11 gibt es keine Handeingabe und keinen Rueckfall mehr: die
 * Bestueckung ist Teil der Verbindung. Ohne gewaehrte Kontrolle stehen alle Zaehler
 * auf 0 -- damit weisen die Bereichspruefungen im Webserver jeden Steuerbefehl von
 * selbst ab, statt ins Leere zu senden.
 */

typedef enum {
    FA_CONN_IDLE = 0,      /* nicht verbunden, Anforderung nicht gesetzt */
    FA_CONN_ACTIVE,        /* Kontrolle gewaehrt */
    FA_CONN_DENIED_DIP,    /* verweigert: Freigabeschalter der Gegenseite steht auf OFF */
    FA_CONN_DENIED_REQ,    /* verweigert: Anforderungsleitung kam dort nicht an */
    FA_CONN_DENIED_OTHER,  /* verweigert: unbekannter Fehlercode */
    FA_CONN_NO_ANSWER,     /* keine Gegenstelle -- Verkabelung/Stromversorgung pruefen */
    /* Monitor-Modus (seit v1.25): nur lesen, ohne GPIO10, ohne 0x64, ohne Watchdog.
     * Das Spiel laeuft ungestoert weiter. Neue Zustaende stehen hinten, die
     * Oberflaeche kennt die alten Nummern (conn === 1 = Kontrolle). */
    FA_CONN_MONITOR,       /* Monitor laeuft, Bestueckung ist gefuellt */
    FA_CONN_NO_MONITOR,    /* Gegenstelle antwortet, kennt 0x6E aber nicht */
    FA_CONN_MON_FORMAT,    /* 0x6E-Antwort passt nicht zur gemeldeten Bestueckung */
} fa_conn_state_t;

typedef struct {
    fa_conn_state_t state;
    char hw[16];           /* Opcode 0, z.B. "AtariFA" */
    char fw_ver[12];       /* Opcode 1, z.B. "0.1.3" */
    char api_ver[12];      /* Opcode 2, z.B. "0.12" */
    char game[12];         /* Opcode 8, Spielnummer */
    /* Bestueckung laut Info-Gruppe 0x03-0x09; 0 heisst "nicht verbunden". */
    uint8_t lamps;         /* Opcode 3 */
    uint8_t coils;         /* Opcode 4 */
    uint8_t switches;      /* Opcode 9 */
    uint8_t sounds;        /* Opcode 5 -- 0 ist gueltig ("kann keinen Ton") */
    uint8_t displays;      /* Opcode 6 */
    uint8_t disp_width[CFG_MAX_DISPLAYS];  /* Opcode 7, Stellen je Display */
    int  last_code;        /* roher Rueckgabewert von 0x64, fuer die Fehlersuche */
} fa_conn_info_t;

/* Verbindung aufbauen. Immer ESP_OK -- das Ergebnis steht in fa_connect_info(). */
esp_err_t fa_connect_run(void);

/* Anforderung zuruecknehmen; die Gegenstelle gibt die Kontrolle ans Spiel zurueck.
 * Beendet ebenso den Monitor-Modus. */
void fa_connect_release(void);

/* Monitor-Modus starten: Info-Gruppe lesen und einen 0x6E-Abzug probieren, ohne die
 * Kontrolle anzufordern. Eine aktive Kontrolle wird vorher zurueckgegeben (das Spiel
 * startet dann neu). Immer ESP_OK -- das Ergebnis steht in fa_connect_info(). */
esp_err_t fa_connect_monitor(void);

/* Laenge, die ein 0x6E-Abzug bei der gemeldeten Bestueckung haben muss. */
int fa_connect_monitor_len(const fa_conn_info_t *ci);

const fa_conn_info_t *fa_connect_info(void);

/* Kurzer Klartext zum aktuellen Zustand, direkt fuer die Weboberflaeche. */
const char *fa_connect_state_str(void);
