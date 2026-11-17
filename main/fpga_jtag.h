#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * JTAG zum FPGA auf dem EP4CE6 dev board v7 (und v7.1 mit EP4CE22) -- Bit-Bang auf
 * TCK/TMS/TDI/TDO (GPIO5/8/9/21), nur in dieser Board-Variante gebaut.
 *
 * Wozu: Das Cyclone IV haelt DCLK auch bei nCONFIG low aktiv low (nCE liegt auf
 * v7 fest auf GND), der ESP kommt so nicht an das EPCS (Befund 24.10.2026,
 * ESP32-C3_SUPERMINI_PIGGYBACK.md §12.1). fpga_prog laedt deshalb vor jedem
 * EPCS-Zugriff per JTAG das Leer-Design as_release in den SRAM: es laesst die
 * vier AS-Leitungen als Eingaenge hochohmig.
 *
 * Der Ablauf ist der, den Quartus fuer as_release.sof als SVF erzeugt
 * (fpga/as_release/make_jtag_blob.py prueft das): PROGRAM, Datenstrom,
 * CHECK_STATUS, STARTUP, BYPASS. Der Datenstrom liegt gepackt in der Firmware
 * (main/blob/as_release.jtag.z) und wird beim Schieben entpackt.
 *
 * GPIO8 ist zugleich die LED: fuer die Dauer eines Zugriffs gesperrt
 * (board_led_lock). Danach sind TCK/TDI/TDO wieder hochohmige Eingaenge.
 */

/* Je bekanntem Device ein eigenes Leer-Design (fpga/as_release, eine Quartus-Revision
 * je Device), gewaehlt per IDCODE:
 *   EP4CE6  Board v7    as_release      main/blob/as_release.jtag.z
 *   EP4CE22 Board v7.1  as_release_e22  main/blob/as_release_e22.jtag.z
 * Die EP4CE22-IDCODE 0x020F30DD am ersten v7.1-Board mit /api/fpgadiag gelesen
 * (06.11.2026). */
#define FPGA_IDCODE_EP4CE6   0x020F10DD
#define FPGA_IDCODE_EP4CE22  0x020F30DD

/* IDCODE lesen. ESP_ERR_NOT_FOUND, wenn nur 0x00000000/0xFFFFFFFF kommt
 * (TDO haengt, keine Kette). */
esp_err_t fpga_jtag_idcode(uint32_t *id);

/* Leer-Design passend zur IDCODE in den SRAM laden. ESP_ERR_INVALID_RESPONSE bei
 * unbekannter IDCODE,
 * ESP_ERR_INVALID_STATE, wenn CHECK_STATUS kein CONF_DONE meldet. Das laufende
 * Design (und damit das Spiel) ist danach weg. */
esp_err_t fpga_jtag_load_release(void);

/* Datenquelle fuer einen gepackten JTAG-Strom (zlib, wie make_jtag_blob.py ihn
 * erzeugt): setzt *p auf die naechsten Bytes und liefert ihre Zahl, 0 = Ende,
 * < 0 = Fehler. Die Bytes muessen bis zum naechsten Aufruf gueltig bleiben. */
typedef int (*fpga_jtag_src_fn)(void *ctx, const uint8_t **p);

typedef struct {
    uint32_t idcode;
    int      conf_done;     /* CHECK_STATUS nach dem Strom */
    size_t   bytes;         /* entpackt und geschoben */
    int64_t  ms;
} fpga_jtag_sram_result_t;

/* Beliebiges Design per JTAG in den SRAM laden (PROGRAM ... STARTUP), Strom
 * stueckweise aus src. cs_len/cs_conf_done: Laenge des CHECK_STATUS-Registers und
 * Lage von CONF_DONE, je Die/Gehaeuse verschieden (make_jtag_blob.py gibt sie aus,
 * im .fjz-Kopf mitgeliefert). Das laufende Design ist danach weg; nach dem
 * Aus- und Einschalten bootet wieder das aus dem EPCS. */
