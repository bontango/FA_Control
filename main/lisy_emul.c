#include "lisy_emul.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fpga_jtag.h"
#include "lisy.h"

/*
 * LISY-Slave in Software, Gegenstueck zu GottFA80 lib_common/fa_lite_sys80.vhd.
 * Das Verhalten (Parameterzahlen, Antworten, 0x00 fuer Unbekanntes, Uebernahme nur mit
 * Steuerbit) ist aus lib_fa_control/fa_control.vhd uebernommen, die Ansteuerung der
 * Ausgaenge (Lampen-Latches, Spulendecoder, Sound) aus lib_common/fa_sys80.vhd.
 * Registerkarte: Kopf von fa_lite_sys80.vhd.
 */

static const char *TAG = "lisy_emul";

/* ---- Registerkarte (fa_lite_sys80.vhd) ------------------------------------ */
#define R_PROJECT   0x00
#define R_MON       0x01    /* 0x01..0x3E = Monitorbytes 1..62 */
#define R_VERSION   0x40    /* lesen: 3 ASCII-Ziffern */
#define R_GAME      0x43    /* lesen: 3 ASCII-Ziffern */
#define R_STATUS    0x46
#define R_CAPS      0x47
#define W_U6A       0x40
#define W_U6B       0x41
#define W_J2DATA    0x43
#define W_J2CTRL    0x44
#define W_PLSVAL    0x46
#define W_PLSMS     0x47
#define W_SIM       0x48    /* 0x48..0x4B */
#define W_BCD       0x50    /* 0x50..0x5D */

#define ST_RUNNING  0x01
#define ST_ACTIVE   0x02
#define ST_SYS80B   0x04

#define MON_SW      13      /* Monitorbyte der Schalter 0..7 (= Adresse R_MON - 1 + 13) */
#define MON_SW_LEN  10

/* Takte in Run-Test/Idle nach einem Scan (~0,5 us je Takt bei ~2 MHz TCK):
 * Lampen-Latch-Phasen wie FA_LAMPS (dort 100 us), J2-Ladepuls im FPGA 13 us. */
#define GAP_LAMP    100
#define GAP_J2      40

/* ---- Profil GottFA80 (fa_sys80.vhd) ---------------------------------------- */
#define P_HW        "GottFA80"
#define P_API       "0.12"
#define P_LAMPS     48
#define P_SOL       9
#define P_SOUNDS    16
#define P_SW        78
#define P_DISP      5
#define P_DTYPE     1       /* BCD7 */
#define P_ALT_DISP  2
#define P_ALT_TYPE  5       /* ASCII */
#define P_ALT_W     20
#define P_MON_LEN   40
#define P_MON_80B   62
#define P_PULSE_DEF 50
static const uint8_t P_DIGITS[7] = { 4, 7, 7, 7, 7, 0, 0 };

/* ---- Zustand ----------------------------------------------------------------- */
static uint8_t  s_cmd[2 + 32];
static int      s_got;
static uint8_t  s_lamps[(P_LAMPS + 7) / 8];
static uint16_t s_sol;              /* gehaltene Spulen, Bit 0 = Spule 1 */
static uint8_t  s_pulse_ms = P_PULSE_DEF;
static bool     s_snd_en;
static uint8_t  s_snd;
static uint8_t  s_bcd[28];          /* Spieler 1..4 je 7 Ziffern, 0xF dunkel */
static uint8_t  s_sw[MON_SW_LEN];   /* zuletzt gemeldete Schalter (Opcode 41) */
static uint8_t  s_sim[4];           /* Sim-Slots wie im FPGA */
static bool     s_ctrl;             /* Uebernahme laut letztem INIT */
static bool     s_80b;              /* 80B-Spiel: ASCII-Anzeige */

/* ---- Registerzugriff --------------------------------------------------------- */

static int reg_read(uint8_t addr, uint8_t *out, int n)
{
    uint32_t ops[P_MON_80B + 2];
    uint16_t res[P_MON_80B + 2];
    if (n > P_MON_80B + 1) {
        n = P_MON_80B + 1;
    }
    for (int i = 0; i < n; i++) {
        ops[i] = FPGA_REG_RD(addr + i);
    }
    ops[n] = FPGA_REG_RD(R_PROJECT);        /* liest das letzte Byte ab */
    if (fpga_vjtag_reg_ops(ops, n + 1, res) < 0) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        out[i] = (uint8_t)res[i + 1];
    }
    return n;
}

