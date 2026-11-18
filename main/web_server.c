#include "web_server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "fa_connect.h"
#include "fw_update.h"
#include "board.h"
#include "fpga_prog.h"
#include "fpga_jtag.h"
#include "lisy.h"
#include "names.h"
#include "repo.h"
#include "rom_boot.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

/* Kachel 09 FPGA: 1 = EP4CE6 dev board v7 (EPCS schreiben), 2 = JTAG-Piggyback
 * (Design per JTAG in den SRAM laden), 0 = Sockel. Seit v1.31 aus der
 * Board-Erkennung (board.c), nicht mehr aus der Build-Variante. */
static int has_fpga(void)
{
    switch (board_type()) {
    case BOARD_V7:   return 1;
    case BOARD_JTAG: return 2;
    default:         return 0;
    }
}

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t logo_png_start[]      asm("_binary_logo_png_start");
extern const uint8_t logo_png_end[]        asm("_binary_logo_png_end");

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

/* Text (hoechstens len Zeichen) als JSON-Stringinhalt nach out: " und \ maskiert,
 * nicht druckbare Zeichen als Leerzeichen. out braucht 2 * len + 1 Byte. Fuer die
 * ASCII-Displays (seit 1.28) -- deren Inhalt kommt vom Spiel bzw. vom Benutzer. */
static void text_to_json(const uint8_t *s, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        uint8_t c = s[i] & 0x7F;
        if (c < 0x20 || c == 0x7F) {
            c = ' ';
        }
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* Groesse der JSON-Liste aller Displays: je Display Text mit Maskierung (2 Byte je
 * Stelle), zwei Anfuehrungszeichen und ein Komma. */
#define DISP_JSON_LEN (CFG_MAX_DISPLAYS * (2 * CFG_MAX_DISP_W + 3) + 4)

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

/* Logo der Kopfleiste. Muss VOR dem Wildcard-Eintrag registriert sein, sonst
 * schluckt der Captive-Portal-Umleiter das Bild und im AP-Modus -- also genau
 * beim Einrichten -- staende dort ein kaputtes Bild. */
static esp_err_t logo_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return httpd_resp_send(req, (const char *)logo_png_start,
                           logo_png_end - logo_png_start);
}

/* ---- API: Konfiguration -------------------------------------------------- */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    const fa_conn_info_t *ci = fa_connect_info();
    char dw[64] = "";
    char dt[64] = "";   /* LISY-Displaytypen, 5/6 = ASCII (seit 1.28) */
    size_t p = 0, q = 0;
    for (int i = 0; i < ci->displays && i < CFG_MAX_DISPLAYS; i++) {
        p += snprintf(dw + p, sizeof(dw) - p, "%s%u", i ? "," : "", ci->disp_width[i]);
        q += snprintf(dt + q, sizeof(dt) - q, "%s%u", i ? "," : "", ci->disp_type[i]);
    }
    /* Kennung dieser Anlage, "<HW>_<GAME>" -- der Name, den die Namensdatei
     * tragen muss. Die Oberflaeche zeigt sie im Menue NAMES an. */
    char gameid[NAMES_MAX_ID];
    names_game_id(gameid, sizeof(gameid));

    /* Was auf diesem Board mit dem laufenden Design geht (seit 1.32). Die Seite
     * blendet danach Modi und Kacheln ein, statt zu raten:
     *   link  "uart" (Sockel, v7) oder die Art des fa_vjtag_link auf dem
     *         JTAG-Piggyback: "full", "monitor", "control", "service", "none"
     *   fsram Design per JTAG in den SRAM laden (.fjz)
     *   fepcs Konfigurationsflash dauerhaft schreiben (.jic/.rbf), bisher nur v7 */
    const char *link = board_type() == BOARD_JTAG ? fpga_vjtag_link_kind() : "uart";

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"lamps\":%u,\"coils\":%u,\"switches\":%u,\"sounds\":%u,"
             "\"displays\":%u,\"dw\":[%s],\"dt\":[%s],\"wd\":%d,\"wdlast\":%d,\"pulse\":%u,"
             "\"conn\":%d,\"connmsg\":\"%s\","
             "\"hw\":\"%s\",\"fwver\":\"%s\",\"apiver\":\"%s\",\"game\":\"%s\","
             "\"namesfs\":%d,\"names\":\"%s\",\"gameid\":\"%s\",\"romfs\":%d,\"fpga\":%d,"
             "\"board\":\"%s\",\"link\":\"%s\",\"fsram\":%d,\"fepcs\":%d}",
             ci->lamps, ci->coils, ci->switches, ci->sounds,
             ci->displays, dw, dt,
             lisy_watchdog_enabled() ? 1 : 0, lisy_watchdog_last_result(),
             g_cfg.coil_pulse_ms,
             (int)ci->state, fa_connect_state_str(),
             ci->hw, ci->fw_ver, ci->api_ver, ci->game,
             names_ready() ? 1 : 0, names_active(), gameid, rom_boot_ready() ? 1 : 0,
             has_fpga(),
             board_name(), link, board_has_jtag() ? 1 : 0, board_type() == BOARD_V7 ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- API: Verbinden / Kontrolle zurueckgeben ----------------------------- */

/* Im Monitor-Modus ist die Bestueckung gefuellt, ohne dass Kontrolle besteht.
 * Die Bereichspruefung allein haelt die Steuerbefehle dann nicht mehr ab. */
static bool in_control(void)
{
    return fa_connect_info()->state == FA_CONN_ACTIVE;
}

#define NO_CONTROL_MSG "Not in control mode"

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    fa_connect_run();
    lisy_lamp_bitmap_clear();
    s_switches_initialized = false;
    memset(s_disp_text, 0, sizeof(s_disp_text));
    return config_get_handler(req);   /* gleich die frischen Werte zurueckliefern */
}

/* Monitor-Modus starten (Umschalter MONITOR MODE). Gibt eine aktive Kontrolle
 * vorher zurueck -- das Spiel startet dann neu. Beendet wird er wie die Kontrolle
 * ueber /api/disconnect. */
