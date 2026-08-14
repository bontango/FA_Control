#include "names.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "fa_connect.h"
#include "repo.h"

static const char *TAG = "names";

#define NAMES_PARTITION "names"
#define NAMES_MOUNT     "/names"
#define NAMES_BASE_URL  "https://lisy.dev/swrep/misc/FA_Control/names/"

_Static_assert(NAMES_MAX_ID + sizeof(NAMES_SUFFIX) - 1 <= NAMES_MAX_NAME,
               "Kennung plus Endung muss in einen Dateinamen passen");

/* Reicht fuer NAMES_MOUNT "/" + Dateiname + NUL. */
#define NAMES_PATH_LEN  (sizeof(NAMES_MOUNT) + NAMES_MAX_NAME + 1)

/* app_config.h kann names.h nicht einbinden (Zirkel), die Groesse steht dort
 * deshalb als Zahl. Hier faellt es auf, wenn eine der beiden wandert. */
_Static_assert(sizeof(((app_config_t *)0)->names_file) == NAMES_MAX_NAME,
               "app_config_t.names_file und NAMES_MAX_NAME muessen zusammenpassen");

static bool s_ready;

/* ---- Pfade und Namen ------------------------------------------------------ */

bool names_valid_filename(const char *file)
{
    return repo_valid_filename(file, NAMES_SUFFIX, NAMES_MAX_NAME);
}

static void full_path(const char *file, char *out, size_t len)
{
    snprintf(out, len, NAMES_MOUNT "/%s", file);
}

/* Datei ohne Ruecksicht auf Gross-/Kleinschreibung suchen und den TATSAECHLICHEN
 * Namen zurueckgeben. LittleFS unterscheidet Gross- und Kleinschreibung, die
 * Kennung soll es nicht -- und wer eine Datei ablegt, soll die Schreibweise
 * behalten duerfen, in der er sie benannt hat. Deshalb wird hier verglichen
 * statt beim Speichern normalisiert.
 *
 * out darf NULL sein, wenn nur interessiert, ob es die Datei gibt. */
static bool names_find(const char *wanted, char *out, size_t len)
{
    if (!s_ready || !wanted || !wanted[0]) {
        return false;
    }
    DIR *dir = opendir(NAMES_MOUNT);
    if (!dir) {
        return false;
    }
    bool hit = false;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcasecmp(de->d_name, wanted) == 0) {
            if (out) {
                strlcpy(out, de->d_name, len);
            }
            hit = true;
            break;
        }
    }
    closedir(dir);
    return hit;
}

/* ---- Mount ---------------------------------------------------------------- */

esp_err_t names_init(void)
{
    /* Erst in der Tabelle nachsehen: auf einem per OTA aktualisierten Geraet
     * gibt es die Partition nicht, und esp_littlefs_mount() wuerde das als
     * Fehler protokollieren, obwohl es der vorgesehene Normalfall ist. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NAMES_PARTITION);
    if (!part) {
        ESP_LOGW(TAG, "Keine Partition '%s' -- Namen sind auf diesem Geraet "
                      "nicht verfuegbar (Partitionstabelle per USB flashen)",
                 NAMES_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = NAMES_MOUNT,
        .partition_label        = NAMES_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS-Mount fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(NAMES_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "Namensdateien gemountet: %u von %u Byte belegt",
                 (unsigned)used, (unsigned)total);
    }
    s_ready = true;

    /* Gespeicherte Auswahl pruefen -- die Datei kann inzwischen geloescht
     * worden sein, dann faellt die Auswahl weg statt ins Leere zu zeigen. */
    if (g_cfg.names_file[0] && !names_find(g_cfg.names_file, NULL, 0)) {
        ESP_LOGW(TAG, "Gewaehlte Datei '%s' fehlt, Auswahl verworfen",
                 g_cfg.names_file);
        g_cfg.names_file[0] = '\0';
        app_config_save();
    }
    return ESP_OK;
}

bool names_ready(void)
{
    return s_ready;
}

/* ---- Auswahl -------------------------------------------------------------- */

const char *names_active(void)
{
    return g_cfg.names_file;
}

/* Auswahl uebernehmen, aber nur schreiben, wenn sie sich wirklich aendert --
 * sonst faende bei jedem Verbinden ein Flash-Schreibvorgang statt. */
