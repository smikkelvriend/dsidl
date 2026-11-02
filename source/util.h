// DSiDL - small helpers
#ifndef DSIDL_UTIL_H
#define DSIDL_UTIL_H

#include <stdbool.h>
#include <stddef.h>

void sanitize_filename(char *name);
void mkdir_p(const char *path);
bool file_exists(const char *path);
void format_size(unsigned long bytes, char *out, size_t outlen);
// Returns <0 if a<b, 0 if equal, >0 if a>b ("1.2.10" > "1.2.9")
int version_compare(const char *a, const char *b);
bool contains_ci(const char *haystack, const char *needle);

#endif
