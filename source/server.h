// DSiDL - upload server so a phone can send files over WiFi
#ifndef DSIDL_SERVER_H
#define DSIDL_SERVER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    volatile bool stop;         // set by UI
    volatile bool receiving;
    volatile unsigned long done;
    volatile unsigned long total;
    volatile int files_ok;
    char current[96];
    char last_msg[96];
    char ip[24];
} server_state_t;

// Blocking: runs until state->stop is set. Returns false if the port could
// not be opened.
bool server_run(server_state_t *state, const char *default_dir);

#endif
