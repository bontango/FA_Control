#include "fpga_jtag.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "miniz.h"
#include "soc/gpio_reg.h"

#include "board.h"

static const char *TAG = "jtag";

extern const uint8_t release_e6_start[]  asm("_binary_as_release_jtag_z_start");
extern const uint8_t release_e6_end[]    asm("_binary_as_release_jtag_z_end");
extern const uint8_t release_e22_start[] asm("_binary_as_release_e22_jtag_z_start");
extern const uint8_t release_e22_end[]   asm("_binary_as_release_e22_jtag_z_end");

/* Ein Leer-Design je Device. cs_len/cs_conf_done: Laenge des CHECK_STATUS-Registers
 * und Lage von CONF_DONE darin, je Die verschieden - make_jtag_blob.py gibt beides
 * aus der SVF aus. */
typedef struct {
    uint32_t       idcode;
    const char    *name;
    const uint8_t *start;
    const uint8_t *end;
    int            cs_len;
    int            cs_conf_done;
} release_t;

static const release_t RELEASES[] = {
    { FPGA_IDCODE_EP4CE6,  "EP4CE6",  release_e6_start,  release_e6_end,  603, 226 },
    { FPGA_IDCODE_EP4CE22, "EP4CE22", release_e22_start, release_e22_end, 732, 286 },
};

/* Cyclone IV: Befehlsregister 10 Bit */
#define IR_LEN          10
#define IR_PROGRAM      0x002
#define IR_STARTUP      0x003
#define IR_CHECK_STATUS 0x004
#define IR_IDCODE       0x006
#define IR_BYPASS       0x3FF


/* ---- Pins: direkte Registerzugriffe, gpio_set_level waere zu langsam ------ */

/* Pins je Board, gesetzt von fpga_jtag_init() (v7 bzw. JTAG-Piggyback). Auf dem
 * Sockel bleibt s_ok false, und jeder Einstieg weist mit ESP_ERR_NOT_SUPPORTED ab:
 * dort sind GPIO5..10 I2C, LISY-UART und CTRL_REQ. Die Masken als Variablen kosten
 * nichts, W1TS/W1TC nehmen ohnehin ein Register. */
static bool     s_ok;
static bool     s_tms_is_led;            /* v7: GPIO8 = TMS = LED */
static int      s_tck, s_tms, s_tdi, s_tdo;
static uint32_t M_TCK, M_TMS, M_TDI;

