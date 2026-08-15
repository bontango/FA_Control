#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"

/*
 * Dateiablage auf lisy.dev.
 *
 * Zwei Dinge werden von dort geholt: Firmware-Images (fw_update.c) und
 * Namensdateien (names.c). Beides liegt in einem Apache-Verzeichnis, beides
 * wird ueber dasselbe Muster gefunden -- Listing nach href="*.<endung>"
 * absuchen. Deshalb steht der Scanner hier einmal statt zweimal dort.
 *
 * Nur im STA-Modus sinnvoll; im AP-Modus gibt es kein Internet. Die Handler in
 * web_server.c weisen das vorher ab.
 */

#define REPO_MAX_FILES 20
#define REPO_MAX_NAME  64

/* Verzeichnis-Listing holen und als {"files":["a.bin","b.bin"]} ablegen,
 * absteigend sortiert (neueste Version zuerst).
 *
 * suffix = "/" listet die UNTERVERZEICHNISSE statt der Dateien -- Apache
 * schreibt sie als href="AtariFA/". Die Namen kommen dann mit Schraegstrich am
 * Ende, so wie sie im Listing stehen. */
esp_err_t repo_list_json(const char *base_url, const char *suffix,
                         char *out, size_t out_len);

/* Datei nach fp streamen, hoechstens max_len Bytes. Schreibt stueckweise, damit
 * kein Puffer in Dateigroesse neben dem TLS-Kontext im Heap stehen muss. */
esp_err_t repo_download_to_file(const char *base_url, const char *file,
                                FILE *fp, size_t max_len);

/* Ein Pfadstueck pruefen: nicht leer, Laenge < max_len, nur [A-Za-z0-9._-] und
 * weder "." noch "..". Damit ist ausgeschlossen, was aus dem Verzeichnis
 * herausfuehren koennte -- "/" ist im Zeichenvorrat gar nicht erst enthalten. */
bool repo_valid_segment(const char *seg, size_t max_len);

/* Wie repo_valid_segment(), zusaetzlich muss die Endung passen und vor ihr noch
 * etwas stehen. */
bool repo_valid_filename(const char *file, const char *suffix, size_t max_len);
