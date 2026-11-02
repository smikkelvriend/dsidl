// DSiDL - streaming zip extractor (stored + deflate), low memory
#ifndef DSIDL_UNZIP_H
#define DSIDL_UNZIP_H

#include <stdbool.h>
#include <stddef.h>

#include "http.h"

// Extracts every entry of 'zip' below 'dest_root' (e.g. "sd:").
// Existing files are replaced. Progress: done/total = bytes of the zip read.
bool unzip_all(const char *zip, const char *dest_root, http_progress_t *prog,
               int *files_out, char *err, size_t errlen);

#endif
