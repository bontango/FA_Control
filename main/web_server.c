#include "web_server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "lisy.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static char s_disp_text[CFG_MAX_DISPLAYS][CFG_MAX_DISP_W + 1];
static bool s_switches_initialized;

/* ---- Helfer ------------------------------------------------------------- */

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/* Query-Parameter holen; Rueckgabe false wenn nicht vorhanden. */
static bool get_param(httpd_req_t *req, const char *key, char *val, size_t len)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, key, val, len) != ESP_OK) {
        return false;
    }
    url_decode(val);
    return true;
}

static int get_param_int(httpd_req_t *req, const char *key, int fallback)
{
    char v[12];
    if (!get_param(req, key, v, sizeof(v))) {
        return fallback;
    }
    return atoi(v);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t send_err(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static void bitmap_to_hex(const uint8_t *bm, size_t len, char *out)
{
    static const char hexc[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hexc[bm[i] >> 4];
        out[i * 2 + 1] = hexc[bm[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* ---- Statische Seite / Captive Portal ----------------------------------- */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (strcmp(req->uri, "/") != 0) {
        /* Captive Portal: alles Unbekannte auf die Hauptseite umleiten */
        httpd_resp_set_status(req, "302 Found");
        if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        } else {
            httpd_resp_set_hdr(req, "Location", "/");
        }
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                           index_html_gz_end - index_html_gz_start);
}

/* ---- API: Konfiguration -------------------------------------------------- */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char dw[64] = "";
    size_t p = 0;
    for (int i = 0; i < g_cfg.displays && i < CFG_MAX_DISPLAYS; i++) {
        p += snprintf(dw + p, sizeof(dw) - p, "%s%u", i ? "," : "", g_cfg.disp_width[i]);
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"lamps\":%u,\"coils\":%u,\"switches\":%u,\"sounds\":%u,"
             "\"displays\":%u,\"dw\":[%s],\"wd\":%d,\"pulse\":%u,"
             "\"maxl\":%d,\"maxc\":%d,\"maxs\":%d,\"maxo\":%d,\"maxd\":%d}",
             g_cfg.lamps, g_cfg.coils, g_cfg.switches, g_cfg.sounds,
             g_cfg.displays, dw, g_cfg.watchdog_en ? 1 : 0, g_cfg.coil_pulse_ms,
             CFG_MAX_LAMPS, CFG_MAX_COILS, CFG_MAX_SWITCHES, CFG_MAX_SOUNDS,
             CFG_MAX_DISPLAYS);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static uint8_t clamp_u8(int v, int min, int max)
{
    if (v < min) {
        v = min;
    }
    if (v > max) {
        v = max;
    }
    return (uint8_t)v;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    g_cfg.lamps    = clamp_u8(get_param_int(req, "lamps", g_cfg.lamps), 1, CFG_MAX_LAMPS);
    g_cfg.coils    = clamp_u8(get_param_int(req, "coils", g_cfg.coils), 1, CFG_MAX_COILS);
    g_cfg.switches = clamp_u8(get_param_int(req, "switches", g_cfg.switches), 1, CFG_MAX_SWITCHES);
    g_cfg.sounds   = clamp_u8(get_param_int(req, "sounds", g_cfg.sounds), 1, CFG_MAX_SOUNDS);
    g_cfg.displays = clamp_u8(get_param_int(req, "displays", g_cfg.displays), 1, CFG_MAX_DISPLAYS);
    g_cfg.coil_pulse_ms = clamp_u8(get_param_int(req, "pulse", g_cfg.coil_pulse_ms), 1, 255);
    g_cfg.watchdog_en = get_param_int(req, "wd", g_cfg.watchdog_en ? 1 : 0) != 0;

    char dw[64];
    if (get_param(req, "dw", dw, sizeof(dw))) {
        char *save = NULL;
        char *tok = strtok_r(dw, ",", &save);
        for (int i = 0; tok && i < CFG_MAX_DISPLAYS; i++) {
            g_cfg.disp_width[i] = clamp_u8(atoi(tok), 1, CFG_MAX_DISP_W);
            tok = strtok_r(NULL, ",", &save);
        }
    }

    esp_err_t err = app_config_save();
    if (err != ESP_OK) {
        return send_err(req, "NVS-Fehler");
    }
    lisy_watchdog_enable(g_cfg.watchdog_en);
    lisy_coil_apply_pulse_time(g_cfg.coils, g_cfg.coil_pulse_ms);
    s_switches_initialized = false;
    ESP_LOGI(TAG, "Konfiguration gespeichert");
    return send_ok(req);
}

/* ---- API: Steuerung ------------------------------------------------------ */

static esp_err_t lamp_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    int on = get_param_int(req, "on", -1);
    if (id < 0 || id >= g_cfg.lamps || on < 0) {
        return send_err(req, "Parameter");
    }
    lisy_lamp_set((uint8_t)id, on != 0);
    return send_ok(req);
}

static esp_err_t coil_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    if (id < 0 || id >= g_cfg.coils) {
        return send_err(req, "Parameter");
    }
    lisy_coil_pulse((uint8_t)id);
    return send_ok(req);
}

static esp_err_t sound_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    int on = get_param_int(req, "on", 1);
    if (id < 0 || id >= g_cfg.sounds) {
        return send_err(req, "Parameter");
    }
    if (on) {
        lisy_sound_play(1, (uint8_t)id);
    } else {
        lisy_sound_stop(1);
    }
    return send_ok(req);
}

static esp_err_t display_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    char text[CFG_MAX_DISP_W + 1] = "";
    get_param(req, "text", text, sizeof(text));
    if (id < 0 || id >= g_cfg.displays) {
        return send_err(req, "Parameter");
    }
    lisy_display_set((uint8_t)id, text, g_cfg.disp_width[id]);
    strlcpy(s_disp_text[id], text, sizeof(s_disp_text[id]));
    return send_ok(req);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    if (!s_switches_initialized) {
        lisy_switches_refresh_all(g_cfg.switches);
        s_switches_initialized = true;
    } else {
        lisy_switches_drain_changes();
    }

    char lamps_hex[LISY_LAMP_BITMAP_LEN * 2 + 1];
    char sw_hex[LISY_SWITCH_BITMAP_LEN * 2 + 1];
    bitmap_to_hex(lisy_lamp_bitmap(), LISY_LAMP_BITMAP_LEN, lamps_hex);
    bitmap_to_hex(lisy_switch_bitmap(), LISY_SWITCH_BITMAP_LEN, sw_hex);

    char disp[CFG_MAX_DISPLAYS * (CFG_MAX_DISP_W + 4) + 4] = "";
    size_t p = 0;
    for (int i = 0; i < g_cfg.displays; i++) {
        p += snprintf(disp + p, sizeof(disp) - p, "%s\"%s\"",
                      i ? "," : "", s_disp_text[i]);
    }

    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"lamps\":\"%s\",\"switches\":\"%s\",\"disp\":[%s]}",
             lamps_hex, sw_hex, disp);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    int r = lisy_init_reset();
    lisy_lamp_bitmap_clear();
    memset(s_disp_text, 0, sizeof(s_disp_text));
    s_switches_initialized = false;
    lisy_coil_apply_pulse_time(g_cfg.coils, g_cfg.coil_pulse_ms);

    char buf[32];
    snprintf(buf, sizeof(buf), "{\"result\":%d}", r);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"ip\":\"%s\",\"wd\":%d,\"wd_last\":%d}",
             wifi_mgr_get_mode() == WIFI_MGR_MODE_AP ? "ap" : "sta",
             ip, lisy_watchdog_enabled() ? 1 : 0, lisy_watchdog_last_result());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- API: WLAN ----------------------------------------------------------- */

