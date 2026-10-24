#include "fpga_jtag.h"

#include <stdbool.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"
#include "soc/gpio_reg.h"

#include "board.h"

static const char *TAG = "jtag";

extern const uint8_t release_z_start[] asm("_binary_as_release_jtag_z_start");
extern const uint8_t release_z_end[]   asm("_binary_as_release_jtag_z_end");

/* Cyclone IV: Befehlsregister 10 Bit */
#define IR_LEN          10
#define IR_PROGRAM      0x002
#define IR_STARTUP      0x003
#define IR_CHECK_STATUS 0x004
#define IR_IDCODE       0x006
#define IR_BYPASS       0x3FF

/* CHECK_STATUS: 603 Bit, Bit 226 = CONF_DONE (aus der SVF von Quartus) */
#define CS_LEN          603
#define CS_CONF_DONE    226

/* ---- Pins: direkte Registerzugriffe, gpio_set_level waere zu langsam ------ */

#define M_TCK  (1UL << BOARD_PIN_JTAG_TCK)
#define M_TMS  (1UL << BOARD_PIN_JTAG_TMS)
#define M_TDI  (1UL << BOARD_PIN_JTAG_TDI)

static inline void pin_hi(uint32_t m) { REG_WRITE(GPIO_OUT_W1TS_REG, m); }
static inline void pin_lo(uint32_t m) { REG_WRITE(GPIO_OUT_W1TC_REG, m); }
static inline int tdo(void)
{
    return (REG_READ(GPIO_IN_REG) >> BOARD_PIN_JTAG_TDO) & 1;
}

/* Ein Takt. TDO wird VOR der steigenden Flanke gelesen (das FPGA stellt es mit
 * der fallenden bereit). TCK-high wird mit einem zweiten Schreibzugriff
 * gestreckt: Cyclone IV verlangt >= 20 ns, ein Zugriff allein ist knapp. */
static inline int clk(int tms, int tdi)
{
    tms ? pin_hi(M_TMS) : pin_lo(M_TMS);
    tdi ? pin_hi(M_TDI) : pin_lo(M_TDI);
    int o = tdo();
    pin_hi(M_TCK);
    pin_hi(M_TCK);
    pin_lo(M_TCK);
    return o;
}

static void tap_open(void)
{
    board_led_lock(true);
    gpio_set_level(BOARD_PIN_JTAG_TCK, 0);
    gpio_set_level(BOARD_PIN_JTAG_TMS, 1);
    gpio_set_level(BOARD_PIN_JTAG_TDI, 1);
    gpio_set_direction(BOARD_PIN_JTAG_TCK, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_PIN_JTAG_TMS, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_PIN_JTAG_TDI, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_PIN_JTAG_TDO, GPIO_MODE_INPUT);
    /* Test-Logic-Reset, dann Run-Test/Idle */
    for (int i = 0; i < 6; i++) {
        clk(1, 1);
    }
    clk(0, 1);
}

static void tap_close(void)
{
    /* Zurueck nach Test-Logic-Reset; dort bleibt der TAP, was TMS auch tut. */
    for (int i = 0; i < 6; i++) {
        clk(1, 1);
    }
    gpio_set_direction(BOARD_PIN_JTAG_TCK, GPIO_MODE_INPUT);
    gpio_set_direction(BOARD_PIN_JTAG_TDI, GPIO_MODE_INPUT);
    /* TMS = LED bleibt Ausgang (high = LED aus = TMS-Ruhepegel) */
    board_led_lock(false);
}

/* Run-Test/Idle -> Shift-IR, n Bit, -> Pause-IR (ENDIR IRPAUSE) */
static void shift_ir(uint32_t ir)
{
    clk(1, 0); clk(1, 0); clk(0, 0); clk(0, 0);
    for (int i = 0; i < IR_LEN; i++) {
        clk(i == IR_LEN - 1, (ir >> i) & 1);
    }
    clk(0, 0);   /* Exit1-IR -> Pause-IR */
}

static void runtest(int n)
{
    for (int i = 0; i < n; i++) {
        clk(0, 0);
    }
}

/* Pause-IR -> Run-Test/Idle, dann n Takte dort (RUNTEST) */
static void runtest_from_pause(int n)
{
    clk(1, 0); clk(1, 0); clk(0, 0);
    runtest(n);
}

/* Run-Test/Idle -> Shift-DR */
static void dr_enter(void)
{
    clk(1, 0); clk(0, 0); clk(0, 0);
}

/* Letztes Bit mit TMS=1 (-> Exit1-DR), dann Update-DR -> Run-Test/Idle */
static int dr_last(int tdi)
{
    int o = clk(1, tdi);
    clk(1, 0);
    clk(0, 0);
    return o;
}

