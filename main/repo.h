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
 * absteigend sortiert (neueste Version zuerst). */
esp_err_t repo_list_json(const char *base_url, const char *suffix,
                         char *out, size_t out_len);

/* Datei nach fp streamen, hoechstens max_len Bytes. Schreibt stueckweise, damit
 * kein Puffer in Dateigroesse neben dem TLS-Kontext im Heap stehen muss. */
esp_err_t repo_download_to_file(const char *base_url, const char *file,
                                FILE *fp, size_t max_len);

/* Dateinamen pruefen, bevor er in eine URL oder einen Pfad eingesetzt wird:
 * Laenge < max_len, Endung == suffix, nur [A-Za-z0-9._-]. Das schliesst "..",
 * "/" und alles andere aus, was aus dem Verzeichnis herausfuehren koennte. */
bool repo_valid_filename(const char *file, const char *suffix, size_t max_len);