/* Flag-Byte des letzten Monitor-Abzugs. Gemerkt statt neu gelesen: jeder Abzug
 * loescht in der Gegenstelle die Spulen-Merkbits. */
static uint8_t s_mon_flags;

static esp_err_t monitor_post_handler(httpd_req_t *req)
{
    s_mon_flags = 0;
    fa_connect_monitor();
    lisy_lamp_bitmap_clear();
    s_switches_initialized = false;
    memset(s_disp_text, 0, sizeof(s_disp_text));
    return config_get_handler(req);
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
    if (in_control()) {
        lisy_coil_apply_pulse_time(fa_connect_info()->coils, g_cfg.coil_pulse_ms);
    }
    ESP_LOGI(TAG, "Spulen-Pulszeit gespeichert: %u ms", g_cfg.coil_pulse_ms);
    return send_ok(req);
}

/* ---- API: Steuerung ------------------------------------------------------ */

/* Alle Steuer-Handler pruefen gegen die vom Geraet gemeldete Bestueckung. Ohne
 * Verbindung steht die auf 0, damit weist schon die Bereichspruefung jeden Befehl
 * ab -- unabhaengig davon, was die Weboberflaeche anbietet. Im Monitor-Modus ist
 * sie gefuellt, deshalb zusaetzlich in_control(). */
static esp_err_t lamp_post_handler(httpd_req_t *req)
{
    int id = get_param_int(req, "id", -1);
    int on = get_param_int(req, "on", -1);
    if (!in_control()) {
        return send_err(req, NO_CONTROL_MSG);
    }
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
    if (!in_control()) {
        return send_err(req, NO_CONTROL_MSG);
    }
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
    if (!in_control()) {
        return send_err(req, NO_CONTROL_MSG);
    }
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
    if (!in_control()) {
        return send_err(req, NO_CONTROL_MSG);
    }
    if (id < 0 || id >= ci->displays) {
        return send_err(req, "Bad parameter");
    }
    lisy_display_set((uint8_t)id, text, ci->disp_width[id], FA_DISP_IS_ASCII(ci->disp_type[id]));
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

    char disp[DISP_JSON_LEN] = "";
    size_t p = 0;
    for (int i = 0; i < ci->displays; i++) {
        char t[2 * CFG_MAX_DISP_W + 1];
        text_to_json((const uint8_t *)s_disp_text[i], CFG_MAX_DISP_W, t);
        p += snprintf(disp + p, sizeof(disp) - p, "%s\"%s\"", i ? "," : "", t);
    }

    char buf[64 + LISY_LAMP_BITMAP_LEN * 2 + LISY_SWITCH_BITMAP_LEN * 2 + DISP_JSON_LEN];
    snprintf(buf, sizeof(buf),
             "{\"lamps\":\"%s\",\"switches\":\"%s\",\"disp\":[%s]}",
             lamps_hex, sw_hex, disp);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* Istzustand im Monitor-Modus. Ein Abzug (0x6E) je Aufruf, zerlegt nach dem
 * Layout aus fa_connect_monitor_len(); die Oberflaeche fragt alle 250 ms. */
/* Schalter fuer das laufende Spiel schliessen. Die Gegenstelle gibt sie von selbst
 * wieder frei, wenn rund 2 s nichts mehr ankommt (Monitor-Abfragen halten das wach). */
static esp_err_t monsw_post_handler(httpd_req_t *req)
{
    const fa_conn_info_t *ci = fa_connect_info();
    if (ci->state != FA_CONN_MONITOR) {
        return send_err(req, "Monitor mode not active");
    }
    if (!(s_mon_flags & LISY_MON_FLAG_SIM)) {
        return send_err(req, "Switch simulation not available");
    }
    if (get_param_int(req, "all", 1) == 0) {
        for (int i = 0; i < ci->switches; i++) {
            lisy_switch_sim((uint8_t)i, false);
        }
        return send_ok(req);
    }
    int id = get_param_int(req, "id", -1);
    int on = get_param_int(req, "on", -1);
    if (id < 0 || id >= ci->switches || on < 0) {
        return send_err(req, "Bad parameter");
    }
    lisy_switch_sim((uint8_t)id, on != 0);
    return send_ok(req);
}

static esp_err_t monitor_get_handler(httpd_req_t *req)
{
    const fa_conn_info_t *ci = fa_connect_info();
    if (ci->state != FA_CONN_MONITOR) {
        return send_err(req, "Monitor mode not active");
    }

    uint8_t m[LISY_MONITOR_MAX];
    int n = lisy_monitor_read(m, sizeof(m));
    if (n != fa_connect_monitor_len(ci)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":0}");
    }

    int nl = (ci->lamps + 7) / 8;
    int nc = (ci->coils + 7) / 8;
    int ns = (ci->switches + 7) / 8;
    const uint8_t *p = m + 1;
    s_mon_flags = m[0];
    char lamps[LISY_LAMP_BITMAP_LEN * 2 + 1];
    char coils[16 * 2 + 1];
    char coilsx[16 * 2 + 1];
    char sw[LISY_SWITCH_BITMAP_LEN * 2 + 1];
    bitmap_to_hex(p, nl, lamps);        p += nl;
    bitmap_to_hex(p, nc, coils);        p += nc;
    bitmap_to_hex(p, nc, coilsx);       p += nc;
    uint8_t snd = *p++;
    bitmap_to_hex(p, ns, sw);           p += ns;

    /* Displays als Zeichenkette aus Ziffern-Nibbles, linke Ziffer zuerst; ASCII-Displays
     * (Typ 5/6) als Text, ein Zeichen je Byte. Die Oberflaeche unterscheidet ueber dt. */
    char disp[DISP_JSON_LEN] = "";
    size_t dp = 0;
    for (int i = 0; i < ci->displays; i++) {
        char d[2 * CFG_MAX_DISP_W + 2];
        int w = ci->disp_width[i];
        if (FA_DISP_IS_ASCII(ci->disp_type[i])) {
            uint8_t t[CFG_MAX_DISP_W];
            memcpy(t, p, w);
            for (int k = 0; k < w; k++) {
                if (t[k] == 0) {
                    t[k] = ' ';   /* sonst endete der Text am ersten NUL */
                }
            }
            text_to_json(t, w, d);
            p += w;
        } else {
            bitmap_to_hex(p, (w + 1) / 2, d);
            p += (w + 1) / 2;
            d[w] = '\0';
        }
        dp += snprintf(disp + dp, sizeof(disp) - dp, "%s\"%s\"", i ? "," : "", d);
    }

    char buf[128 + LISY_LAMP_BITMAP_LEN * 2 + 4 * 16 + LISY_SWITCH_BITMAP_LEN * 2 + DISP_JSON_LEN];
    snprintf(buf, sizeof(buf),
             "{\"ok\":1,\"flags\":%u,\"lamps\":\"%s\",\"coils\":\"%s\",\"coilsx\":\"%s\","
             "\"snd\":%u,\"sw\":\"%s\",\"disp\":[%s]}",
             m[0], lamps, coils, coilsx, snd, sw, disp);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    if (!in_control()) {
        return send_err(req, NO_CONTROL_MSG);
    }
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
    float temp;
    int rssi;
    wifi_mgr_diag(&temp, &rssi);
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"ip\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\",\"bdet\":\"%s\","
             "\"temp\":%.1f,\"rssi\":%d}",
             wifi_mgr_get_mode() == WIFI_MGR_MODE_AP ? "ap" : "sta",
             ip, esp_app_get_description()->version, board_name(), board_detect_info(),
             temp, rssi);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- API: Namensdateien --------------------------------------------------- */

