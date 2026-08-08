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
#define CFG_VERSION 2

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t lamps;
    uint8_t coils;
    uint8_t switches;
    uint8_t sounds;
    uint8_t displays;
    uint8_t disp_width[CFG_MAX_DISPLAYS];
    bool    watchdog_en;
    uint8_t coil_pulse_ms;
    /* true = beim Start automatisch verbinden und die Anzahlen von der Gegenstelle
     * holen. false = es gelten die hier gespeicherten, von Hand eingetragenen Werte. */
    bool    auto_connect;
} app_config_t;

extern app_config_t g_cfg;

void app_config_load(void);
esp_err_t app_config_save(void);
