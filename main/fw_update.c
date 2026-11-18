#include "fw_update.h"

#include <stdio.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "repo.h"
#include "sdkconfig.h"

static const char *TAG = "fw_update";

/* Seit v1.31 ein Image fuer alle Boards (board.c erkennt das Board beim Start),
 * deshalb ein Ordner. bin_v7/ und bin_jtag/ fragen nur noch Geraete mit
 * Firmware bis 1.30 ab. */
#define FW_BASE_URL   "https://lisy.dev/swrep/misc/FA_Control/bin/"
#define FW_MAX_NAME   64

static volatile fw_state_t s_state = FW_IDLE;
static volatile int s_pct;
static char s_err[64];
static char s_url[sizeof(FW_BASE_URL) + FW_MAX_NAME];

/* ---- Verzeichnislisting --------------------------------------------------- */

/* Das Absuchen des Apache-Index steht in repo.c -- die Namensdateien werden
 * genauso gefunden, nur mit anderer Endung. */
esp_err_t fw_update_list_json(char *out, size_t out_len)
{
    return repo_list_json(FW_BASE_URL, ".bin", out, out_len);
}

/* ---- Image-Pruefung -------------------------------------------------------- */

/* Erste Version mit Board-Erkennung. Aeltere Images sind je Variante gebaut,
 * und in bin/ lagen bis 1.30 nur die des Sockels: auf v7 oder dem JTAG-Piggyback
 * haetten sie die falsche Pinbelegung (DIP-Bank auf DCLK/nCONFIG, UART auf TCK). */
#define FW_UNIFIED_MAJOR 1
#define FW_UNIFIED_MINOR 31

const char *fw_image_check(const esp_app_desc_t *d)
{
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(d->project_name, esp_app_get_description()->project_name,
                sizeof(d->project_name)) != 0) {
        return "Not a FA_Control image";
    }
    if (board_type() == BOARD_SOCKET) {
        return NULL;                    /* alle Versionen sind Sockel-Images */
    }
    int maj = 0, min = 0;
    if (sscanf(d->version, "%d.%d", &maj, &min) != 2 ||
        maj < FW_UNIFIED_MAJOR || (maj == FW_UNIFIED_MAJOR && min < FW_UNIFIED_MINOR)) {
        return "Image older than 1.31 is for the socket board only";
    }
    return NULL;
}

/* ---- Update --------------------------------------------------------------- */

/* msg geht per /api/fwstatus in die Weboberflaeche -- deshalb englisch. */
static void set_error(const char *msg)
{
    strlcpy(s_err, msg, sizeof(s_err));
    s_state = FW_ERROR;
    ESP_LOGE(TAG, "Update fehlgeschlagen: %s", msg);
}

static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "Lade %s", s_url);

    esp_http_client_config_t http_cfg = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        set_error(esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(handle, &desc);
    const char *why = (err == ESP_OK) ? fw_image_check(&desc) : "Image header unreadable";
    if (why) {
        esp_https_ota_abort(handle);
        set_error(why);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Image %s %s passt auf Board %s", desc.project_name, desc.version, board_name());

    int image_size = esp_https_ota_get_image_size(handle);
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        if (image_size > 0) {
            s_pct = 100 * esp_https_ota_get_image_len_read(handle) / image_size;
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        set_error(err != ESP_OK ? esp_err_to_name(err) : "Download incomplete");
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(handle);  /* validiert Image + setzt Boot-Partition */
    if (err != ESP_OK) {
        set_error(err == ESP_ERR_OTA_VALIDATE_FAILED ? "Invalid image"
                                                     : esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_pct = 100;
    s_state = FW_OK;
    ESP_LOGI(TAG, "Update erfolgreich, Neustart in 2 s");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

esp_err_t fw_update_start(const char *filename)
{
    if (s_state == FW_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!repo_valid_filename(filename, ".bin", FW_MAX_NAME)) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_url, sizeof(s_url), "%s%s", FW_BASE_URL, filename);
    s_pct = 0;
    s_err[0] = '\0';
    s_state = FW_RUNNING;

    if (xTaskCreate(ota_task, "fw_ota", 8192, NULL, 5, NULL) != pdPASS) {
        s_state = FW_ERROR;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

fw_state_t fw_update_state(int *pct, const char **err_msg)
{
    if (pct) {
        *pct = s_pct;
    }
    if (err_msg) {
        *err_msg = s_err;
    }
    return s_state;
}