/* Beschriftung der Kacheln, sonst nichts. Ohne Partition (Geraet nur per OTA
 * aktualisiert) weisen alle Handler hier ab und die Oberflaeche blendet den
 * Bereich aus -- der Rest von FA_Control merkt davon nichts. */

#define NAMES_NO_FS_MSG   "No name storage on this device"
/* Seit v1.19 ist der Ort die Kennung: Geraeteordner + Datei. */
#define NAMES_BAD_PATH_MSG "Invalid path - use <DEVICE>/<GAME>.cfg"

/* Die aktive Datei roh ausliefern; das Zerlegen macht die Weboberflaeche. */
static esp_err_t names_get_handler(httpd_req_t *req)
{
    if (!names_ready()) {
        return send_err(req, NAMES_NO_FS_MSG);
    }
    const char *active = names_active();
    if (!active[0]) {
        return send_err(req, "No naming file selected");
    }

    FILE *fp = NULL;
    if (names_open(active, &fp) != ESP_OK) {
        return send_err(req, "Naming file not found");
    }

    httpd_resp_set_type(req, "text/plain");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
    }
    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t namelist_get_handler(httpd_req_t *req)
{
    char *buf = malloc(NAMES_LIST_BUF);
    if (!buf) {
        return send_err(req, "Out of memory");
    }
    /* meldet "fs":0 selbst, wenn nichts gemountet ist */
    names_list_json(buf, NAMES_LIST_BUF);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, buf);
    free(buf);
    return ret;
}

static esp_err_t namesel_post_handler(httpd_req_t *req)
{
    if (!names_ready()) {
        return send_err(req, NAMES_NO_FS_MSG);
    }
    char file[NAMES_MAX_NAME] = "";
    get_param(req, "file", file, sizeof(file));   /* leer = Auswahl aufheben */

    esp_err_t err = names_select(file);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_err(req, "Naming file not found");
    }
    if (err != ESP_OK) {
        return send_err(req, NAMES_BAD_PATH_MSG);
    }
    return send_ok(req);
}

static esp_err_t namedel_post_handler(httpd_req_t *req)
{
    if (!names_ready()) {
        return send_err(req, NAMES_NO_FS_MSG);
    }
    char file[NAMES_MAX_NAME];
    if (!get_param(req, "file", file, sizeof(file))) {
        return send_err(req, "Parameter file missing");
    }
    if (names_delete(file) != ESP_OK) {
        return send_err(req, "Naming file not found");
    }
    return send_ok(req);
}

/* Einziger Handler mit Nutzlast im Body -- alles andere kommt als
 * Query-Parameter. Der Inhalt ist eine Textdatei, die sich schlecht in eine URL
 * pressen laesst; der Dateiname bleibt trotzdem Parameter. */
static esp_err_t nameup_post_handler(httpd_req_t *req)
{
    if (!names_ready()) {
        return send_err(req, NAMES_NO_FS_MSG);
    }
    char file[NAMES_MAX_NAME];
    if (!get_param(req, "file", file, sizeof(file))) {
        return send_err(req, "Parameter file missing");
    }
    if (!names_valid_path(file)) {
        return send_err(req, NAMES_BAD_PATH_MSG);
    }
    if (req->content_len == 0 || req->content_len > NAMES_MAX_FILE_SIZE) {
        return send_err(req, "File empty or too large (max 32 kB)");
    }

    char *body = malloc(req->content_len);
    if (!body) {
        return send_err(req, "Out of memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return send_err(req, "Upload interrupted");
        }
        got += n;
    }

    esp_err_t err = names_write(file, body, got);
    free(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, "Not a plain text file");
    }
    if (err != ESP_OK) {
        return send_err(req, "Write failed - storage full?");
    }
    ESP_LOGI(TAG, "Namensdatei %s hochgeladen (%u Byte)", file, (unsigned)got);
    return send_ok(req);
}

/* Zweistufig, weil die Ablage auf lisy.dev genauso gegliedert ist wie die
 * Partition: ohne "dev" kommen die Geraeteordner, mit "dev" deren Inhalt. */
