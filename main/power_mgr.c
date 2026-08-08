#include "power_mgr.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "fa_connect.h"
#include "wifi_mgr.h"

static const char *TAG = "power";

/* Wachfenster nach dem Start, wenn DIP1 auf OFF steht.
 * Das ist keine Bequemlichkeit: im Tiefschlaf verschwindet der USB-Serial/JTAG-Port,
 * "idf.py flash" findet dann kein COM7 mehr. In dieser Frist laesst sich flashen und
 * mitlesen, auch wenn der Build erst noch fertig werden muss. */
#define POWER_BOOT_GRACE_MS   30000

/* Takt des Blink-/Waechter-Tasks. Alle Blinkperioden sind Vielfache davon. */
#define POWER_TICK_MS         50

/* Halbperioden der Blinkmuster (in Ticks). */
#define BLINK_HALF_IDLE       (500 / POWER_TICK_MS)   /* 1 Hz  -- wach, keine Kontrolle */
#define BLINK_HALF_ACTIVE     (100 / POWER_TICK_MS)   /* 5 Hz  -- Kontrolle aktiv */
#define BLINK_HALF_GRACE      (50  / POWER_TICK_MS)   /* 10 Hz -- gleich Tiefschlaf */

/* DIP1 muss so viele Ticks hintereinander OFF sein, bevor wir einschlafen.
 * Ein einzelner Fehlmesswert soll nicht den Flipper stillegen. */
#define DIP_DEBOUNCE_TICKS    3

static bool s_led_enabled;   /* false = DIP2 ON, GPIO8 bleibt frei fuer I2C */

static bool dip_active(void)
{
    return (board_dip_read() & BOARD_DIP_ACTIVE) != 0;
}

static void led(bool on)
{
    if (s_led_enabled) {
        board_led_set(on);
    }
}

/* Blinkt mit der gegebenen Halbperiode und beendet sich, sobald DIP1 auf ON geht
 * oder die Frist abgelaufen ist. Rueckgabe: true = DIP1 ist jetzt ON. */
static bool blink_until_active(int half_period_ticks, int total_ticks)
{
    bool on = false;
    for (int t = 0; t < total_ticks; t++) {
        if (t % half_period_ticks == 0) {
            on = !on;
            led(on);
        }
        if (dip_active()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_TICK_MS));
    }
    return dip_active();
}

/* Kein ESP_ERROR_CHECK: schlaegt der Halt fehl, floatet der Pin im Tiefschlaf --
 * unschoen, aber kein Grund, statt schlafen zu gehen in eine Reboot-Schleife zu
 * laufen. Der Fehler soll nur nicht stumm bleiben. */
static void hold_pin(int gpio)
{
    esp_err_t err = gpio_hold_en(gpio);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d laesst sich nicht halten (%s) -- floatet im Tiefschlaf",
                 gpio, esp_err_to_name(err));
    }
}

/* Kehrt nie zurueck. */
static void enter_deep_sleep(void)
{
    /* Erst die Kontrolle sauber zurueckgeben. Ohne das bekaeme die Gegenstelle sie
     * erst nach ihrem eigenen Watchdog-Timeout (~2 s) wieder -- der Flipper stuende
     * so lange. Nimmt zugleich GPIO10 zurueck und stoppt den Watchdog. */
    fa_connect_release();
    led(false);

    /* WLAN geordnet herunterfahren. wifi_mgr_stop() meldet dafuer erst die
     * Event-Handler ab -- ohne das blieb esp_wifi_stop() haengen und das Geraet
     * lief einfach weiter, statt schlafen zu gehen. Im Boot-Gate laeuft WLAN noch
     * gar nicht; der Fall ist dort abgefangen. */
    wifi_mgr_stop();

    /* Beide Ausgaenge ueber den Tiefschlaf festhalten. Ohne Halt wuerden sie
     * floaten; GPIO10 haenge dann allein am Weak-Pull-Up des FPGA, und GPIO8
     * koennte die LED schwach gluehen lassen. Der ESP32-C3 kann digitale Pads im
     * Tiefschlaf halten (SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP). */
    hold_pin(BOARD_PIN_CTRL_REQ);
    if (s_led_enabled) {
        hold_pin(BOARD_PIN_LED);
    }
    gpio_deep_sleep_hold_en();

    /* DIP1 ON = GND = low. Nur GPIO0..5 koennen den C3 wecken; GPIO0 ist dabei. */
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL << BOARD_PIN_DIP1,
                                                      ESP_GPIO_WAKEUP_GPIO_LOW));

    ESP_LOGI(TAG, "DIP1 = OFF -- Tiefschlaf. Aufwecken: DIP1 auf ON stellen.");
    esp_deep_sleep_start();
}

static void power_task(void *arg)
{
    (void)arg;
    int tick = 0;
    int off_count = 0;
    bool on = false;

    while (true) {
        int half = (fa_connect_info()->state == FA_CONN_ACTIVE)
                   ? BLINK_HALF_ACTIVE : BLINK_HALF_IDLE;
        if (tick % half == 0) {
            on = !on;
            led(on);
        }
        tick++;

        off_count = dip_active() ? 0 : (off_count + 1);
        if (off_count >= DIP_DEBOUNCE_TICKS) {
            enter_deep_sleep();   /* kehrt nicht zurueck */
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_TICK_MS));
    }
}

void power_mgr_boot_gate(void)
{
    /* DIP2 entscheidet, ob GPIO8 ueberhaupt angefasst wird -- der Pin ist mit
     * I2C SCL doppelt belegt (siehe board_pins.h). */
    s_led_enabled = (board_dip_read() & BOARD_DIP_NO_LED) == 0;
    if (s_led_enabled) {
        board_led_init();
    } else {
        ESP_LOGI(TAG, "DIP2 = ON: Blinkanzeige aus, GPIO%d bleibt frei", BOARD_PIN_LED);
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Start: %s",
             cause == ESP_SLEEP_WAKEUP_GPIO ? "aufgeweckt durch DIP1" : "Kaltstart");

    if (dip_active()) {
        return;
    }

    ESP_LOGW(TAG, "DIP1 = OFF -- Tiefschlaf in %d s. Zum Flashen jetzt oder "
                  "DIP1 auf ON stellen.", POWER_BOOT_GRACE_MS / 1000);
    if (blink_until_active(BLINK_HALF_GRACE, POWER_BOOT_GRACE_MS / POWER_TICK_MS)) {
        ESP_LOGI(TAG, "DIP1 waehrend der Frist auf ON gelegt -- bleibe wach");
        return;
    }
    enter_deep_sleep();
}

void power_mgr_start(void)
{
    xTaskCreate(power_task, "power", 2560, NULL, 4, NULL);
}
