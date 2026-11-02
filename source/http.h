// DSiDL - HTTP(S) client
#ifndef DSIDL_HTTP_H
#define DSIDL_HTTP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

// Shared progress info, read by the UI thread.
typedef struct {
    volatile size_t done;
    volatile size_t total;     // 0 if unknown
    volatile bool cancel;      // set by UI to abort
    volatile bool counts;      // done/total are item counts, not bytes
    char status[64];
} http_progress_t;

// Extra cancel flag checked by all requests (e.g. a job's own Cancel button
// while it makes several downloads with their own progress structs).
void http_set_cancel_flag(volatile bool *flag);

// Load the built-in CA certificates. Call once at startup.
bool http_init(void);

// Download URL into a file. Follows redirects. Returns HTTP status (200 ok),
// or a negative value on error. 'err' gets a short description.
int http_get_file(const char *url, const char *path, http_progress_t *prog,
                  char *err, size_t errlen);

// Download URL into a malloc'ed buffer (NUL terminated). Caller frees.
int http_get_mem(const char *url, char **out, size_t *outlen, size_t maxlen,
                 http_progress_t *prog, char *err, size_t errlen);

#endif