static esp_err_t namefetchlist_get_handler(httpd_req_t *req)
{
    if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
        return send_err(req, "No internet in AP mode");
    }
    char dev[NAMES_MAX_NAME] = "";
    get_param(req, "dev", dev, sizeof(dev));   /* leer = Geraeteliste */

    /* bis REPO_MAX_FILES = 256 Eintraege x ~10 Byte ("nnn.cfg",) -- der SternFA-
     * Ordner hat 204; mit 1536 Byte war nach gut 140 Namen Schluss. */
    const size_t len = 4096;
    char *buf = malloc(len);
    if (!buf) {
        return send_err(req, "Out of memory");
    }
    esp_err_t err = dev[0] ? names_fetch_list_json(dev, buf, len)
                           : names_fetch_dev_json(buf, len);
    if (err == ESP_ERR_INVALID_ARG) {
        free(buf);
        return send_err(req, "Invalid device name");
    }
    if (err != ESP_OK) {
        free(buf);
        return send_err(req, "Server unreachable");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, buf);
    free(buf);
    return ret;
}

static esp_err_t namefetch_post_handler(httpd_req_t *req)
{
    if (!names_ready()) {
        return send_err(req, NAMES_NO_FS_MSG);
    }
    if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
        return send_err(req, "No internet in AP mode");
    }
    char file[NAMES_MAX_NAME];
    if (!get_param(req, "file", file, sizeof(file))) {
        return send_err(req, "Parameter file missing");
    }
    esp_err_t err = names_fetch(file);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, NAMES_BAD_PATH_MSG);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_err(req, "File too large (max 32 kB)");
    }
    if (err != ESP_OK) {
        return send_err(req, "Download failed");
    }
    return send_ok(req);
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

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* POST /api/fwupload  Body = FA_Control.bin (rohes App-Image).
 * Wie das Update vom Server, nur kommt das Image vom PC:
 *   curl --data-binary @FA_Control.bin "http://<ip>/api/fwupload"
 * Seit v1.31 ein Image fuer alle Boards; ein ?board= von frueher wird ignoriert.
 * fw_image_check() weist fremde und (ausser auf dem Sockel) alte Images ab.
 * Schreibt in die freie OTA-Partition, esp_ota_end() prueft das Image, danach
 * Boot-Partition umstellen und Neustart. Antwort vorher als JSON. */
static esp_err_t fwupload_post_handler(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image size does not fit");
    }
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(h);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    size_t left = req->content_len;
    int timeouts = 0;
    int64_t t0 = esp_timer_get_time();
    while (left > 0 && err == ESP_OK) {
        int n = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (n <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        timeouts = 0;
        if (left == req->content_len) {
            /* Erster Block: App-Beschreibung hinter Image- und Segmentkopf pruefen */
            const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            const char *why = (size_t)n >= off + sizeof(esp_app_desc_t)
                              ? fw_image_check((const esp_app_desc_t *)(buf + off))
                              : "Image header incomplete";
            if (why) {
                free(buf);
                esp_ota_abort(h);
                ESP_LOGW(TAG, "Firmware-Upload abgewiesen: %s", why);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
            }
        }
        err = esp_ota_write(h, buf, n);
        left -= n;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "Firmware-Upload abgebrochen: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    err = esp_ota_end(h);                       /* prueft das Image */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    long ms = (long)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "Firmware-Upload %u Byte nach %s in %ld ms: %s",
             (unsigned)req->content_len, part->label, ms, esp_err_to_name(err));
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"ok\":%d,\"err\":\"%s\",\"bytes\":%u,\"ms\":%ld,\"partition\":\"%s\"}",
             err == ESP_OK ? 1 : 0, esp_err_to_name(err), (unsigned)req->content_len, ms, part->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    if (err == ESP_OK) {
        xTaskCreate(restart_task, "fw_restart", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
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

/* ---- API: Spiel-ROMs (rom_boot.c) ---------------------------------------- */

#define ROMS_NO_FS_MSG "No rom storage on this device - install over USB once"

static esp_err_t romlist_get_handler(httpd_req_t *req)
{
    char *buf = malloc(ROM_BOOT_LIST_BUF);
    if (!buf) {
        return send_err(req, "Out of memory");
    }
    rom_list_json(buf, ROM_BOOT_LIST_BUF);   /* meldet "fs":0 selbst */
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, buf);
    free(buf);
    return ret;
}

static esp_err_t rom_err(httpd_req_t *req, esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return send_ok(req);
    case ESP_ERR_INVALID_SIZE:
        return send_err(req, "Not a game slot - size must be a multiple of 512 bytes, at most 65536");
    case ESP_ERR_INVALID_CRC:
        return send_err(req, "Checksum mismatch - not a valid game image");
    case ESP_ERR_INVALID_ARG:
        return send_err(req, "Invalid name - expected DEVICE/nnn with nnn = 000-255");
    case ESP_ERR_INVALID_STATE:
        return send_err(req, ROMS_NO_FS_MSG);
    case ESP_ERR_NOT_FOUND:
        return send_err(req, "No rom stored under this name");
    default:
        return send_err(req, "Failed - storage full or server unreachable?");
    }
}

/* Der Spielplatz kommt als Body und geht direkt in die Zwischendatei; im RAM
 * steht nie mehr als ein Stueck davon. */
static esp_err_t romup_post_handler(httpd_req_t *req)
{
    if (!rom_boot_ready()) {
        return send_err(req, ROMS_NO_FS_MSG);
    }
    char id[ROM_BOOT_MAX_ID], hw[ROM_BOOT_MAX_HW + 1];
    int game;
    if (!get_param(req, "file", id, sizeof(id)) ||
        !rom_parse_id(id, hw, sizeof(hw), &game)) {
        return rom_err(req, ESP_ERR_INVALID_ARG);
    }
    if (req->content_len == 0 || req->content_len > ROM_BOOT_MAX_IMAGE) {
        return rom_err(req, ESP_ERR_INVALID_SIZE);
    }

    FILE *fp = rom_tmp_open();
    if (!fp) {
        return rom_err(req, ESP_FAIL);
    }
    char chunk[1024];
    size_t left = req->content_len;
    bool ok = true;
    while (left > 0) {
        int n = httpd_req_recv(req, chunk, left < sizeof(chunk) ? left : sizeof(chunk));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 || fwrite(chunk, 1, n, fp) != (size_t)n) {
            ok = false;
            break;
        }
        left -= n;
    }
    fclose(fp);
    if (!ok) {
        rom_tmp_discard();
        return send_err(req, "Upload interrupted");
    }
    return rom_err(req, rom_import_tmp(hw, game));
}

