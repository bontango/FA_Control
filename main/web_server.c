#include "web_server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "esp_app_desc.h"
#include "fa_connect.h"
#include "fw_update.h"
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

/* Der Text landet per toast("ERROR: …") in der Weboberflaeche und ist damit
 * Oberflaeche -- deshalb englisch, anders als die ESP_LOG-Texte daneben. */
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
    const fa_conn_info_t *ci = fa_connect_info();
    char dw[64] = "";
    size_t p = 0;
    for (int i = 0; i < ci->displays && i < CFG_MAX_DISPLAYS; i++) {
        p += snprintf(dw + p, sizeof(dw) - p, "%s%u", i ? "," : "", ci->disp_width[i]);
    }
    char buf[448];
    snprintf(buf, sizeof(buf),
             "{\"lamps\":%u,\"coils\":%u,\"switches\":%u,\"sounds\":%u,"
             "\"displays\":%u,\"dw\":[%s],\"wd\":%d,\"wdlast\":%d,\"pulse\":%u,"
             "\"conn\":%d,\"connmsg\":\"%s\","
             "\"hw\":\"%s\",\"fwver\":\"%s\",\"apiver\":\"%s\",\"game\":\"%s\"}",
             ci->lamps, ci->coils, ci->switches, ci->sounds,
             ci->displays, dw,
             lisy_watchdog_enabled() ? 1 : 0, lisy_watchdog_last_result(),
             g_cfg.coil_pulse_ms,
             (int)ci->state, fa_connect_state_str(),
             ci->hw, ci->fw_ver, ci->api_ver, ci->game);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- API: Verbinden / Kontrolle zurueckgeben ----------------------------- */

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    fa_connect_run();
    lisy_lamp_bitmap_clear();
    s_switches_initialized = false;
    memset(s_disp_text, 0, sizeof(s_disp_text));
    return config_get_handler(req);   /* gleich die frischen Werte zurueckliefern */
}

static esp_err_t disconnect_post_handler(httpd_req_t *req)
{
    fa_connect_release();
    /* Die Spiegelbilder gehoeren zur alten Verbindung -- ab jetzt steuert wieder
     * das Spiel, was hier stuende, waere geraten. */
    lisy_lamp_bitmap_clear();
    s_switches_initialized = false;
    memset(s_disp_text, 0, sizeof(s_disp_text));
    return config_get_handler(req);
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

/* Die einzige noch speicherbare Einstellung ist die Spulen-Pulszeit. Alles andere
 * kommt vom Geraet (Bestueckung) oder haengt am Verbindungszustand (Watchdog). */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    g_cfg.coil_pulse_ms = clamp_u8(get_param_int(req, "pulse", g_cfg.coil_pulse_ms), 1, 255);

    esp_err_t err = app_config_save();
    if (err != ESP_OK) {
        return send_err(req, "NVS error");
    }
    lisy_coil_apply_pulse_time(fa_connect_info()->coils, g_cfg.coil_pulse_ms);
    ESP_LOGI(TAG, "Spulen-Pulszeit gespeichert: %u ms", g_cfg.coil_pulse_ms);
    return send_ok(req);
}

/* ---- API: Steuerung ------------------------------------------------------ */

/* Alle Steuer-Handler pruefen gegen die vom Geraet gemeldete Bestueckung. Ohne
 * gewaehrte Kontrolle steht die auf 0, damit weist schon die Bereichspruefung
 * jeden Befehl ab -- unabhaengig davon, was die Weboberflaeche anbietet. */
static esp_err_t lamp_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    int on = get_param_int(req, "on", -1);
    if (id < 0 || id >= fa_connect_info()->lamps || on < 0) {
        return send_err(req, "Bad parameter");
    }
    lisy_lamp_set((uint8_t)id, on != 0);
    return send_ok(req);
}

static esp_err_t coil_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    /* Spulen zaehlen ab 1 (LISY-Konvention, = Treiber Q1..Qn im Schaltplan) --
     * anders als Lampen, Schalter und Sounds. */
    if (id < 1 || id > fa_connect_info()->coils) {
        return send_err(req, "Bad parameter");
    }
    lisy_coil_pulse((uint8_t)id);
    return send_ok(req);
}

