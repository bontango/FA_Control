#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_FA_BOARD_JTAG_PIGGYBACK
#include "fpga_jtag.h"
#endif

static const char *TAG = "board";

#if CONFIG_FA_BOARD_JTAG_PIGGYBACK

/* JTAG-Leitungen zum Header P5: im Ruhezustand Eingaenge. TMS/TDI haben auf den
 * Zielboards Pull-ups (v4.x: R5/R8), TCK hat dort keinen Pull-down -- den gibt
 * der ESP intern (BOARD_JTAG_TCK_PULLDOWN, fpga_jtag.c tap_close()). */

void board_init(void)
{
    gpio_hold_dis(BOARD_PIN_LED);
    gpio_deep_sleep_hold_dis();

    gpio_config_t jtag_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_JTAG_TMS) | (1ULL << BOARD_PIN_JTAG_TDI) |
                        (1ULL << BOARD_PIN_JTAG_TDO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&jtag_cfg));
    gpio_config_t tck_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_JTAG_TCK,
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
             BOARD_PIN_JTAG_TCK, BOARD_PIN_JTAG_TMS, BOARD_PIN_JTAG_TDI, BOARD_PIN_JTAG_TDO);
}

#else

#if CONFIG_FA_BOARD_PIGGYBACK_V7

/* Programmierleitungen zum FPGA: im Ruhezustand Eingaenge OHNE internen Pull.
 * Die Pegel geben die Widerstaende der Piggyback vor (R2..R7); ein interner
 * Pull-Up an TCK etwa arbeitete gegen R2. GPIO8 (TMS = LED) fehlt hier, den
 * konfiguriert board_led_init(). */
#define BOARD_PROG_MASK ( (1ULL << BOARD_PIN_AS_DCLK)   | \
                          (1ULL << BOARD_PIN_AS_ASDO)   | \
                          (1ULL << BOARD_PIN_AS_DATA0)  | \
                          (1ULL << BOARD_PIN_AS_NCSO)   | \
                          (1ULL << BOARD_PIN_NCONFIG)   | \
                          (1ULL << BOARD_PIN_CONF_DONE) | \
                          (1ULL << BOARD_PIN_JTAG_TCK)  | \
                          (1ULL << BOARD_PIN_JTAG_TDI)  | \
                          (1ULL << BOARD_PIN_JTAG_TDO) )

#else

/* Eingaenge mit internem Pull-Up: Taster + 4er-DIP-Bank. */
#define BOARD_INPUT_MASK ( (1ULL << BOARD_PIN_BUTTON) | \
                           (1ULL << BOARD_PIN_DIP1)   | \
                           (1ULL << BOARD_PIN_DIP2)   | \
                           (1ULL << BOARD_PIN_DIP3)   | \
                           (1ULL << BOARD_PIN_DIP4) )

#endif

void board_init(void)
{
    /* Vor dem Tiefschlaf haelt power_mgr diese beiden Pins fest (gpio_hold_en).
     * Nach dem Aufwachen muss der Halt weg, sonst laeuft die Konfiguration unten
     * ins Leere und die Pins bleiben auf ihren alten Pegeln stehen. */
    gpio_hold_dis(BOARD_PIN_CTRL_REQ);
    gpio_hold_dis(BOARD_PIN_LED);
    gpio_deep_sleep_hold_dis();

    /* Uebernahme-Anforderung — zuerst inaktiv, damit das Spiel beim Einschalten
     * des ESP nicht angehalten wird. */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_CTRL_REQ),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    board_ctrl_request(false);

#if CONFIG_FA_BOARD_PIGGYBACK_V7
    gpio_config_t prog_cfg = {
        .pin_bit_mask = BOARD_PROG_MASK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&prog_cfg));
    ESP_LOGI(TAG, "Board v7: CtrlReq=GPIO%d, FPGA-Leitungen hochohmig, "
                  "CONF_DONE=%d (ohne DIP-Bank: immer wach)",
             BOARD_PIN_CTRL_REQ, gpio_get_level(BOARD_PIN_CONF_DONE));
#else
    /* Taster + DIP-Bank als Eingaenge mit internem Pull-Up. */
    gpio_config_t in_cfg = {
        .pin_bit_mask = BOARD_INPUT_MASK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* I2C-Pins (GPIO%d/%d) bleiben reserviert — Treiber-Init erst bei Bedarf.
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
#endif
}

#endif /* CONFIG_FA_BOARD_JTAG_PIGGYBACK */

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
#if CONFIG_FA_BOARD_JTAG_PIGGYBACK
    /* Keine Leitung: Bit 0 im Steuerregister von fa_vjtag_link. Totmann im FPGA:
     * ohne Datenscan 2 s lang (Watchdog alle 500 ms) faellt es von selbst ab. */
    esp_err_t err = fpga_vjtag_link_ctrl(active);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Uebernahme-Anforderung %s (vJTAG)", active ? "gesetzt" : "zurueckgenommen");
    } else {
        ESP_LOGW(TAG, "Uebernahme-Anforderung (%s) ueber vJTAG: %s",
                 active ? "setzen" : "zuruecknehmen", esp_err_to_name(err));
    }
#else
    int level = active ? BOARD_CTRL_ACTIVE_LEVEL : !BOARD_CTRL_ACTIVE_LEVEL;
    gpio_set_level(BOARD_PIN_CTRL_REQ, level);
    ESP_LOGI(TAG, "Uebernahme-Anforderung %s", active ? "gesetzt" : "zurueckgenommen");
#endif
}

#if CONFIG_FA_BOARD_JTAG_PIGGYBACK

bool board_button_pressed(void)
{
    return gpio_get_level(BOARD_PIN_BUTTON) == 0;
}

/* Keine Bank: immer wach, Blinkanzeige an. */
uint8_t board_dip_read(void)
{
    return BOARD_DIP_ACTIVE;
}

#elif CONFIG_FA_BOARD_PIGGYBACK_V7

/* GPIO9 ist TDI: der BOOT-Taster wird nicht ausgewertet. */
bool board_button_pressed(void)
{
    return false;
}

/* Keine Bank: immer wach, Blinkanzeige an. */
uint8_t board_dip_read(void)
{
    return BOARD_DIP_ACTIVE;
}

#else

bool board_button_pressed(void)
{
    return gpio_get_level(BOARD_PIN_BUTTON) == 0;
}

uint8_t board_dip_read(void)
{
    uint8_t v = 0;
    if (gpio_get_level(BOARD_PIN_DIP1) == 0) v |= 1 << 0;
    if (gpio_get_level(BOARD_PIN_DIP2) == 0) v |= 1 << 1;
    if (gpio_get_level(BOARD_PIN_DIP3) == 0) v |= 1 << 2;
    if (gpio_get_level(BOARD_PIN_DIP4) == 0) v |= 1 << 3;
    return v;
}

#endif