static inline void pin_hi(uint32_t m) { REG_WRITE(GPIO_OUT_W1TS_REG, m); }
static inline void pin_lo(uint32_t m) { REG_WRITE(GPIO_OUT_W1TC_REG, m); }
static inline int tdo(void)
{
    return (REG_READ(GPIO_IN_REG) >> s_tdo) & 1;
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

static SemaphoreHandle_t s_tap;   /* ein Master am TAP: EPCS, vJTAG, Diagnose */

void fpga_jtag_init(void)
{
    switch (board_type()) {
    case BOARD_V7:
        s_tck = V7_PIN_JTAG_TCK; s_tms = V7_PIN_JTAG_TMS;
        s_tdi = V7_PIN_JTAG_TDI; s_tdo = V7_PIN_JTAG_TDO;
        s_tms_is_led = true;
        break;
    case BOARD_JTAG:
        s_tck = JP_PIN_JTAG_TCK; s_tms = JP_PIN_JTAG_TMS;
        s_tdi = JP_PIN_JTAG_TDI; s_tdo = JP_PIN_JTAG_TDO;
        s_tms_is_led = false;
        break;
    default:
        s_ok = false;
        return;
    }
    M_TCK = 1UL << s_tck;
    M_TMS = 1UL << s_tms;
    M_TDI = 1UL << s_tdi;
    if (!s_tap) {
        s_tap = xSemaphoreCreateMutex();
    }
    s_ok = true;
}

static void tap_open(void)
{
    if (s_tap) {
        xSemaphoreTake(s_tap, portMAX_DELAY);
    }
    if (s_tms_is_led) {
        board_led_lock(true);
    }
    gpio_set_level(s_tck, 0);
    gpio_set_level(s_tms, 1);
    gpio_set_level(s_tdi, 1);
    gpio_set_direction(s_tck, GPIO_MODE_OUTPUT);
    gpio_set_direction(s_tms, GPIO_MODE_OUTPUT);
    gpio_set_direction(s_tdi, GPIO_MODE_OUTPUT);
    gpio_set_direction(s_tdo, GPIO_MODE_INPUT);
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
    gpio_set_direction(s_tck, GPIO_MODE_INPUT);
    gpio_set_direction(s_tdi, GPIO_MODE_INPUT);
    /* TMS bleibt Ausgang, high = Ruhepegel (TAP bleibt im Test-Logic-Reset).
     * Board v7: TMS = LED, high = LED aus. JTAG-Piggyback: TCK bekommt ueber
     * board_init() den internen Pull-down, der bei INPUT erhalten bleibt. */
    if (s_tms_is_led) {
        board_led_lock(false);
    }
    if (s_tap) {
        xSemaphoreGive(s_tap);
    }
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
    *id = 0;
    if (!s_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
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

/* Gepackter Strom aus dem Flash: beim ersten Aufruf die ganze Laenge, dann Ende. */
typedef struct {
    const uint8_t *p;
    size_t         n;
} mem_src_t;

static int src_mem(void *ctx, const uint8_t **p)
{
    mem_src_t *m = ctx;
    *p = m->p;
    int n = (int)m->n;
    m->n = 0;
    return n;
}

/* Entpackt den Strom in einem 32-kB-Ringpuffer (= Woerterbuch von tinfl) und
 * schiebt jedes fertige Stueck sofort. Die gepackten Daten liefert src stueckweise
 * (> 0 Bytes, 0 = Ende, < 0 = Fehler); so kann der Strom auch direkt aus einem
 * HTTP-Upload kommen. Der Decompressor und der Puffer stehen nur fuer diese Dauer
 * im Heap. */
static esp_err_t shift_stream(fpga_jtag_src_fn src, void *ctx, size_t *shifted)
{
    tinfl_decompressor *d = malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    if (!d || !dict) {
        free(d);
        free(dict);
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(d);
    const uint8_t *in = NULL;
    size_t in_left = 0, dict_ofs = 0, total = 0;
    bool eof = false;
    esp_err_t err = ESP_OK;

    /* Ein Stueck zurueckhalten, damit beim letzten Byte feststeht, dass es das
     * letzte ist (TMS=1). */
    uint8_t held = 0;
    bool have_held = false;

    while (true) {
        if (in_left == 0 && !eof) {
            int n = src(ctx, &in);
            if (n < 0) {
                ESP_LOGE(TAG, "Datenquelle meldet Fehler");
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            eof = (n == 0);
            in_left = n;
        }
        size_t in_sz = in_left, out_sz = TINFL_LZ_DICT_SIZE - dict_ofs;
        tinfl_status st = tinfl_decompress(d, in, &in_sz, dict, dict + dict_ofs, &out_sz,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER |
                                           (eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT));
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
        if (st < TINFL_STATUS_DONE || (st == TINFL_STATUS_NEEDS_MORE_INPUT && eof)) {
            ESP_LOGE(TAG, "Entpacken fehlgeschlagen (%d)", st);
            err = ESP_ERR_INVALID_CRC;
            break;
        }
    }
    if (have_held) {
        shift_bytes(&held, 1, true);   /* auch im Fehlerfall: DR sauber verlassen */
    } else {
        dr_last(0);                    /* gar nichts entpackt: DR trotzdem verlassen */
        err = (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }
    free(d);
    free(dict);
    if (shifted) {
        *shifted = total;
    }
    ESP_LOGI(TAG, "Datenstrom: %u Byte geschoben", (unsigned)total);
    return err;
}

/* PROGRAM, Datenstrom, CHECK_STATUS, STARTUP, BYPASS -- die Folge, die Quartus
 * fuer eine .sof als SVF erzeugt (make_jtag_blob.py prueft sie). Bei geoeffnetem TAP. */
static esp_err_t sram_config(fpga_jtag_src_fn src, void *ctx, int cs_len, int cs_conf_done,
                             int *conf_done_out, size_t *shifted)
{
    shift_ir(IR_PROGRAM);
    runtest_from_pause(10000);
    dr_enter();
    esp_err_t err = shift_stream(src, ctx, shifted);

    shift_ir(IR_CHECK_STATUS);
    runtest_from_pause(50);
    dr_enter();
    int conf_done = 0;
    for (int i = 0; i < cs_len; i++) {
        int o = (i == cs_len - 1) ? dr_last(0) : clk(0, 0);
        if (i == cs_conf_done) {
            conf_done = o;
        }
    }

    shift_ir(IR_STARTUP);
    runtest_from_pause(40960);
    runtest(512);
    shift_ir(IR_BYPASS);
    runtest_from_pause(10000);

    if (conf_done_out) {
        *conf_done_out = conf_done;
    }
    if (err != ESP_OK) {
        return err;
    }
    return conf_done ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t fpga_jtag_load_release(void)
{
    if (!s_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    tap_open();
    uint32_t id = read_idcode();
    const release_t *rel = NULL;
    for (size_t k = 0; k < sizeof(RELEASES) / sizeof(RELEASES[0]); k++) {
        if (RELEASES[k].idcode == id) {
            rel = &RELEASES[k];
            break;
        }
    }
    if (!rel) {
        tap_close();
        ESP_LOGE(TAG, "IDCODE 0x%08lX unbekannt (EP4CE6 0x%08X, EP4CE22 0x%08X)",
                 (unsigned long)id, FPGA_IDCODE_EP4CE6, FPGA_IDCODE_EP4CE22);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "IDCODE 0x%08lX = %s", (unsigned long)id, rel->name);

    int64_t t0 = esp_log_timestamp();
    int conf_done = 0;
    mem_src_t m = { rel->start, (size_t)(rel->end - rel->start) };
    esp_err_t err = sram_config(src_mem, &m, rel->cs_len, rel->cs_conf_done, &conf_done, NULL);
    tap_close();

    ESP_LOGI(TAG, "Leer-Design geladen in %ld ms, CHECK_STATUS CONF_DONE=%d",
             (long)(esp_log_timestamp() - t0), conf_done);
    return err;
}

esp_err_t fpga_jtag_load_sram(fpga_jtag_src_fn src, void *ctx, int cs_len, int cs_conf_done,
                              fpga_jtag_sram_result_t *r)
{
    memset(r, 0, sizeof(*r));
    if (cs_len < 1 || cs_len > 4096 || cs_conf_done < 0 || cs_conf_done >= cs_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    tap_open();
    r->idcode = read_idcode();
    if (r->idcode == 0 || r->idcode == 0xFFFFFFFF) {
        tap_close();
        return ESP_ERR_NOT_FOUND;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = sram_config(src, ctx, cs_len, cs_conf_done, &r->conf_done, &r->bytes);
    tap_close();
    r->ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "SRAM-Design geladen: IDCODE 0x%08lX, %u Byte in %ld ms, CONF_DONE=%d (%s)",
             (unsigned long)r->idcode, (unsigned)r->bytes, (long)r->ms, r->conf_done,
             esp_err_to_name(err));
    return err;
}

/* ---- Virtual JTAG (SLD-Hub) ----------------------------------------------- */

#define IR_USER0  0x00C
#define IR_USER1  0x00E
#define SLD_MFG   0x06E

/* Run-Test/Idle -> n Bit DR (n <= 32, LSB zuerst) -> Run-Test/Idle. Liefert die
 * gleichzeitig an TDO gelesenen Bits. */
static uint32_t dr_shift(uint32_t out, int n)
{
    uint32_t in = 0;
    dr_enter();
    for (int i = 0; i < n; i++) {
        int b = (out >> i) & 1;
        int o = (i == n - 1) ? dr_last(b) : clk(0, b);
        in |= (uint32_t)o << i;
    }
    return in;
}

/* n Nullen in einem einzigen DR-Scan (n darf > 32 sein). */
static void dr_zeros(int n)
{
    dr_enter();
    for (int i = 0; i < n - 1; i++) {
        clk(0, 0);
    }
    dr_last(0);
}

/* 8 x 4-Bit-Scans, untere Halbbytes zuerst. */
static uint32_t hub_word(void)
{
    uint32_t w = 0;
    for (int i = 0; i < 8; i++) {
        w |= (dr_shift(0, 4) & 0xF) << (4 * i);
    }
    return w;
}

static void hub_read(fpga_vjtag_hub_t *h)
{
    shift_ir(IR_USER1);
    runtest_from_pause(1);
    dr_zeros(64);                       /* Adresse 0 = der Hub selbst */
    shift_ir(IR_USER0);
    runtest_from_pause(1);

    h->hub_info = hub_word();
    h->n = h->hub_info & 0xFF;
    h->nodes = (h->hub_info >> 19) & 0xFF;
    h->m = 0;
    while ((1 << h->m) < h->nodes + 1) {
        h->m++;
    }
    h->addr = 0;
    if (((h->hub_info >> 8) & 0x7FF) != SLD_MFG) {
        h->nodes = 0;                   /* kein Hub: nichts weiter lesen */
        return;
    }
    for (int k = 1; k <= h->nodes; k++) {
        uint32_t info = hub_word();
        if (k <= FPGA_VJTAG_MAX_NODES) {
            h->node_info[k - 1] = info;
        }
        if (h->addr == 0 && ((info >> 19) & 0xFF) == FPGA_VJTAG_NODE_VJTAG) {
            h->addr = k;
        }
    }
}

/* Knoten + VIR waehlen, danach steht USER0 im Befehlsregister. Die Takte in
 * Run-Test/Idle gibt es fuer den Fall, dass der Knoten Flags aus einem anderen
 * Takt nachzieht (dcfifo, Stufe 3). */
static void vir_select(const fpga_vjtag_hub_t *h, uint32_t vir)
{
    shift_ir(IR_USER1);
    runtest_from_pause(1);
    dr_shift(((uint32_t)h->addr << h->n) | vir, h->m + h->n);
    shift_ir(IR_USER0);
    runtest_from_pause(4);
}

esp_err_t fpga_vjtag_probe(fpga_vjtag_hub_t *hub)
{
    memset(hub, 0, sizeof(*hub));
    if (!s_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    tap_open();
    hub->idcode = read_idcode();
    hub_read(hub);
    tap_close();

    ESP_LOGI(TAG, "vJTAG: IDCODE 0x%08lX, HUB_INFO 0x%08lX (Knoten %d, m %d, n %d)",
             (unsigned long)hub->idcode, (unsigned long)hub->hub_info,
             hub->nodes, hub->m, hub->n);
    for (int k = 1; k <= hub->nodes && k <= FPGA_VJTAG_MAX_NODES; k++) {
        uint32_t i = hub->node_info[k - 1];
        ESP_LOGI(TAG, "vJTAG: NODE_INFO 0x%08lX Adresse %d: ID 0x%02lX, Hersteller 0x%03lX, "
                      "Instanz %lu, Version %lu",
                 (unsigned long)i, k, (unsigned long)((i >> 19) & 0xFF),
                 (unsigned long)((i >> 8) & 0x7FF), (unsigned long)(i & 0xFF),
                 (unsigned long)(i >> 27));
    }
    if (hub->nodes == 0) {
        ESP_LOGW(TAG, "vJTAG: kein SLD-Hub (Design ohne Virtual JTAG oder FPGA leer)");
        return ESP_ERR_NOT_FOUND;
    }
    if (hub->addr == 0) {
        ESP_LOGW(TAG, "vJTAG: Hub ohne Virtual-JTAG-Knoten");
        return ESP_ERR_NOT_FOUND;
    }
    if (hub->m + hub->n > 32) {
        ESP_LOGE(TAG, "vJTAG: m + n = %d > 32", hub->m + hub->n);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

/* Feste Muster zuerst (Randbits, Wechselmuster), danach ein Xorshift. */
static uint32_t test_pattern(int i, uint32_t *state)
{
    static const uint32_t fixed[] = {
        0x00000000, 0xFFFFFFFF, 0x12345678, 0xA5A5A5A5, 0x5A5A5A5A,
        0x80000000, 0x00000001, 0xDEADBEEF,
    };
    if (i < (int)(sizeof(fixed) / sizeof(fixed[0]))) {
        return fixed[i];
    }
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

esp_err_t fpga_vjtag_test(const fpga_vjtag_hub_t *hub, int count, fpga_vjtag_test_t *r)
{
    memset(r, 0, sizeof(*r));
    if (hub->addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    tap_open();
    vir_select(hub, FPGA_VJTAG_VIR_ID);
    r->id = dr_shift(0, 32);
    if ((r->id >> 8) != FPGA_VJTAG_ID_PROBE) {
        /* Kein Testknoten: VIR 1 waere z.B. der Datenkanal von fa_vjtag_link, die
         * Muster gingen als Bytes an FA-Control. */
        tap_close();
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Rueckkanal: jeder Scan schiebt ein neues Muster hinein und liest das des
     * vorigen Scans (Update-DR speichert, Capture-DR liefert zurueck). Der erste
     * Scan liest den alten Inhalt und zaehlt nicht. */
    vir_select(hub, FPGA_VJTAG_VIR_ECHO);
    uint32_t state = 0x2545F491, prev = test_pattern(0, &state);
    bool have_fail = false;
    dr_shift(prev, 32);
    int64_t t0 = esp_timer_get_time();
    for (int i = 1; i <= count; i++) {
        uint32_t p = test_pattern(i, &state);
        uint32_t got = dr_shift(p, 32);
        if (got == prev) {
            r->echo_ok++;
        } else if (!have_fail) {
            have_fail = true;
            r->fail_out = prev;
            r->fail_in = got;
        }
        r->echo_total++;
        prev = p;
    }
    r->echo_us = esp_timer_get_time() - t0;
    tap_close();

    /* Eine komplette Sitzung, wie sie spaeter je LISY-Abfrage anfaellt. */
    t0 = esp_timer_get_time();
    tap_open();
    vir_select(hub, FPGA_VJTAG_VIR_ID);
    uint32_t id2 = dr_shift(0, 32);
    tap_close();
    r->session_us = esp_timer_get_time() - t0;

    ESP_LOGI(TAG, "vJTAG-Test: ID 0x%08lX (zweite Sitzung 0x%08lX), Rueckkanal %d/%d ok, "
                  "%d x 32 Bit in %lld us, Sitzung %lld us",
             (unsigned long)r->id, (unsigned long)id2, r->echo_ok, r->echo_total,
             count, (long long)r->echo_us, (long long)r->session_us);
    if (r->echo_ok != r->echo_total) {
        ESP_LOGW(TAG, "vJTAG-Test: erster Fehler erwartet 0x%08lX, gelesen 0x%08lX",
                 (unsigned long)r->fail_out, (unsigned long)r->fail_in);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return (id2 == r->id) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

uint32_t fpga_vjtag_read_id(const fpga_vjtag_hub_t *hub)
{
    if (hub->addr == 0) {
        return 0;
    }
    tap_open();
    vir_select(hub, FPGA_VJTAG_VIR_ID);
    uint32_t id = dr_shift(0, 32);
    tap_close();
    return id;
}

/* ---- fa_vjtag_link: Bytes und Steuerbit (Stufe 3) ------------------------- */

/* Datenregister (VIR 1, 16 Bit): TDI [7:0] Byte, [8] tx_valid;
 * TDO [7:0] Byte, [8] rx_valid, [9] tx_afull. */
#define LINK_TX_VALID  0x100
#define LINK_RX_VALID  0x100
#define LINK_TX_AFULL  0x200
/* Steuerregister (VIR 3, 8 Bit): TDI [0] Wert, [1] schreiben; TDO [0] CTRL_REQ. */
#define LINK_CTRL_WRITE 0x02

int fpga_vjtag_xfer(const fpga_vjtag_hub_t *hub, const uint8_t *out, int n_out,
                    uint8_t *in, int max_in, int timeout_ms, int *scans)
{
    if (hub->addr == 0) {
        return -1;
    }
    int got = 0, sent = 0, nscan = 0;
    tap_open();
    vir_select(hub, FPGA_VJTAG_VIR_DATA);

    int64_t t_last = esp_timer_get_time();
    const int64_t limit = (int64_t)timeout_ms * 1000;
    bool afull = false;
    while (true) {
        uint32_t o = 0;
        if (sent < n_out && !afull) {
            o = LINK_TX_VALID | out[sent++];
        }
        uint32_t r = dr_shift(o, 16);
        nscan++;
        afull = (r & LINK_TX_AFULL) != 0;
        if (r & LINK_RX_VALID) {
            if (got < max_in) {
                in[got] = (uint8_t)r;
            }
            got++;
            t_last = esp_timer_get_time();
        }
        if (sent >= n_out) {
            if (got >= max_in) {
                break;
            }
            /* Pause seit dem letzten Byte: vor der ersten Antwort zaehlt timeout_ms
             * ab dem letzten gesendeten Byte, danach 2 ms (eine UART-Zeit sind 87 us). */
            int64_t gap = got ? 2000 : limit;
            if (esp_timer_get_time() - t_last > gap) {
                break;
            }
        } else {
            t_last = esp_timer_get_time();
        }
    }
    tap_close();
    if (scans) {
        *scans = nscan;
    }
    return got;
}

/* ---- Byte-Strom fuer lisy.c (Stufe 4) ------------------------------------- */

static fpga_vjtag_hub_t s_link;
static bool    s_link_ok;
static int64_t s_link_checked;          /* letzte Pruefung der Kennung, us */

/* Hub und Kennung zwischenspeichern; Kennung hoechstens alle 2 s neu pruefen,
 * Fehlversuche hoechstens jede Sekunde (laeuft ein Design ohne Link, kostet jeder
 * LISY-Befehl sonst eine Hub-Abfrage). */
static bool link_attach(void)
{
    int64_t now = esp_timer_get_time();
    if (s_link_ok && now - s_link_checked < 2000000) {
        return true;
    }
    if (!s_link_ok && s_link_checked && now - s_link_checked < 1000000) {
        return false;
    }
    s_link_checked = now;
    bool was = s_link_ok;
    s_link_ok = fpga_vjtag_probe(&s_link) == ESP_OK &&
                FPGA_VJTAG_IS_LINK(fpga_vjtag_read_id(&s_link));
    if (s_link_ok != was) {
        ESP_LOGI(TAG, "fa_vjtag_link %s", s_link_ok ? "gefunden" : "nicht (mehr) da");
    }
    return s_link_ok;
}

void fpga_vjtag_link_invalidate(void)
{
    s_link_ok = false;
    s_link_checked = 0;
}

bool fpga_vjtag_link_present(void)
{
    return link_attach();
}

int fpga_vjtag_link_io(const uint8_t *out, int n_out, uint8_t *in, int max_in,
                       int want, int timeout_ms)
{
    if (!link_attach()) {
        return -1;
    }
    int got = 0, sent = 0;
    tap_open();
    vir_select(&s_link, FPGA_VJTAG_VIR_DATA);

    /* Senden; was dabei schon zurueckkommt, wird mitgenommen. Bei tx_afull
     * leere Scans, bis wieder Platz ist (hoechstens 1 s, dann aufgeben). */
    int64_t t0 = esp_timer_get_time();
    bool afull = false, last_rx = false;
    while (sent < n_out) {
        uint32_t o = afull ? 0 : (LINK_TX_VALID | out[sent]);
        uint32_t r = dr_shift(o, 16);
        if (!afull) {
            sent++;
        }
        afull = (r & LINK_TX_AFULL) != 0;
        last_rx = (r & LINK_RX_VALID) != 0;
        if (last_rx) {
            if (got < max_in) {
                in[got++] = (uint8_t)r;
            }
        }
        if (esp_timer_get_time() - t0 > 1000000) {
            ESP_LOGW(TAG, "fa_vjtag_link: Puffer zum FA bleibt voll, %d/%d Byte gesendet",
                     sent, n_out);
            break;
        }
    }

    /* Empfangen: bis want Bytes da sind oder timeout_ms um ist; danach noch
     * abholen, solange Byte auf Byte nachkommt (Mailbox liefert eins je Scan). */
    t0 = esp_timer_get_time();
    const int64_t limit = (int64_t)timeout_ms * 1000;
    while (got < max_in) {
        bool waiting = got < want && esp_timer_get_time() - t0 <= limit;
        if (!waiting && !last_rx) {
            break;
        }
        uint32_t r = dr_shift(0, 16);
        last_rx = (r & LINK_RX_VALID) != 0;
        if (last_rx) {
            in[got++] = (uint8_t)r;
        }
    }
    tap_close();
    return got;
}

esp_err_t fpga_vjtag_link_ctrl(bool on)
{
    if (!link_attach()) {
        return ESP_ERR_NOT_FOUND;
    }
    int v = -1;
    fpga_vjtag_ctrl(&s_link, on ? 1 : 0, &v);
    return v == (on ? 1 : 0) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t fpga_vjtag_ctrl(const fpga_vjtag_hub_t *hub, int set, int *val)
{
    if (hub->addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    tap_open();
    vir_select(hub, FPGA_VJTAG_VIR_CTRL);
    if (set >= 0) {
        dr_shift(LINK_CTRL_WRITE | (set ? 1 : 0), 8);
        runtest(8);                     /* Uebernahme im Systemtakt, Rueckmeldung nach TCK */
    }
    uint32_t r = dr_shift(0, 8);        /* ohne Schreib-Bit: nur lesen */
    tap_close();
    if (val) {
        *val = r & 1;
    }
    return ESP_OK;
}
