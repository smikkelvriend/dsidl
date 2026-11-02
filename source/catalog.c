// DSiDL - minimal streaming JSON parser for Universal-DB full.json
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "catalog.h"

long catalog_error_offset = -1;

typedef struct {
    const char *p, *end;
    bool error;
} js_t;

static void ws(js_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t'))
        j->p++;
}

static bool expect(js_t *j, char c)
{
    ws(j);
    if (j->p < j->end && *j->p == c)
    {
        j->p++;
        return true;
    }
    j->error = true;
    return false;
}

static bool peek(js_t *j, char c)
{
    ws(j);
    return j->p < j->end && *j->p == c;
}

// Map a Unicode code point to something the DS console font can show
static char cp_to_ascii(unsigned cp)
{
    if (cp < 0x80)
        return (char)cp;
    if (cp >= 0xC0 && cp <= 0xC5) return 'A';
    if (cp >= 0xE0 && cp <= 0xE5) return 'a';
    if (cp >= 0xC8 && cp <= 0xCB) return 'E';
    if (cp >= 0xE8 && cp <= 0xEB) return 'e';
    if (cp >= 0xCC && cp <= 0xCF) return 'I';
    if (cp >= 0xEC && cp <= 0xEF) return 'i';
    if (cp >= 0xD2 && cp <= 0xD6) return 'O';
    if (cp >= 0xF2 && cp <= 0xF6) return 'o';
    if (cp >= 0xD9 && cp <= 0xDC) return 'U';
    if (cp >= 0xF9 && cp <= 0xFC) return 'u';
    if (cp == 0xE7) return 'c';
    if (cp == 0xF1) return 'n';
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x2013 || cp == 0x2014) return '-';
    return '?';
}

// Parses a string. If out != NULL, returns a malloc'ed ASCII copy.
static char *str(js_t *j, bool keep)
{
    ws(j);
    if (j->p >= j->end || *j->p != '"')
    {
        j->error = true;
        return NULL;
    }
    j->p++;

    const char *start = j->p;
    // Find the end first to size the buffer
    const char *q = start;
    while (q < j->end && *q != '"')
    {
        if (*q == '\\')
            q++;
        q++;
    }
    if (q >= j->end)
    {
        j->error = true;
        return NULL;
    }

    char *out = NULL, *o = NULL;
    if (keep)
    {
        out = malloc((q - start) + 1);
        o = out;
    }

    const char *s = start;
    while (s < q)
    {
        unsigned cp;
        if (*s == '\\')
        {
            s++;
            char e = *s++;
            switch (e)
            {
                case 'n': cp = '\n'; break;
                case 't': cp = ' '; break;
                case 'r': cp = 0; break;
                case 'b': case 'f': cp = 0; break;
                case 'u':
                {
                    char hex[5] = { 0 };
                    for (int i = 0; i < 4 && s < q; i++)
                        hex[i] = *s++;
                    cp = strtoul(hex, NULL, 16);
                    // Skip low surrogate of a pair
                    if (cp >= 0xD800 && cp <= 0xDBFF && s + 6 <= q && s[0] == '\\' && s[1] == 'u')
                    {
                        s += 6;
                        cp = '?';
                    }
                    break;
                }
                default: cp = (unsigned char)e; break;
            }
        }
        else
        {
            unsigned char c = (unsigned char)*s++;
            if (c < 0x80)
                cp = c;
            else if ((c & 0xE0) == 0xC0 && s < q)
                cp = ((c & 0x1F) << 6) | (*s++ & 0x3F);
            else if ((c & 0xF0) == 0xE0 && s + 1 < q)
            {
                cp = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
                s += 2;
            }
            else if ((c & 0xF8) == 0xF0 && s + 2 < q)
            {
                cp = '?';
                s += 3;
            }
            else
                cp = '?';
        }
        if (o && cp != 0)
            *o++ = cp_to_ascii(cp);
    }
    if (o)
        *o = '\0';

    j->p = q + 1;
    return out;
}

static void skip(js_t *j);

static void skip_container(js_t *j, char open, char close)
{
    expect(j, open);
    if (peek(j, close))
    {
        j->p++;
        return;
    }
    while (!j->error)
    {
        if (open == '{')
        {
            str(j, false);
            expect(j, ':');
        }
        skip(j);
        if (peek(j, ','))
        {
            j->p++;
            continue;
        }
        expect(j, close);
        return;
    }
}

static void skip(js_t *j)
{
    ws(j);
    if (j->p >= j->end)
    {
        j->error = true;
        return;
    }
    char c = *j->p;
    if (c == '"')
        str(j, false);
    else if (c == '{')
        skip_container(j, '{', '}');
    else if (c == '[')
        skip_container(j, '[', ']');
    else
    {
        // number, true, false, null
        while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' &&
               !isspace((unsigned char)*j->p))
            j->p++;
    }
}

static unsigned long number(js_t *j)
{
    ws(j);
    if (j->p < j->end && (isdigit((unsigned char)*j->p) || *j->p == '-'))
    {
        char *e;
        unsigned long v = strtoul(j->p, &e, 10);
        j->p = e;
        skip(j); // eat any fraction/exponent remainder
        return v;
    }
    skip(j);
    return 0;
}

static bool installable(const char *name)
{
    size_t n = strlen(name);
    if (n < 4)
        return false;
    return strcasecmp(name + n - 4, ".nds") == 0 || strcasecmp(name + n - 4, ".dsi") == 0;
}

