// DSiDL - user interface drawing (platform independent)
//
// Original design: light, rounded, friendly. All shapes and icons are drawn
// in code; the font is Poppins (SIL Open Font License).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gfx.h"
#include "qrcodegen.h"
#include "ui.h"

// ---------------------------------------------------------------------------
// Hit regions

#define MAX_HITS 96
static struct { int id, x, y, w, h; } hits[MAX_HITS];
static int num_hits;

void ui_hits_clear(void)
{
    num_hits = 0;
}

void ui_hit_add(int id, int x, int y, int w, int h)
{
    if (num_hits < MAX_HITS)
    {
        hits[num_hits].id = id;
        hits[num_hits].x = x;
        hits[num_hits].y = y;
        hits[num_hits].w = w;
        hits[num_hits].h = h;
        num_hits++;
    }
}

int ui_hit_test(int x, int y)
{
    // Last added wins (dialogs are drawn on top)
    for (int i = num_hits - 1; i >= 0; i--)
        if (x >= hits[i].x && y >= hits[i].y && x < hits[i].x + hits[i].w &&
            y < hits[i].y + hits[i].h)
            return hits[i].id;
    return -1;
}

// ---------------------------------------------------------------------------
// Basics

color_t ui_category_color(const char *c)
{
    if (!c)
        return RGB(136, 149, 167);
    if (strcasecmp(c, "game") == 0)
        return C_ORANGE;
    if (strcasecmp(c, "emulator") == 0)
        return C_PURPLE;
    if (strcasecmp(c, "utility") == 0 || strcasecmp(c, "save-tool") == 0)
        return C_GREEN;
    if (strcasecmp(c, "app") == 0 || strcasecmp(c, "media") == 0)
        return C_ACCENT;
    return RGB(136, 149, 167);
}

void ui_background(surface_t *s)
{
    gfx_noclip(s);
    gfx_vgrad(s, 0, 0, SCR_W, SCR_H, C_BG1, C_BG2);
    // Soft diagonal stripes
    for (int y = 0; y < SCR_H; y++)
        for (int x = (y % 24); x < SCR_W; x += 24)
            for (int k = 0; k < 6 && x + k < SCR_W; k++)
            {
                color_t *p = &s->px[y * SCR_W + x + k];
                *p = gfx_mix(*p, C_WHITE, 70);
            }
}

void ui_card(surface_t *s, int x, int y, int w, int h)
{
    gfx_shadow(s, x, y, w, h, 8);
    gfx_rrect(s, x, y, w, h, 8, C_CARD);
}

void ui_key_glyph(surface_t *s, int x, int y, const char *key, color_t bg)
{
    // Small round button glyph like "(A)"
    int w = gfx_text_w(key, FONT_SMALL);
    int d = w + 6 < 13 ? 13 : w + 6;
    gfx_rrect(s, x, y, d, 13, 6, bg);
    gfx_text(s, x + (d - w) / 2, y + 1, key, FONT_SMALL, C_WHITE);
}

void ui_topbar(surface_t *s, bool wifi, int bars, const char *ssid)
{
    gfx_fill(s, 0, 0, SCR_W, 18, C_WHITE);
    gfx_blend(s, 0, 18, SCR_W, 1, RGB(0, 30, 70), 40);
    gfx_blend(s, 0, 19, SCR_W, 1, RGB(0, 30, 70), 16);

    // App badge
    gfx_rrect(s, 4, 2, 14, 14, 4, C_ACCENT);
    icon_download(s, 5, 3, 12, C_WHITE);
    gfx_text(s, 22, 2, "DSiDL", FONT_BOLD, C_TEXT);

    // WiFi on the right
    int x = SCR_W - 6;
    if (wifi)
    {
        x -= 16;
        icon_wifi(s, x, 4, bars, C_ACCENT, C_LINE);
        if (ssid && ssid[0])
        {
            int w = gfx_text_w(ssid, FONT_SMALL);
            if (w > 110)
                w = 110;
            x -= w + 5;
            gfx_text_fit(s, x, 4, 110, ssid, FONT_SMALL, C_DIM);
        }
    }
    else
    {
        int w = gfx_text_w("Geen wifi", FONT_SMALL);
        x -= w;
        gfx_text(s, x, 4, "Geen wifi", FONT_SMALL, C_RED);
        icon_wifi(s, x - 18, 4, 0, C_ACCENT, C_FAINT);
    }
}

void ui_button(surface_t *s, int id, int x, int y, int w, int h, const char *key,
               const char *label, bool primary)
{
    color_t bg = primary ? C_ACCENT : C_WHITE;
    color_t fg = primary ? C_WHITE : C_TEXT;
    gfx_rrect_blend(s, x, y + 1, w, h, h / 2, RGB(0, 30, 70), 40);
    gfx_rrect(s, x, y, w, h, h / 2, bg);
    if (!primary)
        gfx_rrect_line(s, x, y, w, h, h / 2, 1, C_LINE);

    int font = FONT_BOLD;
    int lw = gfx_text_w(label, font);
    int kw = 0;
    if (key)
    {
        int tw = gfx_text_w(key, FONT_SMALL);
        kw = (tw + 6 < 13 ? 13 : tw + 6) + 4;
    }
    if (lw + kw > w - 10)
    {
        font = FONT_SMALL;
        lw = gfx_text_w(label, font);
    }
    if (lw + kw > w - 8)
        key = NULL, kw = 0;
    int tx = x + (w - lw - kw) / 2;
    int ty = y + (h - gfx_line_h(font)) / 2 + 1;
    if (key)
    {
        ui_key_glyph(s, tx, y + (h - 13) / 2, key, primary ? C_ACCENT2 : C_FAINT);
        tx += kw;
    }
    gfx_text_fit(s, tx, ty, w - 8, label, font, fg);
    if (id >= 0)
        ui_hit_add(id, x, y, w, h);
}

