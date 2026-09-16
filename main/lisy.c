#include "lisy.h"

#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_pins.h"

static const char *TAG = "lisy";

#define LISY_UART       UART_NUM_1
#define LISY_TX_GPIO    BOARD_PIN_LISY_TX
#define LISY_RX_GPIO    BOARD_PIN_LISY_RX
#define LISY_BAUD       115200
#define LISY_RX_BUF     256
#define RESP_TIMEOUT_MS 100

static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_wd_timer;
static volatile bool s_wd_enabled;
static volatile int s_wd_last = -1;

static uint8_t s_switch_bitmap[LISY_SWITCH_BITMAP_LEN];
static uint8_t s_lamp_bitmap[LISY_LAMP_BITMAP_LEN];

/* ---- Low-Level ---------------------------------------------------------- */

static void tx(const uint8_t *data, size_t len)
{
    uart_write_bytes(LISY_UART, data, len);
    if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
        char hex[3 * 8 + 1] = {0};
        size_t n = len > 8 ? 8 : len;
        for (size_t i = 0; i < n; i++) {
            snprintf(hex + i * 3, 4, "%02X ", data[i]);
        }
        ESP_LOGD(TAG, "TX: %s", hex);
    }
}

/* Befehl ohne Antwort */
static void cmd_no_resp(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    tx(data, len);
    xSemaphoreGive(s_mutex);
}

/* Befehl mit 1-Byte-Antwort. Rueckgabe: Byte oder -1 bei Timeout. */
static int cmd_resp1(const uint8_t *data, size_t len)
{
    uint8_t resp;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uart_flush_input(LISY_UART);
    tx(data, len);
    int n = uart_read_bytes(LISY_UART, &resp, 1, pdMS_TO_TICKS(RESP_TIMEOUT_MS));
    xSemaphoreGive(s_mutex);
    return (n == 1) ? resp : -1;
}

/* ---- Direkter Buszugriff (rom_boot.c) ------------------------------------ */

bool lisy_bus_take(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lisy_bus_give(void)
{
    xSemaphoreGive(s_mutex);
}

size_t lisy_bus_available(void)
{
    size_t n = 0;
    uart_get_buffered_data_len(LISY_UART, &n);
    return n;
}

int lisy_bus_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    int n = uart_read_bytes(LISY_UART, buf, len, pdMS_TO_TICKS(timeout_ms));
    return n < 0 ? 0 : n;
}

void lisy_bus_write(const uint8_t *buf, size_t len)
{
    uart_write_bytes(LISY_UART, buf, len);
}

void lisy_bus_drain(void)
{
    /* Ohne Sendepuffer kehrt uart_write_bytes() zurueck, sobald die Bytes im
     * FIFO liegen -- nicht, wenn sie gesendet sind. */
    uart_wait_tx_done(LISY_UART, pdMS_TO_TICKS(2000));
    uart_flush_input(LISY_UART);
}

/* ---- Watchdog ----------------------------------------------------------- */

static void wd_timer_cb(void *arg)
{
    if (!s_wd_enabled) {
        return;
    }
    uint8_t c = LISY_CMD_WATCHDOG;
    s_wd_last = cmd_resp1(&c, 1);
}

void lisy_watchdog_enable(bool en)
{
    s_wd_enabled = en;
    ESP_LOGI(TAG, "Watchdog %s", en ? "aktiviert" : "deaktiviert");
}

bool lisy_watchdog_enabled(void)
{
    return s_wd_enabled;
}

int lisy_watchdog_last_result(void)
{
    return s_wd_last;
}

/* ---- Init --------------------------------------------------------------- */

esp_err_t lisy_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t uc = {
        .baud_rate = LISY_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LISY_UART, LISY_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LISY_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(LISY_UART, LISY_TX_GPIO, LISY_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    const esp_timer_create_args_t targs = {
        .callback = wd_timer_cb,
        .name = "lisy_wd",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_wd_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_wd_timer, 500 * 1000));

    ESP_LOGI(TAG, "UART1 bereit: TX=GPIO%d RX=GPIO%d %d Baud",
             LISY_TX_GPIO, LISY_RX_GPIO, LISY_BAUD);
    return ESP_OK;
}

/* ---- Lampen ------------------------------------------------------------- */

void lisy_lamp_set(uint8_t idx, bool on)
{
    uint8_t buf[2] = { on ? LISY_CMD_LAMP_ON : LISY_CMD_LAMP_OFF, idx };
    cmd_no_resp(buf, 2);
    if (on) {
        s_lamp_bitmap[idx / 8] |= 1 << (idx % 8);
    } else {
        s_lamp_bitmap[idx / 8] &= ~(1 << (idx % 8));
    }
}

const uint8_t *lisy_lamp_bitmap(void)
{
    return s_lamp_bitmap;
}

void lisy_lamp_bitmap_clear(void)
{
    memset(s_lamp_bitmap, 0, sizeof(s_lamp_bitmap));
}

/* ---- Spulen -------------------------------------------------------------
 * Spulennummern gehen 1-basiert ueber die Leitung -- so zaehlt LISY sie
 * (lisy_5_28/src/lisy/lisy_w.c: "sol number starts with 1"), und so heissen die
 * Treiber im Atari-Schaltplan (Q1..Qn). Lampen, Schalter und Sounds bleiben
 * 0-basiert. */

void lisy_coil_pulse(uint8_t no)
{
    uint8_t buf[2] = { LISY_CMD_COIL_PULSE, no };
    cmd_no_resp(buf, 2);
}

