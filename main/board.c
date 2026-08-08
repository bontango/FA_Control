#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "board";

/* Eingaenge mit internem Pull-Up: Taster + 4er-DIP-Bank. */
#define BOARD_INPUT_MASK ( (1ULL << BOARD_PIN_BUTTON) | \
                           (1ULL << BOARD_PIN_DIP1)   | \
                           (1ULL << BOARD_PIN_DIP2)   | \
                           (1ULL << BOARD_PIN_DIP3)   | \
                           (1ULL << BOARD_PIN_DIP4) )

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

void board_led_set(bool on)
{
    gpio_set_level(BOARD_PIN_LED, on ? BOARD_LED_ACTIVE_LEVEL : !BOARD_LED_ACTIVE_LEVEL);
}

void board_ctrl_request(bool active)
{
    int level = active ? BOARD_CTRL_ACTIVE_LEVEL : !BOARD_CTRL_ACTIVE_LEVEL;
    gpio_set_level(BOARD_PIN_CTRL_REQ, level);
    ESP_LOGI(TAG, "Uebernahme-Anforderung %s", active ? "gesetzt" : "zurueckgenommen");
}

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
