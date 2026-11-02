// DSiDL - images
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define STBI_WRITE_NO_STDIO_FALLBACK
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "img.h"

#if defined(__arm__) && defined(__thumb__)
#pragma GCC target("arm")
#endif

uint8_t *img_resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst)
        return NULL;
    // Box filter when shrinking, nearest when growing
    for (int y = 0; y < dh; y++)
    {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++)
        {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0)
                x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++)
                {
                    const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
                    // premultiply so transparent pixels don't darken edges
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    n++;
                }
            uint8_t *d = dst + ((size_t)y * dw + x) * 4;
            if (a)
            {
                d[0] = r / a;
                d[1] = g / a;
                d[2] = b / a;
            }
            else
                d[0] = d[1] = d[2] = 0;
            d[3] = n ? a / n : 0;
        }
    }
    return dst;
}

static bool from_rgba(const uint8_t *rgba, int w, int h, img_t *out)
{
    out->w = w;
    out->h = h;
    out->px = malloc((size_t)w * h * 2);
    out->a = malloc((size_t)w * h);
    if (!out->px || !out->a)
    {
        img_free(out);
        return false;
    }
    bool opaque = true;
    for (int i = 0; i < w * h; i++)
    {
        const uint8_t *p = rgba + i * 4;
        out->px[i] = RGB(p[0], p[1], p[2]);
        out->a[i] = p[3];
        if (p[3] != 255)
            opaque = false;
    }
    if (opaque)
    {
        free(out->a);
        out->a = NULL;
    }
    return true;
}

static bool finish(uint8_t *rgba, int w, int h, img_t *out, int max_w, int max_h)
{
    if (!rgba)
        return false;
    int dw = w, dh = h;
    if (dw > max_w || dh > max_h)
    {
        if ((long)w * max_h > (long)h * max_w)
        {
            dw = max_w;
            dh = h * max_w / w;
        }
        else
        {
            dh = max_h;
            dw = w * max_h / h;
        }
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        uint8_t *sc = img_resize_rgba(rgba, w, h, dw, dh);
        stbi_image_free(rgba);
        if (!sc)
            return false;
        bool ok = from_rgba(sc, dw, dh, out);
        free(sc);
        return ok;
    }
    bool ok = from_rgba(rgba, w, h, out);
    stbi_image_free(rgba);
    return ok;
}

bool img_decode(const void *data, size_t len, img_t *out, int max_w, int max_h)
{
    memset(out, 0, sizeof(*out));
    int w, h, n;
    uint8_t *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    return finish(rgba, w, h, out, max_w, max_h);
}

bool img_load(const char *path, img_t *out, int max_w, int max_h)
{
    memset(out, 0, sizeof(*out));
    int w, h, n;
    uint8_t *rgba = stbi_load(path, &w, &h, &n, 4);
    return finish(rgba, w, h, out, max_w, max_h);
}

bool img_info(const char *path, int *w, int *h)
{
    int n;
    return stbi_info(path, w, h, &n) != 0;
}

uint8_t *img_load_rgba(const char *path, int *w, int *h)
{
    int n;
    return stbi_load(path, w, h, &n, 4);
}

bool img_scaled(const img_t *src, img_t *out, int w, int h)
{
    memset(out, 0, sizeof(*out));
    uint8_t *rgba = malloc((size_t)src->w * src->h * 4);
    if (!rgba)
        return false;
    for (int i = 0; i < src->w * src->h; i++)
    {
        color_t c = src->px[i];
        rgba[i * 4 + 0] = (c & 31) * 255 / 31;
        rgba[i * 4 + 1] = ((c >> 5) & 31) * 255 / 31;
        rgba[i * 4 + 2] = ((c >> 10) & 31) * 255 / 31;
        rgba[i * 4 + 3] = src->a ? src->a[i] : 255;
    }
    uint8_t *sc = img_resize_rgba(rgba, src->w, src->h, w, h);
    free(rgba);
    if (!sc)
        return false;
    bool ok = from_rgba(sc, w, h, out);
    free(sc);
    return ok;
}

void img_free(img_t *img)
{
    free(img->px);
    free(img->a);
    img->px = NULL;
    img->a = NULL;
    img->w = img->h = 0;
}

void gfx_blit(surface_t *s, const img_t *img, int x, int y)
{
    if (!img || !img->px)
        return;
    for (int yy = 0; yy < img->h; yy++)
    {
        int sy = y + yy;
        if (sy < s->cy0 || sy >= s->cy1)
            continue;
        for (int xx = 0; xx < img->w; xx++)
        {
            int sx = x + xx;
            if (sx < s->cx0 || sx >= s->cx1)
                continue;
            int i = yy * img->w + xx;
            color_t *p = &s->px[sy * SCR_W + sx];
            int a = img->a ? img->a[i] : 255;
            if (a >= 250)
                *p = img->px[i];
            else if (a > 4)
                *p = gfx_mix(*p, img->px[i], a);
        }
    }
}

void gfx_blit_round(surface_t *s, const img_t *img, int x, int y, int r)
{
    if (!img || !img->px)
        return;
    for (int yy = 0; yy < img->h; yy++)
    {
        int sy = y + yy;
        if (sy < s->cy0 || sy >= s->cy1)
            continue;
        int dy = yy < r ? r - yy : (yy >= img->h - r ? yy - (img->h - r) + 1 : 0);
        for (int xx = 0; xx < img->w; xx++)
        {
            int sx = x + xx;
            if (sx < s->cx0 || sx >= s->cx1)
                continue;
            int dx = xx < r ? r - xx : (xx >= img->w - r ? xx - (img->w - r) + 1 : 0);
            int a = img->a ? img->a[yy * img->w + xx] : 255;
            if (dx && dy)
            {
                // outside the rounded corner?
                int d2 = (dx * 2 - 1) * (dx * 2 - 1) + (dy * 2 - 1) * (dy * 2 - 1);
                int r2 = r * 2 * r * 2;
                if (d2 > r2)
                    continue;
                if (d2 > r2 - 4 * r)
                    a = a / 2;
            }
            color_t *p = &s->px[sy * SCR_W + sx];
            int i = yy * img->w + xx;
            if (a >= 250)
                *p = img->px[i];
            else if (a > 4)
                *p = gfx_mix(*p, img->px[i], a);
        }
    }
}

bool img_write_png(const char *path, int w, int h, int comp, const void *data)
{
    return stbi_write_png(path, w, h, comp, data, w * comp) != 0;
}
