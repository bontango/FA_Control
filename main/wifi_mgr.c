#include "wifi_mgr.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"
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
static bool s_stopping;   /* siehe wifi_mgr_stop() */
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_cb(void *arg)
{
    if (!s_stopping) {
        esp_wifi_connect();
    }
}

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
    /* Waehrend des Herunterfahrens nichts mehr anfassen: esp_wifi_stop() loest
     * STA_DISCONNECTED aus, und ein esp_wifi_connect() von hier aus griffe mitten
     * in den laufenden Stopp hinein. */
    if (s_stopping) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Getrennt, Grund %d",
                 ((wifi_event_sta_disconnected_t *)data)->reason);
        if (s_mode == WIFI_MGR_MODE_STA) {
            /* Im Betrieb nie aufgeben: frueher war nach STA_MAX_RETRY Schluss, und das
             * Geraet blieb bis zum Neustart offline. Pause waechst bis 30 s. */
            s_retries++;
            uint64_t ms = (s_retries < 6) ? (1000ULL << (s_retries - 1)) : 30000ULL;
            ESP_LOGI(TAG, "Neuer Verbindungsversuch in %llu ms", ms);
            esp_timer_start_once(s_reconnect_timer, ms * 1000);
        } else if (s_retries < STA_MAX_RETRY) {
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

/* Chiptemperatur und Signalstaerke, alle 5 s erneuert, fuer /api/status.
 * Aufgenommen nach den WLAN-Aussetzern am v7 (25.10.2026): Der Supermini sass
 * 6-7 mm ueber der Masseflaeche der Piggyback, die Antenne war verstimmt, und der
 * Sendeverstaerker heizte den Chip binnen Minuten von ~40 auf ueber 50 C auf.
 * Die Temperatur ist deshalb das schnellste Zeichen fuer eine schlechte Einbaulage. */
static temperature_sensor_handle_t s_tsens;
static volatile float s_diag_temp = -99.0f;
static volatile int s_diag_rssi;

static void diag_cb(void *arg)
{
    int rssi = 0;
    esp_wifi_sta_get_rssi(&rssi);
    float t = -99.0f;
    if (s_tsens) {
        temperature_sensor_get_celsius(s_tsens, &t);
    }
    s_diag_rssi = rssi;
    s_diag_temp = t;
}

static void diag_start(void)
{
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
    }
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = diag_cb, .name = "wifi_diag" };
    if (esp_timer_create(&a, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 5 * 1000 * 1000);
    }
}

void wifi_mgr_diag(float *temp_c, int *rssi)
{
    *temp_c = s_diag_temp;
    *rssi = s_diag_rssi;
}

void wifi_mgr_start(void)
{
    s_events = xEventGroupCreate();
    const esp_timer_create_args_t rc = { .callback = reconnect_cb, .name = "wifi_rc" };
    ESP_ERROR_CHECK(esp_timer_create(&rc, &s_reconnect_timer));

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
            diag_start();
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

void wifi_mgr_stop(void)
{
    /* Reihenfolge ist der ganze Punkt dieser Funktion.
     *
     * esp_wifi_stop() erzeugt WIFI_EVENT_STA_DISCONNECTED. Der Handler oben hat
     * darauf frueher mit esp_wifi_connect() geantwortet -- also mit einem
     * WLAN-Aufruf mitten in den Stopp hinein. esp_wifi_stop() kam dann nicht mehr
     * zurueck, und der aufrufende Task blieb stehen: das Geraet ging beim Umlegen
     * von DIP1 nicht schlafen, sondern lief einfach weiter (v1.12).
     *
     * Erst das Flag (schliesst das Rennen mit einem schon laufenden Handler),
     * dann abmelden, dann stoppen. */
    s_stopping = true;
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop(): %s", esp_err_to_name(err));
    }
}

wifi_mgr_mode_t wifi_mgr_get_mode(void)
{
    return s_mode;
}

void wifi_mgr_get_ip(char *buf, size_t len)
{
    strlcpy(buf, s_ip, len);
}
