#include "fpga_prog.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "board.h"
#include "fa_connect.h"

static const char *TAG = "fpga";

/* EPCS-Befehle (EPCS16-Datenblatt; Nachbauten wie W25Q16 verstehen dieselben,
 * dazu 0x9F statt 0xAB). */
#define EPCS_WREN        0x06
#define EPCS_RDSR        0x05
#define EPCS_READ        0x03
#define EPCS_PP          0x02
#define EPCS_SE          0xD8   /* 64-kB-Sektor */
#define EPCS_RES_ID      0xAB   /* Silicon ID, nur Original-EPCS */
#define EPCS_JEDEC_ID    0x9F

#define EPCS_SR_WIP      0x01
#define EPCS_PAGE        256
#define EPCS_SECTOR      0x10000

/* Zeitgrenzen aus dem EPCS16-Datenblatt, grosszuegig aufgerundet:
 * Sektor loeschen max. 3 s, Page Program max. 5 ms. */
#define EPCS_SE_TIMEOUT_MS   4000
#define EPCS_PP_TIMEOUT_MS   20

/* 10 MHz reichen: 360 kB lesen dauern so ~0,3 s, die Zeit geht ins Loeschen.
 * Hoeher erst, wenn Stufe 0 die Signale am Oszilloskop gesehen hat. */
#define SPI_CLOCK_HZ     (10 * 1000 * 1000)
#define READ_CHUNK       4096
#define XFER_MAX         (4 + READ_CHUNK)

/* nCONFIG low -> AS-Pins des FPGA hochohmig. Das FPGA braucht dafuer nur
 * Mikrosekunden; C14 (100 nF) entlaedt der ESP-Ausgang sofort. */
#define NCONFIG_SETTLE_MS  2

static spi_device_handle_t s_dev;
static uint8_t *s_tx;        /* DMA-faehig, XFER_MAX */
static uint8_t *s_rx;

/* Zuletzt Ermitteltes, fuer fpga_prog_info(). */
static int s_id_ab = -1;
static uint32_t s_jedec;
static uint32_t s_size;
static char s_last[FPGA_MSG_LEN] = "";

/* Offener Schreibvorgang */
static bool s_busy;
static bool s_rev;
static uint32_t s_wr_size;
static uint32_t s_wr_pos;           /* Bytes angenommen */
static uint32_t s_erased_end;       /* geloescht bis hierhin (Sektorgrenze) */
static uint8_t s_page[EPCS_PAGE];
static size_t s_page_fill;
static mbedtls_sha256_context s_sha;

/* Der Text geht per /api/fpgainfo bzw. als Fehlermeldung in die Oberflaeche --
 * deshalb englisch, das Log daneben deutsch. */