static esp_err_t set_active(const char *file)
{
    const char *neu = file ? file : "";
    if (strcmp(g_cfg.names_file, neu) == 0) {
        return ESP_OK;
    }
    strlcpy(g_cfg.names_file, neu, sizeof(g_cfg.names_file));
    return app_config_save();
}

esp_err_t names_select(const char *file)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!file || !file[0]) {
        return set_active(NULL);
    }
    if (!names_valid_filename(file)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Ueber names_find, damit auch eine abweichend geschriebene Anfrage trifft --
     * gespeichert wird dann der tatsaechliche Name. */
    char actual[NAMES_MAX_NAME];
    if (!names_find(file, actual, sizeof(actual))) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = set_active(actual);
    ESP_LOGI(TAG, "Aktive Namensdatei: %s", g_cfg.names_file);
    return err;
}

/* ---- Kennung der Anlage --------------------------------------------------- */

/* Nur [A-Za-z0-9_-] uebernehmen; die Schreibweise bleibt, die Kennung wird so
 * angezeigt, wie sie hier entsteht. Rueckgabe = geschriebene Zeichen. */
static size_t sanitize(const char *in, char *out, size_t len)
{
    size_t w = 0;
    for (size_t i = 0; in[i] && w + 1 < len; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            out[w++] = c;
        }
    }
    out[w] = '\0';
    return w;
}

static bool all_digits(const char *s)
{
    if (!s[0]) {
        return false;
    }
    for (size_t i = 0; s[i]; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

void names_game_id(char *out, size_t len)
{
    out[0] = '\0';
    const fa_conn_info_t *ci = fa_connect_info();

    char hw[sizeof(ci->hw)];
    char game[sizeof(ci->game)];
    if (sanitize(ci->hw, hw, sizeof(hw)) == 0) {
        return;
    }
    if (sanitize(ci->game, game, sizeof(game)) == 0) {
        return;
    }

    /* Reine Zahlen werden dreistellig -- so heisst Airborne Avenger auf AtariFA
     * "AtariFA_002" und nicht "AtariFA_2". Alles andere bleibt, wie es kam:
     * was eine kuenftige Gegenstelle als Kennung meldet, ist nicht zu raten. */
    if (all_digits(game) && atoi(game) < 1000) {
        snprintf(out, len, "%s_%03d", hw, atoi(game));
    } else {
        snprintf(out, len, "%s_%s", hw, game);
    }
}

void names_select_for_id(void)
{
    if (!s_ready) {
        return;
    }
    char id[NAMES_MAX_ID];
    names_game_id(id, sizeof(id));
    if (!id[0]) {
        ESP_LOGI(TAG, "Keine Kennung gemeldet, Auswahl unveraendert");
        return;
    }

    char wanted[NAMES_MAX_NAME];
    snprintf(wanted, sizeof(wanted), "%s%s", id, NAMES_SUFFIX);

    char actual[NAMES_MAX_NAME];
    if (names_find(wanted, actual, sizeof(actual))) {
        set_active(actual);
        ESP_LOGI(TAG, "Kennung %s -> %s", id, actual);
    } else {
        /* Bewusst leeren statt die alte Auswahl stehenzulassen: nach einem
         * Spielwechsel zeigte die Oberflaeche sonst weiter die Namen des
         * vorigen Spiels. Falsche Beschriftung ist schlimmer als keine. */
        set_active(NULL);
        ESP_LOGI(TAG, "Kennung %s: keine Datei %s, keine Namen", id, wanted);
    }
}

/* ---- Dateien -------------------------------------------------------------- */

esp_err_t names_delete(const char *file)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_filename(file)) {
        return ESP_ERR_INVALID_ARG;
    }
    char actual[NAMES_MAX_NAME];
    if (!names_find(file, actual, sizeof(actual))) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[NAMES_PATH_LEN];
    full_path(actual, path, sizeof(path));
    if (unlink(path) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strcasecmp(g_cfg.names_file, actual) == 0) {
        set_active(NULL);
    }
    ESP_LOGI(TAG, "%s geloescht", actual);
    return ESP_OK;
}

