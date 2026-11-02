// DSiDL - images (PNG/JPG via stb_image), scaled for the DS screens
#ifndef DSIDL_IMG_H
#define DSIDL_IMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

typedef struct {
    int w, h;
    color_t *px;
    uint8_t *a;   // alpha 0..255 (NULL = opaque)
} img_t;

// Decodes and scales down to fit max_w x max_h (keeps aspect ratio)
bool img_decode(const void *data, size_t len, img_t *out, int max_w, int max_h);
bool img_load(const char *path, img_t *out, int max_w, int max_h);
// Scaled copy (exact size, may change aspect)
bool img_scaled(const img_t *src, img_t *out, int w, int h);
void img_free(img_t *img);
// Reads image size without decoding
bool img_info(const char *path, int *w, int *h);

void gfx_blit(surface_t *s, const img_t *img, int x, int y);
// Rounded-corner blit (for icons)
void gfx_blit_round(surface_t *s, const img_t *img, int x, int y, int r);

// Writes an RGB(A) buffer as PNG (for boxart)
bool img_write_png(const char *path, int w, int h, int comp, const void *data);
// Loads raw RGBA pixels (caller frees with free())
uint8_t *img_load_rgba(const char *path, int *w, int *h);
uint8_t *img_resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh);

#endif