static int reg_read1(uint8_t addr)
{
    uint8_t b;
    return reg_read(addr, &b, 1) == 1 ? b : -1;
}

static void reg_write(const uint32_t *ops, int n)
{
    fpga_vjtag_reg_ops(ops, n, NULL);
}

/* ---- Ausgaenge (Logik aus fa_sys80.vhd) ------------------------------------- */

/* U6 PA (RIOT-Bedeutung) wie FA_SOL: Decoder 1 = SOL1..4, Decoder 2 = SOL5..8, kleinste
 * Nummer gewinnt, SOL9 = PA7 low. Steht ein Sound an, sind die Decoder aus. */
static uint8_t u6a_calc(uint16_t sol)
{
    uint8_t a = 0x7F;
    if (!(sol & 0x100)) {
        a |= 0x80;
    }
    if (s_snd_en) {
        a = (uint8_t)((a & 0xE0) | (~s_snd & 0x0F));   /* PA4 = 0, PA3..0 = Sound negiert */
        return a;
    }
    for (int k = 0; k < 4; k++) {
        if (sol & (1u << k)) {
            a = (uint8_t)((a & ~0x23) | (3 - k));      /* AB1, I1G = 0 */
            break;
        }
    }
    for (int k = 4; k < 8; k++) {
        if (sol & (1u << k)) {
            a = (uint8_t)((a & ~0x4C) | ((3 - (k - 4)) << 2));   /* AB2, I2G = 0 */
            break;
        }
    }
    return a;
}

static void u6a_apply(void)
{
    uint32_t op = FPGA_REG_WR(W_U6A, u6a_calc(s_sol));
    reg_write(&op, 1);
}

/* Lampen wie FA_LAMPS: LD bei FEED 0, FEED = Latch, FEED wieder 0. */
static int lamp_group_ops(int g, uint32_t *ops)
{
    uint8_t nib = (uint8_t)((s_lamps[g / 2] >> (4 * (g % 2))) & 0x0F);
    ops[0] = FPGA_REG_GAP(FPGA_REG_WR(W_U6B, nib), GAP_LAMP);
    ops[1] = FPGA_REG_GAP(FPGA_REG_WR(W_U6B, ((g + 1) << 4) | nib), GAP_LAMP);
    ops[2] = FPGA_REG_GAP(FPGA_REG_WR(W_U6B, nib), GAP_LAMP);
    return 3;
}

static void lamps_write_group(int g)
{
    uint32_t ops[3];
    reg_write(ops, lamp_group_ops(g, ops));
}

static void lamps_write_all(void)
{
    uint32_t ops[3 * 12];
    int n = 0;
    for (int g = 0; g < 12; g++) {
        n += lamp_group_ops(g, ops + n);
    }
    reg_write(ops, n);
}

static void bcd_write_all(void)
{
    uint32_t ops[14];
    for (int k = 0; k < 14; k++) {
        ops[k] = FPGA_REG_WR(W_BCD + k, (s_bcd[2 * k] << 4) | (s_bcd[2 * k + 1] & 0x0F));
    }
    reg_write(ops, 14);
}

/* 80B-Anzeige (Rockwell 10941) roh ueber J2, Ablauf wie boot_message_80B.vhd.
 * W_J2CTRL: bit0/1 = Zeile 1/2 bekommt den Ladepuls, bit2 = D_Reset-Pegel (1 = inaktiv). */
static uint8_t j2_code(char c)
{
    unsigned char n = (unsigned char)c & 0x7F;
    if (n >= 'a' && n <= 'z') {
        n = (unsigned char)(n - 32);
    } else if (n < 0x20 || n == 0x7F) {
        n = ' ';
    }
    return n;
}

