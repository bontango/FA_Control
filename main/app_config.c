#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_config";

#define NVS_NS  "facfg"
#define NVS_KEY "cfg"

app_config_t g_cfg;

static void set_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->lamps = 40;
    c->coils = 20;
    c->switches = 40;
    c->sounds = 16;
    c->displays = 5;
    c->disp_width[0] = 4;   /* Status-/Credit-Display */
    for (int i = 1; i < CFG_MAX_DISPLAYS; i++) {
        c->disp_width[i] = 6;
    }
    c->watchdog_en = true;
    c->coil_pulse_ms = 50;
}

void app_config_load(void)
{
    set_defaults(&g_cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "Keine gespeicherte Konfiguration, Defaults aktiv");
        return;
    }
    app_config_t stored;
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &stored, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(stored)) {
        g_cfg = stored;
        ESP_LOGI(TAG, "Konfiguration aus NVS geladen");
    } else {
        ESP_LOGI(TAG, "Konfiguration ungueltig (%s), Defaults aktiv", esp_err_to_name(err));
    }
}

esp_err_t app_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, &g_cfg, sizeof(g_cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
