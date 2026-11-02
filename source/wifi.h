// DSiDL - WiFi manager (scan, choose network, enter password, saved networks)
#ifndef DSIDL_WIFI_H
#define DSIDL_WIFI_H

#include <stdbool.h>
#include <stddef.h>

// Initialises WiFi and connects automatically: first to a network saved by
// DSiDL that is in range, otherwise to the networks set in the DSi settings.
// 'status' (optional) is called with progress text.
bool wifi_start(const char *saved_file, bool autoconnect);

// Opens the WiFi manager screen. Returns true when connected afterwards.
bool wifi_manager(void);

bool wifi_connected(void);
const char *wifi_ssid(void);   // name of the current network ("" if unknown)

// Provided by main.c
bool ui_text_input(const char *title, const char *hint, char *buf, size_t len);

#endif
