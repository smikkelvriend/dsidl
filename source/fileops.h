// DSiDL - file operations used by the on-DSi file manager
#ifndef DSIDL_FILEOPS_H
#define DSIDL_FILEOPS_H

#include <stdbool.h>
#include <stddef.h>

#include "http.h"

// Copies a file or a whole folder. Progress: done/total of the current file.
bool fo_copy(const char *src, const char *dst, http_progress_t *p);
// Deletes a file or an empty folder
bool fo_delete(const char *path, bool *not_empty);
// "dir/name", "dir/name (2)", ... whichever doesn't exist yet
void fo_unique(const char *dir, const char *name, char *out, size_t len);
bool fo_is_dir(const char *path);

#endif
