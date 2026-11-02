// DSiDL - user interface: theme, icons, widgets and screens
#ifndef DSIDL_UI_H
#define DSIDL_UI_H

#include <stdbool.h>
#include <stddef.h>

#include "gfx.h"
#include "img.h"

// ---------------------------------------------------------------------------
// Theme

typedef struct {
    color_t bg1, bg2, stripe;     // screen background gradient + stripes
    color_t card, surface, surface2; // cards, bars/fields, secondary fields
    color_t line, text, dim, faint;
    color_t accent, accent2, accent_l;
    bool dark;
} theme_t;

extern theme_t theme;

#define C_BG1      theme.bg1
#define C_BG2      theme.bg2
#define C_CARD     theme.card
#define C_SURFACE  theme.surface
#define C_SURFACE2 theme.surface2
#define C_LINE     theme.line
#define C_TEXT     theme.text
#define C_DIM      theme.dim
#define C_FAINT    theme.faint
#define C_ACCENT   theme.accent
#define C_ACCENT2  theme.accent2
#define C_ACCENT_L theme.accent_l
#define C_GREEN    RGB(38, 190, 110)
#define C_RED      RGB(238, 72, 82)
#define C_ORANGE   RGB(255, 140, 48)
#define C_PURPLE   RGB(140, 100, 242)
#define C_WHITE    RGB(255, 255, 255)
#define C_BLACK    RGB(0, 0, 0)

#define UI_ACCENTS 6
extern const char *const ui_accent_names[UI_ACCENTS];
void ui_set_theme(bool dark, int accent);

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
    const img_t *icon;      // 44x44 (may be NULL)
} ui_item_t;

typedef struct {
    const char *title;
    const char *subtitle;
    const char *category;   // for the tile color (may be NULL)
    const char *badge;      // small text on the right (may be NULL)
    const img_t *icon;      // 22x22 icon (may be NULL)
    int kind;               // for file lists: 0 file, 1 folder
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
void ui_browser_bottom(const ui_row_t *rows, int count, int sel, int scroll_px, int cursor_px,
                       const char *search, const char *const *tabs, int ntabs, int tab, int tab_px,
                       const char *empty_text);
#define BROWSER_ROWS 4
#define BROWSER_STEP 30
#define LIST_TOP 56

// Dialog over the bottom screen. buttons: labels, keys ("A", "B", "Y"...)
// anim: 0..256 (pop-in progress), pressed: button shown pressed (-1 none)
void ui_dialog(const char *title, const char *text, const char *const *labels,
               const char *const *keys, int nbuttons, int primary, int anim, int pressed);

void ui_progress_screen(const char *title, const char *name, const char *status,
                        unsigned long done, unsigned long total,
                        unsigned long speed, long eta_s, const char *cancel_label);
// Same, but done/total are item counts ("12 / 48")
extern int ui_progress_counts;

typedef struct {
    const char *label;
    int icon;              // 0 phone, 1 wifi, 2 refresh, 3 update, 4 info, 5 back, 6 moon
    color_t color;
} ui_tile_t;
void ui_menu(const ui_tile_t *tiles, int n, int sel, const char *top_title,
             const char *top_text, bool wifi, int bars, const char *ssid,
             int anim, int ring_x, int ring_y);
void ui_menu_tile_pos(int i, int *x, int *y);

void ui_server_top(const char *url);
void ui_server_bottom(const char *url, bool busy, const char *what, const char *name,
                      unsigned long done, unsigned long total, unsigned long speed,
                      long eta_s, const char *last, int files);

void ui_splash(const char *const *lines, int nlines, int frame);

// Generic list screen (folder picker, wifi): header + rows + footer buttons
void ui_list_screen(const char *header, const char *subheader, const ui_row_t *rows,
                    int count, int sel, int scroll_px, int cursor_px, int icon_kind,
                    const char *const *labels, const char *const *keys, int nbuttons,
                    const char *empty_text);
#define LIST_ROWS 4
#define LIST_STEP 33
// icon_kind: 0 = letter tile, 1 = folder, 2 = wifi (badge "1".."4" = bars,
// subtitle "lock" -> lock icon)

// Details page of a catalog entry
void ui_details_top(const ui_item_t *it, const img_t *shot, int idx, int count, bool loading,
                    int frame);
void ui_details_bottom(const ui_item_t *it, const char *dest, const char *space, bool warn);

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
    int pressed_key;     // key that was just pressed (flash)
    int press_t;         // frames left for the flash
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