static void set_last(const char *msg)
{
    strlcpy(s_last, msg, sizeof(s_last));
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

/* ---- nCONFIG / CONF_DONE -------------------------------------------------- */

static bool conf_done(void)
{
    return gpio_get_level(BOARD_PIN_CONF_DONE) != 0;
}

/* Open-drain: erst den Pegel setzen, dann die Richtung -- so gibt es beim
 * Umschalten keinen Moment, in dem der Pin aktiv high treibt. */
static void nconfig_hold(void)
{
    gpio_set_level(BOARD_PIN_NCONFIG, 0);
    gpio_set_direction(BOARD_PIN_NCONFIG, GPIO_MODE_INPUT_OUTPUT_OD);
    vTaskDelay(pdMS_TO_TICKS(NCONFIG_SETTLE_MS));
}

static void nconfig_release(void)
{
    gpio_set_direction(BOARD_PIN_NCONFIG, GPIO_MODE_INPUT);
}

/* Nach dem Loslassen: CONF_DONE muss kommen. */
static esp_err_t wait_conf_done(void)
{
    for (int t = 0; t < FPGA_CONF_DONE_MS; t += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (conf_done()) {
            ESP_LOGI(TAG, "CONF_DONE nach ~%d ms", t + 10);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "CONF_DONE bleibt low");
    return ESP_ERR_TIMEOUT;
}

/* ---- SPI-Bus: nur waehrend eines Vorgangs belegt ------------------------- */

static void pins_hiz(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_AS_DCLK) | (1ULL << BOARD_PIN_AS_ASDO) |
                        (1ULL << BOARD_PIN_AS_DATA0) | (1ULL << BOARD_PIN_AS_NCSO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void bus_close(void)
{
    if (s_dev) {
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
        spi_bus_free(SPI2_HOST);
    }
    heap_caps_free(s_tx);
    heap_caps_free(s_rx);
    s_tx = s_rx = NULL;
    /* spi_bus_free laesst die Pins an der GPIO-Matrix haengen; zurueck auf
     * hochohmig, damit das FPGA seine AS-Leitungen wieder allein hat. */
    pins_hiz();
}

static esp_err_t bus_open(void)
{
    s_tx = heap_caps_malloc(XFER_MAX, MALLOC_CAP_DMA);
    s_rx = heap_caps_malloc(XFER_MAX, MALLOC_CAP_DMA);
    if (!s_tx || !s_rx) {
        bus_close();
        return ESP_ERR_NO_MEM;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_PIN_AS_ASDO,
        .miso_io_num = BOARD_PIN_AS_DATA0,
        .sclk_io_num = BOARD_PIN_AS_DCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = XFER_MAX,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        heap_caps_free(s_tx);
        heap_caps_free(s_rx);
        s_tx = s_rx = NULL;
        return err;
    }
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = SPI_CLOCK_HZ,
        .spics_io_num = BOARD_PIN_AS_NCSO,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        spi_bus_free(SPI2_HOST);
        bus_close();
    }
    return err;
}

/* Vollduplex: n Byte aus s_tx raus, n Byte nach s_rx rein, CS die ganze Zeit low. */
static esp_err_t xfer(size_t n)
{
    spi_transaction_t t = {
        .length = n * 8,
        .tx_buffer = s_tx,
        .rx_buffer = s_rx,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

static esp_err_t cmd_only(uint8_t cmd)
{
    s_tx[0] = cmd;
    return xfer(1);
}

static void put_addr(uint8_t cmd, uint32_t addr)
{
    s_tx[0] = cmd;
    s_tx[1] = (uint8_t)(addr >> 16);
    s_tx[2] = (uint8_t)(addr >> 8);
    s_tx[3] = (uint8_t)addr;
}

static esp_err_t wait_ready(uint32_t timeout_ms)
{
    uint32_t start = esp_log_timestamp();
    while (true) {
        s_tx[0] = EPCS_RDSR;
        s_tx[1] = 0;
        esp_err_t err = xfer(2);
        if (err != ESP_OK) {
            return err;
        }
        if ((s_rx[1] & EPCS_SR_WIP) == 0) {
            return ESP_OK;
        }
        if (esp_log_timestamp() - start > timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        /* Beim Loeschen dauert es Sekunden -- dann den Prozessor hergeben. Beim
         * Page Program (~1 ms) lohnt sich das nicht. */
        if (timeout_ms > EPCS_PP_TIMEOUT_MS) {
            vTaskDelay(1);
        }
    }
}

/* ---- Kennung -------------------------------------------------------------- */

static uint32_t size_from_ab(int id)
{
    switch (id) {
    case 0x10: return 128 * 1024;        /* EPCS1 */
    case 0x12: return 512 * 1024;        /* EPCS4 */
    case 0x14: return 2 * 1024 * 1024;   /* EPCS16 */
    case 0x16: return 8 * 1024 * 1024;   /* EPCS64 */
    case 0x18: return 16 * 1024 * 1024;  /* EPCS128 */
    default:   return 0;
    }
}

/* Bus offen, nCONFIG gehalten. Ergebnis in s_id_ab/s_jedec/s_size. */
static esp_err_t read_id(void)
{
    memset(s_tx, 0, 5);
    s_tx[0] = EPCS_RES_ID;              /* Befehl + 3 Dummy-Byte, dann die ID */
    esp_err_t err = xfer(5);
    if (err != ESP_OK) {
        return err;
    }
    int ab = s_rx[4];

    memset(s_tx, 0, 4);
    s_tx[0] = EPCS_JEDEC_ID;
    err = xfer(4);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t jedec = (uint32_t)s_rx[1] << 16 | (uint32_t)s_rx[2] << 8 | s_rx[3];

    s_id_ab = ab;
    s_jedec = jedec;
    s_size = size_from_ab(ab);
    /* Nachbauten (W25Q16, IS25LP016 ...) antworten auf 0xAB mit ihrer eigenen
     * Device-ID; massgeblich ist dann das Kapazitaetsbyte der JEDEC-Kennung. */
    uint8_t cap = jedec & 0xFF;
    if (jedec != 0 && jedec != 0xFFFFFF && cap >= 0x10 && cap <= 0x19) {
        s_size = 1UL << cap;
    }
    ESP_LOGI(TAG, "EPCS: 0xAB-ID 0x%02X, JEDEC 0x%06lX, Groesse %lu kB",
             ab, (unsigned long)jedec, (unsigned long)(s_size / 1024));
    return s_size ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---- Sitzung: nCONFIG halten + Bus ---------------------------------------- */

static esp_err_t session_open(void)
{
    /* Erst die Kontrolle sauber zurueckgeben -- das FPGA geht gleich in den
     * Reset, mit ihm die Totmannschaltung der Gegenseite. */
    fa_connect_release();
    nconfig_hold();
    esp_err_t err = bus_open();
    if (err != ESP_OK) {
        nconfig_release();
        ESP_LOGE(TAG, "SPI-Bus: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t session_close(void)
{
    bus_close();
    nconfig_release();
    return wait_conf_done();
}

/* ---- oeffentlich ---------------------------------------------------------- */

void fpga_prog_init(void)
{
    /* board_init() hat alle Leitungen hochohmig gemacht; hier nur melden. */
    ESP_LOGI(TAG, "FPGA-Programmierung bereit, CONF_DONE=%d", conf_done());
}

void fpga_prog_info(fpga_info_t *out)
{
    out->conf_done = conf_done();
    out->busy = s_busy;
    out->id_ab = s_id_ab;
    out->jedec = s_jedec;
    out->size = s_size;
    strlcpy(out->last, s_last, sizeof(out->last));
}

esp_err_t fpga_prog_reconfig(void)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    fa_connect_release();
    nconfig_hold();
    nconfig_release();
    esp_err_t err = wait_conf_done();
    set_last(err == ESP_OK ? "Reconfigured, CONF_DONE high"
                           : "Reconfigured, but CONF_DONE stays low - no valid image?");
    return err;
}

esp_err_t fpga_prog_probe(void)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = session_open();
    if (err != ESP_OK) {
        set_last("SPI bus not available");
        return err;
    }
    esp_err_t id_err = read_id();
    err = session_close();
    if (id_err != ESP_OK) {
        set_last("No EPCS found");
        return id_err;
    }
    set_last(err == ESP_OK ? "EPCS identified" : "EPCS identified, but CONF_DONE stays low");
    return ESP_OK;   /* Kennung gelesen -- CONF_DONE steht in info */
}

esp_err_t fpga_prog_read(uint32_t addr, uint32_t len, fpga_sink_t sink, void *ctx)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = session_open();
    if (err != ESP_OK) {
        set_last("SPI bus not available");
        return err;
    }
    err = read_id();
    if (err == ESP_OK && (addr >= s_size || len > s_size - addr)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    while (err == ESP_OK && len > 0) {
        uint32_t n = len < READ_CHUNK ? len : READ_CHUNK;
        memset(s_tx, 0, 4 + n);
        put_addr(EPCS_READ, addr);
        err = xfer(4 + n);
        if (err == ESP_OK) {
            err = sink(ctx, s_rx + 4, n);
        }
        addr += n;
        len -= n;
    }
    esp_err_t cerr = session_close();
    set_last(err == ESP_OK               ? "EPCS read" :
             err == ESP_ERR_INVALID_SIZE ? "Range beyond the end of the EPCS" :
             err == ESP_ERR_NOT_FOUND    ? "No EPCS found" : "EPCS read failed");
    ESP_LOGI(TAG, "Lesen: %s, CONF_DONE %s", esp_err_to_name(err),
             cerr == ESP_OK ? "high" : "low");
    return err;
}

/* -- Schreiben -- */

static esp_err_t erase_sector(uint32_t addr)
{
    ESP_LOGI(TAG, "Loesche Sektor 0x%06lX", (unsigned long)addr);
    esp_err_t err = cmd_only(EPCS_WREN);
    if (err == ESP_OK) {
        put_addr(EPCS_SE, addr);
        err = xfer(4);
    }
    return err == ESP_OK ? wait_ready(EPCS_SE_TIMEOUT_MS) : err;
}

/* Eine volle oder (am Ende) angefangene Page aus s_page schreiben. Der Sektor
 * wird beim ersten Betreten geloescht -- so verteilt sich das Loeschen ueber den
 * Upload, statt vorher Sekunden lang den Empfang aufzuhalten. */
static esp_err_t flush_page(void)
{
    uint32_t addr = s_wr_pos - s_page_fill;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && addr + s_page_fill > s_erased_end) {
        err = erase_sector(s_erased_end);
        s_erased_end += EPCS_SECTOR;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = cmd_only(EPCS_WREN);
    if (err == ESP_OK) {
        put_addr(EPCS_PP, addr);
        memcpy(s_tx + 4, s_page, s_page_fill);
        err = xfer(4 + s_page_fill);
    }
    if (err == ESP_OK) {
        err = wait_ready(EPCS_PP_TIMEOUT_MS);
    }
    s_page_fill = 0;
    return err;
}

esp_err_t fpga_prog_write_begin(uint32_t size, bool rev)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size == 0) {
        set_last("Empty file");
        return ESP_ERR_INVALID_SIZE;
    }
    if (size > FPGA_IMAGE_MAX) {
        set_last("Image too large - at most 512 kB, the game ROMs start at 0x080000");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = session_open();
    if (err != ESP_OK) {
        set_last("SPI bus not available");
        return err;
    }
    err = read_id();
    if (err != ESP_OK) {
        set_last("No EPCS found");
        session_close();
        return err;
    }
    if (size > s_size) {
        set_last("Image larger than the EPCS");
        session_close();
        return ESP_ERR_INVALID_SIZE;
    }
    s_busy = true;
    s_last[0] = '\0';
    s_rev = rev;
    s_wr_size = size;
    s_wr_pos = 0;
    s_erased_end = 0;
    s_page_fill = 0;
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    ESP_LOGI(TAG, "Schreibe %lu Byte ab 0x000000, Bitdrehung %s",
             (unsigned long)size, rev ? "an" : "aus");
    return ESP_OK;
}

esp_err_t fpga_prog_write_chunk(const uint8_t *data, size_t len)
{
    if (!s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > s_wr_size - s_wr_pos) {
        set_last("More data than announced");
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        s_page[s_page_fill++] = s_rev ? rev8(data[i]) : data[i];
        s_wr_pos++;
        if (s_page_fill == EPCS_PAGE) {
            mbedtls_sha256_update(&s_sha, s_page, EPCS_PAGE);
            esp_err_t err = flush_page();
            if (err != ESP_OK) {
                set_last(err == ESP_ERR_TIMEOUT ? "EPCS does not respond (write timeout)"
                                                : "EPCS write failed");
                return err;
            }
        }
    }
    return ESP_OK;
}

void fpga_prog_write_abort(void)
{
    if (!s_busy) {
        return;
    }
    mbedtls_sha256_free(&s_sha);
    s_busy = false;
    session_close();
    ESP_LOGW(TAG, "Schreiben abgebrochen bei %lu von %lu Byte",
             (unsigned long)s_wr_pos, (unsigned long)s_wr_size);
    if (s_last[0] == '\0') {   /* sonst steht dort schon der genauere Grund */
        set_last("Upload aborted - the FPGA will not start until a complete image is written");
    }
}

esp_err_t fpga_prog_write_end(void)
{
    if (!s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (s_wr_pos != s_wr_size) {
        set_last("Upload incomplete");
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && s_page_fill > 0) {
        mbedtls_sha256_update(&s_sha, s_page, s_page_fill);
        err = flush_page();
        if (err != ESP_OK) {
            set_last("EPCS write failed");
        }
    }
    uint8_t want[32], got[32];
    mbedtls_sha256_finish(&s_sha, want);
    mbedtls_sha256_free(&s_sha);

    /* Zuruecklesen: dieselben Bytes, die geschrieben wurden (nach der Drehung). */
    if (err == ESP_OK) {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        for (uint32_t a = 0; err == ESP_OK && a < s_wr_size; a += READ_CHUNK) {
            uint32_t n = s_wr_size - a < READ_CHUNK ? s_wr_size - a : READ_CHUNK;
            memset(s_tx, 0, 4 + n);
            put_addr(EPCS_READ, a);
            err = xfer(4 + n);
            mbedtls_sha256_update(&sha, s_rx + 4, n);
        }
        mbedtls_sha256_finish(&sha, got);
        mbedtls_sha256_free(&sha);
        if (err == ESP_OK && memcmp(want, got, sizeof(want)) != 0) {
            ESP_LOGE(TAG, "Verify: SHA-256 weicht ab");
            set_last("Verify failed - EPCS content differs from the upload");
            err = ESP_ERR_INVALID_CRC;
        }
    }

    s_busy = false;
    esp_err_t cerr = session_close();
    if (err == ESP_OK) {
        err = cerr;
        set_last(cerr == ESP_OK ? "Written and verified, FPGA started (CONF_DONE high)"
                                : "Written and verified, but CONF_DONE stays low - "
                                  "wrong file or bit order?");
    }
    ESP_LOGI(TAG, "Schreiben beendet: %s", esp_err_to_name(err));
    return err;
}