static uint32_t read_idcode(void)
{
    shift_ir(IR_IDCODE);
    runtest_from_pause(1);
    dr_enter();
    uint32_t id = 0;
    for (int i = 0; i < 32; i++) {
        int o = (i == 31) ? dr_last(1) : clk(0, 1);
        id |= (uint32_t)o << i;
    }
    return id;
}

esp_err_t fpga_jtag_idcode(uint32_t *id)
{
    tap_open();
    *id = read_idcode();
    tap_close();
    ESP_LOGI(TAG, "IDCODE 0x%08lX", (unsigned long)*id);
    return (*id == 0 || *id == 0xFFFFFFFF) ? ESP_ERR_NOT_FOUND : ESP_OK;
}

/* ---- Datenstrom: entpacken und schieben ---------------------------------- */

/* Byte fuer Byte, Bit 0 zuerst. Das allerletzte Bit des Stroms geht mit TMS=1. */
static void shift_bytes(const uint8_t *p, size_t n, bool last_chunk)
{
    for (size_t k = 0; k < n; k++) {
        uint8_t b = p[k];
        bool last_byte = last_chunk && k == n - 1;
        for (int i = 0; i < 8; i++) {
            if (last_byte && i == 7) {
                dr_last((b >> 7) & 1);
            } else {
                clk(0, (b >> i) & 1);
            }
        }
    }
}

/* Entpackt den Strom in einem 32-kB-Ringpuffer (= Woerterbuch von tinfl) und
 * schiebt jedes fertige Stueck sofort. Der Decompressor und der Puffer stehen
 * nur fuer diese Dauer im Heap. */
static esp_err_t shift_stream(void)
{
    tinfl_decompressor *d = malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    if (!d || !dict) {
        free(d);
        free(dict);
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(d);
    const uint8_t *in = release_z_start;
    size_t in_left = release_z_end - release_z_start;
    size_t dict_ofs = 0, total = 0;
    esp_err_t err = ESP_OK;

    /* Ein Stueck zurueckhalten, damit beim letzten Byte feststeht, dass es das
     * letzte ist (TMS=1). */
    uint8_t held = 0;
    bool have_held = false;

    while (true) {
        size_t in_sz = in_left, out_sz = TINFL_LZ_DICT_SIZE - dict_ofs;
        tinfl_status st = tinfl_decompress(d, in, &in_sz, dict, dict + dict_ofs, &out_sz,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER);
        in += in_sz;
        in_left -= in_sz;
        if (out_sz > 0) {
            if (have_held) {
                shift_bytes(&held, 1, false);
            }
            shift_bytes(dict + dict_ofs, out_sz - 1, false);
            held = dict[dict_ofs + out_sz - 1];
            have_held = true;
            total += out_sz;
            dict_ofs = (dict_ofs + out_sz) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (st == TINFL_STATUS_DONE) {
            break;
        }
        if (st < TINFL_STATUS_DONE || (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_left == 0)) {
            ESP_LOGE(TAG, "Entpacken fehlgeschlagen (%d)", st);
            err = ESP_ERR_INVALID_CRC;
            break;
        }
    }
    if (have_held) {
        shift_bytes(&held, 1, true);   /* auch im Fehlerfall: DR sauber verlassen */
    }
    free(d);
    free(dict);
    ESP_LOGI(TAG, "Datenstrom: %u Byte geschoben", (unsigned)total);
    return err;
}

esp_err_t fpga_jtag_load_release(void)
{
    tap_open();
    uint32_t id = read_idcode();
    if (id != FPGA_IDCODE_EP4CE6) {
        tap_close();
        ESP_LOGE(TAG, "IDCODE 0x%08lX statt 0x%08X", (unsigned long)id, FPGA_IDCODE_EP4CE6);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int64_t t0 = esp_log_timestamp();
    shift_ir(IR_PROGRAM);
    runtest_from_pause(10000);
    dr_enter();
    esp_err_t err = shift_stream();

    shift_ir(IR_CHECK_STATUS);
    runtest_from_pause(50);
    dr_enter();
    int conf_done = 0;
    for (int i = 0; i < CS_LEN; i++) {
        int o = (i == CS_LEN - 1) ? dr_last(0) : clk(0, 0);
        if (i == CS_CONF_DONE) {
            conf_done = o;
        }
    }

    shift_ir(IR_STARTUP);
    runtest_from_pause(40960);
    runtest(512);
    shift_ir(IR_BYPASS);
    runtest_from_pause(10000);
    tap_close();

    ESP_LOGI(TAG, "Leer-Design geladen in %ld ms, CHECK_STATUS CONF_DONE=%d",
             (long)(esp_log_timestamp() - t0), conf_done);
    if (err != ESP_OK) {
        return err;
    }
    return conf_done ? ESP_OK : ESP_ERR_INVALID_STATE;
}
