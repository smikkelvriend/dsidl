// DSiDL - homebrew catalog (Universal-DB)
#ifndef DSIDL_CATALOG_H
#define DSIDL_CATALOG_H

#include <stddef.h>

#define MAX_DL 6

typedef struct {
    char *name;
    char *url;
    unsigned long size;
} cat_download_t;

typedef struct {
    char *title;
    char *author;
    char *description;
    char *version;
    char *category;
    int num_dl;
    cat_download_t dl[MAX_DL];
} cat_entry_t;

typedef struct {
    cat_entry_t *entries;
    int count;
} catalog_t;

// Parses Universal-DB full.json, keeping only DS entries that have a
// directly installable .nds or .dsi file. Returns number of entries or -1.
int catalog_parse(catalog_t *cat, const char *json, size_t len);
extern long catalog_error_offset;
void catalog_free(catalog_t *cat);

#endif
