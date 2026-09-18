#include "repo.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "repo";

#define CHUNK_SIZE    1024
/* Ueberhang des Lesefensters: ein angeschnittenes href="<name>" muss bis zum
 * naechsten Lesen hineinpassen, laengere Namen verwirft der Filter ohnehin. */
#define LIST_TAIL     (REPO_MAX_NAME + 8)
#define POOL_STEP     1024

/* ---- Dateinamen ---------------------------------------------------------- */

bool repo_valid_segment(const char *seg, size_t max_len)
{
    if (!seg || !seg[0]) {
        return false;
    }
    size_t len = strlen(seg);
    if (len >= max_len) {
        return false;
    }
    /* "." und ".." bestehen aus erlaubten Zeichen, meinen aber Verzeichnisse.
     * Sie muessen einzeln heraus, sonst fuehrt ein Pfadstueck doch wieder aus
     * dem Verzeichnis heraus. */
    if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = seg[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

bool repo_valid_filename(const char *file, const char *suffix, size_t max_len)
{
    if (!file || !suffix) {
        return false;
    }
    size_t len = strlen(file);
    size_t slen = strlen(suffix);
    if (len <= slen) {
        return false;   /* vor der Endung muss noch etwas stehen */
    }
    if (strcmp(file + len - slen, suffix) != 0) {
        return false;
    }
    return repo_valid_segment(file, max_len);
}

/* ---- Verzeichnislisting --------------------------------------------------- */

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

/* Gesammelte Namen: hintereinander in einem Pool, dazu ihre Anfangsoffsets.
 * Offsets statt Zeiger, weil realloc() den Pool verschieben darf. */
typedef struct {
    const char *suffix;
    size_t slen;
    char *pool;
    size_t pool_len, pool_cap;
    size_t off[REPO_MAX_FILES];
    int count;
    bool full;      /* REPO_MAX_FILES erreicht, weitere Treffer ignoriert */
    bool oom;
} list_scan_t;

/* href="<name><suffix>" pruefen und uebernehmen.
 *
 * Schraegstriche sind nur INNERHALB des Suffix erlaubt. Fuer Dateien
 * (".cfg", ".bin") heisst das: gar keine. Fuer suffix = "/" heisst es: genau
 * der eine am Ende -- so faellt der Elternlink href="/swrep/misc/FA_Control/"
 * ueber seine uebrigen Schraegstriche heraus, und die Sortierlinks
 * href="?C=N;O=D" schon ueber das Suffix. */
static void scan_take(list_scan_t *s, const char *p, size_t len)
{
    if (len <= s->slen || len >= REPO_MAX_NAME ||
        strncmp(p + len - s->slen, s->suffix, s->slen) != 0 ||
        memchr(p, '/', len - s->slen)) {
        return;
    }
    if (s->count >= REPO_MAX_FILES) {
        s->full = true;
        return;
    }
    if (s->pool_len + len + 1 > s->pool_cap) {
        char *np = realloc(s->pool, s->pool_cap + POOL_STEP);
        if (!np) {
            s->oom = true;
            return;
        }
        s->pool = np;
        s->pool_cap += POOL_STEP;
    }
    memcpy(s->pool + s->pool_len, p, len);
    s->pool[s->pool_len + len] = '\0';
    s->off[s->count++] = s->pool_len;
    s->pool_len += len + 1;
}

static const char *find_href(const char *p, const char *end)
{
    for (; end - p >= 6; p++) {
        if (memcmp(p, "href=\"", 6) == 0) {
            return p;
        }
    }
    return NULL;
}

/* Alle vollstaendigen href="..." in buf[0..n) auswerten. Rueckgabe: wie viele
 * Bytes verbraucht sind; der Rest (hoechstens LIST_TAIL) ist ein angeschnittener
 * Eintrag und kommt beim naechsten Lesen vorn wieder dazu. last = nichts mehr zu
 * erwarten, dann zaehlt ein angeschnittener Rest nicht. */
static size_t scan_window(list_scan_t *s, const char *buf, size_t n, bool last)
{
    const char *end = buf + n;
    const char *pos = buf;
    for (;;) {
        const char *h = find_href(pos, end);
        if (!h) {
            /* hoechstens ein angeschnittenes 'href="' (5 Byte) aufheben */
            if (last || end - pos <= 5) {
                return last ? n : (size_t)(pos - buf);
            }
            return n - 5;
        }
        const char *p = h + 6;
        const char *q = memchr(p, '"', end - p);
        if (!q) {
            if (last) {
                return n;
            }
            if (end - h > LIST_TAIL) {
                pos = p;   /* Name laenger als erlaubt: verwerfen, weitersuchen */
                continue;
            }
            return h - buf;
        }
        scan_take(s, p, q - p);
        pos = q + 1;
    }
}

esp_err_t repo_list_json(const char *base_url, const char *suffix,
                         char *out, size_t out_len)
{
    /* Das Listing wird beim Lesen ausgewertet statt erst komplett gepuffert:
     * neben dem TLS-Kontext stehen so nur das Lesefenster und die Namen im
     * Heap, auch bei einem Ordner mit 256 Eintraegen. */
    list_scan_t *s = calloc(1, sizeof(*s));
    char *win = malloc(CHUNK_SIZE + LIST_TAIL);
    if (!s || !win) {
        free(s);
        free(win);
        return ESP_ERR_NO_MEM;
    }
    s->suffix = suffix;
    s->slen = strlen(suffix);

    esp_http_client_config_t cfg = {
        .url = base_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(s);
        free(win);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "Listing HTTP %d", status);
            err = ESP_FAIL;
        }
    }
    size_t keep = 0;
    while (err == ESP_OK && !s->oom) {
        int n = esp_http_client_read(client, win + keep, CHUNK_SIZE + LIST_TAIL - keep);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        size_t have = keep + n;
        size_t used = scan_window(s, win, have, n == 0);
        if (n == 0) {
            break;   /* fertig */
        }
        keep = have - used;
        memmove(win, win + used, keep);
    }
    esp_http_client_cleanup(client);
    free(win);
    if (err == ESP_OK && s->oom) {
        err = ESP_ERR_NO_MEM;
    }
    const char **idx = NULL;
    if (err == ESP_OK && s->count) {
        idx = malloc(s->count * sizeof(*idx));
        if (!idx) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        free(s->pool);
        free(s);
        return err;
    }
    int count = s->count;
    for (int i = 0; i < count; i++) {
        idx[i] = s->pool + s->off[i];
    }
    if (s->full) {
        ESP_LOGW(TAG, "mehr als %d Eintraege '%s', der Rest wird ignoriert",
                 REPO_MAX_FILES, suffix);
    }

    qsort(idx, count, sizeof(idx[0]), cmp_desc);

    /* Jeder Eintrag nur, wenn danach noch der Abschluss "]}" passt. */
    size_t w = snprintf(out, out_len, "{\"files\":[");
    int shown = 0;
    for (int i = 0; i < count; i++) {
        size_t need = strlen(idx[i]) + 3 + (i ? 1 : 0);
        if (w + need + 3 > out_len) {
            break;
        }
        w += snprintf(out + w, out_len - w, "%s\"%s\"", i ? "," : "", idx[i]);
        shown++;
    }
    snprintf(out + w, out_len - w, "]}");
    free(idx);
    free(s->pool);
    free(s);
    if (shown < count) {
        ESP_LOGW(TAG, "%d von %d Eintraegen '%s' passen nicht in die Antwort",
                 count - shown, count, suffix);
    }
    ESP_LOGI(TAG, "%d Dateien '%s' gefunden", count, suffix);
    return ESP_OK;
}

