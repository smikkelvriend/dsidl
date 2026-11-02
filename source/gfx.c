// DSiDL - software renderer (platform independent part)
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

#if defined(__arm__) && defined(__thumb__)
// The renderer is the hot path: compile it as 32-bit ARM code (faster)
#pragma GCC target("arm")
#endif

surface_t scr_top, scr_bot;

void gfx_clip(surface_t *s, int x, int y, int w, int h)
{
    s->cx0 = x < 0 ? 0 : x;
    s->cy0 = y < 0 ? 0 : y;
    s->cx1 = x + w > SCR_W ? SCR_W : x + w;
    s->cy1 = y + h > SCR_H ? SCR_H : y + h;
}

void gfx_noclip(surface_t *s)
{
    s->cx0 = 0;
    s->cy0 = 0;
    s->cx1 = SCR_W;
    s->cy1 = SCR_H;
}

// Blends two 15-bit colors. Red+blue and green are handled in two parallel
// integer operations instead of three separate channels.
static inline color_t mix5(color_t a, color_t b, uint32_t al /* 0..32 */)
{
    uint32_t ia = 32 - al;
    uint32_t rb = (((a & 0x7C1F) * ia + (b & 0x7C1F) * al) >> 5) & 0x7C1F;
    uint32_t g = (((a & 0x03E0) * ia + (b & 0x03E0) * al) >> 5) & 0x03E0;
    return (color_t)(rb | g | 0x8000);
}

color_t gfx_mix(color_t a, color_t b, int alpha)
{
    if (alpha <= 0)
        return a | 0x8000;
    if (alpha >= 256)
        return b;
    return mix5(a, b, (uint32_t)(alpha + 4) >> 3);
}

static inline void plot_a(surface_t *s, int x, int y, color_t c, int alpha)
{
    if (x < s->cx0 || y < s->cy0 || x >= s->cx1 || y >= s->cy1 || alpha <= 4)
        return;
    color_t *p = &s->px[y * SCR_W + x];
    *p = alpha >= 252 ? c : mix5(*p, c, (uint32_t)(alpha + 4) >> 3);
}

void gfx_fill(surface_t *s, int x, int y, int w, int h, color_t c)
{
    int x0 = x < s->cx0 ? s->cx0 : x, y0 = y < s->cy0 ? s->cy0 : y;
    int x1 = x + w > s->cx1 ? s->cx1 : x + w, y1 = y + h > s->cy1 ? s->cy1 : y + h;
    if (x1 <= x0 || y1 <= y0)
        return;
    uint32_t c2 = c | ((uint32_t)c << 16);
    for (int yy = y0; yy < y1; yy++)
    {
        color_t *p = &s->px[yy * SCR_W + x0];
        int n = x1 - x0;
        if (((uintptr_t)p & 2) && n)
        {
            *p++ = c;
            n--;
        }
        uint32_t *q = (uint32_t *)p;
        for (int i = 0; i < n / 2; i++)
            q[i] = c2;
        if (n & 1)
            p[n - 1] = c;
    }
}

void gfx_blend(surface_t *s, int x, int y, int w, int h, color_t c, int alpha)
{
    int x0 = x < s->cx0 ? s->cx0 : x, y0 = y < s->cy0 ? s->cy0 : y;
    int x1 = x + w > s->cx1 ? s->cx1 : x + w, y1 = y + h > s->cy1 ? s->cy1 : y + h;
    if (alpha <= 4)
        return;
    if (alpha >= 252)
    {
        gfx_fill(s, x, y, w, h, c);
        return;
    }
    uint32_t al = (uint32_t)(alpha + 4) >> 3, ia = 32 - al;
    uint32_t crb = (c & 0x7C1F) * al, cg = (c & 0x03E0) * al;
    for (int yy = y0; yy < y1; yy++)
    {
        color_t *p = &s->px[yy * SCR_W + x0];
        for (int xx = x0; xx < x1; xx++, p++)
        {
            color_t a = *p;
            *p = (color_t)(((((a & 0x7C1F) * ia + crb) >> 5) & 0x7C1F) |
                           ((((a & 0x03E0) * ia + cg) >> 5) & 0x03E0) | 0x8000);
        }
    }
}

void gfx_vgrad(surface_t *s, int x, int y, int w, int h, color_t top, color_t bottom)
{
    for (int i = 0; i < h; i++)
        gfx_fill(s, x, y + i, w, 1, gfx_mix(top, bottom, h > 1 ? i * 256 / (h - 1) : 0));
}

