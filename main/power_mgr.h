#pragma once

/*
 * Betriebsart und Stromsparen.
 *
 * FA_Control ist ein Werkzeug fuer die Fehlersuche, kein Dauerbetrieb -- es haengt
 * aber dauerhaft im Flipper. Seit v1.12 ist deshalb der Tiefschlaf der Normalfall
 * und Options-DIP 1 der Ein/Aus-Schalter:
 *
 *   DIP1 ON  -> wach: WLAN, Webserver, Steuerung. Die blaue LED blinkt.
 *   DIP1 OFF -> Tiefschlaf (~einige uA). Aufgeweckt wird ueber denselben Pin.
 *
 * Aufwecken kann der ESP32-C3 nur ueber GPIO0..GPIO5; DIP1 liegt auf GPIO0 und
 * ist damit der einzige Kandidat der Bank (der Taster auf GPIO9 koennte es nicht).
 *
 * Das Blinkmuster sagt, ob FA_Control gerade am Steuer ist -- das sieht man aus
 * zwei Metern, ohne die Weboberflaeche aufzumachen:
 *   1 Hz  = wach, keine Kontrolle ueber die Anlage
 *   5 Hz  = Kontrolle aktiv (Watchdog laeuft)
 *   10 Hz = Gnadenfrist nach dem Start, gleich geht es in den Tiefschlaf
 */

/*
 * Kehrt nur zurueck, wenn DIP1 auf ON steht.
 *
 * Steht er auf OFF, blinkt die LED POWER_BOOT_GRACE_MS lang hektisch und das
 * Geraet legt sich danach schlafen -- diese Funktion kehrt dann nie zurueck.
 * Wird DIP1 innerhalb der Frist auf ON gelegt, geht der Start normal weiter.
 *
 * Direkt nach board_init() aufrufen, noch vor lisy_init(): den UART hochzufahren,
 * nur um gleich einzuschlafen, waere verschwendet.
 */
void power_mgr_boot_gate(void);

/*
 * Blink-Task starten und DIP1 im laufenden Betrieb ueberwachen. Faellt DIP1 auf
 * OFF, wird die Kontrolle zurueckgegeben und das Geraet schlaeft ein.
 * Erst aufrufen, wenn alles andere laeuft.
 */
void power_mgr_start(void);
