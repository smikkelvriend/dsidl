// DSiDL - finds .nds/.dsi programs on the SD card and reads their banners
#ifndef DSIDL_NDSSCAN_H
#define DSIDL_NDSSCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "img.h"

typedef struct {
    char *path;        // full path, e.g. "sd:/roms/nds/game.nds"
    char *name;        // file name
    char title[96];    // from the banner (first line), or the file name
    char sub[96];      // second/third banner lines (publisher etc.)
    char code[5];      // game code from the header
    unsigned long size;
    img_t icon;        // 32x32 banner icon (may be empty)
    bool icon_read;
} ndsfile_t;

typedef struct {
    ndsfile_t *items;
    int count;
} ndslist_t;

// Scans the drive (skips system folders). max = maximum number of files.
int  nds_scan(ndslist_t *list, const char *drive, int max);
void nds_free(ndslist_t *list);
// Reads title + icon from the file's banner (on demand, cached in the item)
bool nds_read_banner(ndsfile_t *f);
// True if a file with this name exists in the list (case-insensitive)
bool nds_has_name(const ndslist_t *list, const char *name);

#endif