static esp_err_t sound_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    int on = get_param_int(req, "on", 1);
    if (id < 0 || id >= fa_connect_info()->sounds) {
        return send_err(req, "Bad parameter");
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
    const fa_conn_info_t *ci = fa_connect_info();
    int id = get_param_int(req, "id", -1);
    char text[CFG_MAX_DISP_W + 1] = "";
    get_param(req, "text", text, sizeof(text));
    if (id < 0 || id >= ci->displays) {
        return send_err(req, "Bad parameter");
    }
    lisy_display_set((uint8_t)id, text, ci->disp_width[id]);
    strlcpy(s_disp_text[id], text, sizeof(s_disp_text[id]));
    return send_ok(req);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    const fa_conn_info_t *ci = fa_connect_info();

    /* Ohne gewaehrte Kontrolle wird nichts abgefragt: der Bus gehoert dann dem
     * Spiel, und ein Schalter-Poll waere eine Einmischung. */
    if (ci->state == FA_CONN_ACTIVE) {
        if (!s_switches_initialized) {
            lisy_switches_refresh_all(ci->switches);
            s_switches_initialized = true;
        } else {
            lisy_switches_drain_changes();
        }
    }

    char lamps_hex[LISY_LAMP_BITMAP_LEN * 2 + 1];
    char sw_hex[LISY_SWITCH_BITMAP_LEN * 2 + 1];
    bitmap_to_hex(lisy_lamp_bitmap(), LISY_LAMP_BITMAP_LEN, lamps_hex);
    bitmap_to_hex(lisy_switch_bitmap(), LISY_SWITCH_BITMAP_LEN, sw_hex);

    char disp[CFG_MAX_DISPLAYS * (CFG_MAX_DISP_W + 4) + 4] = "";
    size_t p = 0;
    for (int i = 0; i < ci->displays; i++) {
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
    lisy_coil_apply_pulse_time(fa_connect_info()->coils, g_cfg.coil_pulse_ms);

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
             "{\"mode\":\"%s\",\"ip\":\"%s\",\"ver\":\"%s\"}",
             wifi_mgr_get_mode() == WIFI_MGR_MODE_AP ? "ap" : "sta",
             ip, esp_app_get_description()->version);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- API: Firmware-Update (OTA von lisy.dev) ------------------------------ */

static esp_err_t fwlist_get_handler(httpd_req_t *req)
{
    if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
        return send_err(req, "No internet in AP mode");
    }
    char *buf = malloc(1536);
    if (!buf) {
        return send_err(req, "Out of memory");
    }
    esp_err_t err = fw_update_list_json(buf, 1536);
    if (err != ESP_OK) {
        free(buf);
        return send_err(req, "Server unreachable");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, buf);
    free(buf);
    return ret;
}

static esp_err_t fwupdate_post_handler(httpd_req_t *req)
{
    if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
        return send_err(req, "No internet in AP mode");
    }
    char file[64];
    if (!get_param(req, "file", file, sizeof(file))) {
        return send_err(req, "Parameter file missing");
    }
    esp_err_t err = fw_update_start(file);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_err(req, "Update already running");
    }
    if (err != ESP_OK) {
        return send_err(req, "Invalid file name");
    }
    return send_ok(req);
}

static esp_err_t fwstatus_get_handler(httpd_req_t *req)
{
    static const char *names[] = { "idle", "running", "ok", "error" };
    int pct;
    const char *msg;
    fw_state_t st = fw_update_state(&pct, &msg);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"pct\":%d,\"msg\":\"%s\"}",
             names[st], pct, msg);
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
        return send_err(req, "SSID missing");
    }
    get_param(req, "pass", pass, sizeof(pass));

    if (wifi_mgr_set_credentials(ssid, pass) != ESP_OK) {
        return send_err(req, "NVS error");
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
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    /* TLS-Client (fwlist via mbedTLS) laeuft im httpd-Task -> mehr Stack noetig */
    cfg.stack_size = 10240;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/api/config",  .method = HTTP_GET,  .handler = config_get_handler },
        { .uri = "/api/config",  .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_post_handler },
        { .uri = "/api/disconnect", .method = HTTP_POST, .handler = disconnect_post_handler },
        { .uri = "/api/lamp",    .method = HTTP_POST, .handler = lamp_post_handler },
        { .uri = "/api/coil",    .method = HTTP_POST, .handler = coil_post_handler },
        { .uri = "/api/sound",   .method = HTTP_POST, .handler = sound_post_handler },
        { .uri = "/api/display", .method = HTTP_POST, .handler = display_post_handler },
        { .uri = "/api/state",   .method = HTTP_GET,  .handler = state_get_handler },
        { .uri = "/api/reset",   .method = HTTP_POST, .handler = reset_post_handler },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get_handler },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/api/fwlist",  .method = HTTP_GET,  .handler = fwlist_get_handler },
        { .uri = "/api/fwupdate",.method = HTTP_POST, .handler = fwupdate_post_handler },
        { .uri = "/api/fwstatus",.method = HTTP_GET,  .handler = fwstatus_get_handler },
        { .uri = "/*",           .method = HTTP_GET,  .handler = root_get_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Webserver gestartet (Port 80)");
    return ESP_OK;
}