void ui_progress(surface_t *s, int x, int y, int w, int h, int permille, color_t c)
{
    if (permille < 0)
        permille = 0;
    if (permille > 1000)
        permille = 1000;
    gfx_rrect(s, x, y, w, h, h / 2, RGB(229, 235, 243));
    int fw = w * permille / 1000;
    if (fw > 0)
    {
        if (fw < h)
            fw = h;
        gfx_rrect(s, x, y, fw, h, h / 2, c);
        // glossy highlight
        gfx_rrect_blend(s, x + 2, y + 1, fw - 4 > 0 ? fw - 4 : 0, h / 2 - 1, h / 4, C_WHITE, 70);
    }
}

int ui_chip_w(const char *text)
{
    return gfx_text_w(text, FONT_SMALL) + 12;
}

void ui_chip(surface_t *s, int x, int y, const char *text, color_t c)
{
    int w = ui_chip_w(text);
    gfx_rrect(s, x, y, w, 14, 7, gfx_mix(C_WHITE, c, 46));
    gfx_text(s, x + 6, y + 1, text, FONT_SMALL, gfx_mix(c, C_BLACK, 40));
}

void ui_spinner(surface_t *s, int cx, int cy, int r, int frame)
{
    static const signed char dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const signed char dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    for (int i = 0; i < 8; i++)
    {
        int age = (frame / 4 - i) & 7;
        color_t c = gfx_mix(C_ACCENT, C_BG1, age * 30);
        gfx_circle(s, cx + dx[i] * r / 10, cy + dy[i] * r / 10, r / 4 + 1, c);
    }
}

void ui_letter_tile(surface_t *s, int x, int y, int size, const char *title, color_t c)
{
    gfx_rrect(s, x, y, size, size, size / 4, c);
    gfx_rrect_blend(s, x + 1, y + 1, size - 2, size / 2, size / 4, C_WHITE, 50);
    char l[2] = { title && title[0] ? title[0] : '?', 0 };
    if (l[0] >= 'a' && l[0] <= 'z')
        l[0] -= 32;
    int font = size >= 40 ? FONT_BIG : size >= 24 ? FONT_TITLE : FONT_BOLD;
    int lh = fonts[font].ascent;
    gfx_text_center(s, x + size / 2, y + (size - lh) / 2 - (font == FONT_BIG ? 1 : 0), l, font, C_WHITE);
}

void ui_toast(surface_t *s, const char *text)
{
    int w = gfx_text_w(text, FONT_BOLD) + 24;
    if (w > 240)
        w = 240;
    int x = (SCR_W - w) / 2;
    gfx_rrect_blend(s, x, 160, w, 24, 12, RGB(30, 36, 48), 230);
    gfx_text_fit(s, x + 12, 165, w - 24, text, FONT_BOLD, C_WHITE);
}

// ---------------------------------------------------------------------------
// Icons

void icon_download(surface_t *s, int x, int y, int sz, color_t c)
{
    int cx = x + sz / 2;
    int t = sz >= 20 ? 3 : 2;
    int head = sz / 3;                 // arrow head half width
    int shaft_top = y + sz / 8;
    int head_top = y + sz / 2 - head / 2;
    gfx_fill(s, cx - t / 2, shaft_top, t, head_top - shaft_top + 1, c);
    for (int i = 0; i <= head; i++)
        gfx_fill(s, cx - (head - i), head_top + i, (head - i) * 2 + 1, 1, c);
    gfx_fill(s, x + sz / 6, y + sz - sz / 8 - t, sz - sz / 3, t, c);
}

void icon_folder(surface_t *s, int x, int y, int sz, color_t c)
{
    int h = sz * 3 / 4;
    int top = y + (sz - h) / 2;
    gfx_rrect(s, x + 1, top, sz / 2, 4, 2, gfx_mix(c, C_BLACK, 30));
    gfx_rrect(s, x + 1, top + 2, sz - 2, h - 2, 3, c);
    gfx_rrect_blend(s, x + 2, top + 4, sz - 4, 3, 1, C_WHITE, 70);
}

void icon_wifi(surface_t *s, int x, int y, int bars, color_t on, color_t off)
{
    // 4 rising bars, 14x10
    for (int i = 0; i < 4; i++)
    {
        int h = 3 + i * 2;
        gfx_rrect(s, x + i * 4, y + 10 - h, 3, h, 1, i < bars ? on : off);
    }
}

void icon_lock(surface_t *s, int x, int y, color_t c)
{
    gfx_ring(s, x + 4, y + 4, 3, 1, c);
    gfx_rrect(s, x, y + 4, 9, 7, 2, c);
}

void icon_search(surface_t *s, int x, int y, color_t c)
{
    gfx_ring(s, x + 5, y + 5, 5, 2, c);
    gfx_line(s, x + 8, y + 8, x + 12, y + 12, 2, c);
}

void icon_phone(surface_t *s, int x, int y, int sz, color_t c)
{
    int w = sz * 5 / 9, h = sz * 8 / 9;
    int px = x + (sz - w) / 2, py = y + (sz - h) / 2;
    gfx_rrect(s, px, py, w, h, 3, c);
    gfx_rrect(s, px + 2, py + 3, w - 4, h - 7, 1, C_WHITE);
    gfx_fill(s, px + w / 2 - 1, py + h - 3, 2, 2, C_WHITE);
}

void icon_refresh(surface_t *s, int x, int y, int sz, color_t c)
{
    int r = sz / 2 - 2;
    int cx = x + sz / 2, cy = y + sz / 2;
    gfx_ring(s, cx, cy, r, sz >= 20 ? 3 : 2, c);
    // arrow head at the top right of the ring
    int ax = cx + r / 2, ay = y + 1;
    for (int i = 0; i < sz / 4 + 1; i++)
        gfx_fill(s, ax - i, ay + i, i + 1, 1, c), gfx_fill(s, ax, ay + i, sz / 4 + 1 - i, 1, c);
}