static void restart_timer_cb(void *arg)
{
    esp_restart();
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char ssid[33], pass[65] = "";
    if (!get_param(req, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return send_err(req, "SSID fehlt");
    }
    get_param(req, "pass", pass, sizeof(pass));

    if (wifi_mgr_set_credentials(ssid, pass) != ESP_OK) {
        return send_err(req, "NVS-Fehler");
    }
    ESP_LOGI(TAG, "WLAN-Credentials gespeichert, Neustart in 1 s");
    send_ok(req);

    const esp_timer_create_args_t targs = {
        .callback = restart_timer_cb,
        .name = "restart",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        esp_timer_start_once(t, 1000 * 1000);
    }
    return ESP_OK;
}

/* ---- Start --------------------------------------------------------------- */

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/api/config",  .method = HTTP_GET,  .handler = config_get_handler },
        { .uri = "/api/config",  .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/lamp",    .method = HTTP_POST, .handler = lamp_post_handler },
        { .uri = "/api/coil",    .method = HTTP_POST, .handler = coil_post_handler },
        { .uri = "/api/sound",   .method = HTTP_POST, .handler = sound_post_handler },
        { .uri = "/api/display", .method = HTTP_POST, .handler = display_post_handler },
        { .uri = "/api/state",   .method = HTTP_GET,  .handler = state_get_handler },
        { .uri = "/api/reset",   .method = HTTP_POST, .handler = reset_post_handler },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get_handler },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/*",           .method = HTTP_GET,  .handler = root_get_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Webserver gestartet (Port 80)");
    return ESP_OK;
}
