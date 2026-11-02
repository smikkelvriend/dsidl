// DSiDL - Google Drive folders shared with "anyone with the link"
#ifndef DSIDL_GDRIVE_H
#define DSIDL_GDRIVE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char id[72];
    char name[160];
    bool folder;
} gd_entry_t;

// Extracts the folder id (and resource key) from a link or a bare id
bool gd_parse_link(const char *link, char *id, size_t idlen, char *rkey, size_t rklen);
void gd_folder_url(const char *id, const char *rkey, char *out, size_t len);
void gd_file_url(const char *id, char *out, size_t len);
// Parses the embedded folder view page. Returns number of entries (malloc'ed)
int gd_parse_listing(const char *html, gd_entry_t **out);

#endif
