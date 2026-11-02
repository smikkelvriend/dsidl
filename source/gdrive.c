// DSiDL - Google Drive folders shared with "anyone with the link"
//
// Listing uses https://drive.google.com/embeddedfolderview?id=<folder>,
// a light HTML page Google provides for embedding folders (no login or API
// key needed). Files are downloaded from drive.usercontent.google.com.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gdrive.h"

static bool id_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_';
}

static void copy_id(const char *p, char *out, size_t len)
{
    size_t n = 0;
    while (id_char(p[n]) && n + 1 < len)
    {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
}

bool gd_parse_link(const char *link, char *id, size_t idlen, char *rkey, size_t rklen)
{
    id[0] = '\0';
    rkey[0] = '\0';
    const char *p;
    if ((p = strstr(link, "/folders/")))
        copy_id(p + 9, id, idlen);
    else if ((p = strstr(link, "id=")))
        copy_id(p + 3, id, idlen);
    else
    {
        while (*link == ' ')
            link++;
        copy_id(link, id, idlen);
    }
    if ((p = strstr(link, "resourcekey=")))
        copy_id(p + 12, rkey, rklen);
    return strlen(id) >= 10;
}

void gd_folder_url(const char *id, const char *rkey, char *out, size_t len)
{
    if (rkey && rkey[0])
        snprintf(out, len, "https://drive.google.com/embeddedfolderview?id=%s&resourcekey=%s", id, rkey);
    else
        snprintf(out, len, "https://drive.google.com/embeddedfolderview?id=%s", id);
}

void gd_file_url(const char *id, char *out, size_t len)
{
    // confirm=t skips the "can't scan for viruses" page of big files
    snprintf(out, len, "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t", id);
}

// Decodes &amp; &#39; &quot; &lt; &gt; and numeric entities (to UTF-8)
static void unescape(char *s)
{
    char *o = s;
    while (*s)
    {
        if (*s == '&')
        {
            char *semi = strchr(s, ';');
            if (semi && semi - s < 10)
            {
                unsigned cp = 0;
                if (s[1] == '#')
                    cp = (s[2] == 'x' || s[2] == 'X') ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
                else if (!strncmp(s, "&amp;", 5)) cp = '&';
                else if (!strncmp(s, "&quot;", 6)) cp = '"';
                else if (!strncmp(s, "&lt;", 4)) cp = '<';
                else if (!strncmp(s, "&gt;", 4)) cp = '>';
                else if (!strncmp(s, "&apos;", 6)) cp = '\'';
                if (cp)
                {
                    if (cp < 0x80)
                        *o++ = cp;
                    else if (cp < 0x800)
                    {
                        *o++ = 0xC0 | (cp >> 6);
                        *o++ = 0x80 | (cp & 0x3F);
                    }
                    else
                    {
                        *o++ = 0xE0 | ((cp >> 12) & 0x0F);
                        *o++ = 0x80 | ((cp >> 6) & 0x3F);
                        *o++ = 0x80 | (cp & 0x3F);
                    }
                    s = semi + 1;
                    continue;
                }
            }
        }
        *o++ = *s++;
    }
    *o = '\0';
}

static int cmp_entry(const void *a, const void *b)
{
    const gd_entry_t *x = a, *y = b;
    if (x->folder != y->folder)
        return x->folder ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

int gd_parse_listing(const char *html, gd_entry_t **out)
{
    *out = NULL;
    int n = 0, cap = 0;
    const char *p = html;
    while ((p = strstr(p, "id=\"entry-")) != NULL)
    {
        p += 10;
        const char *next = strstr(p, "id=\"entry-");
        size_t blen = next ? (size_t)(next - p) : strlen(p);

        gd_entry_t e = { 0 };
        copy_id(p, e.id, sizeof(e.id));

        // Look inside this entry's block only
        char *blk = malloc(blen + 1);
        if (!blk)
            break;
        memcpy(blk, p, blen);
        blk[blen] = '\0';
        e.folder = strstr(blk, "/folders/") != NULL;
        const char *t = strstr(blk, "flip-entry-title\">");
        if (t)
        {
            t += 18;
            size_t k = 0;
            while (t[k] && t[k] != '<' && k + 1 < sizeof(e.name))
            {
                e.name[k] = t[k];
                k++;
            }
            e.name[k] = '\0';
            unescape(e.name);
        }
        free(blk);
        if (!e.name[0])
            snprintf(e.name, sizeof(e.name), "%s", e.id);

        if (e.id[0])
        {
            if (n == cap)
            {
                cap = cap ? cap * 2 : 32;
                gd_entry_t *nw = realloc(*out, sizeof(gd_entry_t) * cap);
                if (!nw)
                    break;
                *out = nw;
            }
            (*out)[n++] = e;
        }
        p = next ? next : p + blen;
    }
    if (n > 1)
        qsort(*out, n, sizeof(gd_entry_t), cmp_entry);
    return n;
}
