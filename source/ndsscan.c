// DSiDL - .nds/.dsi scanner and banner reader
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <nds.h>

#include "ndsscan.h"

static const char *skip_dirs[] = {
    "_nds", "_dsidl", "DCIM", "private", "title", "shared1", "shared2", "sys",
    "photo", "ticket", "import", "tmp", "progress", NULL
};

static bool is_prog(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (strcasecmp(dot, ".nds") == 0 || strcasecmp(dot, ".dsi") == 0 ||
                   strcasecmp(dot, ".srl") == 0);
}

static int cmp_items(const void *a, const void *b)
{
    const ndsfile_t *x = a, *y = b;
    return strcasecmp(x->name, y->name);
}

static void walk(ndslist_t *l, const char *dir, int depth, int max, int *cap)
{
    if (depth > 5 || l->count >= max)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    int seen = 0;
    while ((de = readdir(d)) != NULL && l->count < max)
    {
        if (de->d_name[0] == '.')
            continue;
        if ((++seen & 31) == 0)
            cothread_yield();
        char path[512];
        size_t dl = strlen(dir);
        snprintf(path, sizeof(path), "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", de->d_name);
        if (de->d_type == DT_DIR)
        {
            bool skip = false;
            if (depth == 0)
                for (int i = 0; skip_dirs[i]; i++)
                    if (strcasecmp(de->d_name, skip_dirs[i]) == 0)
                        skip = true;
            if (!skip)
                walk(l, path, depth + 1, max, cap);
            continue;
        }
        if (!is_prog(de->d_name))
            continue;
        if (l->count == *cap)
        {
            *cap = *cap ? *cap * 2 : 64;
            ndsfile_t *n = realloc(l->items, sizeof(ndsfile_t) * *cap);
            if (!n)
                break;
            l->items = n;
        }
        ndsfile_t *f = &l->items[l->count];
        memset(f, 0, sizeof(*f));
        f->path = strdup(path);
        f->name = strdup(de->d_name);
        struct stat st;
        f->size = stat(path, &st) == 0 ? (unsigned long)st.st_size : 0;
        snprintf(f->title, sizeof(f->title), "%s", de->d_name);
        if (f->path && f->name)
            l->count++;
        else
        {
            free(f->path);
            free(f->name);
        }
    }
    closedir(d);
}

int nds_scan(ndslist_t *list, const char *drive, int max)
{
    memset(list, 0, sizeof(*list));
    int cap = 0;
    char root[16];
    snprintf(root, sizeof(root), "%s/", drive);
    walk(list, root, 0, max, &cap);
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(ndsfile_t), cmp_items);
    return list->count;
}

void nds_free(ndslist_t *list)
{
    for (int i = 0; i < list->count; i++)
    {
        free(list->items[i].path);
        free(list->items[i].name);
        img_free(&list->items[i].icon);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

// UTF-16LE banner text -> UTF-8, lines split by '\n'
static void utf16_line(const uint8_t *src, int max_chars, int line, char *out, size_t outlen)
{
    int cur = 0;
    size_t o = 0;
    for (int i = 0; i < max_chars && o + 4 < outlen; i++)
    {
        unsigned c = src[i * 2] | (src[i * 2 + 1] << 8);
        if (c == 0)
            break;
        if (c == '\n')
        {
            if (cur == line)
                break;
            cur++;
            continue;
        }
        if (cur != line)
            continue;
        if (c < 0x80)
            out[o++] = (char)c;
        else if (c < 0x800)
        {
            out[o++] = 0xC0 | (c >> 6);
            out[o++] = 0x80 | (c & 0x3F);
        }
        else
        {
            out[o++] = 0xE0 | (c >> 12);
            out[o++] = 0x80 | ((c >> 6) & 0x3F);
            out[o++] = 0x80 | (c & 0x3F);
        }
    }
    out[o] = '\0';
}

bool nds_read_banner(ndsfile_t *f)
{
    if (f->icon_read)
        return f->icon.px != NULL;
    f->icon_read = true;

    FILE *fp = fopen(f->path, "rb");
    if (!fp)
        return false;
    uint8_t hdr[0x70];
    bool ok = false;
    if (fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr))
    {
        for (int i = 0; i < 4; i++)
            f->code[i] = isalnum(hdr[0x0C + i]) ? hdr[0x0C + i] : 0;
        f->code[4] = 0;
        uint32_t boff = hdr[0x68] | (hdr[0x69] << 8) | (hdr[0x6A] << 16) | ((uint32_t)hdr[0x6B] << 24);
        uint8_t *b = malloc(0x840);
        if (boff && b && fseek(fp, boff, SEEK_SET) == 0 && fread(b, 1, 0x840, fp) == 0x840)
        {
            // English title (offset 0x340), fall back to Japanese (0x240)
            char t[96], l2[96], l3[96];
            int base = 0x340;
            utf16_line(b + base, 128, 0, t, sizeof(t));
            if (!t[0])
            {
                base = 0x240;
                utf16_line(b + base, 128, 0, t, sizeof(t));
            }
            utf16_line(b + base, 128, 1, l2, sizeof(l2));
            utf16_line(b + base, 128, 2, l3, sizeof(l3));
            if (t[0])
                snprintf(f->title, sizeof(f->title), "%s", t);
            if (l3[0])
                snprintf(f->sub, sizeof(f->sub), "%s - %s", l2, l3);
            else
                snprintf(f->sub, sizeof(f->sub), "%s", l2);

            // 32x32 icon: 4bpp, 8x8 tiles, palette at 0x220 (index 0 = transparent)
            img_t *im = &f->icon;
            im->w = im->h = 32;
            im->px = malloc(32 * 32 * 2);
            im->a = malloc(32 * 32);
            if (im->px && im->a)
            {
                const uint8_t *bits = b + 0x20;
                const uint8_t *pal = b + 0x220;
                for (int ty = 0; ty < 4; ty++)
                    for (int tx = 0; tx < 4; tx++)
                        for (int y = 0; y < 8; y++)
                            for (int x = 0; x < 8; x++)
                            {
                                int byte = bits[(ty * 4 + tx) * 32 + y * 4 + x / 2];
                                int idx = x & 1 ? byte >> 4 : byte & 15;
                                int px = (ty * 8 + y) * 32 + tx * 8 + x;
                                uint16_t c = pal[idx * 2] | (pal[idx * 2 + 1] << 8);
                                im->px[px] = c | 0x8000;
                                im->a[px] = idx ? 255 : 0;
                            }
                ok = true;
            }
            else
                img_free(im);
        }
        free(b);
    }
    fclose(fp);
    return ok;
}

bool nds_has_name(const ndslist_t *list, const char *name)
{
    for (int i = 0; i < list->count; i++)
        if (strcasecmp(list->items[i].name, name) == 0)
            return true;
    return false;
}