static void entry_free(cat_entry_t *e)
{
    free(e->title);
    free(e->author);
    free(e->description);
    free(e->version);
    free(e->category);
    for (int i = 0; i < e->num_dl; i++)
    {
        free(e->dl[i].name);
        free(e->dl[i].url);
    }
    memset(e, 0, sizeof(*e));
}

static void parse_downloads(js_t *j, cat_entry_t *e)
{
    expect(j, '{');
    if (peek(j, '}'))
    {
        j->p++;
        return;
    }
    while (!j->error)
    {
        char *name = str(j, true);
        expect(j, ':');
        if (name && installable(name) && e->num_dl < MAX_DL && peek(j, '{'))
        {
            cat_download_t *d = &e->dl[e->num_dl];
            d->name = name;
            name = NULL;
            j->p++;
            if (!peek(j, '}'))
            {
                while (!j->error)
                {
                    char *k = str(j, true);
                    expect(j, ':');
                    if (k && strcmp(k, "url") == 0 && peek(j, '"'))
                        d->url = str(j, true);
                    else if (k && strcmp(k, "size") == 0)
                        d->size = number(j);
                    else
                        skip(j);
                    free(k);
                    if (peek(j, ','))
                    {
                        j->p++;
                        continue;
                    }
                    break;
                }
            }
            expect(j, '}');
            if (d->url)
                e->num_dl++;
            else
            {
                free(d->name);
                d->name = NULL;
            }
        }
        else
        {
            skip(j);
        }
        free(name);
        if (peek(j, ','))
        {
            j->p++;
            continue;
        }
        expect(j, '}');
        return;
    }
}

// Parses an array of strings. Returns first string (malloc'ed) if keep_first,
// and sets *has_ds if any element equals "DS".
static char *parse_str_array(js_t *j, bool keep_first, bool *has_ds)
{
    char *first = NULL;
    expect(j, '[');
    if (peek(j, ']'))
    {
        j->p++;
        return NULL;
    }
    while (!j->error)
    {
        char *s = str(j, true);
        if (s)
        {
            if (has_ds && strcasecmp(s, "DS") == 0)
                *has_ds = true;
            if (keep_first && !first)
                first = s;
            else
                free(s);
        }
        if (peek(j, ','))
        {
            j->p++;
            continue;
        }
        expect(j, ']');
        break;
    }
    return first;
}

static bool parse_entry(js_t *j, cat_entry_t *e)
{
    bool is_ds = false;
    memset(e, 0, sizeof(*e));

    expect(j, '{');
    if (peek(j, '}'))
    {
        j->p++;
        return false;
    }
    while (!j->error)
    {
        char *k = str(j, true);
        expect(j, ':');
        if (!k)
            break;

        bool is_str = peek(j, '"');
        if (strcmp(k, "title") == 0 && is_str)
            e->title = str(j, true);
        else if (strcmp(k, "author") == 0 && is_str)
            e->author = str(j, true);
        else if (strcmp(k, "description") == 0 && is_str)
            e->description = str(j, true);
        else if (strcmp(k, "version") == 0 && is_str)
            e->version = str(j, true);
        else if (strcmp(k, "categories") == 0 && peek(j, '['))
            e->category = parse_str_array(j, true, NULL);
        else if (strcmp(k, "systems") == 0 && peek(j, '['))
            parse_str_array(j, false, &is_ds);
        else if (strcmp(k, "downloads") == 0 && peek(j, '{'))
            parse_downloads(j, e);
        else
            skip(j);
        free(k);

        if (peek(j, ','))
        {
            j->p++;
            continue;
        }
        expect(j, '}');
        break;
    }

    if (j->error || !is_ds || e->num_dl == 0 || e->title == NULL)
    {
        entry_free(e);
        return false;
    }
    return true;
}

static int cmp_title(const void *a, const void *b)
{
    return strcasecmp(((const cat_entry_t *)a)->title, ((const cat_entry_t *)b)->title);
}

int catalog_parse(catalog_t *cat, const char *json, size_t len)
{
    js_t j = { json, json + len, false };
    int cap = 128;
    cat->entries = calloc(cap, sizeof(cat_entry_t));
    cat->count = 0;
    if (!cat->entries)
        return -1;

    if (!expect(&j, '['))
        return -1;
    if (peek(&j, ']'))
        return 0;

    while (!j.error)
    {
        cat_entry_t e;
        if (peek(&j, '{'))
        {
            if (parse_entry(&j, &e))
            {
                if (cat->count == cap)
                {
                    cap *= 2;
                    cat_entry_t *n = realloc(cat->entries, cap * sizeof(cat_entry_t));
                    if (!n)
                    {
                        entry_free(&e);
                        break;
                    }
                    cat->entries = n;
                }
                cat->entries[cat->count++] = e;
            }
        }
        else
        {
            skip(&j);
        }
        if (peek(&j, ','))
        {
            j.p++;
            continue;
        }
        break;
    }

    if (j.error)
        catalog_error_offset = (long)(j.p - json);
    if (cat->count == 0 && j.error)
        return -1;

    qsort(cat->entries, cat->count, sizeof(cat_entry_t), cmp_title);
    return cat->count;
}

void catalog_free(catalog_t *cat)
{
    for (int i = 0; i < cat->count; i++)
        entry_free(&cat->entries[i]);
    free(cat->entries);
    cat->entries = NULL;
    cat->count = 0;
}
