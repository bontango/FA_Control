#include "fw_update.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fw_update";

#define FW_BASE_URL   "https://lisy.dev/swrep/misc/FA_Control/bin/"
#define FW_MAX_NAME   64
#define FW_MAX_FILES  20
#define LIST_BUF_SIZE (16 * 1024)

static volatile fw_state_t s_state = FW_IDLE;
static volatile int s_pct;
static char s_err[64];
static char s_url[sizeof(FW_BASE_URL) + FW_MAX_NAME];

/* ---- Verzeichnislisting --------------------------------------------------- */

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

esp_err_t fw_update_list_json(char *out, size_t out_len)
{
    char *body = malloc(LIST_BUF_SIZE);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = FW_BASE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    int total = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "Listing HTTP %d", status);
            err = ESP_FAIL;
        } else {
            while (total < LIST_BUF_SIZE - 1) {
                int n = esp_http_client_read(client, body + total,
                                             LIST_BUF_SIZE - 1 - total);
                if (n <= 0) {
                    break;
                }
                total += n;
            }
        }
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(body);
        return err;
    }
    body[total] = '\0';

    /* href="<name>.bin" einsammeln */
    char names[FW_MAX_FILES][FW_MAX_NAME];
    char *idx[FW_MAX_FILES];
    int count = 0;
    char *p = body;
    while (count < FW_MAX_FILES && (p = strstr(p, "href=\"")) != NULL) {
        p += 6;
        char *q = strchr(p, '"');
        if (!q) {
            break;
        }
        size_t len = q - p;
        if (len >= 4 && len < FW_MAX_NAME && strncmp(q - 4, ".bin", 4) == 0 &&
            !memchr(p, '/', len)) {
            memcpy(names[count], p, len);
            names[count][len] = '\0';
            idx[count] = names[count];
            count++;
        }
        p = q + 1;
    }
    free(body);

    qsort(idx, count, sizeof(idx[0]), cmp_desc);

    size_t w = snprintf(out, out_len, "{\"files\":[");
    for (int i = 0; i < count && w < out_len; i++) {
        w += snprintf(out + w, out_len - w, "%s\"%s\"", i ? "," : "", idx[i]);
    }
    if (w < out_len) {
        snprintf(out + w, out_len - w, "]}");
    }
    ESP_LOGI(TAG, "%d Firmware-Dateien gefunden", count);
    return ESP_OK;
}

/* ---- Update --------------------------------------------------------------- */

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
        set_error(err != ESP_OK ? esp_err_to_name(err) : "Download unvollstaendig");
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(handle);  /* validiert Image + setzt Boot-Partition */
    if (err != ESP_OK) {
        set_error(err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image ungueltig"
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
    size_t len = strlen(filename);
    if (len < 5 || len >= FW_MAX_NAME || strcmp(filename + len - 4, ".bin") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < len; i++) {
        char c = filename[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') {
            return ESP_ERR_INVALID_ARG;
        }
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