void icon_star(surface_t *s, int x, int y, int sz, color_t c)
{
    // up arrow in a circle = "update"
    int cx = x + sz / 2;
    gfx_fill(s, cx - 1, y + sz / 4, 3, sz / 2 + 1, c);
    for (int i = 0; i < sz / 3; i++)
        gfx_fill(s, cx - i, y + sz / 5 + i, i * 2 + 1, 1, c);
    gfx_fill(s, x + sz / 5, y + sz - sz / 5, sz - sz * 2 / 5, 2, c);
}

void icon_info(surface_t *s, int x, int y, int sz, color_t c)
{
    gfx_ring(s, x + sz / 2, y + sz / 2, sz / 2 - 1, 2, c);
    gfx_fill(s, x + sz / 2 - 1, y + sz / 2 - 1, 3, sz / 4 + 1, c);
    gfx_fill(s, x + sz / 2 - 1, y + sz / 4, 3, 3, c);
}

void icon_back(surface_t *s, int x, int y, int sz, color_t c)
{
    int cy = y + sz / 2;
    gfx_line(s, x + sz / 4, cy, x + sz - sz / 5, cy, 3, c);
    gfx_line(s, x + sz / 4, cy, x + sz / 2, cy - sz / 4, 3, c);
    gfx_line(s, x + sz / 4, cy, x + sz / 2, cy + sz / 4, 3, c);
}

void icon_check(surface_t *s, int x, int y, int sz, color_t c)
{
    gfx_line(s, x + sz / 5, y + sz / 2, x + sz * 2 / 5, y + sz * 7 / 10, 3, c);
    gfx_line(s, x + sz * 2 / 5, y + sz * 7 / 10, x + sz * 4 / 5, y + sz * 3 / 10, 3, c);
}

void icon_up(surface_t *s, int x, int y, int sz, color_t c)
{
    int cx = x + sz / 2;
    for (int i = 0; i < sz / 2; i++)
        gfx_fill(s, cx - i, y + sz / 4 + i, i * 2 + 1, 1, c);
}

static void draw_icon(surface_t *s, int kind, int x, int y, int sz, color_t c)
{
    switch (kind)
    {
        case 0: icon_phone(s, x, y, sz, c); break;
        case 1: icon_wifi(s, x + (sz - 14) / 2, y + (sz - 10) / 2, 4, c, c); break;
        case 2: icon_refresh(s, x, y, sz, c); break;
        case 3: icon_star(s, x, y, sz, c); break;
        case 4: icon_info(s, x, y, sz, c); break;
        default: icon_back(s, x, y, sz, c); break;
    }
}

// ---------------------------------------------------------------------------
// QR code

void ui_qr(surface_t *s, int cx, int cy, int max_size, const char *text)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 6,
                              qrcodegen_Mask_AUTO, true))
        return;
    int n = qrcodegen_getSize(qr);
    int total = n + 8; // quiet zone of 4 modules
    int m = max_size / total;
    if (m < 1)
        m = 1;
    int size = total * m;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    gfx_rrect(s, x0, y0, size, size, 6, C_WHITE);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y))
                gfx_fill(s, x0 + (x + 4) * m, y0 + (y + 4) * m, m, m, C_TEXT);
}

// ---------------------------------------------------------------------------
// Screens

static void wrap_desc(surface_t *s, int x, int y, int w, int lines, const char *t)
{
    if (t && t[0])
        gfx_text_wrap(s, x, y, w, lines, 13, t, FONT_BODY, C_TEXT);
}

void ui_browser_top(const ui_item_t *it, bool wifi, int bars, const char *ssid)
{
    surface_t *s = &scr_top;
    ui_background(s);
    ui_topbar(s, wifi, bars, ssid);

    ui_card(s, 8, 26, 240, 158);
    if (!it)
    {
        icon_download(s, 112, 60, 32, C_FAINT);
        gfx_text_center(s, 128, 100, "Nog geen lijst", FONT_TITLE, C_TEXT);
        gfx_text_center(s, 128, 122, "Kies X > Lijst vernieuwen", FONT_BODY, C_DIM);
        return;
    }

    color_t cc = ui_category_color(it->category);
    ui_letter_tile(s, 18, 36, 44, it->title, cc);

    gfx_clip(s, 70, 30, 172, 60);
    int tl = gfx_text_wrap(s, 70, 34, 170, 2, 17, it->title, FONT_TITLE, C_TEXT);
    gfx_noclip(s);
    int y = 34 + tl * 17;
    char by[80];
    snprintf(by, sizeof(by), "door %s", it->author ? it->author : "onbekend");
    gfx_text_fit(s, 70, y, 170, by, FONT_SMALL, C_DIM);
    y += 14;

    int cx = 70;
    if (it->category)
    {
        ui_chip(s, cx, y, it->category, cc);
        cx += ui_chip_w(it->category) + 4;
    }
    if (it->version)
    {
        char v[40];
        snprintf(v, sizeof(v), "%s%.24s", (it->version[0] >= '0' && it->version[0] <= '9') ? "v" : "", it->version);
        if (cx + ui_chip_w(v) < 244)
            ui_chip(s, cx, y, v, RGB(120, 132, 150));
    }

    int dy = y + 20 > 86 ? y + 20 : 86;
    gfx_fill(s, 18, dy - 4, 220, 1, C_LINE);
    int lines = (156 - dy) / 13;
    wrap_desc(s, 18, dy, 220, lines, it->description);

    // File line at the bottom of the card
    gfx_fill(s, 8, 160, 240, 24, RGB(244, 247, 251));
    gfx_rrect(s, 8, 168, 240, 16, 8, RGB(244, 247, 251));
    gfx_fill(s, 8, 160, 240, 1, C_LINE);
    char size[24];
    unsigned long b = it->size;
    if (b >= 1024UL * 1024UL)
        snprintf(size, sizeof(size), "%lu.%lu MB", b / 1048576UL, (b % 1048576UL) * 10 / 1048576UL);
    else
        snprintf(size, sizeof(size), "%lu KB", b / 1024);
    icon_download(s, 16, 165, 12, C_ACCENT);
    int sw = gfx_text_w(size, FONT_SMALL);
    gfx_text_fit(s, 32, 165, 200 - sw - 40, it->file ? it->file : "", FONT_SMALL, C_TEXT);
    gfx_text(s, 240 - sw, 165, size, FONT_SMALL, C_DIM);
    if (it->dl_count > 1)
    {
        char v[24];
        snprintf(v, sizeof(v), "L/R %d/%d", it->dl_index + 1, it->dl_count);
        gfx_text(s, 240 - sw - 8 - gfx_text_w(v, FONT_SMALL), 165, v, FONT_SMALL, C_ACCENT);
    }
}