void lisy_coil_apply_pulse_time(uint8_t count, uint8_t ms)
{
    for (uint8_t no = 1; no <= count; no++) {
        uint8_t buf[3] = { LISY_CMD_COIL_PULSETIME, no, ms };
        cmd_no_resp(buf, 3);
    }
    ESP_LOGI(TAG, "Pulszeit %u ms fuer %u Spulen gesetzt", ms, count);
}

/* ---- Sound -------------------------------------------------------------- */

void lisy_sound_play(uint8_t track, uint8_t idx)
{
    uint8_t buf[3] = { LISY_CMD_SOUND_PLAY, track, idx };
    cmd_no_resp(buf, 3);
}

void lisy_sound_stop(uint8_t track)
{
    uint8_t buf[2] = { LISY_CMD_SOUND_STOP, track };
    cmd_no_resp(buf, 2);
}

/* ---- Displays ----------------------------------------------------------- */

void lisy_display_set(uint8_t d, const char *text, uint8_t width)
{
    if (d > 6) {  /* Befehle 0x1E-0x24 = Displays 0-6 */
        return;
    }
    uint8_t buf[2 + 16];
    if (width > 16) {
        width = 16;
    }
    buf[0] = LISY_CMD_DISPLAY_BASE + d;
    buf[1] = width;

    /* rechtsbuendig: links mit Blank (0x0F) auffuellen */
    size_t tlen = strlen(text);
    if (tlen > width) {
        tlen = width;
    }
    size_t pad = width - tlen;
    for (size_t i = 0; i < width; i++) {
        char c = (i < pad) ? ' ' : text[i - pad];
        buf[2 + i] = (c >= '0' && c <= '9') ? (uint8_t)(c - '0') : 0x0F;
    }
    cmd_no_resp(buf, 2 + width);
}

/* ---- Schalter ----------------------------------------------------------- */

static void switch_bitmap_set(uint8_t idx, bool on)
{
    if (idx >= 127) {
        return;
    }
    if (on) {
        s_switch_bitmap[idx / 8] |= 1 << (idx % 8);
    } else {
        s_switch_bitmap[idx / 8] &= ~(1 << (idx % 8));
    }
}

void lisy_switches_refresh_all(uint8_t count)
{
    if (count > 127) {
        count = 127;
    }
    for (uint8_t i = 0; i < count; i++) {
        uint8_t buf[2] = { LISY_CMD_SWITCH_GET, i };
        int st = cmd_resp1(buf, 2);
        if (st < 0) {
            ESP_LOGW(TAG, "Schalter %u: keine Antwort, Abbruch", i);
            return;
        }
        switch_bitmap_set(i, st == 1);
    }
    ESP_LOGI(TAG, "%u Schalterzustaende eingelesen", count);
}

void lisy_switches_drain_changes(void)
{
    for (int i = 0; i < 64; i++) {
        uint8_t c = LISY_CMD_SWITCH_CHANGED;
        int r = cmd_resp1(&c, 1);
        if (r < 0 || (r & 0x7F) == 127) {
            return; /* keine Antwort oder keine Aenderung mehr */
        }
        switch_bitmap_set(r & 0x7F, (r & 0x80) != 0);
    }
}

const uint8_t *lisy_switch_bitmap(void)
{
    return s_switch_bitmap;
}

/* ---- Init/Reset --------------------------------------------------------- */

int lisy_init_reset(void)
{
    uint8_t c = LISY_CMD_INIT_RESET;
    int r = cmd_resp1(&c, 1);
    ESP_LOGI(TAG, "Init/Reset -> %d", r);
    return r;
}

/* ---- Abfragen (Info-Gruppe 0..9) ---------------------------------------- */

int lisy_get_byte(uint8_t cmd)
{
    return cmd_resp1(&cmd, 1);
}

int lisy_get_byte_p(uint8_t cmd, uint8_t param)
{
    uint8_t buf[2] = { cmd, param };
    return cmd_resp1(buf, 2);
}

int lisy_get_2bytes(uint8_t cmd, uint8_t param, uint8_t *b1, uint8_t *b2)
{
    uint8_t buf[2] = { cmd, param };
    uint8_t resp[2];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uart_flush_input(LISY_UART);
    tx(buf, sizeof(buf));
    int n = uart_read_bytes(LISY_UART, resp, 2, pdMS_TO_TICKS(RESP_TIMEOUT_MS));
    xSemaphoreGive(s_mutex);

    if (n != 2) {
        return -1;
    }
    *b1 = resp[0];
    *b2 = resp[1];
    return 0;
}

int lisy_get_string(uint8_t cmd, char *out, size_t len)
{
    if (len == 0) {
        return -1;
    }
    out[0] = '\0';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uart_flush_input(LISY_UART);
    tx(&cmd, 1);

    /* Bis zum NUL lesen, hoechstens len-1 Zeichen. Ein Timeout mittendrin
     * beendet den String -- der Aufrufer sieht dann eine kuerzere Antwort. */
    size_t p = 0;
    int timeout = 0;
    while (p < len - 1) {
        uint8_t c;
        if (uart_read_bytes(LISY_UART, &c, 1, pdMS_TO_TICKS(RESP_TIMEOUT_MS)) != 1) {
            timeout = 1;
            break;
        }
        if (c == 0) {
            break;
        }
        if (c >= 0x20 && c < 0x7F) {
            out[p++] = (char)c;
        }
    }
    xSemaphoreGive(s_mutex);

    out[p] = '\0';
    if (timeout && p == 0) {
        return -1;
    }
    return (int)p;
}