// Coverage (0..256) of a pixel at distance d (in 1/16 px) inside radius r
static int corner_cov(int px, int py, int r)
{
    // px, py: offset of pixel center from the corner circle center, *16
    int r16 = r * 16;
    int d2 = px * px + py * py;
    int in = (r16 - 8) * (r16 - 8), out = (r16 + 8) * (r16 + 8);
    if (d2 <= in)
        return 256;
    if (d2 >= out)
        return 0;
    return (out - d2) * 256 / (out - in);
}

// Precomputed corner coverage per radius: cov[r][cy * r + cx] (0..255)
#define MAX_R 40
static uint8_t *cov_lut[MAX_R + 1];

static const uint8_t *corner_lut(int r)
{
    if (r > MAX_R)
        r = MAX_R;
    if (!cov_lut[r])
    {
        uint8_t *t = malloc(r * r ? r * r : 1);
        if (!t)
            return NULL;
        for (int cy = 1; cy <= r; cy++)
            for (int cx = 1; cx <= r; cx++)
            {
                int c = corner_cov(cx * 16 - 8, cy * 16 - 8, r);
                t[(cy - 1) * r + (cx - 1)] = c > 255 ? 255 : c;
            }
        cov_lut[r] = t;
    }
    return cov_lut[r];
}

static void rrect_impl(surface_t *s, int x, int y, int w, int h, int r, color_t c, int alpha)
{
    if (w <= 0 || h <= 0)
        return;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r > MAX_R)
        r = MAX_R;
    const uint8_t *lut = r > 0 ? corner_lut(r) : NULL;
    for (int yy = 0; yy < h; yy++)
    {
        int cy = -1;
        if (yy < r)
            cy = r - yy;
        else if (yy >= h - r)
            cy = yy - (h - r) + 1;
        if (cy < 0 || !lut)
        {
            gfx_blend(s, x, y + yy, w, 1, c, alpha);
            continue;
        }
        const uint8_t *row = lut + (cy - 1) * r;
        // Corners
        for (int cx = r; cx >= 1; cx--)
        {
            int a = row[cx - 1];
            if (a == 0)
                continue;
            a = a == 255 ? alpha : a * alpha >> 8;
            plot_a(s, x + (r - cx), y + yy, c, a);
            plot_a(s, x + w - 1 - (r - cx), y + yy, c, a);
        }
        // Middle part of the row
        gfx_blend(s, x + r, y + yy, w - 2 * r, 1, c, alpha);
    }
}

void gfx_rrect(surface_t *s, int x, int y, int w, int h, int r, color_t c)
{
    rrect_impl(s, x, y, w, h, r, c, 256);
}

void gfx_rrect_blend(surface_t *s, int x, int y, int w, int h, int r, color_t c, int alpha)
{
    rrect_impl(s, x, y, w, h, r, c, alpha);
}

void gfx_rrect_line(surface_t *s, int x, int y, int w, int h, int r, int t, color_t c)
{
    // Draw outer then punch inner using the existing background is not
    // possible, so draw the border as four rounded strips.
    for (int i = 0; i < t; i++)
    {
        int xx = x + i, yy = y + i, ww = w - 2 * i, hh = h - 2 * i, rr = r - i;
        if (rr < 0)
            rr = 0;
        // top & bottom edges
        gfx_fill(s, xx + rr, yy, ww - 2 * rr, 1, c);
        gfx_fill(s, xx + rr, yy + hh - 1, ww - 2 * rr, 1, c);
        gfx_fill(s, xx, yy + rr, 1, hh - 2 * rr, c);
        gfx_fill(s, xx + ww - 1, yy + rr, 1, hh - 2 * rr, c);
        // corners
        for (int a = 0; a < rr; a++)
        {
            for (int b = 0; b < rr; b++)
            {
                int px = (rr - a) * 16 - 8, py = (rr - b) * 16 - 8;
                int d2 = px * px + py * py;
                int ro = rr * 16, ri = (rr - 1) * 16;
                int cov;
                if (d2 > (ro + 8) * (ro + 8) || d2 < (ri - 8) * (ri - 8))
                    cov = 0;
                else
                {
                    int co = corner_cov(px, py, rr);
                    int ci = rr > 1 ? corner_cov(px, py, rr - 1) : 0;
                    cov = co - ci;
                }
                plot_a(s, xx + a, yy + b, c, cov);
                plot_a(s, xx + ww - 1 - a, yy + b, c, cov);
                plot_a(s, xx + a, yy + hh - 1 - b, c, cov);
                plot_a(s, xx + ww - 1 - a, yy + hh - 1 - b, c, cov);
            }
        }
    }
}

