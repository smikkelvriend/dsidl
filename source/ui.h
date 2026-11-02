// DSiDL - user interface: theme, icons, widgets and screens
#ifndef DSIDL_UI_H
#define DSIDL_UI_H

#include <stdbool.h>
#include <stddef.h>

#include "gfx.h"

// ---------------------------------------------------------------------------
// Theme

#define C_BG1      RGB(246, 248, 252)
#define C_BG2      RGB(226, 233, 243)
#define C_CARD     RGB(255, 255, 255)
#define C_LINE     RGB(222, 228, 237)
#define C_TEXT     RGB(40, 48, 61)
#define C_DIM      RGB(123, 135, 152)
#define C_FAINT    RGB(178, 188, 202)
#define C_ACCENT   RGB(18, 158, 230)
#define C_ACCENT2  RGB(10, 128, 200)
#define C_ACCENT_L RGB(222, 242, 253)
#define C_GREEN    RGB(38, 190, 110)
#define C_RED      RGB(238, 72, 82)
#define C_ORANGE   RGB(255, 140, 48)
#define C_PURPLE   RGB(140, 100, 242)
#define C_WHITE    RGB(255, 255, 255)
#define C_BLACK    RGB(0, 0, 0)

color_t ui_category_color(const char *category);

// ---------------------------------------------------------------------------
// Touch regions: screens register what can be tapped while drawing

void ui_hits_clear(void);
void ui_hit_add(int id, int x, int y, int w, int h);
int  ui_hit_test(int x, int y);   // returns id or -1

// ---------------------------------------------------------------------------
// Building blocks

void ui_background(surface_t *s);
void ui_topbar(surface_t *s, bool wifi, int wifi_bars, const char *ssid);
void ui_card(surface_t *s, int x, int y, int w, int h);
void ui_button(surface_t *s, int id, int x, int y, int w, int h, const char *key,
               const char *label, bool primary);
void ui_key_glyph(surface_t *s, int x, int y, const char *key, color_t bg);
void ui_progress(surface_t *s, int x, int y, int w, int h, int permille, color_t c);
void ui_chip(surface_t *s, int x, int y, const char *text, color_t c);
int  ui_chip_w(const char *text);
void ui_spinner(surface_t *s, int cx, int cy, int r, int frame);
void ui_letter_tile(surface_t *s, int x, int y, int size, const char *title, color_t c);
void ui_toast(surface_t *s, const char *text);

// Icons (size = box size in pixels)
void icon_download(surface_t *s, int x, int y, int sz, color_t c);
void icon_folder(surface_t *s, int x, int y, int sz, color_t c);
void icon_wifi(surface_t *s, int x, int y, int bars, color_t on, color_t off);
void icon_lock(surface_t *s, int x, int y, color_t c);
void icon_search(surface_t *s, int x, int y, color_t c);
void icon_phone(surface_t *s, int x, int y, int sz, color_t c);
void icon_refresh(surface_t *s, int x, int y, int sz, color_t c);
void icon_star(surface_t *s, int x, int y, int sz, color_t c);
void icon_info(surface_t *s, int x, int y, int sz, color_t c);
void icon_back(surface_t *s, int x, int y, int sz, color_t c);
void icon_check(surface_t *s, int x, int y, int sz, color_t c);
void icon_up(surface_t *s, int x, int y, int sz, color_t c);

// QR code of 'text' centered in a box
void ui_qr(surface_t *s, int cx, int cy, int max_size, const char *text);

// ---------------------------------------------------------------------------
// Composite screens (pure drawing; logic lives in main.c / wifi.c)

typedef struct {
    const char *title, *author, *version, *category, *description;
    const char *file;
    unsigned long size;
    int dl_index, dl_count;
} ui_item_t;

typedef struct {
    const char *title;
    const char *subtitle;
    const char *category;   // for the tile color (may be NULL)
    const char *badge;      // small text on the right (may be NULL)
} ui_row_t;

// Hit ids used by the screens
enum {
    HIT_NONE = -1,
    HIT_ROW = 0,          // HIT_ROW + index (0..999)
    HIT_SEARCH = 1000,
    HIT_MENU,
    HIT_TAB,              // HIT_TAB + n (0..9)
    HIT_BTN = 1100,       // HIT_BTN + n for dialog / footer buttons
    HIT_KEY = 1200,       // HIT_KEY + key index
    HIT_LIST_UP = 1400,
    HIT_LIST_DOWN,
    HIT_TILE = 1500,      // HIT_TILE + n
};

void ui_browser_top(const ui_item_t *it, bool wifi, int bars, const char *ssid);
void ui_browser_bottom(const ui_row_t *rows, int count, int sel, int scroll,
                       const char *search, const char *const *tabs, int ntabs, int tab,
                       const char *empty_text);
#define BROWSER_ROWS 4

// Dialog over the bottom screen. buttons: labels, keys ("A", "B", "Y"...)
void ui_dialog(const char *title, const char *text, const char *const *labels,
               const char *const *keys, int nbuttons, int primary);

void ui_progress_screen(const char *title, const char *name, const char *status,
                        unsigned long done, unsigned long total,
                        unsigned long speed, long eta_s, const char *cancel_label);

typedef struct {
    const char *label;
    int icon;              // 0 phone, 1 wifi, 2 refresh, 3 star, 4 info, 5 back
    color_t color;
} ui_tile_t;
void ui_menu(const ui_tile_t *tiles, int n, int sel, const char *top_title,
             const char *top_text, bool wifi, int bars, const char *ssid);

void ui_server_top(const char *url);
void ui_server_bottom(const char *url, bool busy, const char *what, const char *name,
                      unsigned long done, unsigned long total, unsigned long speed,
                      long eta_s, const char *last, int files);

void ui_splash(const char *const *lines, int nlines, int frame);

// Generic list screen (folder picker, wifi): header + rows + footer buttons
void ui_list_screen(const char *header, const char *subheader, const ui_row_t *rows,
                    int count, int sel, int scroll, int icon_kind,
                    const char *const *labels, const char *const *keys, int nbuttons,
                    const char *empty_text);
#define LIST_ROWS 4
// icon_kind: 0 = letter tile, 1 = folder, 2 = wifi (badge "1".."4" = bars,
// subtitle "lock" -> lock icon)

void ui_info_top(const char *title, const char *const *lines, int nlines,
                 bool wifi, int bars, const char *ssid);

// On-screen keyboard
typedef struct {
    const char *title;
    const char *hint;
    char *buf;
    size_t size;
    bool masked;
    bool shift;
    bool symbols;
    int cursor_key;      // key index highlighted for D-pad use
} ui_kbd_t;
void ui_keyboard(const ui_kbd_t *k);
int  ui_kbd_count(const ui_kbd_t *k);
// Returns the character for key i, or special codes:
#define KB_SHIFT  -2
#define KB_BACK   -3
#define KB_SYMS   -4
#define KB_OK     -5
#define KB_SPACE  ' '
int  ui_kbd_key(const ui_kbd_t *k, int i);
void ui_kbd_move(ui_kbd_t *k, int dx, int dy);

#endif
