#include "repo.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "repo";

#define LIST_BUF_SIZE (16 * 1024)
#define CHUNK_SIZE    1024

/* ---- Dateinamen ---------------------------------------------------------- */

bool repo_valid_filename(const char *file, const char *suffix, size_t max_len)
{
    if (!file || !suffix) {
        return false;
    }
    size_t len = strlen(file);
    size_t slen = strlen(suffix);
    if (len <= slen || len >= max_len) {
        return false;
    }
    if (strcmp(file + len - slen, suffix) != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = file[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

/* ---- Verzeichnislisting --------------------------------------------------- */

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

esp_err_t repo_list_json(const char *base_url, const char *suffix,
                         char *out, size_t out_len)
{
    size_t slen = strlen(suffix);
    char *body = malloc(LIST_BUF_SIZE);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = base_url,
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

    /* href="<name><suffix>" einsammeln */
    char names[REPO_MAX_FILES][REPO_MAX_NAME];
    char *idx[REPO_MAX_FILES];
    int count = 0;
    char *p = body;
    while (count < REPO_MAX_FILES && (p = strstr(p, "href=\"")) != NULL) {
        p += 6;
        char *q = strchr(p, '"');
        if (!q) {
            break;
        }
        size_t len = q - p;
        if (len > slen && len < REPO_MAX_NAME &&
            strncmp(q - slen, suffix, slen) == 0 && !memchr(p, '/', len)) {
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
    ESP_LOGI(TAG, "%d Dateien '%s' gefunden", count, suffix);
    return ESP_OK;
}

/* ---- Download ------------------------------------------------------------- */

esp_err_t repo_download_to_file(const char *base_url, const char *file,
                                FILE *fp, size_t max_len)
{
    char url[128];
    snprintf(url, sizeof(url), "%s%s", base_url, file);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "Download %s: HTTP %d", file, status);
            err = ESP_FAIL;
        }
    }

    size_t total = 0;
    char buf[CHUNK_SIZE];
    while (err == ESP_OK) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;   /* fertig */
        }
        if (total + n > max_len) {
            ESP_LOGW(TAG, "Download %s laenger als %u Byte", file, (unsigned)max_len);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(buf, 1, n, fp) != (size_t)n) {
            err = ESP_FAIL;
            break;
        }
        total += n;
    }
    esp_http_client_cleanup(client);

    if (err == ESP_OK && total == 0) {
        err = ESP_FAIL;   /* leere Datei ist kein brauchbares Ergebnis */
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s geladen (%u Byte)", file, (unsigned)total);
    }
    return err;
}