void gfx_shadow(surface_t *s, int x, int y, int w, int h, int r)
{
    // Soft drop shadow: a few expanding translucent layers
    // Only the edges that stay visible around the card are drawn
    gfx_rrect_blend(s, x - 1, y + h - r, w + 2, r + 3, r + 1, RGB(0, 20, 60), 30);
    gfx_blend(s, x - 1, y + r, 1, h - 2 * r, RGB(0, 20, 60), 18);
    gfx_blend(s, x + w, y + r, 1, h - 2 * r, RGB(0, 20, 60), 18);
}

void gfx_circle(surface_t *s, int cx, int cy, int r, color_t c)
{
    rrect_impl(s, cx - r, cy - r, r * 2, r * 2, r, c, 256);
}

void gfx_ring(surface_t *s, int cx, int cy, int r, int t, color_t c)
{
    gfx_rrect_line(s, cx - r, cy - r, r * 2, r * 2, r, t, c);
}

void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, int t, color_t c)
{
    // Simple thick line: stamp small squares along the path
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int steps = dx > dy ? dx : dy;
    if (steps == 0)
        steps = 1;
    for (int i = 0; i <= steps; i++)
    {
        int x = x0 + (x1 - x0) * i / steps;
        int y = y0 + (y1 - y0) * i / steps;
        gfx_fill(s, x - t / 2, y - t / 2, t, t, c);
    }
}

// ---------------------------------------------------------------------------
// Text

// Decodes one UTF-8 character, returns glyph code point we can draw
static unsigned next_cp(const char **ps)
{
    const unsigned char *p = (const unsigned char *)*ps;
    unsigned cp = *p++;
    if (cp >= 0x80)
    {
        if ((cp & 0xE0) == 0xC0 && *p)
            cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
        else if ((cp & 0xF0) == 0xE0 && p[0] && p[1])
        {
            cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        }
        else
        {
            while (*p && (*p & 0xC0) == 0x80)
                p++;
            cp = '?';
        }
    }
    *ps = (const char *)p;
    switch (cp)
    {
        case 0x2018: case 0x2019: return '\'';
        case 0x201C: case 0x201D: return '"';
        case 0x2013: case 0x2014: return '-';
        case 0x2022: return 0xB7; // middle dot
        default: break;
    }
    return cp;
}

static const font_glyph_t *glyph_for(const font_t *f, unsigned cp)
{
    if (cp >= 32 && cp <= 126)
        return &f->glyphs[cp - 32];
    if (cp >= 160 && cp <= 255)
        return &f->glyphs[95 + cp - 160];
    return &f->glyphs['?' - 32];
}

int gfx_line_h(int font)
{
    return fonts[font].ascent + fonts[font].descent;
}

int gfx_text_w(const char *str, int font)
{
    const font_t *f = &fonts[font];
    int w = 0;
    while (*str)
    {
        unsigned cp = next_cp(&str);
        if (cp == '\n')
            break;
        w += glyph_for(f, cp)->advance;
    }
    return w;
}

static void draw_glyph(surface_t *s, int x, int y, const font_t *f, const font_glyph_t *g, color_t c)
{
    const unsigned char *d = f->data + g->offset;
    int gx = x + g->x, gy = y + g->y;
    // Fully clipped?
    if (gx >= s->cx1 || gy >= s->cy1 || gx + g->w <= s->cx0 || gy + g->h <= s->cy0)
        return;
    bool inside = gx >= s->cx0 && gy >= s->cy0 && gx + g->w <= s->cx1 && gy + g->h <= s->cy1;
    uint32_t crb = c & 0x7C1F, cg = c & 0x03E0;
    int i = 0;
    for (int yy = 0; yy < g->h; yy++)
    {
        color_t *row = &s->px[(gy + yy) * SCR_W + gx];
        for (int xx = 0; xx < g->w; xx++, i++)
        {
            uint32_t a = (i & 1) ? (d[i >> 1] >> 4) : (d[i >> 1] & 15);
            if (!a)
                continue;
            if (!inside && (gx + xx < s->cx0 || gx + xx >= s->cx1 || gy + yy < s->cy0 || gy + yy >= s->cy1))
                continue;
            color_t *p = &row[xx];
            if (a == 15)
                *p = c;
            else
            {
                uint32_t al = (a * 32 + 7) / 15, ia = 32 - al;
                color_t b = *p;
                *p = (color_t)(((((b & 0x7C1F) * ia + crb * al) >> 5) & 0x7C1F) |
                               ((((b & 0x03E0) * ia + cg * al) >> 5) & 0x03E0) | 0x8000);
            }
        }
    }
}