static void draw_row(surface_t *s, int x, int y, int w, int h, const ui_row_t *r, bool sel,
                     int icon_kind)
{
    if (sel)
    {
        gfx_rrect(s, x - 2, y - 2, w + 4, h + 4, 9, C_ACCENT);
        gfx_rrect(s, x, y, w, h, 7, RGB(240, 249, 255));
    }
    else
    {
        gfx_rrect_blend(s, x, y + 1, w, h, 7, RGB(0, 30, 70), 26);
        gfx_rrect(s, x, y, w, h, 7, C_WHITE);
    }

    int ix = x + 4, iy = y + (h - 22) / 2;
    int right = x + w - 6;
    if (icon_kind == 1)
        icon_folder(s, ix, iy, 22, RGB(255, 196, 64));
    else if (icon_kind == 2)
    {
        gfx_rrect(s, ix, iy, 22, 22, 6, C_ACCENT_L);
        int bars = r->badge ? r->badge[0] - '0' : 0;
        icon_wifi(s, ix + 4, iy + 6, bars, C_ACCENT, RGB(190, 215, 235));
    }
    else
        ui_letter_tile(s, ix, iy, 22, r->title, ui_category_color(r->category));

    if (icon_kind != 2 && r->badge && r->badge[0])
    {
        int bw = ui_chip_w(r->badge);
        ui_chip(s, right - bw, y + (h - 14) / 2, r->badge, C_ACCENT);
        right -= bw + 4;
    }
    if (icon_kind == 2 && r->subtitle && strstr(r->subtitle, "lock"))
        ; // lock drawn with subtitle below

    int tx = x + 31;
    int tw = right - tx;
    if (r->subtitle && r->subtitle[0])
    {
        gfx_text_fit(s, tx, y + (h - 25) / 2, tw, r->title, FONT_BOLD, C_TEXT);
        const char *sub = r->subtitle;
        int sx = tx;
        if (icon_kind == 2 && strncmp(sub, "lock", 4) == 0)
        {
            icon_lock(s, tx, y + (h - 25) / 2 + 14, C_DIM);
            sx += 12;
            sub += 4;
            while (*sub == ' ')
                sub++;
        }
        gfx_text_fit(s, sx, y + (h - 25) / 2 + 13, right - sx, sub, FONT_SMALL, C_DIM);
    }
    else
    {
        gfx_text_fit(s, tx, y + (h - 14) / 2, tw, r->title, FONT_BOLD, C_TEXT);
    }
}

static void scrollbar(surface_t *s, int x, int y, int h, int first, int visible, int count)
{
    if (count <= visible)
        return;
    gfx_rrect(s, x, y, 4, h, 2, RGB(214, 222, 233));
    int th = h * visible / count;
    if (th < 12)
        th = 12;
    int ty = y + (h - th) * first / (count - visible);
    gfx_rrect(s, x, ty, 4, th, 2, C_ACCENT);
}

void ui_browser_bottom(const ui_row_t *rows, int count, int sel, int scroll,
                       const char *search, const char *const *tabs, int ntabs, int tab,
                       const char *empty_text)
{
    surface_t *s = &scr_bot;
    ui_hits_clear();
    ui_background(s);

    // Search field + menu button
    gfx_rrect_blend(s, 6, 6, 186, 22, 11, RGB(0, 30, 70), 30);
    gfx_rrect(s, 6, 5, 186, 22, 11, C_WHITE);
    icon_search(s, 14, 9, C_DIM);
    if (search && search[0])
        gfx_text_fit(s, 32, 9, 150, search, FONT_BODY, C_TEXT);
    else
        gfx_text(s, 32, 9, "Zoeken", FONT_BODY, C_FAINT);
    ui_key_glyph(s, 173, 9, "Y", C_FAINT);
    ui_hit_add(HIT_SEARCH, 6, 5, 186, 22);
    ui_button(s, HIT_MENU, 197, 5, 53, 22, "X", "Menu", false);

    // Category tabs
    int tw = (SCR_W - 12) / ntabs;
    for (int i = 0; i < ntabs; i++)
    {
        int x = 6 + i * tw;
        if (i == tab)
        {
            gfx_rrect(s, x + 1, 32, tw - 2, 18, 9, C_ACCENT);
            gfx_text_center(s, x + tw / 2, 34, tabs[i], FONT_BOLD, C_WHITE);
        }
        else
        {
            gfx_text_center(s, x + tw / 2, 34, tabs[i], FONT_BOLD, C_DIM);
        }
        ui_hit_add(HIT_TAB + i, x, 30, tw, 22);
    }

    // List
    const int top = 56, rh = 26;
    if (count == 0)
    {
        gfx_text_center(s, 128, 96, empty_text ? empty_text : "Niets gevonden", FONT_BOLD, C_DIM);
    }
    for (int i = 0; i < BROWSER_ROWS && scroll + i < count; i++)
    {
        int idx = scroll + i;
        draw_row(s, 8, top + i * (rh + 4), 234, rh, &rows[idx], idx == sel, 0);
        ui_hit_add(HIT_ROW + idx, 6, top + i * (rh + 4) - 2, 238, rh + 4);
    }
    scrollbar(s, 248, top, 4 * (rh + 4) - 4, scroll, BROWSER_ROWS, count);

    // Footer
    gfx_blend(s, 0, 176, SCR_W, 16, C_WHITE, 200);
    gfx_fill(s, 0, 176, SCR_W, 1, C_LINE);
    int x = 8;
    ui_key_glyph(s, x, 178, "A", C_ACCENT);
    x += 16;
    x += gfx_text(s, x, 178, "Downloaden", FONT_SMALL, C_TEXT) + 10;
    ui_key_glyph(s, x, 178, "B", C_FAINT);
    x += 16;
    gfx_text(s, x, 178, "Wissen", FONT_SMALL, C_TEXT);
    if (count > 0)
    {
        char pos[24];
        snprintf(pos, sizeof(pos), "%d / %d", sel + 1, count);
        gfx_text(s, 248 - gfx_text_w(pos, FONT_SMALL), 178, pos, FONT_SMALL, C_DIM);
    }
}