static esp_err_t romdel_post_handler(httpd_req_t *req)
{
    char id[ROM_BOOT_MAX_ID];
    if (!get_param(req, "file", id, sizeof(id))) {
        return rom_err(req, ESP_ERR_INVALID_ARG);
    }
    return rom_err(req, rom_delete(id));
}

/* Wie namefetchlist: ohne "dev" die Geraeteordner, mit "dev" deren .bin */
static esp_err_t romfetchlist_get_handler(httpd_req_t *req)
{
    if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
        return send_err(req, "No internet in AP mode");
    }
    char dev[ROM_BOOT_MAX_HW + 1] = "";
    get_param(req, "dev", dev, sizeof(dev));

    /* bis REPO_MAX_FILES = 256 Eintraege x ~17 Byte ("nnn_xxxxx.bin",) */
    const size_t len = 5120;
    char *buf = malloc(len);
    if (!buf) {
        return send_err(req, "Out of memory");
    }
    esp_err_t err = dev[0] ? rom_fetch_list_json(dev, buf, len)
                           : rom_fetch_dev_json(buf, len);
    if (err != ESP_OK) {
        free(buf);
        return send_err(req, err == ESP_ERR_INVALID_ARG ? "Invalid device name"
                                                        : "Server unreachable");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, buf);
    free(buf);
    return ret;
}

static esp_err_t romfetch_post_handler(httpd_req_t *req)
{
    if (wifi_mgr_get_mode() == WIFI_MGR_MODE_AP) {
        return send_err(req, "No internet in AP mode");
    }
    if (!rom_boot_ready()) {
        return send_err(req, ROMS_NO_FS_MSG);
    }
    char path[REPO_MAX_NAME + ROM_BOOT_MAX_HW + 2];
    if (!get_param(req, "file", path, sizeof(path))) {
        return rom_err(req, ESP_ERR_INVALID_ARG);
    }
    return rom_err(req, rom_fetch(path));
}

/* ---- API: FPGA programmieren (fpga_prog.c, nur Board v7) ------------------ */

/* Jeder Zugriff auf das EPCS haelt das FPGA im Reset: das Spiel startet danach
 * neu, eine FA-Control-Sitzung endet. Die Oberflaeche sagt das vorher. */

static esp_err_t fpga_err(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) {
        return send_ok(req);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_err(req, "FPGA programming in progress");
    }
    fpga_info_t fi;
    fpga_prog_info(&fi);
    return send_err(req, fi.last[0] ? fi.last : esp_err_to_name(err));
}

/* ?probe=1 liest vorher die Kennung des EPCS (startet das FPGA neu). */
static esp_err_t fpgainfo_get_handler(httpd_req_t *req)
{
    if (get_param_int(req, "probe", 0) == 1) {
        esp_err_t err = fpga_prog_probe();
        if (err == ESP_ERR_INVALID_STATE) {
            return fpga_err(req, err);
        }
    }
    fpga_info_t fi;
    fpga_prog_info(&fi);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"conf_done\":%d,\"busy\":%d,\"epcs_ab\":%d,\"epcs_jedec\":%lu,"
             "\"size\":%lu,\"max\":%d,\"last\":\"%s\"}",
             fi.conf_done ? 1 : 0, fi.busy ? 1 : 0, fi.id_ab,
             (unsigned long)fi.jedec, (unsigned long)fi.size, FPGA_IMAGE_MAX, fi.last);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t fpgareconf_post_handler(httpd_req_t *req)
{
    return fpga_err(req, fpga_prog_reconfig());
}

