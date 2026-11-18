#include "board.h"

#include <stdio.h>
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fpga_jtag.h"

static const char *TAG = "board";

static board_type_t s_board = BOARD_SOCKET;
static char s_detect_info[96] = "not run";

board_type_t board_type(void)
{
    return s_board;
}

const char *board_name(void)
{
    switch (s_board) {
    case BOARD_V7:   return "v7";
    case BOARD_JTAG: return "jtag";
    default:         return "socket";
    }
}

const char *board_detect_info(void)
{
    return s_detect_info;
}

/* ==== Board-Erkennung ====================================================== *
 *
 * Gemessen wird nur mit Eingaengen und den internen Pulls (~45 kOhm), es wird
 * nichts getrieben. Jedes Board hat aus vorhandenen Widerstaenden eine eigene
 * Signatur (ESP32-C3_EVALUIERUNG.md bzw. TECHNICAL_REFERENCE.md "Board detection"):
 *
 *            | Sockel             | JTAG-Piggyback       | Board v7 / v7.1
 *   GPIO4    | DIP4: 0 V / offen  | Teiler 10k/10k 1,65 V| nCSO
 *   GPIO5    | 4,7k Pull-up (SDA) | TDO hochohmig        | TCK, R2 10k nach GND
 *   GPIO2    | DIP3: 0 V / offen  | frei                 | nCONFIG, R6 10k nach 3,3 V
 *
 * Reihenfolge:
 *  1. JTAG-Piggyback, wenn GPIO4 mit internem Pull-up UND Pull-down im Fenster
 *     1,25..2,05 V liegt. Der Teiler (5k Thevenin) haelt gegen den internen Pull
 *     (+-0,2 V), ein offener DIP-Pin oder ein FPGA-Pin springt dagegen. Streng,
 *     weil diese Variante als einzige GPIO6 als Ausgang treibt (auf dem Sockel
 *     der LISY-RX-Ausgang des FPGA).
 *  2. v7, wenn GPIO5 mit Pull-up low liest UND GPIO2 mit Pull-down > 2,0 V hat.
 *     Unbedenklich: 45k nach GND an nCONFIG laesst es bei ~2,7 V (kein Reset),
 *     ohne TCK-Flanke passiert am TAP nichts.
 *  3. sonst Sockel. Das ist die eingesetzte Hardware und die Variante, die am
 *     wenigsten treibt (GPIO7/10/8). Der Pull-up an GPIO5 wird nur protokolliert.
 */

#define DET_SETTLE_MS     5      /* C14 100 nF an nCONFIG: tau ~0,8 ms */
#define DET_SAMPLES       8
#define VT_MIN_MV      1250
#define VT_MAX_MV      2050
#define NCONF_MIN_MV   2000

typedef enum { PULL_NONE, PULL_UP, PULL_DOWN } pull_t;

static void set_pull(int pin, pull_t p)
{
    gpio_set_pull_mode(pin, p == PULL_UP ? GPIO_PULLUP_ONLY :
                            p == PULL_DOWN ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING);
}

static int read_digital(int pin, pull_t p)
{
    set_pull(pin, p);
    vTaskDelay(pdMS_TO_TICKS(DET_SETTLE_MS));
    return gpio_get_level(pin);
}

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t         cali;
} det_adc_t;

/* Mittelwert in mV. Ohne Kalibrierung linear ueber 0..2500 mV (DB_12 beim C3);
 * fuer die groben Fenster oben reicht das. */
static int read_mv(det_adc_t *a, int pin, adc_channel_t ch, pull_t p)
{
    set_pull(pin, p);
    vTaskDelay(pdMS_TO_TICKS(DET_SETTLE_MS));
    int sum = 0;
    for (int i = 0; i < DET_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(a->unit, ch, &raw) != ESP_OK) {
            return -1;
        }
        sum += raw;
    }
    int raw = sum / DET_SAMPLES;
    int mv = raw * 2500 / 4095;
    if (a->cali) {
        adc_cali_raw_to_voltage(a->cali, raw, &mv);
    }
    return mv;
}

static void float_input(uint64_t mask)
{
    gpio_config_t c = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
}

static board_type_t board_detect(void)
{
    /* GPIO2, 4 (ADC) und 5 (digital): alles Eingang ohne Pull als Ausgangslage */
    float_input((1ULL << 2) | (1ULL << 4) | (1ULL << 5));

    det_adc_t a = { 0 };
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    bool adc_ok = adc_oneshot_new_unit(&ucfg, &a.unit) == ESP_OK;
    if (adc_ok) {
        adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_ok = adc_oneshot_config_channel(a.unit, ADC_CHANNEL_4, &ccfg) == ESP_OK &&
                 adc_oneshot_config_channel(a.unit, ADC_CHANNEL_2, &ccfg) == ESP_OK;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t kcfg = {
            .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&kcfg, &a.cali) != ESP_OK) {
            a.cali = NULL;
        }
#endif
    }

    int vt_up = -1, vt_dn = -1, nc_dn = -1;
    if (adc_ok) {
        vt_up = read_mv(&a, JP_PIN_VTARGET, ADC_CHANNEL_4, PULL_UP);
        vt_dn = read_mv(&a, JP_PIN_VTARGET, ADC_CHANNEL_4, PULL_DOWN);
        nc_dn = read_mv(&a, BOARD_PIN_NCONFIG, ADC_CHANNEL_2, PULL_DOWN);
    }
    int io5_up = read_digital(5, PULL_UP);
    int io5_dn = read_digital(5, PULL_DOWN);

    if (a.cali) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(a.cali);