void ui_dialog(const char *title, const char *text, const char *const *labels,
               const char *const *keys, int nbuttons, int primary)
{
    surface_t *s = &scr_bot;
    ui_hits_clear();
    gfx_noclip(s);
    gfx_blend(s, 0, 0, SCR_W, SCR_H, RGB(20, 30, 45), 120);

    // Measure text height
    int lines = 0;
    {
        // count lines by drawing into a clipped-away area
        surface_t tmp = *s;
        tmp.cx0 = tmp.cx1 = 0;
        tmp.cy0 = tmp.cy1 = 0;
        lines = gfx_text_wrap(&tmp, 0, 0, 196, 7, 14, text ? text : "", FONT_BODY, 0);
    }
    int h = 34 + lines * 14 + 12 + (nbuttons ? 30 : 0);
    if (h < 80)
        h = 80;
    int y = (SCR_H - h) / 2;
    ui_card(s, 16, y, 224, h);
    gfx_rrect(s, 16, y, 224, 6, 3, C_ACCENT);
    gfx_fill(s, 16, y + 3, 224, 3, C_WHITE);
    gfx_text_fit(s, 30, y + 10, 196, title, FONT_TITLE, C_TEXT);
    gfx_text_wrap(s, 30, y + 32, 196, 7, 14, text ? text : "", FONT_BODY, C_DIM);

    if (nbuttons > 0)
    {
        int bw = (204 - (nbuttons - 1) * 6) / nbuttons;
        for (int i = 0; i < nbuttons; i++)
            ui_button(s, HIT_BTN + i, 26 + i * (bw + 6), y + h - 32, bw, 24,
                      keys ? keys[i] : NULL, labels[i], i == primary);
    }
}

static void fmt_size(unsigned long b, char *o, size_t n)
{
    if (b >= 1024UL * 1024UL)
        snprintf(o, n, "%lu.%lu MB", b / 1048576UL, (b % 1048576UL) * 10 / 1048576UL);
    else if (b >= 1024)
        snprintf(o, n, "%lu KB", b / 1024);
    else
        snprintf(o, n, "%lu B", b);
}

