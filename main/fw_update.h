#pragma once

#include <stddef.h>
#include "esp_app_desc.h"
#include "esp_err.h"

typedef enum {
    FW_IDLE,
    FW_RUNNING,
    FW_OK,
    FW_ERROR,
} fw_state_t;

/* Verzeichnislisting von lisy.dev holen, .bin-Namen als JSON {"files":[...]} nach out. */
esp_err_t fw_update_list_json(char *out, size_t out_len);

/* Update-Task starten. ESP_ERR_INVALID_ARG = ungueltiger Dateiname,
   ESP_ERR_INVALID_STATE = Update laeuft bereits. */
esp_err_t fw_update_start(const char *filename);

/* Aktueller Zustand; pct (0-100) und err_msg optional (NULL erlaubt). */
fw_state_t fw_update_state(int *pct, const char **err_msg);

/* Passt das Image auf dieses Geraet? NULL = ja, sonst der Grund (englisch, geht
 * in die Oberflaeche). Prueft Projektname und -- ausser auf dem Sockel -- dass es
 * ein Image mit Board-Erkennung ist (ab 1.31). */
const char *fw_image_check(const esp_app_desc_t *d);
