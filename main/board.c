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
    /* Status-Ausgang (externer Pull-Up) — zuerst inaktiv. */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_STATUS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    board_status_set(false);

    /* Taster + DIP-Bank als Eingaenge mit internem Pull-Up. */
    gpio_config_t in_cfg = {
        .pin_bit_mask = BOARD_INPUT_MASK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* I2C-Pins (GPIO%d/%d) bleiben reserviert — Treiber-Init erst bei Bedarf. */

    ESP_LOGI(TAG,
             "Pins reserviert: Status=GPIO%d Taster=GPIO%d DIP=%d/%d/%d/%d "
             "I2C(SDA=%d,SCL=%d) Reserve=GPIO%d",
             BOARD_PIN_STATUS, BOARD_PIN_BUTTON,
             BOARD_PIN_DIP1, BOARD_PIN_DIP2, BOARD_PIN_DIP3, BOARD_PIN_DIP4,
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_PIN_RESERVED);
}

void board_status_set(bool active)
{
    int level = active ? BOARD_STATUS_ACTIVE_LEVEL : !BOARD_STATUS_ACTIVE_LEVEL;
    gpio_set_level(BOARD_PIN_STATUS, level);
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
