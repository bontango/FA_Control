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

/* "AtariFA/002.cfg" in seine beiden Teile zerlegen. Genau ein Schraegstrich,
 * beide Seiten nicht leer -- alles andere ist kein gueltiger Ort. */
static bool split_path(const char *path, char *dev, size_t devlen,
                       char *file, size_t filelen)
{
    if (!path) {
        return false;
    }
    const char *slash = strchr(path, '/');
    if (!slash || slash == path || strchr(slash + 1, '/')) {
        return false;
    }
    size_t dlen = slash - path;
    if (dlen >= devlen) {
        return false;
    }
    memcpy(dev, path, dlen);
    dev[dlen] = '\0';
    strlcpy(file, slash + 1, filelen);
    return true;
}

bool names_valid_path(const char *path)
{
    if (!path || strlen(path) >= NAMES_MAX_NAME) {
        return false;
    }
    char dev[NAMES_MAX_NAME], file[NAMES_MAX_NAME];
    if (!split_path(path, dev, sizeof(dev), file, sizeof(file))) {
        return false;
    }
    return repo_valid_segment(dev, NAMES_MAX_NAME) &&
           repo_valid_filename(file, NAMES_SUFFIX, NAMES_MAX_NAME);
}

/* Flacher Name aus der Zeit vor v1.18. Nur zum Loeschen zugelassen -- siehe
 * names_delete(). */
static bool names_valid_legacy(const char *file)
{
    return file && !strchr(file, '/') &&
           repo_valid_filename(file, NAMES_SUFFIX, NAMES_MAX_NAME);
}

static void full_path(const char *file, char *out, size_t len)
{
    snprintf(out, len, NAMES_MOUNT "/%s", file);
}

/* Einen Eintrag ohne Ruecksicht auf Gross-/Kleinschreibung suchen und den
 * TATSAECHLICHEN Namen zurueckgeben. LittleFS unterscheidet Gross- und
 * Kleinschreibung, die Kennung soll es nicht -- und wer eine Datei ablegt, soll
 * die Schreibweise behalten duerfen, in der er sie benannt hat. Deshalb wird
 * hier verglichen statt beim Speichern normalisiert.
 *
 * out darf NULL sein, wenn nur interessiert, ob es den Eintrag gibt. */