static void j2_row(int row, const char *txt)
{
    uint32_t ops[3 + P_ALT_W];
    int n = 0;
    ops[n++] = FPGA_REG_WR(W_J2CTRL, 0x04 | (row == 0 ? 0x01 : 0x02));
    ops[n++] = FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x01), GAP_J2);
    ops[n++] = FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0xC0), GAP_J2);   /* Zeiger 0 */
    for (int i = 0; i < P_ALT_W; i++) {
        ops[n++] = FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, j2_code(txt[i])), GAP_J2);
    }
    reg_write(ops, n);
}

static void j2_init(void)
{
    static const char blank[P_ALT_W + 1] = "                    ";
    uint32_t op = FPGA_REG_WR(W_J2CTRL, 0x00);              /* D_Reset aktiv */
    reg_write(&op, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t a[] = { FPGA_REG_WR(W_J2CTRL, 0x04), FPGA_REG_WR(W_J2DATA, 0x80) };  /* D7: parallel */
    reg_write(a, 2);
    vTaskDelay(pdMS_TO_TICKS(1));
    op = FPGA_REG_WR(W_J2DATA, 0x00);
    reg_write(&op, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    uint32_t b[] = {
        FPGA_REG_WR(W_J2CTRL, 0x07),                        /* beide Zeilen */
        FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x01), GAP_J2), FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x94), GAP_J2),
        FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x01), GAP_J2), FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x06), GAP_J2),
        FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x01), GAP_J2), FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x5C), GAP_J2),
        FPGA_REG_WR(W_J2CTRL, 0x05),                        /* nur Zeile 1: Refresh an */
        FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x01), GAP_J2), FPGA_REG_GAP(FPGA_REG_WR(W_J2DATA, 0x0E), GAP_J2),
    };
    reg_write(b, sizeof(b) / sizeof(b[0]));
    j2_row(0, blank);
    j2_row(1, blank);
}

/* Sim-Slot: Schalter 10 * Strobe + Return; Slot = bit7 gueltig, 5..3 Strobe, 2..0 Return. */
static void sim_set(uint8_t idx, bool closed)
{
    int s = idx / 10, r = idx % 10;
    if (s > 7 || r > 7) {
        return;
    }
    uint8_t v = (uint8_t)(0x80 | (s << 3) | r);
    int slot = -1;
    for (int k = 0; k < 4; k++) {
        if (s_sim[k] == v) {
            slot = k;
        }
    }
    if (closed) {
        if (slot < 0) {
            for (int k = 0; k < 4 && slot < 0; k++) {
                if (!(s_sim[k] & 0x80)) {
                    slot = k;
                }
            }
        }
        if (slot < 0) {
            ESP_LOGW(TAG, "Schalter %u: alle 4 Sim-Slots belegt", idx);
            return;
        }
        s_sim[slot] = v;
    } else {
        if (slot < 0) {
            return;
        }
        s_sim[slot] = 0;
    }
    uint32_t op = FPGA_REG_WR(W_SIM + slot, s_sim[slot]);
    reg_write(&op, 1);
}

/* ---- Antworten ---------------------------------------------------------------- */

static void put_str(lisy_emul_put_fn put, const char *s)
{
    put((const uint8_t *)s, (int)strlen(s) + 1);    /* mit NUL */
}

static void put1(lisy_emul_put_fn put, uint8_t b)
{
    put(&b, 1);
}

/* Parameterbytes je Opcode wie fa_control.vhd (S_OPCODE/S_PAR1). -1 = Display:
 * Laenge im ersten Parameter, dann so viele Bytes. */
static int params_of(uint8_t op)
{
    switch (op) {
    case 7: case 10: case 11: case 12: case 20: case 21: case 22: case 23:
    case 40: case 51: case 111:
        return 1;
    case 24: case 25: case 50:
        return 2;
    default:
        if (op >= 30 && op <= 36) {
            return -1;
        }
        return 0;
    }
}

static bool cmd_complete(void)
{
    int p = params_of(s_cmd[0]);
    if (p >= 0) {
        return s_got >= 1 + p;
    }
    return s_got >= 2 && s_got >= 2 + s_cmd[1];
}

