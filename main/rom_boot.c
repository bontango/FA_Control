#include "rom_boot.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lisy.h"
#include "repo.h"

static const char *TAG = "rom_boot";

#define ROM_PARTITION   "roms"
#define ROM_MOUNT       "/roms"
/* Beginnt mit einem Punkt und ist eine Datei: die Liste sieht nur Ordner mit
 * gueltigem Namen an, die Zwischendatei taucht dort also nie auf. */
#define ROM_TMP         ROM_MOUNT "/.tmp"
#define ROMS_BASE_URL   "https://lisy.dev/swrep/misc/FA_Control/roms/"

#define HDR_SIZE        8
#define PATH_LEN        48
#define CHUNK           1024

/* Boot-Protokoll, siehe rom_boot.h */
#define B_HDR0          0xA5
#define B_HDR1          0x5A
#define B_REQUEST       0x52   /* 'R' */
#define B_NONE          0x4E   /* 'N' */
#define B_DATA          0x44   /* 'D' */
#define MAX_SECTORS     128

#define POLL_MS         10

static bool s_ready;

/* Letzte Boot-Anfrage, fuer die Oberflaeche */
static char s_last_hw[ROM_BOOT_MAX_HW + 1];
static volatile int s_last_game = -1;
static volatile char s_last_resp;
static volatile int64_t s_last_us;

/* Sendepuffer des Boot-Tasks. Statisch, und NUR von ihm benutzt. */
static uint8_t s_tx[CHUNK];

/* ---- CRC ------------------------------------------------------------------ */

uint16_t rom_boot_crc16(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ---- Namen und Pfade -------------------------------------------------------- */

static bool valid_hw(const char *hw)
{
    return repo_valid_segment(hw, ROM_BOOT_MAX_HW + 1);
}

/* Genau "nnn.bin" mit nnn <= 255. Rueckgabe: Spielnummer oder -1. */
static int game_of_leaf(const char *leaf)
{
    if (strlen(leaf) != 7 || strcmp(leaf + 3, ".bin") != 0) {
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        if (leaf[i] < '0' || leaf[i] > '9') {
            return -1;
        }
    }
    int g = atoi(leaf);
    return g <= 255 ? g : -1;
}

bool rom_parse_id(const char *id, char *hw, size_t hw_len, int *game)
{
    if (!id) {
        return false;
    }
    const char *slash = strchr(id, '/');
    if (!slash || slash == id || strchr(slash + 1, '/')) {
        return false;
    }
    size_t hl = slash - id;
    if (hl >= hw_len || hl > ROM_BOOT_MAX_HW) {
        return false;
    }
    memcpy(hw, id, hl);
    hw[hl] = '\0';
    const char *g = slash + 1;
    if (strlen(g) != 3 || !valid_hw(hw)) {
        return false;
    }
    char leaf[8];
    snprintf(leaf, sizeof(leaf), "%s.bin", g);
    int n = game_of_leaf(leaf);
    if (n < 0) {
        return false;
    }
    *game = n;
    return true;
}

/* Den Geraeteordner ohne Ruecksicht auf Gross-/Kleinschreibung suchen und die
 * tatsaechliche Schreibweise liefern -- wie names.c. Das Board meldet "SternFA",
 * auf lisy.dev koennte der Ordner "sternfa" heissen; gemeint ist derselbe. */
static bool find_hw_dir(const char *hw, char *actual, size_t len)
{
    DIR *dir = opendir(ROM_MOUNT);
    if (!dir) {
        return false;
    }
    bool hit = false;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcasecmp(de->d_name, hw) == 0) {
            char p[PATH_LEN];
            struct stat st;
            snprintf(p, sizeof(p), ROM_MOUNT "/%.16s", de->d_name);
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                strlcpy(actual, de->d_name, len);
                hit = true;
                break;
            }
        }
    }
    closedir(dir);
    return hit;
}

static void rom_path(const char *hw_actual, int game, char *out, size_t len)
{
    snprintf(out, len, ROM_MOUNT "/%.15s/%03d.bin", hw_actual, game);
}

