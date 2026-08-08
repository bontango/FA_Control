#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Maxima laut LISY-Protokoll v0.08 */
#define CFG_MAX_LAMPS    255
#define CFG_MAX_COILS    127
#define CFG_MAX_SWITCHES 127
#define CFG_MAX_SOUNDS   255
#define CFG_MAX_DISPLAYS 7
#define CFG_MAX_DISP_W   16   /* max. Stellen pro Display (UI-Begrenzung) */

/* Kennung und Version des NVS-Blobs.
 * Vorher pruefte app_config_load() nur die Groesse. Wird das Struct erweitert und
 * bleibt dabei zufaellig gleich gross, waeren alte Daten falsch interpretiert
 * worden; aendert es die Groesse, verschwand die Konfiguration kommentarlos.
 * Mit Magic und Version ist beides eindeutig. CFG_VERSION bei jeder Aenderung
 * der Feldbelegung hochzaehlen. */
#define CFG_MAGIC   0xFA
#define CFG_VERSION 3

/*
 * Was hier steht, muss einen Neustart ueberleben und darf nicht vom Geraet kommen.
 * Das ist seit Version 1.11 nur noch die Spulen-Pulszeit: die Bestueckung (Anzahl
 * Lampen, Spulen, Schalter, Sounds, Displays) meldet die Gegenstelle beim Verbinden
 * und steht deshalb in fa_conn_info_t, nicht hier. Sie zusaetzlich zu speichern
 * hiesse, zwei Wahrheiten zu pflegen, von denen eine veraltet.
 */
typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t coil_pulse_ms;
} app_config_t;

extern app_config_t g_cfg;

void app_config_load(void);
esp_err_t app_config_save(void);