static esp_err_t fpgadiag_get_handler(httpd_req_t *req)
{
    char buf[768];
    esp_err_t err = fpga_prog_diag(buf, sizeof(buf));
    if (err != ESP_OK) {
        return fpga_err(req, err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

typedef struct {
    httpd_req_t *req;
    bool started;
} dump_ctx_t;

static esp_err_t dump_sink(void *ctx, const uint8_t *data, size_t len)
{
    dump_ctx_t *d = ctx;
    d->started = true;
    return httpd_resp_send_chunk(d->req, (const char *)data, len);
}

/* EPCS roh als Datei -- fuer Stufe 0: Bitreihenfolge und Offset gegen die .rbf,
 * und vor/nach einem Upload ab 0x80000, ob die Spiel-ROMs unberuehrt bleiben.
 * addr/len dezimal oder 0x... */
static esp_err_t fpgadump_get_handler(httpd_req_t *req)
{
    char v[16];
    uint32_t addr = get_param(req, "addr", v, sizeof(v)) ? strtoul(v, NULL, 0) : 0;
    uint32_t len = get_param(req, "len", v, sizeof(v)) ? strtoul(v, NULL, 0) : FPGA_IMAGE_MAX;
    if (len == 0 || len > 0x1000000) {
        return send_err(req, "Invalid length");
    }
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"epcs_%06lX.bin\"",
             (unsigned long)addr);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    dump_ctx_t d = { .req = req };
    esp_err_t err = fpga_prog_read(addr, len, dump_sink, &d);
    if (err != ESP_OK && !d.started) {
        httpd_resp_set_hdr(req, "Content-Disposition", "inline");
        return fpga_err(req, err);
    }
    if (err != ESP_OK) {
        /* Mitten im Strom laesst sich kein Status mehr setzen: Verbindung
         * abbrechen, damit der Browser keine verkuerzte Datei fuer ganz haelt. */
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Die Daten kommen als Body und gehen Page fuer Page ins EPCS; im RAM steht nie
 * mehr als ein Stueck davon. ?addr= waehlt den Bereich: 0 (Default) = Image,
 * ab 0x080000 = Spiel-ROMs (fpga_prog.h). Die Bitdrehung folgt dem Bereich: eine
 * .rbf muss gedreht ins EPCS, ROM-Daten ungedreht; ?rev= ueberstimmt das. */
/* Kopf einer .rbf (Cyclone IV/10, komprimiert wie unkomprimiert): 32 Byte 0xFF,
 * dann 0x6A. In EPCS-Reihenfolge (.rpd) ist 0x6A gespiegelt 0x56. Damit faellt
 * eine Datei im falschen Abschnitt auf, BEVOR etwas geloescht ist: eine ROM-Datei
 * als Image wuerde gedreht ueber den Bitstream geschrieben, eine .rbf als ROMs
 * ueber die Spiele. */
#define RBF_HEAD 33

static bool rbf_head(const uint8_t *b, uint8_t sync)
{
    for (int i = 0; i < RBF_HEAD - 1; i++) {
        if (b[i] != 0xFF) {
            return false;
        }
    }
    return b[RBF_HEAD - 1] == sync;
}

static esp_err_t fpgaup_post_handler(httpd_req_t *req)
{
    char v[16];
    uint32_t addr = get_param(req, "addr", v, sizeof(v)) ? strtoul(v, NULL, 0) : 0;
    bool rev = get_param_int(req, "rev", addr == 0 ? 1 : 0) == 1;

    /* Erst den Kopf lesen und pruefen, dann erst das FPGA anfassen. */
    char chunk[1024];
    size_t head = req->content_len < RBF_HEAD ? req->content_len : RBF_HEAD;
    size_t got = 0;
    while (got < head) {
        int n = httpd_req_recv(req, chunk + got, head - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return send_err(req, "Upload interrupted");
        }
        got += n;
    }
    const uint8_t *h = (const uint8_t *)chunk;
    bool is_image = got == RBF_HEAD && (rbf_head(h, 0x6A) || rbf_head(h, 0x56));
    if (addr == 0 && !(got == RBF_HEAD && rbf_head(h, rev ? 0x6A : 0x56))) {
        return send_err(req, "Not an FPGA image (.rbf starts with 32 x FF, then 6A) - "
                             "game ROMs go to WRITE GAME ROMS");
    }
    if (addr != 0 && is_image) {
        return send_err(req, "This is an FPGA image (.rbf) - write it with WRITE, not as game ROMs");
    }

    esp_err_t err = fpga_prog_write_begin(addr, req->content_len, rev);
    if (err != ESP_OK) {
        return fpga_err(req, err);
    }
    err = fpga_prog_write_chunk(h, got);
    if (err != ESP_OK) {
        fpga_prog_write_abort();
        return fpga_err(req, err);
    }
    size_t left = req->content_len - got;
    while (left > 0) {
        int n = httpd_req_recv(req, chunk, left < sizeof(chunk) ? left : sizeof(chunk));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            fpga_prog_write_abort();
            return fpga_err(req, ESP_FAIL);
        }
        err = fpga_prog_write_chunk((const uint8_t *)chunk, n);
        if (err != ESP_OK) {
            fpga_prog_write_abort();
            return fpga_err(req, err);
        }
        left -= n;
    }
    return fpga_err(req, fpga_prog_write_end());
}

/* ---- API: Virtual JTAG (Stufe 2 von "FA-Control ueber JTAG") -------------- */

/* Ein LISY-Befehl ueber fa_vjtag_link als JSON-Feld: "name":{"tx":n,"rx":n,"us":t,"str":"..","hex":".."} */
static int vjtag_lisy_field(const fpga_vjtag_hub_t *hub, const char *name, const uint8_t *cmd,
                            int n, char *out, size_t max)
{
    uint8_t rx[80];
    int scans = 0;
    int64_t t0 = esp_timer_get_time();
    int got = fpga_vjtag_xfer(hub, cmd, n, rx, sizeof(rx), 50, &scans);
    int64_t us = esp_timer_get_time() - t0;
    int stored = got < (int)sizeof(rx) ? got : (int)sizeof(rx);
    int len = snprintf(out, max, ",\"%s\":{\"rx\":%d,\"scans\":%d,\"us\":%lld,\"str\":\"",
                       name, got, scans, (long long)us);
    for (int i = 0; i < stored && len < (int)max - 8; i++) {
        char c = (char)rx[i];
        if (c == 0) {
            break;
        }
        out[len++] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? c : '?';
    }
    len += snprintf(out + len, max - len, "\",\"hex\":\"");
    for (int i = 0; i < stored && i < 24 && len < (int)max - 8; i++) {
        len += snprintf(out + len, max - len, "%02X", rx[i]);
    }
    len += snprintf(out + len, max - len, "\"}");
    return len < (int)max ? len : (int)max - 1;
}

/* fa_vjtag_link (Stufe 3): Kennung, Bytes, Monitor, Steuerbit. ?dead=1 prueft
 * zusaetzlich die Totmannzeit (Bit setzen, 2,5 s nichts tun, lesen: muss 0 sein). */
static int vjtag_link_test(httpd_req_t *req, uint32_t id, char *out, size_t max)
{
    fpga_vjtag_hub_t hub;
    fpga_vjtag_probe(&hub);
    int len = snprintf(out, max, ",\"id\":\"0x%08lX\",\"node\":\"fa_vjtag_link\"", (unsigned long)id);

    static const uint8_t c_hw[]  = { LISY_CMD_G_HW };
    static const uint8_t c_ver[] = { LISY_CMD_G_LISY_VER };
    static const uint8_t c_api[] = { LISY_CMD_G_API_VER };
    static const uint8_t c_mon[] = { LISY_CMD_MONITOR };
    len += vjtag_lisy_field(&hub, "hw", c_hw, 1, out + len, max - len);
    len += vjtag_lisy_field(&hub, "ver", c_ver, 1, out + len, max - len);
    len += vjtag_lisy_field(&hub, "api", c_api, 1, out + len, max - len);
    len += vjtag_lisy_field(&hub, "mon", c_mon, 1, out + len, max - len);

    int v1 = -1, v0 = -1, vd = -1;
    fpga_vjtag_ctrl(&hub, 1, &v1);
    fpga_vjtag_ctrl(&hub, 0, &v0);
    if (get_param_int(req, "dead", 0)) {
        fpga_vjtag_ctrl(&hub, 1, NULL);
        vTaskDelay(pdMS_TO_TICKS(2500));
        fpga_vjtag_ctrl(&hub, -1, &vd);
    }
    len += snprintf(out + len, max - len, ",\"ctrl_set1\":%d,\"ctrl_set0\":%d,\"ctrl_dead\":%d,\"ok\":1}",
                    v1, v0, vd);
    return len < (int)max ? len : (int)max - 1;
}

/* ---- API: beliebiges Design per JTAG in den SRAM ------------------------- */

typedef struct {
    httpd_req_t *req;
    size_t       left;      /* noch zu empfangen */
    uint8_t      buf[2048];
} http_src_t;

static int http_recv_some(http_src_t *s, uint8_t *dst, size_t max)
{
    for (int tries = 0; tries < 5; tries++) {
        int n = httpd_req_recv(s->req, (char *)dst, max);
        if (n > 0) {
            s->left -= n;
            return n;
        }
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return -1;
        }
    }
    return -1;
}

static int http_src(void *ctx, const uint8_t **p)
{
    http_src_t *s = ctx;
    if (s->left == 0) {
        return 0;
    }
    int n = http_recv_some(s, s->buf, s->left < sizeof(s->buf) ? s->left : sizeof(s->buf));
    *p = s->buf;
    return n;
}

/* POST /api/fpgasram  Body = .fjz (make_jtag_blob.py --sof ... --out ...):
 *   "FJZ1", u16 CHECK_STATUS-Laenge, u16 CONF_DONE-Bit (little endian), zlib-Strom.
 * Der Strom wird beim Empfang entpackt und sofort geschoben, nichts zwischengespeichert.
 * Nur SRAM: nach Aus/Ein bootet wieder das EPCS-Design. Nur Designs laden, die fuer
 * genau dieses Board gebaut sind -- die IDCODE sagt nur, welches Die es ist. */
static esp_err_t fpgasram_post_handler(httpd_req_t *req)
{
    http_src_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    s->req = req;
    s->left = req->content_len;

    uint8_t hdr[8];
    size_t got = 0;
    while (got < sizeof(hdr) && s->left > 0) {
        int n = http_recv_some(s, hdr + got, sizeof(hdr) - got);
        if (n < 0) {
            break;
        }
        got += n;
    }
    if (got < sizeof(hdr) || memcmp(hdr, "FJZ1", 4) != 0) {
        free(s);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a .fjz file (FJZ1 header)");
    }
    int cs_len = hdr[4] | (hdr[5] << 8);
    int cs_bit = hdr[6] | (hdr[7] << 8);

    /* Das neue Design startet von vorn: eine laufende Verbindung ist damit zu Ende.
     * Sauber freigeben wie DISCONNECT, sonst hielte der Watchdog sie fuer lebendig. */
    fa_connect_release();
    lisy_lamp_bitmap_clear();
    s_switches_initialized = false;
    memset(s_disp_text, 0, sizeof(s_disp_text));

    fpga_jtag_sram_result_t r;
    esp_err_t err = fpga_jtag_load_sram(http_src, s, cs_len, cs_bit, &r);
    fpga_vjtag_link_invalidate();   /* neues Design, neuer Hub */
    /* Rest verwerfen, falls abgebrochen wurde -- sonst haengt der Client */
    while (s->left > 0 && http_recv_some(s, s->buf, s->left < sizeof(s->buf) ? s->left : sizeof(s->buf)) > 0) {
    }
    free(s);

    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%d,\"err\":\"%s\",\"idcode\":\"0x%08lX\",\"conf_done\":%d,"
             "\"bytes\":%u,\"ms\":%lld,\"cs_len\":%d,\"cs_bit\":%d}",
             err == ESP_OK ? 1 : 0, esp_err_to_name(err), (unsigned long)r.idcode,
             r.conf_done, (unsigned)r.bytes, (long long)r.ms, cs_len, cs_bit);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* GET /api/vjtag[?n=1000][&dead=1] -- Hub abfragen; findet sich ein Virtual-JTAG-
 * Knoten, nach Kennung unterscheiden:
 *   "GF8x" Testknoten vjtag_probe: Kennung, n Rueckkanal-Scans, Zeiten
 *   "FALx" fa_vjtag_link: LISY-Befehle 0/1/2/110 und das Steuerbit (s. o.)
 * Auf einem Design ohne Hub passiert am TAP nichts ausser USER0/USER1 ins Leere. */
static esp_err_t vjtag_get_handler(httpd_req_t *req)
{
    int count = get_param_int(req, "n", 1000);
    if (count < 1) {
        count = 1;
    } else if (count > 100000) {
        count = 100000;
    }

    fpga_vjtag_hub_t hub;
    esp_err_t perr = fpga_vjtag_probe(&hub);

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
                       "{\"idcode\":\"0x%08lX\",\"hub_info\":\"0x%08lX\",\"nodes\":%d,"
                       "\"m\":%d,\"n\":%d,\"addr\":%d,\"node_info\":[",
                       (unsigned long)hub.idcode, (unsigned long)hub.hub_info,
                       hub.nodes, hub.m, hub.n, hub.addr);
    for (int k = 0; k < hub.nodes && k < FPGA_VJTAG_MAX_NODES && len < (int)sizeof(buf); k++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s\"0x%08lX\"",
                        k ? "," : "", (unsigned long)hub.node_info[k]);
    }
    if (len < (int)sizeof(buf)) {
        len += snprintf(buf + len, sizeof(buf) - len, "]");
    }

    uint32_t id = (perr == ESP_OK) ? fpga_vjtag_read_id(&hub) : 0;
    if (get_param_int(req, "info", 0) && len < (int)sizeof(buf)) {
        /* Nur Hub und Kennung -- keine LISY-Bytes, kein Steuerbit: stoert eine
         * laufende Verbindung nicht (Menue FPGA auf dem JTAG-Piggyback). */
        const char *node = FPGA_VJTAG_IS_LINK(id)          ? "fa_vjtag_link" :
                           (id >> 8) == FPGA_VJTAG_ID_PROBE ? "vjtag_probe"   :
                           perr == ESP_OK                   ? "unknown"       : "none";
        /* Art des FA-Designs: full / monitor / control / service */
        const char *kind = fpga_vjtag_kind_str(id);
        len += snprintf(buf + len, sizeof(buf) - len,
                        ",\"id\":\"0x%08lX\",\"node\":\"%s\",\"kind\":\"%s\",\"ok\":1}",
                        (unsigned long)id, node, kind);
    } else if (perr == ESP_OK && FPGA_VJTAG_IS_LINK(id) && len < (int)sizeof(buf)) {
        len += vjtag_link_test(req, id, buf + len, sizeof(buf) - len);
    } else if (perr == ESP_OK && len < (int)sizeof(buf)) {
        fpga_vjtag_test_t t;
        esp_err_t terr = fpga_vjtag_test(&hub, count, &t);
        /* Nutzdaten je Sekunde: 4 Byte je Rueckkanal-Scan */
        long long bps = t.echo_us > 0 ? (long long)t.echo_total * 4 * 1000000 / t.echo_us : 0;
        len += snprintf(buf + len, sizeof(buf) - len,
                        ",\"id\":\"0x%08lX\",\"echo_total\":%d,\"echo_ok\":%d,"
                        "\"fail_out\":\"0x%08lX\",\"fail_in\":\"0x%08lX\","
                        "\"echo_us\":%lld,\"bytes_per_s\":%lld,\"session_us\":%lld,\"ok\":%d}",
                        (unsigned long)t.id, t.echo_total, t.echo_ok,
                        (unsigned long)t.fail_out, (unsigned long)t.fail_in,
                        (long long)t.echo_us, bps, (long long)t.session_us,
                        terr == ESP_OK ? 1 : 0);
    } else if (len < (int)sizeof(buf)) {
        len += snprintf(buf + len, sizeof(buf) - len, ",\"ok\":0,\"err\":\"%s\"}",
                        esp_err_to_name(perr));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ---- Start --------------------------------------------------------------- */

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* Muss >= Anzahl der Eintraege in uris[] sein, sonst werden die letzten
     * stillschweigend nicht registriert und laufen in den Captive-Portal-
     * Fallback. Beim Hinzufuegen eines Endpunkts hier mitzaehlen. */
    cfg.max_uri_handlers = 44;   /* 33 gemeinsame Eintraege + 5 v7 bzw. 2 JTAG = hoechstens 38 */
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
        { .uri = "/api/monitor", .method = HTTP_POST, .handler = monitor_post_handler },
        { .uri = "/api/monitor", .method = HTTP_GET,  .handler = monitor_get_handler },
        { .uri = "/api/monsw",   .method = HTTP_POST, .handler = monsw_post_handler },
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
        { .uri = "/api/fwupload",.method = HTTP_POST, .handler = fwupload_post_handler },
        { .uri = "/api/names",   .method = HTTP_GET,  .handler = names_get_handler },
        { .uri = "/api/namelist",.method = HTTP_GET,  .handler = namelist_get_handler },
        { .uri = "/api/namesel", .method = HTTP_POST, .handler = namesel_post_handler },
        { .uri = "/api/namedel", .method = HTTP_POST, .handler = namedel_post_handler },
        { .uri = "/api/nameup",  .method = HTTP_POST, .handler = nameup_post_handler },
        { .uri = "/api/namefetchlist", .method = HTTP_GET,  .handler = namefetchlist_get_handler },
        { .uri = "/api/namefetch",     .method = HTTP_POST, .handler = namefetch_post_handler },
        { .uri = "/api/romlist", .method = HTTP_GET,  .handler = romlist_get_handler },
        { .uri = "/api/romup",   .method = HTTP_POST, .handler = romup_post_handler },
        { .uri = "/api/romdel",  .method = HTTP_POST, .handler = romdel_post_handler },
        { .uri = "/api/romfetchlist", .method = HTTP_GET,  .handler = romfetchlist_get_handler },
        { .uri = "/api/romfetch",     .method = HTTP_POST, .handler = romfetch_post_handler },
        { .uri = "/logo.png",    .method = HTTP_GET,  .handler = logo_get_handler },
    };
    /* FPGA-Endpunkte nur auf dem passenden Board (board.c). Auf den anderen fallen
     * sie in den Wildcard-Eintrag, wie vor v1.31 in den anderen Builds. */
    const httpd_uri_t uris_v7[] = {
        { .uri = "/api/fpgainfo",   .method = HTTP_GET,  .handler = fpgainfo_get_handler },
        { .uri = "/api/fpgareconf", .method = HTTP_POST, .handler = fpgareconf_post_handler },
        { .uri = "/api/fpgadump",   .method = HTTP_GET,  .handler = fpgadump_get_handler },
        { .uri = "/api/fpgadiag",   .method = HTTP_GET,  .handler = fpgadiag_get_handler },
        { .uri = "/api/fpgaup",     .method = HTTP_POST, .handler = fpgaup_post_handler },
    };
    const httpd_uri_t uris_jtag[] = {
        { .uri = "/api/vjtag",      .method = HTTP_GET,  .handler = vjtag_get_handler },
        { .uri = "/api/fpgasram",   .method = HTTP_POST, .handler = fpgasram_post_handler },
    };
    /* Der Wildcard-Eintrag faengt alles Uebrige und muss zuletzt stehen. */
    const httpd_uri_t uri_root =
        { .uri = "/*",           .method = HTTP_GET,  .handler = root_get_handler };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    if (board_type() == BOARD_V7) {
        for (size_t i = 0; i < sizeof(uris_v7) / sizeof(uris_v7[0]); i++) {
            httpd_register_uri_handler(server, &uris_v7[i]);
        }
    }
    if (board_has_jtag()) {
        for (size_t i = 0; i < sizeof(uris_jtag) / sizeof(uris_jtag[0]); i++) {
            httpd_register_uri_handler(server, &uris_jtag[i]);
        }
    }
    httpd_register_uri_handler(server, &uri_root);

    ESP_LOGI(TAG, "Webserver gestartet (Port 80)");
    return ESP_OK;
}