/* ---- Download ------------------------------------------------------------- */

esp_err_t repo_download_to_file(const char *base_url, const char *file,
                                FILE *fp, size_t max_len)
{
    /* Abschneiden waere hier besonders tueckisch: die gekuerzte URL koennte auf
     * eine andere, existierende Datei zeigen. Lieber gar nicht erst laden. */
    char url[160];
    if (strlen(base_url) + strlen(file) >= sizeof(url)) {
        ESP_LOGW(TAG, "URL zu lang: %s%s", base_url, file);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(url, sizeof(url), "%s%s", base_url, file);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "Download %s: HTTP %d", file, status);
            err = ESP_FAIL;
        }
    }

    size_t total = 0;
    char buf[CHUNK_SIZE];
    while (err == ESP_OK) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;   /* fertig */
        }
        if (total + n > max_len) {
            ESP_LOGW(TAG, "Download %s laenger als %u Byte", file, (unsigned)max_len);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(buf, 1, n, fp) != (size_t)n) {
            err = ESP_FAIL;
            break;
        }
        total += n;
    }
    esp_http_client_cleanup(client);

    if (err == ESP_OK && total == 0) {
        err = ESP_FAIL;   /* leere Datei ist kein brauchbares Ergebnis */
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s geladen (%u Byte)", file, (unsigned)total);
    }
    return err;
}