esp_err_t fpga_jtag_load_sram(fpga_jtag_src_fn src, void *ctx, int cs_len, int cs_conf_done,
                              fpga_jtag_sram_result_t *r);

/* Einmal beim Start: legt die Sperre an, die alle TAP-Zugriffe gegeneinander
 * absichert (EPCS-Programmierung, Virtual JTAG, Diagnose). */
void fpga_jtag_init(void);

/* ---- Virtual JTAG (SLD-Hub), Stufe 2 von "FA-Control ueber JTAG" -------------
 *
 * Das FPGA-Design enthaelt eine sld_virtual_jtag-Instanz; Quartus fuegt dazu den
 * SLD-Hub ein. Ablauf (am Board v4.x mit quartus_stp bestaetigt, 17.11.2026):
 *   USER1 (0x00E) + 64 Nullen -> Hub selbst gewaehlt
 *   USER0 (0x00C), 8 x 4-Bit-DR -> HUB_INFO, je Knoten 8 x 4 Bit -> NODE_INFO
 *   Knoten waehlen: USER1-DR {Adresse (m Bit), VIR (n Bit)}, dann USER0-DR an ihn
 * n kommt aus HUB_INFO und ist NICHT die sld_ir_width des Knotens (gemessen: 4
 * statt 2). Konzept: N:\Projekte\misc\USB_BLASTER_DIY\ESP32-C3_EVALUIERUNG.md 15. */

#define FPGA_VJTAG_MAX_NODES  8
#define FPGA_VJTAG_NODE_VJTAG 0x08   /* Knoten-ID von sld_virtual_jtag */

typedef struct {
    uint32_t idcode;
    uint32_t hub_info;                         /* roh */
    int      nodes;                            /* N laut HUB_INFO */
    int      m;                                /* Adressbreite ceil(log2(N+1)) */
    int      n;                                /* VIR-Breite laut HUB_INFO */
    uint32_t node_info[FPGA_VJTAG_MAX_NODES];  /* roh, Adresse k = Index k-1 */
    int      addr;                             /* erster Knoten mit ID 0x08, 0 = keiner */
} fpga_vjtag_hub_t;

/* IDCODE lesen und den Hub abfragen. ESP_ERR_NOT_FOUND, wenn kein Hub antwortet
 * (Hersteller in HUB_INFO nicht 0x06E: Design ohne Hub oder unkonfiguriert) oder
 * kein Virtual-JTAG-Knoten dabei ist. hub ist auch dann gefuellt. */
esp_err_t fpga_vjtag_probe(fpga_vjtag_hub_t *hub);

/* Testknoten lib_common/vjtag_probe.vhd (GottFA80 smd_hw11_cyclone_iv_v4_vj):
 * VIR 2 = Kennung, VIR 1 = 32-Bit-Rueckkanal. */
#define FPGA_VJTAG_VIR_ECHO 1
#define FPGA_VJTAG_VIR_ID   2

/* Kennung (VIR 2) >> 8: welcher Knoten antwortet. Das untere Byte ist die BOARD_ID
 * als ASCII-Ziffer. */
#define FPGA_VJTAG_ID_PROBE 0x474638u   /* "GF8": Testknoten vjtag_probe */
/* fa_vjtag_link: "FA" + Art + BOARD_ID. Art L = volles FA-Control, M = nur Monitor,
 * C = nur Steuerung ohne 80B-Text, S = Service-Design (Steuerung mit 80B-Text, ohne
 * Spiel) -- Sparversionen fuer EP4CE6/10CL006, GottFA80 FA_SPAR. */
#define FPGA_VJTAG_ID_LINK  0x46414Cu   /* "FAL" */
#define FPGA_VJTAG_IS_LINK(id) (((id) >> 16) == 0x4641u && \
                                (((id) >> 8 & 0xFF) == 'L' || ((id) >> 8 & 0xFF) == 'M' || \
                                 ((id) >> 8 & 0xFF) == 'C' || ((id) >> 8 & 0xFF) == 'S'))
