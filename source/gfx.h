// DSiDL - small software renderer for two 256x192 16-bit bitmap screens
#ifndef DSIDL_GFX_H
#define DSIDL_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "font.h"

#define SCR_W 256
#define SCR_H 192

typedef uint16_t color_t;

// 8-bit RGB to DS 15-bit color (bit 15 = opaque in bitmap mode)
#define RGB(r, g, b) ((color_t)((((r) >> 3) & 31) | ((((g) >> 3) & 31) << 5) | \
                                 ((((b) >> 3) & 31) << 10) | 0x8000))

typedef struct {
    color_t *px;   // SCR_W * SCR_H
    int cx0, cy0, cx1, cy1; // clip rectangle (exclusive end)
} surface_t;

extern surface_t scr_top, scr_bot;

void gfx_init(void);      // platform: set up video, allocate buffers
void gfx_present(void);   // platform: copy back buffers to the screens

void gfx_clip(surface_t *s, int x, int y, int w, int h);
void gfx_noclip(surface_t *s);

color_t gfx_mix(color_t a, color_t b, int alpha /* 0..256, amount of b */);

void gfx_fill(surface_t *s, int x, int y, int w, int h, color_t c);
void gfx_blend(surface_t *s, int x, int y, int w, int h, color_t c, int alpha);
void gfx_vgrad(surface_t *s, int x, int y, int w, int h, color_t top, color_t bottom);
void gfx_rrect(surface_t *s, int x, int y, int w, int h, int r, color_t c);
void gfx_rrect_blend(surface_t *s, int x, int y, int w, int h, int r, color_t c, int alpha);
void gfx_rrect_line(surface_t *s, int x, int y, int w, int h, int r, int t, color_t c);
void gfx_shadow(surface_t *s, int x, int y, int w, int h, int r);
void gfx_circle(surface_t *s, int cx, int cy, int r, color_t c);
void gfx_ring(surface_t *s, int cx, int cy, int r, int t, color_t c);
void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, int t, color_t c);

// Text. y is the top of the line box. Returns the width drawn.
int gfx_text(surface_t *s, int x, int y, const char *str, int font, color_t c);
int gfx_text_w(const char *str, int font);
int gfx_line_h(int font);
// Draws text cut to max_w with "..." when needed
int gfx_text_fit(surface_t *s, int x, int y, int max_w, const char *str, int font, color_t c);
// Centered horizontally around cx
void gfx_text_center(surface_t *s, int cx, int y, const char *str, int font, color_t c);
// Word-wrapped text. Returns the number of lines used.
int gfx_text_wrap(surface_t *s, int x, int y, int w, int max_lines, int line_h,
                  const char *str, int font, color_t c);

#endif