/* Datei zu <hw>/<game> oeffnen und den Kopf pruefen. NULL = nicht vorhanden
 * oder unbrauchbar. */
static FILE *rom_open(const char *hw, int game, uint8_t *fill, uint16_t *dlen)
{
    char actual[ROM_BOOT_MAX_HW + 1];
    if (!s_ready || !valid_hw(hw) || !find_hw_dir(hw, actual, sizeof(actual))) {
        return NULL;
    }
    char path[PATH_LEN];
    rom_path(actual, game, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    uint8_t h[HDR_SIZE];
    if (fread(h, 1, HDR_SIZE, fp) != HDR_SIZE ||
        h[0] != 'F' || h[1] != 'A' || h[2] != 'R' || h[3] != 1) {
        ESP_LOGW(TAG, "%s: kein gueltiger Kopf", path);
        fclose(fp);
        return NULL;
    }
    *fill = h[4];
    *dlen = ((uint16_t)h[6] << 8) | h[7];
    /* Laenge gegen die Datei pruefen, BEVOR 'D' gesendet wird -- danach liesse
     * sich ein Fehler nur noch ueber eine falsche CRC melden. */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < HDR_SIZE + (long)*dlen) {
        ESP_LOGW(TAG, "%s: %ld Byte, Kopf sagt %u", path, size, *dlen);
        fclose(fp);
        return NULL;
    }
    fseek(fp, HDR_SIZE, SEEK_SET);
    return fp;
}

/* ---- Partition ------------------------------------------------------------ */