static void exec(lisy_emul_put_fn put)
{
    uint8_t op = s_cmd[0], p1 = s_cmd[1], p2 = s_cmd[2];
    char buf[8];
    uint8_t b[P_MON_80B];

    switch (op) {
    case 0:
        put_str(put, P_HW);
        break;
    case 1:
        if (reg_read(R_VERSION, b, 3) == 3) {
            buf[0] = b[0]; buf[1] = '.'; buf[2] = b[1]; buf[3] = '.'; buf[4] = b[2]; buf[5] = 0;
            put_str(put, buf);
        } else {
            put_str(put, "");
        }
        break;
    case 2:
        put_str(put, P_API);
        break;
    case 3: put1(put, P_LAMPS); break;
    case 4: put1(put, P_SOL); break;
    case 5: put1(put, P_SOUNDS); break;
    case 9: put1(put, P_SW); break;
    case 6: {
        int st = reg_read1(R_STATUS);
        s_80b = st >= 0 && (st & ST_SYS80B);
        put1(put, s_80b ? P_ALT_DISP : P_DISP);
        break;
    }
    case 7: {
        uint8_t r[2] = { 0, 0 };
        if (s_80b) {
            if (p1 < P_ALT_DISP) { r[0] = P_ALT_TYPE; r[1] = P_ALT_W; }
        } else if (p1 < P_DISP) {
            r[0] = P_DTYPE; r[1] = P_DIGITS[p1];
        }
        put(r, 2);
        break;
    }
    case 8:
        if (reg_read(R_GAME, b, 3) == 3) {
            buf[0] = b[0]; buf[1] = b[1]; buf[2] = b[2]; buf[3] = 0;
            put_str(put, buf);
        } else {
            put_str(put, "");
        }
        break;

    case 10:
        put1(put, (p1 < P_LAMPS && (s_lamps[p1 / 8] >> (p1 % 8) & 1)) ? 1 : 0);
        break;
    case 11: case 12:
        if (s_ctrl && p1 < P_LAMPS) {
            if (op == 11) {
                s_lamps[p1 / 8] |= (uint8_t)(1 << (p1 % 8));
            } else {
                s_lamps[p1 / 8] &= (uint8_t)~(1 << (p1 % 8));
            }
            lamps_write_group(p1 / 4);
        }
        break;
    case 20:
        put1(put, (p1 >= 1 && p1 <= P_SOL && (s_sol >> (p1 - 1) & 1)) ? 1 : 0);
        break;
    case 21: case 22:
        if (s_ctrl && p1 >= 1 && p1 <= P_SOL) {
            if (op == 21) {
                s_sol |= (uint16_t)(1u << (p1 - 1));
            } else {
                s_sol &= (uint16_t)~(1u << (p1 - 1));
            }
            u6a_apply();
        }
        break;
    case 23:
        if (s_ctrl && p1 >= 1 && p1 <= P_SOL) {
            /* Pulswert = Grundwert mit dieser Spule; das FPGA zaehlt die ms und kehrt
             * danach selbst zum Grundwert zurueck. */
            uint32_t ops[2] = {
                FPGA_REG_WR(W_PLSVAL, u6a_calc(s_sol | (uint16_t)(1u << (p1 - 1)))),
                FPGA_REG_WR(W_PLSMS, s_pulse_ms),
            };
            reg_write(ops, 2);
        }
        break;
    case 24:
        if (p2 > 0) {
            s_pulse_ms = p2;        /* Nummer bewusst ignoriert, wie fa_control */
        }
        break;
    case 25:
        break;

    case 40: {
        int v = 0;
        if (p1 < P_SW) {
            int r = reg_read1(R_MON + MON_SW - 1 + p1 / 8);
            v = (r >= 0 && (r >> (p1 % 8) & 1)) ? 1 : 0;
        }
        put1(put, (uint8_t)v);
        break;
    }
    case 41: {
        uint8_t now[MON_SW_LEN];
        uint8_t resp = 0x7F;
        if (reg_read(R_MON + MON_SW - 1, now, MON_SW_LEN) == MON_SW_LEN) {
            for (int i = 0; i < MON_SW_LEN && resp == 0x7F; i++) {
                uint8_t d = now[i] ^ s_sw[i];
                if (d) {
                    int bit = __builtin_ctz(d);
                    int idx = i * 8 + bit;
                    if (idx < P_SW) {
                        bool on = (now[i] >> bit) & 1;
                        s_sw[i] ^= (uint8_t)(1 << bit);
                        resp = (uint8_t)((on ? 0x80 : 0) | idx);
                    } else {
                        s_sw[i] = now[i];
                    }
                }
            }
        }
        put1(put, resp);
        break;
    }

    case 50:
        if (s_ctrl && p2 < P_SOUNDS) {
            s_snd = p2 & 0x0F;
            s_snd_en = true;
            u6a_apply();
        }
        break;
    case 51:
        if (s_ctrl) {
            s_snd_en = false;
            u6a_apply();
        }
        break;

    case 100: {
        int st = reg_read1(R_STATUS);
        memset(s_lamps, 0, sizeof(s_lamps));
        s_sol = 0;
        s_snd_en = false;
        s_pulse_ms = P_PULSE_DEF;
        memset(s_bcd, 0x0F, sizeof(s_bcd));
        reg_read(R_MON + MON_SW - 1, s_sw, MON_SW_LEN);     /* Schalter neu aufsetzen */
        s_ctrl = st >= 0 && (st & ST_ACTIVE);
        if (s_ctrl) {
            s_80b = (st & ST_SYS80B) != 0;
            u6a_apply();
            lamps_write_all();
            bcd_write_all();
            if (s_80b) {
                j2_init();
            }
        }
        ESP_LOGI(TAG, "INIT: Status 0x%02X -> %s", st & 0xFF, s_ctrl ? "Kontrolle" : "verweigert");
        put1(put, s_ctrl ? LISY_INIT_OK : LISY_INIT_NO_REQUEST);
        break;
    }
    case 101: case 102: {
        /* Watchdog: haelt den Totmann im FPGA wach (jeder Scan zaehlt). Unter
         * Uebernahme dabei die Lampen-Latches auffrischen. */
        int st = reg_read1(R_STATUS);
        if (st >= 0 && !(st & ST_ACTIVE)) {
            s_ctrl = false;
        } else if (s_ctrl && op == 101) {
            lamps_write_all();
        }
        put1(put, 0);
        break;
    }

    case 110: {
        int st = reg_read1(R_STATUS);
        int len = (st >= 0 && (st & ST_SYS80B)) ? P_MON_80B : P_MON_LEN;
        if (reg_read(R_MON, b, len) == len) {
            put1(put, (uint8_t)len);
            put(b, len);
        } else {
            put1(put, 0);
        }
        break;
    }
    case 111:
        if (!s_ctrl) {
            sim_set(p1 & 0x7F, (p1 & 0x80) != 0);
        }
        break;

    default:
        if (op >= 30 && op <= 36) {
            int d = op - 30, n = p1;
            const uint8_t *dd = s_cmd + 2;
            if (!s_ctrl) {
                break;
            }
            if (s_80b) {
                if (d < P_ALT_DISP) {
                    char row[P_ALT_W];
                    for (int i = 0; i < P_ALT_W; i++) {
                        row[i] = i < n ? (char)dd[i] : ' ';
                    }
                    j2_row(d, row);
                }
            } else if (d >= 1 && d <= 4) {          /* Display 0 (Status) ohne Anzeige */
                for (int i = 0; i < 7 && i < n; i++) {
                    s_bcd[(d - 1) * 7 + i] = dd[i] & 0x0F;
                }
                bcd_write_all();
            }
            break;
        }
        put1(put, 0);                               /* unbekannt, wie fa_control */
        break;
    }
}

void lisy_emul_feed(const uint8_t *data, size_t len, lisy_emul_put_fn put)
{
    for (size_t i = 0; i < len; i++) {
        if (s_got < (int)sizeof(s_cmd)) {
            s_cmd[s_got] = data[i];
        }
        s_got++;
        if (cmd_complete()) {
            exec(put);
            s_got = 0;
        }
    }
}

void lisy_emul_reset(void)
{
    s_got = 0;
    s_ctrl = false;
    memset(s_sim, 0, sizeof(s_sim));
}
