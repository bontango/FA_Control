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

wifi_mgr_mode_t wifi_mgr_get_mode(void);
void wifi_mgr_get_ip(char *buf, size_t len);

/* Credentials in NVS speichern (Neustart macht der Aufrufer). */
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass);
