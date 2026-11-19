#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * LISY-Slave in Software fuer FA-Control "lite" (19.11.2026).
 *
 * Auf dem JTAG-Piggyback mit einem Design der Art R (Kennung "FAR", GottFA80
 * lib_common/fa_jtag_regs.vhd + fa_lite_sys80.vhd) gibt es im FPGA keinen LISY-Parser
 * mehr, nur eine Registerbank. lisy.c schickt seine Befehle trotzdem als LISY-Bytes;
 * dieser Emulator nimmt sie an, fuehrt sie ueber die Register aus und liefert die
 * Antwortbytes so, wie fa_control.vhd sie liefern wuerde. lisy.c, fa_connect.c und die
 * Oberflaeche merken davon nichts.
 *
 * Konzept: N:\Projekte\misc\USB_BLASTER_DIY\ESP32-C3_EVALUIERUNG.md, Abschnitt 15.11.
 * Aufgerufen nur unter dem LISY-Mutex (lisy.c).
 */

/* Antwortbytes abgeben (lisy.c: in den Empfangsring). */
typedef void (*lisy_emul_put_fn)(const uint8_t *p, int n);

/* LISY-Bytes vom Host. Befehle duerfen ueber mehrere Aufrufe verteilt kommen. */
void lisy_emul_feed(const uint8_t *data, size_t len, lisy_emul_put_fn put);

/* Halb empfangenen Befehl verwerfen (neues Design, Neuverbindung). */
void lisy_emul_reset(void);
