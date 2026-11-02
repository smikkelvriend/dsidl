// DSiDL - small helpers
#ifndef DSIDL_UTIL_H
#define DSIDL_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void sanitize_filename(char *name);
void mkdir_p(const char *path);
bool file_exists(const char *path);
void format_size(unsigned long bytes, char *out, size_t outlen);
// Returns <0 if a<b, 0 if equal, >0 if a>b ("1.2.10" > "1.2.9")
int version_compare(const char *a, const char *b);
bool contains_ci(const char *haystack, const char *needle);
// Free and total space of a drive ("sd:"). Returns false if unknown.
bool disk_space(const char *drive, unsigned long long *free_b, unsigned long long *total_b);
void format_size64(unsigned long long bytes, char *out, size_t outlen);
uint32_t fnv1a(const char *s);

#endif