esp_err_t rom_boot_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ROM_PARTITION);
    if (!part) {
        ESP_LOGW(TAG, "Keine Partition '%s' -- Spiel-ROMs sind auf diesem Geraet "
                      "nicht verfuegbar (Partitionstabelle per USB flashen)",
                 ROM_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = ROM_MOUNT,
        .partition_label        = ROM_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS-Mount fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    unlink(ROM_TMP);   /* Rest eines abgebrochenen Uploads */

    size_t total = 0, used = 0;
    if (esp_littlefs_info(ROM_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "Spiel-ROMs gemountet: %u von %u Byte belegt",
                 (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

bool rom_boot_ready(void)
{
    return s_ready;
}

/* ---- Ablegen ------------------------------------------------------------- */

FILE *rom_tmp_open(void)
{
    return s_ready ? fopen(ROM_TMP, "wb") : NULL;
}

void rom_tmp_discard(void)
{
    unlink(ROM_TMP);
}

esp_err_t rom_import_tmp(const char *hw, int game)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_hw(hw) || game < 0 || game > 255) {
        rom_tmp_discard();
        return ESP_ERR_INVALID_ARG;
    }
    FILE *in = fopen(ROM_TMP, "rb");
    if (!in) {
        return ESP_FAIL;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    if (size < 512 || size > ROM_BOOT_MAX_IMAGE || size % 512 != 0) {
        fclose(in);
        rom_tmp_discard();
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        fclose(in);
        rom_tmp_discard();
        return ESP_ERR_NO_MEM;
    }

    /* Ein voller 64-kB-Platz traegt seine CRC in den letzten beiden Bytes; die
     * gehoeren dann nicht zu den Daten. */
    bool has_crc = (size == ROM_BOOT_MAX_IMAGE);
    long body = has_crc ? size - 2 : size;

    /* Fuellbyte = letztes Datenbyte des Platzes */
    fseek(in, body - 1, SEEK_SET);
    int fill = fgetc(in);

    /* Ein Durchgang: CRC ueber 32 kB und das letzte Byte, das nicht Fuellbyte ist */
    fseek(in, 0, SEEK_SET);
    uint16_t crc = 0xFFFF;
    long pos = 0, last = -1;
    esp_err_t err = ESP_OK;
    while (pos < body) {
        size_t n = (body - pos) < CHUNK ? (size_t)(body - pos) : CHUNK;
        if (fread(buf, 1, n, in) != n) {
            err = ESP_FAIL;
            break;
        }
        if (has_crc && pos < ROM_BOOT_CRC_SPAN) {
            size_t m = (ROM_BOOT_CRC_SPAN - pos) < (long)n ? (size_t)(ROM_BOOT_CRC_SPAN - pos) : n;
            crc = rom_boot_crc16(crc, buf, m);
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != fill) {
                last = pos + i;
            }
        }
        pos += n;
    }
    if (err == ESP_OK && has_crc) {
        uint8_t t[2];
        if (fread(t, 1, 2, in) != 2) {
            err = ESP_FAIL;
        } else if ((((uint16_t)t[0] << 8) | t[1]) != crc) {
            ESP_LOGW(TAG, "%s/%03d: CRC %04X, im Abbild %02X%02X", hw, game, crc, t[0], t[1]);
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err != ESP_OK) {
        free(buf);
        fclose(in);
        rom_tmp_discard();
        return err;
    }

    uint16_t dlen = (uint16_t)(last + 1);

    /* Vorhandenen Geraeteordner in seiner Schreibweise weiterbenutzen */
    char actual[ROM_BOOT_MAX_HW + 1];
    bool dir_existed = find_hw_dir(hw, actual, sizeof(actual));
    if (!dir_existed) {
        strlcpy(actual, hw, sizeof(actual));
        char dir[PATH_LEN];
        snprintf(dir, sizeof(dir), ROM_MOUNT "/%.15s", actual);
        if (mkdir(dir, 0775) != 0) {
            free(buf);
            fclose(in);
            rom_tmp_discard();
            return ESP_FAIL;
        }
    }

    char path[PATH_LEN];
    rom_path(actual, game, path, sizeof(path));
    FILE *out = fopen(path, "wb");
    bool ok = out != NULL;
    if (ok) {
        uint8_t h[HDR_SIZE] = { 'F', 'A', 'R', 1, (uint8_t)fill, 0, dlen >> 8, dlen & 0xFF };
        ok = fwrite(h, 1, HDR_SIZE, out) == HDR_SIZE;
        fseek(in, 0, SEEK_SET);
        long left = dlen;
        while (ok && left > 0) {
            size_t n = left < CHUNK ? (size_t)left : CHUNK;
            ok = fread(buf, 1, n, in) == n && fwrite(buf, 1, n, out) == n;
            left -= n;
        }
        ok = (fclose(out) == 0) && ok;
    }
    free(buf);
    fclose(in);
    rom_tmp_discard();

    if (!ok) {
        unlink(path);   /* halbe Datei ist schlimmer als keine */
        if (!dir_existed) {
            char dir[PATH_LEN];
            snprintf(dir, sizeof(dir), ROM_MOUNT "/%.15s", actual);
            rmdir(dir);
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s/%03d gespeichert: %ld Byte Platz, %u Byte Daten, Fuellbyte %02X%s",
             actual, game, size, dlen, fill, has_crc ? ", CRC ok" : "");
    return ESP_OK;
}

esp_err_t rom_delete(const char *id)
{
    char hw[ROM_BOOT_MAX_HW + 1], actual[ROM_BOOT_MAX_HW + 1];
    int game;
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rom_parse_id(id, hw, sizeof(hw), &game) ||
        !find_hw_dir(hw, actual, sizeof(actual))) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[PATH_LEN];
    rom_path(actual, game, path, sizeof(path));
    if (unlink(path) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    char dir[PATH_LEN];
    snprintf(dir, sizeof(dir), ROM_MOUNT "/%.15s", actual);
    rmdir(dir);   /* nur wenn leer -- sonst scheitert es still */
    ESP_LOGI(TAG, "%s/%03d geloescht", actual, game);
    return ESP_OK;
}

esp_err_t rom_list_json(char *out, size_t len)
{
    size_t w = snprintf(out, len, "{\"fs\":%d,\"roms\":[", s_ready ? 1 : 0);
    int count = 0;
    DIR *root = s_ready ? opendir(ROM_MOUNT) : NULL;
    if (root) {
        struct dirent *de;
        while ((de = readdir(root)) != NULL && count < ROM_BOOT_MAX_LIST) {
            if (!valid_hw(de->d_name)) {
                continue;
            }
            char dpath[PATH_LEN];
            snprintf(dpath, sizeof(dpath), ROM_MOUNT "/%.15s", de->d_name);
            DIR *sub = opendir(dpath);
            if (!sub) {
                continue;
            }
            struct dirent *fe;
            while ((fe = readdir(sub)) != NULL && count < ROM_BOOT_MAX_LIST &&
                   w + 64 < len) {
                int g = game_of_leaf(fe->d_name);
                if (g < 0) {
                    continue;
                }
                char fpath[PATH_LEN];
                rom_path(de->d_name, g, fpath, sizeof(fpath));
                uint8_t h[HDR_SIZE] = { 0 };
                FILE *fp = fopen(fpath, "rb");
                if (fp) {
                    fread(h, 1, HDR_SIZE, fp);
                    fclose(fp);
                }
                w += snprintf(out + w, len - w, "%s{\"n\":\"%.15s/%03d\",\"s\":%u}",
                              count ? "," : "", de->d_name, g,
                              ((unsigned)h[6] << 8) | h[7]);
                count++;
            }
            closedir(sub);
        }
        closedir(root);
    }

    size_t total = 0, used = 0;
    if (s_ready) {
        esp_littlefs_info(ROM_PARTITION, &total, &used);
    }
    if (w < len) {
        w += snprintf(out + w, len - w, "],\"total\":%u,\"used\":%u",
                      (unsigned)total, (unsigned)used);
    }
    if (s_last_game >= 0 && w < len) {
        w += snprintf(out + w, len - w,
                      ",\"last\":{\"hw\":\"%s\",\"g\":%d,\"r\":\"%c\",\"ms\":%lld}",
                      s_last_hw, s_last_game, s_last_resp, (long long)(s_last_us / 1000));
    }
    if (w < len) {
        snprintf(out + w, len - w, "}");
    }
    return ESP_OK;
}

/* ---- Nachladen von lisy.dev ------------------------------------------------ */

esp_err_t rom_fetch_dev_json(char *out, size_t len)
{
    return repo_list_json(ROMS_BASE_URL, "/", out, len);
}

esp_err_t rom_fetch_list_json(const char *dev, char *out, size_t len)
{
    if (!valid_hw(dev)) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[128];
    snprintf(url, sizeof(url), ROMS_BASE_URL "%s/", dev);
    return repo_list_json(url, ".bin", out, len);
}

esp_err_t rom_fetch(const char *path)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *slash = path ? strchr(path, '/') : NULL;
    if (!slash || slash == path || strchr(slash + 1, '/') ||
        (size_t)(slash - path) > ROM_BOOT_MAX_HW) {
        return ESP_ERR_INVALID_ARG;
    }
    char dev[ROM_BOOT_MAX_HW + 1];
    memcpy(dev, path, slash - path);
    dev[slash - path] = '\0';
    const char *leaf = slash + 1;
    /* <nnn>.bin oder <nnn>_<Titel>.bin -- nur die Zahl zaehlt */
    if (!valid_hw(dev) || !repo_valid_filename(leaf, ".bin", REPO_MAX_NAME) ||
        strlen(leaf) < 7 || (leaf[3] != '.' && leaf[3] != '_')) {
        return ESP_ERR_INVALID_ARG;
    }
    char num[8];
    snprintf(num, sizeof(num), "%.3s.bin", leaf);
    int game = game_of_leaf(num);
    if (game < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = rom_tmp_open();
    if (!fp) {
        return ESP_FAIL;
    }
    esp_err_t err = repo_download_to_file(ROMS_BASE_URL, path, fp, ROM_BOOT_MAX_IMAGE);
    fclose(fp);
    if (err != ESP_OK) {
        rom_tmp_discard();
        return err;
    }
    return rom_import_tmp(dev, game);
}

/* ---- Boot-Antwort --------------------------------------------------------- */

static void answer(const char *hw, int game, int sectors)
{
    uint8_t fill = 0;
    uint16_t dlen = 0;
    FILE *fp = rom_open(hw, game, &fill, &dlen);
    uint8_t head[3] = { B_HDR0, B_HDR1, fp ? B_DATA : B_NONE };
    lisy_bus_write(head, 3);

    char resp = 'N';
    if (fp) {
        size_t total = (size_t)sectors * 512;
        size_t sent = 0;
        uint16_t crc = 0xFFFF;
        bool broken = false;
        while (sent < total) {
            size_t n = (total - sent) < CHUNK ? total - sent : CHUNK;
            size_t got = 0;
            if (sent < dlen) {
                size_t want = (dlen - sent) < n ? dlen - sent : n;
                got = fread(s_tx, 1, want, fp);
                if (got != want) {
                    broken = true;
                }
            }
            memset(s_tx + got, fill, n - got);
            crc = rom_boot_crc16(crc, s_tx, n);
            lisy_bus_write(s_tx, n);
            sent += n;
        }
        fclose(fp);
        /* Ein Lesefehler mitten im Senden laesst sich nur noch so melden: mit
         * einer falschen CRC. Das FPGA verwirft dann und nimmt die SD-Karte. */
        if (broken) {
            crc = ~crc;
        }
        uint8_t tail[2] = { crc >> 8, crc & 0xFF };
        lisy_bus_write(tail, 2);
        resp = broken ? 'E' : 'D';
    }
    /* Das FPGA wiederholt die Anfrage, bis es die Kennung sieht; was davon
     * waehrend des Sendens noch einlief, darf keine zweite Antwort ausloesen --
     * die landete sonst bei fa_control. */
    lisy_bus_drain();

    strlcpy(s_last_hw, hw, sizeof(s_last_hw));
    s_last_resp = resp;
    s_last_us = esp_timer_get_time();
    s_last_game = game;
    ESP_LOGI(TAG, "Boot-Anfrage %s/%03d, %d Sektoren -> %s", hw, game, sectors,
             resp == 'D' ? "ROM gesendet" : resp == 'E' ? "Lesefehler" : "kein ROM");
}

typedef enum { P_H0, P_H1, P_CMD, P_LEN, P_HW, P_GAME, P_SEC } parse_t;

static void boot_task(void *arg)
{
    parse_t st = P_H0;
    char hw[ROM_BOOT_MAX_HW + 1];
    int hw_len = 0, hw_pos = 0, game = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        /* Den Bus nur nehmen, wenn wirklich etwas im Empfang liegt -- sonst
         * wuerden die LISY-Befehle der Oberflaeche jedes Mal auf diesen Task warten. */
        if (lisy_bus_available() == 0 || !lisy_bus_take(50)) {
            continue;
        }
        uint8_t b;
        /* Eine Anfrage ist hoechstens 21 Byte = 1,8 ms; 5 ms reichen fuer den Rest. */
        while (lisy_bus_read(&b, 1, 5) == 1) {
            bool done = false;
            switch (st) {
            case P_H0:
                st = (b == B_HDR0) ? P_H1 : P_H0;
                break;
            case P_H1:
                st = (b == B_HDR1) ? P_CMD : (b == B_HDR0 ? P_H1 : P_H0);
                break;
            case P_CMD:
                st = (b == B_REQUEST) ? P_LEN : (b == B_HDR0 ? P_H1 : P_H0);
                break;
            case P_LEN:
                if (b >= 1 && b <= ROM_BOOT_MAX_HW) {
                    hw_len = b;
                    hw_pos = 0;
                    st = P_HW;
                } else {
                    st = (b == B_HDR0) ? P_H1 : P_H0;
                }
                break;
            case P_HW:
                hw[hw_pos++] = (char)b;
                if (hw_pos == hw_len) {
                    hw[hw_len] = '\0';
                    st = P_GAME;
                }
                break;
            case P_GAME:
                game = b;
                st = P_SEC;
                break;
            case P_SEC:
                st = P_H0;
                if (b >= 1 && b <= MAX_SECTORS && valid_hw(hw)) {
                    answer(hw, game, b);
                    done = true;
                }
                break;
            }
            if (done) {
                break;
            }
        }
        lisy_bus_give();
    }
}

esp_err_t rom_boot_start(void)
{
    if (xTaskCreate(boot_task, "rom_boot", 3584, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Boot-Antwort bereit (%s)", s_ready ? "Ablage da" : "ohne Ablage, antwortet 'N'");
    return ESP_OK;
}