static void fmt_time(long s, char *o, size_t n)
{
    if (s < 0)
        snprintf(o, n, "--:--");
    else if (s >= 3600)
        snprintf(o, n, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(o, n, "%ld:%02ld", s / 60, s % 60);
}

static void progress_block(surface_t *s, int x, int y, int w, unsigned long done,
                           unsigned long total, unsigned long speed, long eta)
{
    char a[20], b[20], sp[24], t[20];
    fmt_size(done, a, sizeof(a));
    int pm = total ? (int)((unsigned long long)done * 1000 / total) : 0;
    ui_progress(s, x, y, w, 14, total ? pm : 0, C_ACCENT);

    char line[64];
    if (total)
    {
        fmt_size(total, b, sizeof(b));
        snprintf(line, sizeof(line), "%s / %s", a, b);
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", pm / 10);
        gfx_text(s, x + w - gfx_text_w(pct, FONT_TITLE), y + 20, pct, FONT_TITLE, C_ACCENT);
    }
    else
        snprintf(line, sizeof(line), "%s", a);
    gfx_text(s, x, y + 22, line, FONT_BODY, C_TEXT);

    if (speed)
    {
        fmt_size(speed, sp, sizeof(sp));
        strcat(sp, "/s");
    }
    else
        snprintf(sp, sizeof(sp), "...");
    fmt_time(total && speed ? eta : -1, t, sizeof(t));

    gfx_text(s, x, y + 44, "Snelheid", FONT_SMALL, C_DIM);
    gfx_text(s, x, y + 56, sp, FONT_BOLD, C_TEXT);
    if (total)
    {
        gfx_text(s, x + w / 2, y + 44, "Resterend", FONT_SMALL, C_DIM);
        gfx_text(s, x + w / 2, y + 56, t, FONT_BOLD, C_TEXT);
    }
}

void ui_progress_screen(const char *title, const char *name, const char *status,
                        unsigned long done, unsigned long total,
                        unsigned long speed, long eta_s, const char *cancel_label)
{
    surface_t *s = &scr_bot;
    ui_hits_clear();
    ui_background(s);
    ui_card(s, 10, 10, 236, 140);
    gfx_rrect(s, 22, 22, 26, 26, 8, C_ACCENT_L);
    icon_download(s, 24, 24, 22, C_ACCENT);
    gfx_text_fit(s, 56, 20, 180, title, FONT_TITLE, C_TEXT);
    gfx_text_fit(s, 56, 37, 180, name ? name : "", FONT_SMALL, C_DIM);
    progress_block(s, 22, 60, 212, done, total, speed, eta_s);
    gfx_text_fit(s, 22, 135, 212, status ? status : "", FONT_SMALL, C_FAINT);

    ui_button(s, HIT_BTN, 64, 160, 128, 24, "B", cancel_label ? cancel_label : "Annuleren", false);
}

void ui_menu(const ui_tile_t *tiles, int n, int sel, const char *top_title,
             const char *top_text, bool wifi, int bars, const char *ssid)
{
    // Top: description of the selected tile
    surface_t *t = &scr_top;
    ui_background(t);
    ui_topbar(t, wifi, bars, ssid);
    ui_card(t, 8, 30, 240, 150);
    if (sel >= 0 && sel < n)
    {
        gfx_rrect(t, 104, 42, 48, 48, 14, tiles[sel].color);
        draw_icon(t, tiles[sel].icon, 112, 50, 32, C_WHITE);
    }
    gfx_text_center(t, 128, 98, top_title ? top_title : "", FONT_TITLE, C_TEXT);
    if (top_text)
        gfx_text_wrap(t, 24, 120, 208, 4, 13, top_text, FONT_BODY, C_DIM);

    // Bottom: tile grid
    surface_t *s = &scr_bot;
    ui_hits_clear();
    ui_background(s);
    gfx_text(s, 10, 6, "Menu", FONT_TITLE, C_TEXT);
    const int cols = 3, tw = 76, th = 64, gx = 7, gy = 8, x0 = 9, y0 = 28;
    for (int i = 0; i < n; i++)
    {
        int x = x0 + (i % cols) * (tw + gx), y = y0 + (i / cols) * (th + gy);
        if (i == sel)
            gfx_rrect(s, x - 3, y - 3, tw + 6, th + 6, 13, C_ACCENT);
        ui_card(s, x, y, tw, th);
        gfx_rrect(s, x + tw / 2 - 14, y + 8, 28, 28, 9, tiles[i].color);
        draw_icon(s, tiles[i].icon, x + tw / 2 - 10, y + 12, 20, C_WHITE);
        gfx_clip(s, x + 2, y, tw - 4, th);
        gfx_text_center(s, x + tw / 2, y + 42, tiles[i].label, FONT_BOLD, C_TEXT);
        gfx_noclip(s);
        ui_hit_add(HIT_TILE + i, x, y, tw, th);
    }
    gfx_blend(s, 0, 176, SCR_W, 16, C_WHITE, 200);
    gfx_fill(s, 0, 176, SCR_W, 1, C_LINE);
    ui_key_glyph(s, 8, 178, "A", C_ACCENT);
    gfx_text(s, 24, 178, "Kiezen", FONT_SMALL, C_TEXT);
    ui_key_glyph(s, 70, 178, "B", C_FAINT);
    gfx_text(s, 86, 178, "Terug", FONT_SMALL, C_TEXT);
}

void ui_server_top(const char *url)
{
    surface_t *s = &scr_top;
    ui_background(s);
    ui_card(s, 8, 8, 240, 176);
    ui_qr(s, 70, 96, 116, url);
    gfx_text(s, 136, 30, "Scan met je", FONT_TITLE, C_TEXT);
    gfx_text(s, 136, 48, "telefoon", FONT_TITLE, C_TEXT);
    gfx_text_wrap(s, 136, 74, 104, 4, 13, "Zelfde wifi-netwerk. Of typ het adres in je browser:", FONT_SMALL, C_DIM);
    gfx_rrect(s, 134, 138, 108, 22, 11, C_ACCENT_L);
    gfx_text_fit(s, 142, 142, 96, url + (strncmp(url, "http://", 7) == 0 ? 7 : 0), FONT_BOLD, C_ACCENT2);
}

void ui_server_bottom(const char *url, bool busy, const char *what, const char *name,
                      unsigned long done, unsigned long total, unsigned long speed,
                      long eta_s, const char *last, int files)
{
    (void)url;
    surface_t *s = &scr_bot;
    ui_hits_clear();
    ui_background(s);
    ui_card(s, 10, 8, 236, 146);
    gfx_rrect(s, 22, 22, 26, 26, 8, busy ? C_ACCENT_L : RGB(225, 247, 235));
    icon_phone(s, 24, 24, 22, busy ? C_ACCENT : C_GREEN);
    if (busy)
    {
        gfx_text(s, 56, 20, what ? what : "Bezig", FONT_TITLE, C_TEXT);
        gfx_text_fit(s, 56, 37, 180, name ? name : "", FONT_SMALL, C_DIM);
        progress_block(s, 22, 60, 212, done, total, speed, eta_s);
    }
    else
    {
        gfx_text(s, 56, 20, "Klaar voor gebruik", FONT_TITLE, C_TEXT);
        gfx_text(s, 56, 37, "Wachten op je telefoon...", FONT_SMALL, C_DIM);
        gfx_text_wrap(s, 22, 64, 212, 3, 14,
                      "Bekijk, upload, verplaats en kopieer bestanden op je SD-kaart vanuit de browser.",
                      FONT_BODY, C_TEXT);
    }
    gfx_fill(s, 22, 131, 212, 1, C_LINE);
    char cnt[48];
    snprintf(cnt, sizeof(cnt), "%d ontvangen", files);
    gfx_text_fit(s, 22, 135, 140, last && last[0] ? last : "Nog niets ontvangen", FONT_SMALL, C_DIM);
    gfx_text(s, 234 - gfx_text_w(cnt, FONT_SMALL), 135, cnt, FONT_SMALL, C_DIM);

    ui_button(s, HIT_BTN, 64, 160, 128, 24, "B", "Stoppen", false);
}

void ui_splash(const char *const *lines, int nlines, int frame)
{
    surface_t *t = &scr_top;
    ui_background(t);
    gfx_shadow(t, 98, 40, 60, 60, 18);
    gfx_rrect(t, 98, 40, 60, 60, 18, C_ACCENT);
    gfx_rrect_blend(t, 100, 42, 56, 26, 14, C_WHITE, 60);
    icon_download(t, 108, 50, 40, C_WHITE);
    gfx_text_center(t, 128, 110, "DSiDL", FONT_BIG, C_TEXT);
    gfx_text_center(t, 128, 140, "Homebrew downloader", FONT_BODY, C_DIM);

    surface_t *s = &scr_bot;
    ui_hits_clear();
    ui_background(s);
    ui_card(s, 16, 24, 224, 144);
    ui_spinner(s, 128, 56, 12, frame);
    int y = 82;
    int first = nlines > 5 ? nlines - 5 : 0;
    for (int i = first; i < nlines; i++)
    {
        color_t c = i == nlines - 1 ? C_TEXT : C_FAINT;
        gfx_text_center(s, 128, y, lines[i], i == nlines - 1 ? FONT_BOLD : FONT_BODY, c);
        y += 16;
    }
}

void ui_list_screen(const char *header, const char *subheader, const ui_row_t *rows,
                    int count, int sel, int scroll, int icon_kind,
                    const char *const *labels, const char *const *keys, int nbuttons,
                    const char *empty_text)
{
    surface_t *s = &scr_bot;
    ui_hits_clear();
    ui_background(s);

    gfx_fill(s, 0, 0, SCR_W, 26, C_WHITE);
    gfx_blend(s, 0, 26, SCR_W, 1, RGB(0, 30, 70), 40);
    gfx_text_fit(s, 10, 1, 236, header, FONT_BOLD, C_TEXT);
    if (subheader)
        gfx_text_fit(s, 10, 13, 236, subheader, FONT_SMALL, C_ACCENT2);

    const int top = 32, rh = 28, step = 33;
    if (count == 0)
        gfx_text_center(s, 128, 80, empty_text ? empty_text : "Leeg", FONT_BOLD, C_DIM);
    for (int i = 0; i < LIST_ROWS && scroll + i < count; i++)
    {
        int idx = scroll + i;
        draw_row(s, 8, top + i * step, 234, rh, &rows[idx], idx == sel, icon_kind);
        ui_hit_add(HIT_ROW + idx, 6, top + i * step - 1, 238, step);
    }
    scrollbar(s, 248, top, LIST_ROWS * step - 3, scroll, LIST_ROWS, count);

    if (nbuttons > 0)
    {
        gfx_blend(s, 0, 166, SCR_W, 26, C_WHITE, 170);
        int bw = (244 - (nbuttons - 1) * 4) / nbuttons;
        for (int i = 0; i < nbuttons; i++)
            ui_button(s, HIT_BTN + i, 6 + i * (bw + 4), 168, bw, 21,
                      keys ? keys[i] : NULL, labels[i], i == 0);
    }
}

void ui_info_top(const char *title, const char *const *lines, int nlines,
                 bool wifi, int bars, const char *ssid)
{
    surface_t *s = &scr_top;
    ui_background(s);
    ui_topbar(s, wifi, bars, ssid);
    ui_card(s, 8, 26, 240, 158);
    gfx_text(s, 20, 34, title, FONT_TITLE, C_TEXT);
    int y = 58;
    for (int i = 0; i < nlines && y < 176; i++)
    {
        if (lines[i][0] == '\0')
        {
            y += 6;
            continue;
        }
        bool hl = lines[i][0] == '!';
        int n = gfx_text_wrap(s, 20, y, 216, 3, 13, lines[i] + (hl ? 1 : 0), FONT_BODY,
                              hl ? C_ACCENT2 : C_TEXT);
        y += n * 13 + 2;
    }
}

// ---------------------------------------------------------------------------
// Keyboard

static const char *kb_rows[2][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._" },
    { "!@#$%&*()=", "+/\\?;:'\"[]", "<>{}~^|`,_", "1234567890" },
};

// Layout: rows 0-2: 10 keys each; row 3: SHIFT + 7..9 chars? We use:
// row 3: [shift] + 7 keys + [back]  ; row 4: [syms] [space] [ok]
typedef struct { int x, y, w, h, code; char label[8]; } kkey_t;

static int build_keys(const ui_kbd_t *k, kkey_t *keys)
{
    int n = 0;
    int set = k->symbols ? 1 : 0;
    const int kw = 23, kh = 25, gap = 2, x0 = 3, y0 = 44;
    for (int r = 0; r < 3; r++)
    {
        const char *row = kb_rows[set][r];
        for (int i = 0; i < 10; i++)
        {
            char c = row[i];
            if (!set && k->shift && c >= 'a' && c <= 'z')
                c -= 32;
            keys[n].x = x0 + i * (kw + gap);
            keys[n].y = y0 + r * (kh + 3);
            keys[n].w = kw;
            keys[n].h = kh;
            keys[n].code = (unsigned char)c;
            keys[n].label[0] = c;
            keys[n].label[1] = 0;
            n++;
        }
    }
    // Row 3
    int y = y0 + 3 * (kh + 3);
    keys[n] = (kkey_t){ x0, y, kw * 2 - 4, kh, KB_SHIFT, "Shift" };
    if (set)
        strcpy(keys[n].label, " ");
    n++;
    const char *row = kb_rows[set][3];
    int cx = x0 + kw * 2 - 4 + gap;
    int count = set ? 7 : 7;
    for (int i = 0; i < count; i++)
    {
        char c = row[i];
        if (!set && k->shift && c >= 'a' && c <= 'z')
            c -= 32;
        keys[n] = (kkey_t){ cx, y, kw, kh, (unsigned char)c, { c, 0 } };
        cx += kw + gap;
        n++;
    }
    keys[n] = (kkey_t){ cx, y, 253 - cx, kh, KB_BACK, "Wis" };
    n++;
    // Row 4
    y += kh + 3;
    keys[n] = (kkey_t){ x0, y, 48, kh, KB_SYMS, "" };
    strcpy(keys[n].label, set ? "abc" : "#+=");
    n++;
    keys[n] = (kkey_t){ x0 + 50, y, 138, kh, ' ', "spatie" };
    n++;
    keys[n] = (kkey_t){ x0 + 190, y, 60, kh, KB_OK, "OK" };
    n++;
    return n;
}

int ui_kbd_count(const ui_kbd_t *k)
{
    kkey_t keys[64];
    return build_keys(k, keys);
}

int ui_kbd_key(const ui_kbd_t *k, int i)
{
    kkey_t keys[64];
    int n = build_keys(k, keys);
    if (i < 0 || i >= n)
        return 0;
    return keys[i].code;
}

void ui_kbd_move(ui_kbd_t *k, int dx, int dy)
{
    kkey_t keys[64];
    int n = build_keys(k, keys);
    kkey_t *cur = &keys[k->cursor_key < n ? k->cursor_key : 0];
    int cx = cur->x + cur->w / 2, cy = cur->y + cur->h / 2;
    if (dx)
    {
        int best = -1, bestd = 1 << 30;
        for (int i = 0; i < n; i++)
        {
            if (keys[i].y != cur->y)
                continue;
            int kx = keys[i].x + keys[i].w / 2;
            int d = (kx - cx) * dx;
            if (d > 0 && d < bestd)
            {
                bestd = d;
                best = i;
            }
        }
        if (best < 0) // wrap around
        {
            for (int i = 0; i < n; i++)
            {
                if (keys[i].y != cur->y)
                    continue;
                int kx = keys[i].x + keys[i].w / 2;
                int d = -(kx - cx) * dx;
                if (d > 0 && (best < 0 || d > bestd))
                {
                    bestd = d;
                    best = i;
                }
            }
        }
        if (best >= 0)
            k->cursor_key = best;
    }
    if (dy)
    {
        int best = -1, bestd = 1 << 30;
        for (int i = 0; i < n; i++)
        {
            int ky = keys[i].y + keys[i].h / 2;
            if ((ky - cy) * dy <= 0)
                continue;
            int kx = keys[i].x + keys[i].w / 2;
            int d = abs(ky - cy) * 4 + abs(kx - cx);
            if (d < bestd)
            {
                bestd = d;
                best = i;
            }
        }
        if (best >= 0)
            k->cursor_key = best;
    }
}

void ui_keyboard(const ui_kbd_t *k)
{
    surface_t *s = &scr_bot;
    ui_hits_clear();
    gfx_noclip(s);
    gfx_vgrad(s, 0, 0, SCR_W, SCR_H, RGB(232, 237, 245), RGB(208, 217, 230));

    // Input field
    gfx_text_fit(s, 8, 1, 240, k->title ? k->title : "", FONT_SMALL, C_DIM);
    gfx_rrect(s, 4, 14, 248, 26, 9, C_WHITE);
    gfx_rrect_line(s, 4, 14, 248, 26, 9, 2, C_ACCENT);
    char shown[80];
    size_t len = strlen(k->buf);
    size_t n = len < sizeof(shown) - 1 ? len : sizeof(shown) - 1;
    for (size_t i = 0; i < n; i++)
        shown[i] = k->masked ? '*' : k->buf[i];
    shown[n] = '\0';
    // Show the end of long text
    const char *vis = shown;
    while (gfx_text_w(vis, FONT_TITLE) > 230 && *vis)
        vis++;
    int w = gfx_text(s, 12, 18, vis, FONT_TITLE, C_TEXT);
    gfx_fill(s, 13 + w, 19, 2, 17, C_ACCENT);
    if (len == 0 && k->hint)
        gfx_text_fit(s, 20, 21, 220, k->hint, FONT_SMALL, C_FAINT);

    kkey_t keys[64];
    int nk = build_keys(k, keys);
    for (int i = 0; i < nk; i++)
    {
        kkey_t *kk = &keys[i];
        bool special = kk->code < 0 || kk->code == ' ';
        bool active = (kk->code == KB_SHIFT && k->shift) || (kk->code == KB_SYMS && k->symbols);
        color_t bg = kk->code == KB_OK ? C_ACCENT : active ? C_ACCENT_L : special ? RGB(241, 244, 249) : C_WHITE;
        gfx_rrect_blend(s, kk->x, kk->y + 1, kk->w, kk->h, 6, RGB(0, 30, 70), 50);
        gfx_rrect(s, kk->x, kk->y, kk->w, kk->h, 6, bg);
        if (i == k->cursor_key)
            gfx_rrect_line(s, kk->x - 1, kk->y - 1, kk->w + 2, kk->h + 2, 7, 2, C_ORANGE);
        int font = special ? FONT_SMALL : FONT_TITLE;
        if (kk->code == KB_OK)
            font = FONT_BOLD;
        color_t fc = kk->code == KB_OK ? C_WHITE : C_TEXT;
        int lh = font == FONT_TITLE ? 16 : 12;
        if (kk->code == KB_SHIFT && !k->symbols)
            icon_up(s, kk->x + kk->w / 2 - 6, kk->y + 6, 12, active ? C_ACCENT : C_TEXT);
        else if (kk->code == KB_BACK)
            icon_back(s, kk->x + kk->w / 2 - 8, kk->y + kk->h / 2 - 8, 16, C_TEXT);
        else
            gfx_text_center(s, kk->x + kk->w / 2, kk->y + (kk->h - lh) / 2 - (font == FONT_TITLE ? 2 : 0),
                            kk->label, font, fc);
        ui_hit_add(HIT_KEY + i, kk->x - 1, kk->y - 1, kk->w + 2, kk->h + 3);
    }
}
