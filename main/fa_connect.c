#include "fa_connect.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "board.h"
#include "lisy.h"
#include "names.h"

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

/* Diese Texte gehen als "connmsg" ueber /api/config direkt ins Verbindungsbanner
 * der Weboberflaeche -- sie sind Oberflaeche und deshalb englisch, obwohl sie hier
 * in einer C-Datei stehen. */
const char *fa_connect_state_str(void)
{
    switch (s_info.state) {
        case FA_CONN_ACTIVE:       return "Control granted";
        case FA_CONN_DENIED_DIP:   return "Control denied - set option DIP 4 to ON";
        case FA_CONN_DENIED_REQ:   return "Control denied - request line (GPIO10) not seen";
        case FA_CONN_DENIED_OTHER: return "Control denied";
        case FA_CONN_NO_ANSWER:    return "No answer - check wiring and power";
        default:                   return "Not connected";
    }
}

/* Info-Gruppe 0..9 abfragen und s_info damit fuellen.
 * Nur plausible Antworten werden uebernommen; was das Geraet nicht beantwortet,
 * bleibt auf 0 und ist damit in der Oberflaeche schlicht nicht vorhanden. */
static void query_counts(void)
{
    int v;

    lisy_get_string(LISY_CMD_G_HW,        s_info.hw,      sizeof(s_info.hw));
    lisy_get_string(LISY_CMD_G_LISY_VER,  s_info.fw_ver,  sizeof(s_info.fw_ver));
    lisy_get_string(LISY_CMD_G_API_VER,   s_info.api_ver, sizeof(s_info.api_ver));
    lisy_get_string(LISY_CMD_G_GAME_INFO, s_info.game,    sizeof(s_info.game));

    v = lisy_get_byte(LISY_CMD_G_NO_LAMPS);
    if (v > 0 && v <= CFG_MAX_LAMPS) {
        s_info.lamps = (uint8_t)v;
    }
    v = lisy_get_byte(LISY_CMD_G_NO_SOL);
    if (v > 0 && v <= CFG_MAX_COILS) {
        s_info.coils = (uint8_t)v;
    }
    v = lisy_get_byte(LISY_CMD_G_NO_SW);
    if (v > 0 && v <= CFG_MAX_SWITCHES) {
        s_info.switches = (uint8_t)v;
    }
    /* 0 Sounds ist eine gueltige Aussage ("dieses Geraet kann keinen Ton"). */
    v = lisy_get_byte(LISY_CMD_G_NO_SOUNDS);
    if (v >= 0 && v <= CFG_MAX_SOUNDS) {
        s_info.sounds = (uint8_t)v;
    }
    v = lisy_get_byte(LISY_CMD_G_NO_DISP);
    if (v > 0 && v <= CFG_MAX_DISPLAYS) {
        s_info.displays = (uint8_t)v;
        /* Breite je Display einzeln nachfragen (Opcode 7 liefert Typ + Stellen). */
        for (int i = 0; i < s_info.displays; i++) {
            uint8_t type, digits;
            if (lisy_get_2bytes(LISY_CMD_G_DISP_DETAIL, (uint8_t)i, &type, &digits) == 0) {
                if (type != 0 && digits > 0 && digits <= CFG_MAX_DISP_W) {
                    s_info.disp_width[i] = digits;
                }
            }
        }
    }

    ESP_LOGI(TAG, "Gegenstelle: %s FW %s API %s Spiel %s",
             s_info.hw, s_info.fw_ver, s_info.api_ver, s_info.game);
    ESP_LOGI(TAG, "Bestueckung: %u Lampen, %u Spulen, %u Schalter, %u Sounds, %u Displays",
             s_info.lamps, s_info.coils, s_info.switches, s_info.sounds, s_info.displays);
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
         * gewaehrte Kontrolle waere nur ein Stolperdraht. Der Watchdog muss hier
         * mit aus -- ein zweiter, gescheiterter Verbindungsversuch darf kein
         * Lebenszeichen aus dem vorherigen stehenlassen. */
        board_ctrl_request(false);
        lisy_watchdog_enable(false);
        ESP_LOGW(TAG, "%s (Code %d)", fa_connect_state_str(), r);
        return ESP_OK;
    }

    query_counts();

    /* Der Watchdog ist die Totmannschaltung der Gegenstelle: bleibt er aus, gibt
     * sie die Kontrolle nach kurzer Zeit (dort ~2 s) von selbst zurueck. Er ist
     * damit kein Komfortmerkmal, sondern Bedingung der aktiven Kontrolle -- und
     * genau deshalb an sie gekoppelt statt einstellbar. */
    lisy_watchdog_enable(true);
    lisy_coil_apply_pulse_time(s_info.coils, g_cfg.coil_pulse_ms);

    /* Namensdatei zur Kennung dieser Anlage waehlen ("<HW>_<GAME>"). Gibt es
     * keine, wird die Auswahl geleert -- sonst stuenden nach einem Spielwechsel
     * die Namen des vorigen Spiels auf den Kacheln. Muss nach query_counts()
     * stehen: die Kennung kommt aus s_info. */
    names_select_for_id();

    ESP_LOGI(TAG, "Kontrolle aktiv");
    return ESP_OK;
}

void fa_connect_release(void)
{
    board_ctrl_request(false);
    /* Watchdog aus, bevor die Bestueckung geloescht wird: ohne Kontrolle hat
     * niemand etwas von einem Lebenszeichen auf dem Bus. */
    lisy_watchdog_enable(false);
    memset(&s_info, 0, sizeof(s_info));
    s_info.state = FA_CONN_IDLE;
    ESP_LOGI(TAG, "Kontrolle zurueckgegeben");
}