#endif
    }
    if (a.unit) {
        adc_oneshot_del_unit(a.unit);
    }
    /* Pulls wieder weg; die endgueltige Konfiguration macht board_init() */
    float_input((1ULL << 2) | (1ULL << 4) | (1ULL << 5));

    board_type_t t = BOARD_SOCKET;
    const char *why;
    if (vt_up >= VT_MIN_MV && vt_up <= VT_MAX_MV && vt_dn >= VT_MIN_MV && vt_dn <= VT_MAX_MV) {
        t = BOARD_JTAG;
        why = "divider on GPIO4";
    } else if (io5_up == 0 && nc_dn >= NCONF_MIN_MV) {
        t = BOARD_V7;
        why = "TCK pull-down + nCONFIG pull-up";
    } else if (io5_dn == 1) {
        why = "I2C pull-up on GPIO5";
    } else {
        why = "no signature, fallback";
    }
    snprintf(s_detect_info, sizeof(s_detect_info),
             "%s; io4 %d/%d mV, io2 %d mV, io5 %d/%d%s",
             why, vt_up, vt_dn, nc_dn, io5_up, io5_dn, adc_ok ? "" : ", no ADC");
    return t;
}

/* ==== Initialisierung ======================================================= */

static void init_jtag_piggyback(void)
{
    /* JTAG-Leitungen zum Header P5: im Ruhezustand Eingaenge. TMS/TDI haben auf
     * den Zielboards Pull-ups (v4.x: R5/R8), TCK hat dort keinen Pull-down --
     * den gibt der ESP intern (fpga_jtag.c tap_close()). */
    float_input((1ULL << JP_PIN_JTAG_TMS) | (1ULL << JP_PIN_JTAG_TDI) |
                (1ULL << JP_PIN_JTAG_TDO) | (1ULL << JP_PIN_VTARGET));
    gpio_config_t tck_cfg = {
        .pin_bit_mask = 1ULL << JP_PIN_JTAG_TCK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&tck_cfg));
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    ESP_LOGI(TAG, "JTAG-Piggyback: TCK=GPIO%d TMS=GPIO%d TDI=GPIO%d TDO=GPIO%d hochohmig, "
                  "kein LISY-UART/CTRL_REQ (ohne DIP-Bank: immer wach)",
             JP_PIN_JTAG_TCK, JP_PIN_JTAG_TMS, JP_PIN_JTAG_TDI, JP_PIN_JTAG_TDO);
}

static void init_ctrl_req(void)
{
    /* Uebernahme-Anforderung -- zuerst inaktiv, damit das Spiel beim Einschalten
     * des ESP nicht angehalten wird. */
    gpio_set_level(BOARD_PIN_CTRL_REQ, !BOARD_CTRL_ACTIVE_LEVEL);
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_CTRL_REQ),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    board_ctrl_request(false);
}

static void init_v7(void)
{
    init_ctrl_req();
    /* Programmierleitungen zum FPGA: im Ruhezustand Eingaenge OHNE internen Pull.
     * Die Pegel geben die Widerstaende der Piggyback vor (R2..R7); ein interner
     * Pull-Up an TCK etwa arbeitete gegen R2. GPIO8 (TMS = LED) fehlt hier, den
     * konfiguriert board_led_init(). */
    float_input((1ULL << BOARD_PIN_AS_DCLK)   | (1ULL << BOARD_PIN_AS_ASDO)  |
                (1ULL << BOARD_PIN_AS_DATA0)  | (1ULL << BOARD_PIN_AS_NCSO)  |
                (1ULL << BOARD_PIN_NCONFIG)   | (1ULL << BOARD_PIN_CONF_DONE) |
                (1ULL << V7_PIN_JTAG_TCK)     | (1ULL << V7_PIN_JTAG_TDI)    |
                (1ULL << V7_PIN_JTAG_TDO));
    ESP_LOGI(TAG, "Board v7: CtrlReq=GPIO%d, FPGA-Leitungen hochohmig, "
                  "CONF_DONE=%d (ohne DIP-Bank: immer wach)",
             BOARD_PIN_CTRL_REQ, gpio_get_level(BOARD_PIN_CONF_DONE));
}

