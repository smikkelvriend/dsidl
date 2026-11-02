// DSiDL - .rar extraction (RAR4 and RAR5) with a RAM-friendly window
#ifndef DSIDL_RARX_H
#define DSIDL_RARX_H

#include <stdbool.h>
#include <stddef.h>

#include "http.h"

// Extracts every file of 'rar' below 'dest'. Progress: bytes of the archive.
bool rar_extract_all(const char *rar, const char *dest, http_progress_t *prog,
                     int *files_out, char *err, size_t errlen);

#endif
