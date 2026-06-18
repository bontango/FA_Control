#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "board.h"
#include "lisy.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_config_load();
    board_init();
    ESP_ERROR_CHECK(lisy_init());
    lisy_watchdog_enable(g_cfg.watchdog_en);
    lisy_coil_apply_pulse_time(g_cfg.coils, g_cfg.coil_pulse_ms);

    wifi_mgr_start();
    ESP_ERROR_CHECK(web_server_start());

    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "FA_Control bereit: http://%s/", ip);
}
