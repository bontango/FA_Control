#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "board.h"
#include "lisy.h"
#include "names.h"
#include "power_mgr.h"
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

    /* Ein/Aus-Schalter der Anlage: kehrt nur zurueck, wenn DIP1 auf ON steht.
     * Steht er auf OFF, geht es nach der Gnadenfrist in den Tiefschlaf -- deshalb
     * vor lisy_init(), den UART braucht ein Schlafender nicht. */
    power_mgr_boot_gate();

    ESP_ERROR_CHECK(lisy_init());

    /* Namensdateien sind Beiwerk: fehlt die Partition (Geraet nur per OTA
     * aktualisiert, die Tabelle wird dabei nicht geschrieben), laeuft alles
     * uebrige unveraendert weiter -- deshalb kein ESP_ERROR_CHECK. */
    names_init();

    /* Beim Start wird bewusst nicht verbunden: solange niemand in der Oberflaeche
     * "CONNECT" drueckt, gehoert der Flipper sich selbst. Watchdog und Pulszeit
     * setzt fa_connect_run(), sobald die Gegenstelle die Kontrolle gewaehrt. */

    wifi_mgr_start();
    ESP_ERROR_CHECK(web_server_start());

    /* Blinkanzeige und DIP1-Waechter zuletzt: ab hier kann jederzeit der
     * Tiefschlaf einsetzen, und dann soll alles andere schon gestanden haben. */
    power_mgr_start();

    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "FA_Control bereit: http://%s/", ip);
}