#define FPGA_VJTAG_LINK_KIND(id) ((char)((id) >> 8 & 0xFF))

/* fa_vjtag_link (GottFA80 lib_common/fa_vjtag_link.vhd): VIR 1 = Datenkanal zu
 * fa_control (16 Bit je Scan, ein Byte je Richtung), VIR 3 = Steuerbit CTRL_REQ
 * mit Totmann im FPGA (2 s ohne Datenscan/Schreiben -> geloescht). */
#define FPGA_VJTAG_VIR_DATA 1
#define FPGA_VJTAG_VIR_CTRL 3

typedef struct {
    uint32_t id;            /* VIR 2 */
    int      echo_total;    /* Anzahl gepruefter Rueckkanal-Scans */
    int      echo_ok;
    uint32_t fail_out;      /* erster Fehler: erwartet ... */
    uint32_t fail_in;       /* ... gelesen */
    int64_t  echo_us;       /* Dauer der Rueckkanal-Schleife */
    int64_t  session_us;    /* eine komplette Sitzung: oeffnen, VIR, 32 Bit, schliessen */
} fpga_vjtag_test_t;

/* Kennung lesen, dann count Rueckkanal-Scans (je 32 Bit; jeder Scan liest den
 * Wert des vorigen zurueck) und eine Einzelsitzung timen. */
esp_err_t fpga_vjtag_test(const fpga_vjtag_hub_t *hub, int count, fpga_vjtag_test_t *r);

/* Kennung (VIR 2) lesen, 0 ohne Knoten. */
uint32_t fpga_vjtag_read_id(const fpga_vjtag_hub_t *hub);

/* fa_vjtag_link: n_out Bytes senden, dann Antwortbytes sammeln, bis max_in da sind
 * oder eine Pause eintritt (timeout_ms bis zum ersten Byte, danach 2 ms). Alles in
 * einer TAP-Sitzung. Liefert die Zahl empfangener Bytes (kann > max_in sein, dann
 * wurden nur max_in gespeichert), -1 ohne Knoten. scans darf NULL sein. */
int fpga_vjtag_xfer(const fpga_vjtag_hub_t *hub, const uint8_t *out, int n_out,
                    uint8_t *in, int max_in, int timeout_ms, int *scans);

/* Steuerbit: set 0/1 schreibt, -1 liest nur. val = Stand nach dem Schreiben. */
esp_err_t fpga_vjtag_ctrl(const fpga_vjtag_hub_t *hub, int set, int *val);

/* ---- Byte-Strom fuer lisy.c (JTAG-Piggyback, Stufe 4) ------------------------
 * Hub und Knoten werden zwischengespeichert; die Kennung "FAL" wird hoechstens alle
 * 2 s neu geprueft. Laeuft kein Design mit fa_vjtag_link, liefern die Funktionen
 * sofort einen Fehler. */

/* true, wenn fa_vjtag_link antwortet. */
bool fpga_vjtag_link_present(void);

/* Nach dem Laden eines anderen Designs: Hub beim naechsten Zugriff neu abfragen. */
void fpga_vjtag_link_invalidate(void);

/* n_out Bytes senden, dann empfangen, bis want Bytes da sind oder timeout_ms um ist;
 * Bytes, die ohne Pause nachkommen, werden mitgenommen (bis max_in). Liefert die
 * Zahl empfangener Bytes, -1 ohne Link. */
int fpga_vjtag_link_io(const uint8_t *out, int n_out, uint8_t *in, int max_in,
                       int want, int timeout_ms);

/* Steuerbit (ersetzt CTRL_REQ/GPIO10). Totmann im FPGA: 2 s ohne Datenscan -> aus. */
esp_err_t fpga_vjtag_link_ctrl(bool on);