static void init_socket(void)
{
    init_ctrl_req();
    /* Taster + DIP-Bank als Eingaenge mit internem Pull-Up. */
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BUTTON) | (1ULL << BOARD_PIN_DIP1) |
                        (1ULL << BOARD_PIN_DIP2)   | (1ULL << BOARD_PIN_DIP3) |
                        (1ULL << BOARD_PIN_DIP4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));
    /* Damit der Pull-Up an den DIPs steht, bevor power_mgr DIP1 liest. */
    esp_rom_delay_us(100);

    /* I2C-Pins bleiben reserviert — Treiber-Init erst bei Bedarf.
     * GPIO8 (= SCL = LED) bleibt hier bewusst unangetastet, siehe board_led_init(). */
    uint8_t dip = board_dip_read();
    ESP_LOGI(TAG,
             "Pins reserviert: CtrlReq=GPIO%d Taster=GPIO%d DIP=%d/%d/%d/%d "
             "I2C(SDA=%d,SCL=%d) Reserve=GPIO%d",
             BOARD_PIN_CTRL_REQ, BOARD_PIN_BUTTON,
             BOARD_PIN_DIP1, BOARD_PIN_DIP2, BOARD_PIN_DIP3, BOARD_PIN_DIP4,
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_PIN_RESERVED);
    ESP_LOGI(TAG, "DIP-Bank 0x%X: DIP1=%s (%s), DIP2=%s (Blinkanzeige %s)",
             dip,
             (dip & BOARD_DIP_ACTIVE) ? "ON" : "OFF",
             (dip & BOARD_DIP_ACTIVE) ? "wach" : "Tiefschlaf",
             (dip & BOARD_DIP_NO_LED) ? "ON" : "OFF",
             (dip & BOARD_DIP_NO_LED) ? "aus" : "an");
}

void board_init(void)
{
    /* Vor dem Tiefschlaf haelt power_mgr CTRL_REQ und LED fest (gpio_hold_en).
     * Nach dem Aufwachen muss der Halt weg, sonst laeuft die Konfiguration ins
     * Leere. Beide Pins sind nicht an der Erkennung beteiligt. */
    gpio_hold_dis(BOARD_PIN_CTRL_REQ);
    gpio_hold_dis(BOARD_PIN_LED);
    gpio_deep_sleep_hold_dis();

    s_board = board_detect();
    ESP_LOGI(TAG, "Board erkannt: %s (%s)", board_name(), s_detect_info);

    switch (s_board) {
    case BOARD_JTAG: init_jtag_piggyback(); break;
    case BOARD_V7:   init_v7();             break;
    default:         init_socket();         break;
    }
}

/* ---- Blaue Onboard-LED (GPIO8, active low) ------------------------------- */

void board_led_init(void)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    board_led_set(false);
}

static volatile bool s_led_locked;

void board_led_set(bool on)
{
    if (s_led_locked) {
        return;
    }
    gpio_set_level(BOARD_PIN_LED, on ? BOARD_LED_ACTIVE_LEVEL : !BOARD_LED_ACTIVE_LEVEL);
}

void board_led_lock(bool locked)
{
    if (locked) {
        s_led_locked = true;
        /* Der Blink-Task (Prio 4) kann gerade mitten in board_led_set() stehen,
         * vom httpd-Task (Prio 5) verdraengt. Kurz warten, dann ist er durch. */
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        gpio_set_level(BOARD_PIN_LED, !BOARD_LED_ACTIVE_LEVEL);
        s_led_locked = false;
    }
}

void board_ctrl_request(bool active)
{
    if (s_board == BOARD_JTAG) {
        /* Keine Leitung: Bit 0 im Steuerregister von fa_vjtag_link. Totmann im
         * FPGA: ohne Datenscan 2 s lang (Watchdog alle 500 ms) faellt es ab. */
        esp_err_t err = fpga_vjtag_link_ctrl(active);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Uebernahme-Anforderung %s (vJTAG)", active ? "gesetzt" : "zurueckgenommen");
        } else {
            ESP_LOGW(TAG, "Uebernahme-Anforderung (%s) ueber vJTAG: %s",
                     active ? "setzen" : "zuruecknehmen", esp_err_to_name(err));
        }
        return;
    }
    int level = active ? BOARD_CTRL_ACTIVE_LEVEL : !BOARD_CTRL_ACTIVE_LEVEL;
    gpio_set_level(BOARD_PIN_CTRL_REQ, level);
    ESP_LOGI(TAG, "Uebernahme-Anforderung %s", active ? "gesetzt" : "zurueckgenommen");
}

bool board_button_pressed(void)
{
    /* v7: GPIO9 ist TDI, der BOOT-Taster wird nicht ausgewertet. */
    if (s_board == BOARD_V7) {
        return false;
    }
    return gpio_get_level(BOARD_PIN_BUTTON) == 0;
}

uint8_t board_dip_read(void)
{
    /* Nur der Sockel hat eine Bank; sonst immer wach, Blinkanzeige an. */
    if (s_board != BOARD_SOCKET) {
        return BOARD_DIP_ACTIVE;
    }
    uint8_t v = 0;
    if (gpio_get_level(BOARD_PIN_DIP1) == 0) v |= 1 << 0;
    if (gpio_get_level(BOARD_PIN_DIP2) == 0) v |= 1 << 1;
    if (gpio_get_level(BOARD_PIN_DIP3) == 0) v |= 1 << 2;
    if (gpio_get_level(BOARD_PIN_DIP4) == 0) v |= 1 << 3;
    return v;
}
