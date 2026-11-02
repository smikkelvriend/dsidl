// DSiDL - software renderer (platform independent part)
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

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

color_t gfx_mix(color_t a, color_t b, int alpha)
{
    if (alpha <= 0)
        return a;
    if (alpha >= 256)
        return b;
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int r = ar + (((br - ar) * alpha) >> 8);
    int g = ag + (((bg - ag) * alpha) >> 8);
    int bl = ab + (((bb - ab) * alpha) >> 8);
    return (color_t)(r | (g << 5) | (bl << 10) | 0x8000);
}

static inline void plot_a(surface_t *s, int x, int y, color_t c, int alpha)
{
    if (x < s->cx0 || y < s->cy0 || x >= s->cx1 || y >= s->cy1 || alpha <= 0)
        return;
    color_t *p = &s->px[y * SCR_W + x];
    *p = alpha >= 256 ? c : gfx_mix(*p, c, alpha);
}

void gfx_fill(surface_t *s, int x, int y, int w, int h, color_t c)
{
    int x0 = x < s->cx0 ? s->cx0 : x, y0 = y < s->cy0 ? s->cy0 : y;
    int x1 = x + w > s->cx1 ? s->cx1 : x + w, y1 = y + h > s->cy1 ? s->cy1 : y + h;
    for (int yy = y0; yy < y1; yy++)
    {
        color_t *p = &s->px[yy * SCR_W + x0];
        for (int xx = x0; xx < x1; xx++)
            *p++ = c;
    }
}

void gfx_blend(surface_t *s, int x, int y, int w, int h, color_t c, int alpha)
{
    int x0 = x < s->cx0 ? s->cx0 : x, y0 = y < s->cy0 ? s->cy0 : y;
    int x1 = x + w > s->cx1 ? s->cx1 : x + w, y1 = y + h > s->cy1 ? s->cy1 : y + h;
    for (int yy = y0; yy < y1; yy++)
    {
        color_t *p = &s->px[yy * SCR_W + x0];
        for (int xx = x0; xx < x1; xx++, p++)
            *p = gfx_mix(*p, c, alpha);
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

static void rrect_impl(surface_t *s, int x, int y, int w, int h, int r, color_t c, int alpha)
{
    if (w <= 0 || h <= 0)
        return;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    for (int yy = 0; yy < h; yy++)
    {
        int cy = -1;
        if (yy < r)
            cy = r - yy;           // distance rows above the inner area
        else if (yy >= h - r)
            cy = yy - (h - r) + 1;
        if (cy < 0)
        {
            if (alpha >= 256)
                gfx_fill(s, x, y + yy, w, 1, c);
            else
                gfx_blend(s, x, y + yy, w, 1, c, alpha);
            continue;
        }
        for (int xx = 0; xx < w; xx++)
        {
            int cx;
            if (xx < r)
                cx = r - xx;
            else if (xx >= w - r)
                cx = xx - (w - r) + 1;
            else
                cx = 0;
            int a;
            if (cx == 0)
                a = 256;
            else
                a = corner_cov(cx * 16 - 8, cy * 16 - 8, r);
            plot_a(s, x + xx, y + yy, c, a * alpha >> 8);
        }
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
    gfx_rrect_blend(s, x - 1, y + 1, w + 2, h + 2, r + 1, RGB(0, 20, 60), 18);
    gfx_rrect_blend(s, x, y + 2, w, h + 1, r, RGB(0, 20, 60), 22);
    gfx_rrect_blend(s, x + 1, y + 3, w - 2, h, r, RGB(0, 20, 60), 16);
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
    int i = 0;
    for (int yy = 0; yy < g->h; yy++)
    {
        for (int xx = 0; xx < g->w; xx++, i++)
        {
            int a = (i & 1) ? (d[i >> 1] >> 4) : (d[i >> 1] & 15);
            if (a)
                plot_a(s, x + g->x + xx, y + g->y + yy, c, a == 15 ? 256 : a * 17);
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
