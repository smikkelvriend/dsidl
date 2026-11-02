// DSiDL - interactive UI helpers on the DS (input, dialogs, keyboard)
#ifndef DSIDL_APPUI_H
#define DSIDL_APPUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

typedef struct {
    uint32_t down, held;
    int hit;        // tapped region id (on release), -1 if none
    int drag_dy;    // vertical drag since last frame (pixels)
    bool touching;
} input_t;

extern volatile uint32_t g_frames;

void app_wait_frame(void);          // waits one vblank
input_t app_input(void);            // reads keys + touch

// Dialog on the bottom screen. keys: "A", "B", "X", "Y", "L", "R", "START"...
// Returns the chosen button index, or -1 when B is pressed and no button
// uses "B".
int  app_dialog(const char *title, const char *text, const char *const *labels,
                const char *const *keys, int n, int primary);
void app_message(const char *title, const char *text);
bool app_confirm(const char *title, const char *text, const char *yes, const char *no);

// Shows a small status dialog without waiting (for blocking work)
void app_status(const char *title, const char *text);

// Keyboard text input. Returns false if cancelled.
bool app_text_input(const char *title, const char *hint, char *buf, size_t size,
                    bool password);

// Top bar wifi info used by several screens
void app_wifi_info(bool *connected, int *bars, const char **ssid);

#endif