int gfx_text(surface_t *s, int x, int y, const char *str, int font, color_t c)
{
    const font_t *f = &fonts[font];
    int x0 = x;
    while (*str)
    {
        unsigned cp = next_cp(&str);
        if (cp == '\n')
            break;
        const font_glyph_t *g = glyph_for(f, cp);
        if (g->w)
            draw_glyph(s, x, y, f, g, c);
        x += g->advance;
    }
    return x - x0;
}

int gfx_text_fit(surface_t *s, int x, int y, int max_w, const char *str, int font, color_t c)
{
    if (gfx_text_w(str, font) <= max_w)
        return gfx_text(s, x, y, str, font, c);
    const font_t *f = &fonts[font];
    int dots = glyph_for(f, '.')->advance * 3;
    int w = 0;
    const char *p = str;
    while (*p)
    {
        const char *q = p;
        unsigned cp = next_cp(&q);
        int a = glyph_for(f, cp)->advance;
        if (w + a + dots > max_w)
            break;
        const font_glyph_t *g = glyph_for(f, cp);
        if (g->w)
            draw_glyph(s, x + w, y, f, g, c);
        w += a;
        p = q;
    }
    w += gfx_text(s, x + w, y, "...", font, c);
    return w;
}

void gfx_text_center(surface_t *s, int cx, int y, const char *str, int font, color_t c)
{
    gfx_text(s, cx - gfx_text_w(str, font) / 2, y, str, font, c);
}

int gfx_text_wrap(surface_t *s, int x, int y, int w, int max_lines, int line_h,
                  const char *str, int font, color_t c)
{
    const font_t *f = &fonts[font];
    int lines = 0;
    const char *p = str;
    char buf[160];

    while (*p && lines < max_lines)
    {
        while (*p == ' ' || *p == '\n')
            p++;
        if (!*p)
            break;

        // Find how many bytes fit on this line, breaking at spaces
        const char *q = p, *last_space = NULL;
        int width = 0;
        while (*q && *q != '\n')
        {
            const char *r = q;
            unsigned cp = next_cp(&r);
            int a = glyph_for(f, cp)->advance;
            if (width + a > w)
                break;
            if (cp == ' ')
                last_space = q;
            width += a;
            q = r;
        }
        const char *end = q;
        if (*q && *q != '\n' && last_space && last_space > p)
            end = last_space;

        size_t n = end - p;
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        memcpy(buf, p, n);
        buf[n] = '\0';

        lines++;
        if (lines == max_lines && *end && *end != '\n')
        {
            // Last allowed line but more text: end with "..."
            size_t rest = strlen(end) < sizeof(buf) - n - 1 ? strlen(end) : sizeof(buf) - n - 1;
            memcpy(buf + n, end, rest);
            buf[n + rest] = '\0';
            gfx_text_fit(s, x, y, w, buf, font, c);
        }
        else
        {
            gfx_text(s, x, y, buf, font, c);
        }
        y += line_h;
        p = end;
    }
    return lines;
}

void gfx_shift_x(surface_t *s, int x, int y, int w, int h, int dx, color_t fill)
{
    if (dx == 0 || w <= 0)
        return;
    if (dx >= w || -dx >= w)
    {
        gfx_fill(s, x, y, w, h, fill);
        return;
    }
    for (int yy = y; yy < y + h && yy < SCR_H; yy++)
    {
        if (yy < 0)
            continue;
        color_t *row = &s->px[yy * SCR_W + x];
        if (dx > 0)
        {
            memmove(row + dx, row, (w - dx) * 2);
            for (int i = 0; i < dx; i++)
                row[i] = fill;
        }
        else
        {
            memmove(row, row - dx, (w + dx) * 2);
            for (int i = w + dx; i < w; i++)
                row[i] = fill;
        }
    }
}
