#include "wifi_mgr.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "wifi_mgr";

#define NVS_NS        "wifi"
#define AP_SSID       "FA-Control"
#define AP_IP_STR     "192.168.4.1"
#define STA_MAX_RETRY 5
#define MDNS_HOST     "fa-control"

#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1

static EventGroupHandle_t s_events;
static wifi_mgr_mode_t s_mode = WIFI_MGR_MODE_AP;
static char s_ip[16] = AP_IP_STR;
static int s_retries;

/* ---- Mini-DNS-Server: beantwortet alle Anfragen mit 192.168.4.1 --------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS-Socket fehlgeschlagen");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[256];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                           (struct sockaddr *)&src, &slen);
        if (len < 12) {
            continue;
        }
        /* Antwort: Header kopieren, Response-Flag, 1 Answer */
        buf[2] = 0x84;  /* QR=1, AA=1 */
        buf[3] = 0x00;
        buf[6] = 0x00; buf[7] = 0x01;  /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        /* Answer: Pointer auf Frage (0xC00C), Typ A, Klasse IN, TTL 60, IP */
        uint8_t ans[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                          0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
                          192, 168, 4, 1 };
        memcpy(buf + len, ans, sizeof(ans));
        sendto(sock, buf, len + sizeof(ans), 0, (struct sockaddr *)&src, slen);
    }
}

/* ---- Events ------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < STA_MAX_RETRY) {
            s_retries++;
            ESP_LOGI(TAG, "Verbindungsversuch %d/%d", s_retries, STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

/* ---- Credentials -------------------------------------------------------- */

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &pass_len);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0';
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- Start -------------------------------------------------------------- */

static void start_ap(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_mode = WIFI_MGR_MODE_AP;
    strcpy(s_ip, AP_IP_STR);
    xTaskCreate(dns_task, "dns", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "AP-Modus: SSID '%s', IP %s (Captive Portal aktiv)", AP_SSID, s_ip);
}

void wifi_mgr_start(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    char ssid[33] = {0}, pass[65] = {0};
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Verbinde mit '%s' ...", ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(20000));
        if (bits & BIT_CONNECTED) {
            s_mode = WIFI_MGR_MODE_STA;
            ESP_LOGI(TAG, "Verbunden, IP %s", s_ip);
            if (mdns_init() == ESP_OK) {
                mdns_hostname_set(MDNS_HOST);
                mdns_instance_name_set("FA Control");
                mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOST);
            }
            return;
        }
        ESP_LOGW(TAG, "STA fehlgeschlagen, wechsle in AP-Modus");
        ESP_ERROR_CHECK(esp_wifi_stop());
    } else {
        ESP_LOGI(TAG, "Keine WLAN-Credentials gespeichert");
    }

    start_ap();
}

wifi_mgr_mode_t wifi_mgr_get_mode(void)
{
    return s_mode;
}

void wifi_mgr_get_ip(char *buf, size_t len)
{
    strlcpy(buf, s_ip, len);
}
