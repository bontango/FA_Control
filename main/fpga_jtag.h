#pragma once

#include <stdint.h>
#include "esp_err.h"

/*
 * JTAG zum FPGA auf dem EP4CE6 dev board v7 -- Bit-Bang auf TCK/TMS/TDI/TDO
 * (GPIO5/8/9/21), nur in dieser Board-Variante gebaut.
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

#define FPGA_IDCODE_EP4CE6   0x020F10DD

/* IDCODE lesen. ESP_ERR_NOT_FOUND, wenn nur 0x00000000/0xFFFFFFFF kommt
 * (TDO haengt, keine Kette). */
esp_err_t fpga_jtag_idcode(uint32_t *id);

/* Leer-Design in den SRAM laden. ESP_ERR_INVALID_RESPONSE bei falschem IDCODE,
 * ESP_ERR_INVALID_STATE, wenn CHECK_STATUS kein CONF_DONE meldet. Das laufende
 * Design (und damit das Spiel) ist danach weg. */
esp_err_t fpga_jtag_load_release(void);
