#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_AP,
} wifi_mgr_mode_t;

/* Blockiert bis STA verbunden ist oder der AP-Fallback laeuft. */
void wifi_mgr_start(void);

/*
 * WLAN geordnet herunterfahren -- vor dem Tiefschlaf aufzurufen.
 * Meldet zuerst die Event-Handler ab: ohne das antwortet der Reconnect-Handler
 * auf das vom Stopp erzeugte STA_DISCONNECTED mit esp_wifi_connect(), und
 * esp_wifi_stop() kehrt nicht zurueck. Darf auch aufgerufen werden, wenn WLAN
 * noch gar nicht gestartet wurde.
 */
void wifi_mgr_stop(void);

wifi_mgr_mode_t wifi_mgr_get_mode(void);
void wifi_mgr_get_ip(char *buf, size_t len);

/* Credentials in NVS speichern (Neustart macht der Aufrufer). */
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass);
