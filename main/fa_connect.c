#include "fa_connect.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "board.h"
#include "lisy.h"

static const char *TAG = "fa_connect";

/* Die Gegenstelle synchronisiert die Anforderungsleitung ueber zwei Flipflops ein;
 * 20 ms sind dafuer masslos ausreichend und decken auch einen langsamen Pegelwechsel
 * am externen Pull-Up ab. */
#define CTRL_SETTLE_MS 20

static fa_conn_info_t s_info = { .state = FA_CONN_IDLE };

const fa_conn_info_t *fa_connect_info(void)
{
    return &s_info;
}

const char *fa_connect_state_str(void)
{
    switch (s_info.state) {
        case FA_CONN_ACTIVE:       return "Kontrolle aktiv";
        case FA_CONN_DENIED_DIP:   return "Kontrolle verweigert - Option-DIP 4 auf ON stellen";
        case FA_CONN_DENIED_REQ:   return "Kontrolle verweigert - Anforderungsleitung (GPIO10) kam nicht an";
        case FA_CONN_DENIED_OTHER: return "Kontrolle verweigert";
        case FA_CONN_NO_ANSWER:    return "Keine Antwort - Verkabelung und Stromversorgung pruefen";
        default:                   return "Nicht verbunden";
    }
}

/* Info-Gruppe 0..9 abfragen und g_cfg damit fuellen.
 * Nur was wirklich beantwortet wird, ueberschreibt den bisherigen Wert -- eine
 * halb beantwortete Abfrage soll die Konfiguration nicht verstuemmeln. */
static void query_counts(void)
{
    int v;

    lisy_get_string(LISY_CMD_G_HW,        s_info.hw,      sizeof(s_info.hw));
    lisy_get_string(LISY_CMD_G_LISY_VER,  s_info.fw_ver,  sizeof(s_info.fw_ver));
    lisy_get_string(LISY_CMD_G_API_VER,   s_info.api_ver, sizeof(s_info.api_ver));
    lisy_get_string(LISY_CMD_G_GAME_INFO, s_info.game,    sizeof(s_info.game));

    v = lisy_get_byte(LISY_CMD_G_NO_LAMPS);
    if (v > 0 && v <= CFG_MAX_LAMPS) {
        g_cfg.lamps = (uint8_t)v;
    }
    v = lisy_get_byte(LISY_CMD_G_NO_SOL);
    if (v > 0 && v <= CFG_MAX_COILS) {
        g_cfg.coils = (uint8_t)v;
    }
    v = lisy_get_byte(LISY_CMD_G_NO_SW);
    if (v > 0 && v <= CFG_MAX_SWITCHES) {
        g_cfg.switches = (uint8_t)v;
    }
    /* 0 Sounds ist eine gueltige Aussage ("dieses Geraet kann keinen Ton"). */
    v = lisy_get_byte(LISY_CMD_G_NO_SOUNDS);
    if (v >= 0 && v <= CFG_MAX_SOUNDS) {
        g_cfg.sounds = (uint8_t)v;
    }
    v = lisy_get_byte(LISY_CMD_G_NO_DISP);
    if (v > 0 && v <= CFG_MAX_DISPLAYS) {
        g_cfg.displays = (uint8_t)v;
        /* Breite je Display einzeln nachfragen (Opcode 7 liefert Typ + Stellen). */
        for (int i = 0; i < g_cfg.displays; i++) {
            uint8_t type, digits;
            if (lisy_get_2bytes(LISY_CMD_G_DISP_DETAIL, (uint8_t)i, &type, &digits) == 0) {
                if (type != 0 && digits > 0 && digits <= CFG_MAX_DISP_W) {
                    g_cfg.disp_width[i] = digits;
                }
            }
        }
    }

    s_info.counts_from_device = true;

    ESP_LOGI(TAG, "Gegenstelle: %s FW %s API %s Spiel %s",
             s_info.hw, s_info.fw_ver, s_info.api_ver, s_info.game);
    ESP_LOGI(TAG, "Bestueckung: %u Lampen, %u Spulen, %u Schalter, %u Sounds, %u Displays",
             g_cfg.lamps, g_cfg.coils, g_cfg.switches, g_cfg.sounds, g_cfg.displays);
}

esp_err_t fa_connect_run(void)
{
    memset(&s_info, 0, sizeof(s_info));

    board_ctrl_request(true);
    vTaskDelay(pdMS_TO_TICKS(CTRL_SETTLE_MS));

    int r = lisy_init_reset();
    s_info.last_code = r;

    if (r < 0) {
        s_info.state = FA_CONN_NO_ANSWER;
    } else if (r == LISY_INIT_OK) {
        s_info.state = FA_CONN_ACTIVE;
    } else if (r == LISY_INIT_NO_ALLOW) {
        s_info.state = FA_CONN_DENIED_DIP;
    } else if (r == LISY_INIT_NO_REQUEST) {
        s_info.state = FA_CONN_DENIED_REQ;
    } else {
        s_info.state = FA_CONN_DENIED_OTHER;
    }

    if (s_info.state != FA_CONN_ACTIVE) {
        /* Anforderung sofort zuruecknehmen: eine dauerhaft gesetzte Leitung ohne
         * gewaehrte Kontrolle waere nur ein Stolperdraht. */
        board_ctrl_request(false);
        ESP_LOGW(TAG, "%s (Code %d)", fa_connect_state_str(), r);
        return ESP_OK;
    }

    query_counts();

    /* Der Watchdog ist die Totmannschaltung der Gegenstelle: bleibt er aus, gibt
     * sie die Kontrolle nach kurzer Zeit von selbst zurueck. Solange wir steuern,
     * laeuft er deshalb unabhaengig von der Einstellung in der Weboberflaeche. */
    lisy_watchdog_enable(true);
    lisy_coil_apply_pulse_time(g_cfg.coils, g_cfg.coil_pulse_ms);

    ESP_LOGI(TAG, "Kontrolle aktiv");
    return ESP_OK;
}

void fa_connect_release(void)
{
    board_ctrl_request(false);
    s_info.state = FA_CONN_IDLE;
    s_info.counts_from_device = false;
    lisy_watchdog_enable(g_cfg.watchdog_en);
    ESP_LOGI(TAG, "Kontrolle zurueckgegeben");
}