esp_err_t names_open(const char *file, FILE **fp)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_filename(file)) {
        return ESP_ERR_INVALID_ARG;
    }
    char actual[NAMES_MAX_NAME];
    if (!names_find(file, actual, sizeof(actual))) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[NAMES_PATH_LEN];
    full_path(actual, path, sizeof(path));
    *fp = fopen(path, "r");
    return *fp ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Grobe Plausibilitaet: keine Steuerzeichen ausser Tab und Zeilenumbruch. Bytes
 * ab 0x80 bleiben erlaubt, damit ein Name wie "Rechte Stossstange" auch mit
 * Umlaut hochgeladen werden kann -- die Seite ist UTF-8. Das ist kein Parser,
 * es soll nur verhindern, dass versehentlich ein Binaerbrocken die Partition
 * fuellt; solche Dateien enthalten praktisch immer Nullbytes. */
static bool looks_like_text(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

esp_err_t names_write(const char *file, const char *data, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_filename(file)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > NAMES_MAX_FILE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!looks_like_text(data, len)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Liegt dieselbe Datei bereits in anderer Schreibweise, wird SIE ersetzt.
     * Sonst staenden "AtariFA_002.cfg" und "atarifa_002.cfg" nebeneinander und
     * beide passten auf dieselbe Kennung -- welche gilt, waere Zufall. */
    char actual[NAMES_MAX_NAME];
    if (!names_find(file, actual, sizeof(actual))) {
        strlcpy(actual, file, sizeof(actual));
    }

    char path[NAMES_PATH_LEN];
    full_path(actual, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    if (written != len) {
        unlink(path);   /* halbe Datei ist schlimmer als keine */
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s gespeichert (%u Byte)", actual, (unsigned)len);
    return ESP_OK;
}

/* ---- Nachladen von lisy.dev ---------------------------------------------- */

esp_err_t names_fetch_list_json(char *out, size_t out_len)
{
    return repo_list_json(NAMES_BASE_URL, NAMES_SUFFIX, out, out_len);
}

esp_err_t names_fetch(const char *file)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_filename(file)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Wie beim Upload: eine vorhandene Datei gleichen Namens ersetzen, statt
     * eine zweite Schreibweise danebenzulegen. */
    char actual[NAMES_MAX_NAME];
    if (!names_find(file, actual, sizeof(actual))) {
        strlcpy(actual, file, sizeof(actual));
    }

    char path[NAMES_PATH_LEN];
    full_path(actual, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return ESP_FAIL;
    }
    esp_err_t err = repo_download_to_file(NAMES_BASE_URL, file, fp,
                                          NAMES_MAX_FILE_SIZE);
    fclose(fp);
    if (err != ESP_OK) {
        /* Abgebrochener Download hinterlaesst sonst eine halbe Datei, die beim
         * naechsten Blick in die Liste wie eine gueltige aussaehe. */
        unlink(path);
    }
    return err;
}

/* ---- Auflisten ------------------------------------------------------------ */

esp_err_t names_list_json(char *out, size_t out_len)
{
    size_t w = snprintf(out, out_len, "{\"fs\":%d,\"active\":\"%s\",\"files\":[",
                        s_ready ? 1 : 0, g_cfg.names_file);
    if (!s_ready) {
        snprintf(out + w, out_len - w, "]}");
        return ESP_OK;
    }

    DIR *dir = opendir(NAMES_MOUNT);
    if (!dir) {
        snprintf(out + w, out_len - w, "]}");
        return ESP_FAIL;
    }

    /* Begrenzt, damit das JSON in den Puffer des Aufrufers passt: ein Eintrag ist
     * hoechstens ~45 Byte, und es muss noch Platz fuer den Abschluss bleiben. */
    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < NAMES_MAX_LIST && w + 64 < out_len) {
        if (!names_valid_filename(de->d_name)) {
            continue;
        }
        char path[NAMES_PATH_LEN];
        struct stat st;
        full_path(de->d_name, path, sizeof(path));
        long size = (stat(path, &st) == 0) ? (long)st.st_size : 0;
        w += snprintf(out + w, out_len - w, "%s{\"n\":\"%s\",\"s\":%ld}",
                      count ? "," : "", de->d_name, size);
        count++;
    }
    closedir(dir);

    if (w < out_len) {
        size_t total = 0, used = 0;
        esp_littlefs_info(NAMES_PARTITION, &total, &used);
        snprintf(out + w, out_len - w, "],\"total\":%u,\"used\":%u}",
                 (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}
