#pragma once

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