static bool find_entry(const char *dirpath, const char *wanted,
                       char *out, size_t len)
{
    if (!wanted || !wanted[0]) {
        return false;
    }
    DIR *dir = opendir(dirpath);
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

/* Wie find_entry(), aber ueber beide Ebenen: erst den Geraeteordner, dann die
 * Datei darin. Zurueck kommt der Pfad in der Schreibweise, in der er wirklich
 * auf der Partition steht.
 *
 * Ein Name OHNE Schraegstrich wird flach im Wurzelverzeichnis gesucht -- das
 * sind die Altbestaende, die nur noch geloescht werden koennen. Welche der
 * beiden Formen an dieser Stelle ueberhaupt zulaessig ist, hat der Aufrufer mit
 * names_valid_path() bzw. names_valid_legacy() bereits entschieden. */
static bool names_find(const char *wanted, char *out, size_t len)
{
    if (!s_ready || !wanted || !wanted[0]) {
        return false;
    }
    if (!strchr(wanted, '/')) {
        return find_entry(NAMES_MOUNT, wanted, out, len);
    }

    char dev[NAMES_MAX_NAME], file[NAMES_MAX_NAME];
    if (!split_path(wanted, dev, sizeof(dev), file, sizeof(file))) {
        return false;
    }

    char actual_dev[NAMES_MAX_NAME];
    if (!find_entry(NAMES_MOUNT, dev, actual_dev, sizeof(actual_dev))) {
        return false;
    }

    char devpath[NAMES_PATH_LEN];
    full_path(actual_dev, devpath, sizeof(devpath));

    char actual_file[NAMES_MAX_NAME];
    if (!find_entry(devpath, file, actual_file, sizeof(actual_file))) {
        return false;
    }
    if (out) {
        snprintf(out, len, "%s/%s", actual_dev, actual_file);
    }
    return true;
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
     * worden sein, dann faellt die Auswahl weg statt ins Leere zu zeigen.
     *
     * Geprueft wird auch die FORM: nach dem Umstieg auf Geraeteordner (v1.19)
     * steht in NVS moeglicherweise noch ein flacher Altname. Den findet
     * names_find() im Wurzelverzeichnis durchaus -- verwendbar ist er trotzdem
     * nicht, names_open() weist ihn ab. Die Auswahl saehe dann gueltig aus und
     * lieferte doch keine Namen. */
    if (g_cfg.names_file[0] &&
        (!names_valid_path(g_cfg.names_file) ||
         !names_find(g_cfg.names_file, NULL, 0))) {
        ESP_LOGW(TAG, "Gewaehlte Datei '%s' fehlt oder ist ein Altname, "
                      "Auswahl verworfen", g_cfg.names_file);
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
    if (!names_valid_path(file)) {
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

    /* Reine Zahlen werden dreistellig -- so liegt Airborne Avenger auf AtariFA
     * unter "AtariFA/002" und nicht unter "AtariFA/2". Alles andere bleibt, wie
     * es kam: was eine kuenftige Gegenstelle als Kennung meldet, ist nicht zu
     * raten. */
    if (all_digits(game) && atoi(game) < 1000) {
        snprintf(out, len, "%s/%03d", hw, atoi(game));
    } else {
        snprintf(out, len, "%s/%s", hw, game);
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

/* Leeren Geraeteordner entfernen. Ohne das bliebe nach dem Loeschen der letzten
 * Datei ein Ordner stehen, den ueber die Oberflaeche niemand mehr loswird. */
static void rmdir_if_empty(const char *dev)
{
    char devpath[NAMES_PATH_LEN];
    full_path(dev, devpath, sizeof(devpath));
    DIR *dir = opendir(devpath);
    if (!dir) {
        return;
    }
    bool empty = true;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(dir);
    if (empty && rmdir(devpath) == 0) {
        ESP_LOGI(TAG, "Leerer Ordner %s entfernt", dev);
    }
}

/* Nimmt beide Formen an: den Ort "AtariFA/002.cfg" und den flachen Altnamen
 * "AtariFA_002.cfg". Letzterer laesst sich nur noch loeschen, nicht mehr
 * verwenden -- sonst blieben die Altbestaende unsichtbar liegen und belegten
 * Flash, ohne dass jemand an sie herankaeme. */
esp_err_t names_delete(const char *file)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_path(file) && !names_valid_legacy(file)) {
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

    char dev[NAMES_MAX_NAME], leaf[NAMES_MAX_NAME];
    if (split_path(actual, dev, sizeof(dev), leaf, sizeof(leaf))) {
        rmdir_if_empty(dev);
    }
    return ESP_OK;
}

esp_err_t names_open(const char *file, FILE **fp)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_path(file)) {
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

/* Geraeteordner anlegen, falls er fehlt. Gibt zurueck, ob er neu entstanden ist
 * -- dann muss er beim Scheitern des Schreibens wieder weg. */
static bool ensure_dir(const char *dev, bool *created)
{
    *created = false;
    char devpath[NAMES_PATH_LEN];
    full_path(dev, devpath, sizeof(devpath));
    struct stat st;
    if (stat(devpath, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(devpath, 0777) != 0) {
        ESP_LOGE(TAG, "Ordner %s laesst sich nicht anlegen", dev);
        return false;
    }
    *created = true;
    return true;
}

esp_err_t names_write(const char *file, const char *data, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_path(file)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > NAMES_MAX_FILE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!looks_like_text(data, len)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Liegt dieselbe Datei bereits in anderer Schreibweise, wird SIE ersetzt.
     * Sonst staenden "AtariFA/002.cfg" und "atarifa/002.cfg" nebeneinander und
     * beide passten auf dieselbe Kennung -- welche gilt, waere Zufall. */
    char actual[NAMES_MAX_NAME];
    if (!names_find(file, actual, sizeof(actual))) {
        strlcpy(actual, file, sizeof(actual));
    }

    char dev[NAMES_MAX_NAME], leaf[NAMES_MAX_NAME];
    if (!split_path(actual, dev, sizeof(dev), leaf, sizeof(leaf))) {
        return ESP_ERR_INVALID_ARG;
    }
    bool dir_created = false;
    if (!ensure_dir(dev, &dir_created)) {
        return ESP_FAIL;
    }

    char path[NAMES_PATH_LEN];
    full_path(actual, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) {
        if (dir_created) {
            rmdir_if_empty(dev);
        }
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    if (written != len) {
        unlink(path);   /* halbe Datei ist schlimmer als keine */
        if (dir_created) {
            rmdir_if_empty(dev);
        }
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s gespeichert (%u Byte)", actual, (unsigned)len);
    return ESP_OK;
}

/* ---- Nachladen von lisy.dev ---------------------------------------------- */

/* Die Ablage auf lisy.dev ist genauso gegliedert wie die Partition: ein Ordner
 * je Geraet. Deshalb zwei Schritte -- erst die Ordner, dann deren Inhalt. */
esp_err_t names_fetch_dev_json(char *out, size_t out_len)
{
    return repo_list_json(NAMES_BASE_URL, "/", out, out_len);
}

esp_err_t names_fetch_list_json(const char *dev, char *out, size_t out_len)
{
    if (!repo_valid_segment(dev, NAMES_MAX_NAME)) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[128];
    if (snprintf(url, sizeof(url), "%s%s/", NAMES_BASE_URL, dev) >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    return repo_list_json(url, NAMES_SUFFIX, out, out_len);
}

esp_err_t names_fetch(const char *path_in)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!names_valid_path(path_in)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Wie beim Upload: eine vorhandene Datei gleichen Namens ersetzen, statt
     * eine zweite Schreibweise danebenzulegen. */
    char actual[NAMES_MAX_NAME];
    if (!names_find(path_in, actual, sizeof(actual))) {
        strlcpy(actual, path_in, sizeof(actual));
    }

    char dev[NAMES_MAX_NAME], leaf[NAMES_MAX_NAME];
    if (!split_path(actual, dev, sizeof(dev), leaf, sizeof(leaf))) {
        return ESP_ERR_INVALID_ARG;
    }
    bool dir_created = false;
    if (!ensure_dir(dev, &dir_created)) {
        return ESP_FAIL;
    }

    char path[NAMES_PATH_LEN];
    full_path(actual, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) {
        if (dir_created) {
            rmdir_if_empty(dev);
        }
        return ESP_FAIL;
    }
    /* Angefragt wird der Pfad, wie er auf dem Server steht -- die Schreibweise
     * auf dem Geraet darf davon abweichen und tut es nach einem Umbenennen. */
    esp_err_t err = repo_download_to_file(NAMES_BASE_URL, path_in, fp,
                                          NAMES_MAX_FILE_SIZE);
    fclose(fp);
    if (err != ESP_OK) {
        /* Abgebrochener Download hinterlaesst sonst eine halbe Datei, die beim
         * naechsten Blick in die Liste wie eine gueltige aussaehe. */
        unlink(path);
        if (dir_created) {
            rmdir_if_empty(dev);
        }
    }
    return err;
}

/* ---- Auflisten ------------------------------------------------------------ */

/* Einen Eintrag anhaengen. count zaehlt mit, damit das Komma stimmt. */
static void append_entry(char *out, size_t out_len, size_t *w, int *count,
                         const char *name, const char *path, bool old)
{
    struct stat st;
    long size = (stat(path, &st) == 0) ? (long)st.st_size : 0;
    *w += snprintf(out + *w, out_len - *w, "%s{\"n\":\"%s\",\"s\":%ld%s}",
                   *count ? "," : "", name, size, old ? ",\"old\":1" : "");
    (*count)++;
}

esp_err_t names_list_json(char *out, size_t out_len)
{
    size_t w = snprintf(out, out_len, "{\"fs\":%d,\"active\":\"%s\",\"files\":[",
                        s_ready ? 1 : 0, g_cfg.names_file);
    if (!s_ready) {
        snprintf(out + w, out_len - w, "]}");
        return ESP_OK;
    }

    DIR *root = opendir(NAMES_MOUNT);
    if (!root) {
        snprintf(out + w, out_len - w, "]}");
        return ESP_FAIL;
    }

    /* Begrenzt, damit das JSON in den Puffer des Aufrufers passt: ein Eintrag
     * ist hoechstens ~56 Byte, und es muss noch Platz fuer den Abschluss
     * bleiben.
     *
     * Zwei Sorten Eintrag: die Dateien in den Geraeteordnern -- das sind die
     * gueltigen -- und die flachen .cfg im Wurzelverzeichnis, die von vor v1.18
     * stammen. Letztere kommen mit "old":1 mit, damit sie sich loeschen lassen;
     * unsichtbar wuerden sie nur unerklaerlich Platz belegen. */
    int count = 0;
    struct dirent *de;
    while ((de = readdir(root)) != NULL && count < NAMES_MAX_LIST && w + 80 < out_len) {
        char path[NAMES_PATH_LEN];
        full_path(de->d_name, path, sizeof(path));

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        if (!S_ISDIR(st.st_mode)) {
            if (names_valid_legacy(de->d_name)) {
                append_entry(out, out_len, &w, &count, de->d_name, path, true);
            }
            continue;
        }
        if (!repo_valid_segment(de->d_name, NAMES_MAX_NAME)) {
            continue;
        }

        DIR *sub = opendir(path);
        if (!sub) {
            continue;
        }
        struct dirent *fe;
        while ((fe = readdir(sub)) != NULL && count < NAMES_MAX_LIST && w + 80 < out_len) {
            if (!repo_valid_filename(fe->d_name, NAMES_SUFFIX, NAMES_MAX_NAME)) {
                continue;
            }
            char rel[NAMES_MAX_NAME];
            if (snprintf(rel, sizeof(rel), "%s/%s", de->d_name, fe->d_name)
                    >= (int)sizeof(rel)) {
                continue;   /* passt nicht in einen Bezeichner, also nicht nutzbar */
            }
            char fpath[NAMES_PATH_LEN];
            full_path(rel, fpath, sizeof(fpath));
            append_entry(out, out_len, &w, &count, rel, fpath, false);
        }
        closedir(sub);
    }
    closedir(root);

    if (w < out_len) {
        size_t total = 0, used = 0;
        esp_littlefs_info(NAMES_PARTITION, &total, &used);
        snprintf(out + w, out_len - w, "],\"total\":%u,\"used\":%u}",
                 (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}
