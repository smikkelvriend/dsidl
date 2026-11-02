// DSiDL - tiny HTTP server: phone opens http://<dsi-ip>/ and uploads files
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fat.h>

#include <nds.h>
#include <dswifi9.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "server.h"
#include "util.h"
#include <stdint.h>

extern char drive[8];

// Web page (web/index.html, embedded at build time)
#include "webui_bin.h"
#include "unrarjs_bin.h"
#include "unrarwasm_bin.h"

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    int idle = 0;
    while (len > 0)
    {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { 0, 0 }; // poll, see recv_wait()
        int s = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (s < 0)
            return false;
        if (s == 0)
        {
            cothread_yield_irq(IRQ_VBLANK);
            idle += 17;
            if (idle > 20000)
                return false;
            continue;
        }
        int r = send(fd, p, len, 0);
        if (r <= 0)
            return false;
        idle = 0;
        p += r;
        len -= r;
    }
    return true;
}

// recv() that never blocks for long: waits in small steps with select() and
// yields to the UI thread in between. Gives up after idle_ms without data or
// when the user stops the server. Returns like recv().
static int recv_wait(int fd, void *buf, size_t len, server_state_t *st, int idle_ms)
{
    int waited = 0;
    while (!st->stop)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        // Poll only: dswifi doesn't honour select() timeouts while the socket
        // is idle, so a real timeout would block forever (and ignore B).
        struct timeval tv = { 0, 0 };
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0)
            return recv(fd, buf, len, 0);
        if (r < 0)
            return -1;
        cothread_yield_irq(IRQ_VBLANK);
        waited += 17;
        if (waited >= idle_ms)
            return -1;
    }
    return -1;
}

static void reply(int fd, int code, const char *ctype, const char *body, size_t len)
{
    char hdr[256];
    const char *msg = code == 200 ? "OK" : code == 404 ? "Not Found" :
                      code == 400 ? "Bad Request" : code == 403 ? "Forbidden" :
                      code == 409 ? "Conflict" : "Internal Server Error";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                     code, msg, ctype, (unsigned)len);
    send_all(fd, hdr, n);
    send_all(fd, body, len);
}

static void reply_text(int fd, int code, const char *text)
{
    reply(fd, code, "text/plain; charset=utf-8", text, strlen(text));
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes %XX and '+' in place
static void url_decode(char *s)
{
    char *o = s;
    while (*s)
    {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0)
        {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 3;
        }
        else if (*s == '+')
        {
            *o++ = ' ';
            s++;
        }
        else
        {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

// Extracts a query parameter value (decoded) into out
static bool query_param(const char *query, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p)
    {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            p += klen + 1;
            size_t n = strcspn(p, "& ");
            if (n >= outlen)
                n = outlen - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return false;
}

// Makes a directory path safe: no "..", only forward slashes, starts with '/'
static bool sanitize_dir(const char *in, char *out, size_t outlen)
{
    if (strstr(in, ".."))
        return false;
    size_t o = 0;
    if (in[0] != '/')
        out[o++] = '/';
    for (const char *p = in; *p && o < outlen - 1; p++)
    {
        char c = *p == '\\' ? '/' : *p;
        if (strchr("<>:\"|?*", c) || (unsigned char)c < 32)
            continue;
        if (c == '/' && o > 0 && out[o - 1] == '/')
            continue;
        out[o++] = c;
    }
    while (o > 1 && out[o - 1] == '/')
        o--;
    out[o] = '\0';
    return true;
}

static void handle_upload(int fd, server_state_t *st, const char *query,
                          unsigned long content_length,
                          const char *body0, size_t body0len)
{
    char name[128], rawdir[128], dir[128];
    if (!query_param(query, "name", name, sizeof(name)))
    {
        reply_text(fd, 400, "Bestandsnaam ontbreekt");
        return;
    }
    if (!query_param(query, "dir", rawdir, sizeof(rawdir)))
        strcpy(rawdir, "/roms/nds");

    sanitize_filename(name);
    if (name[0] == '\0' || !sanitize_dir(rawdir, dir, sizeof(dir)))
    {
        reply_text(fd, 400, "Ongeldige naam of map");
        return;
    }

    char sddir[160], path[300], tmp[310];
    snprintf(sddir, sizeof(sddir), "%s%s", drive, strcmp(dir, "/") == 0 ? "" : dir);
    mkdir_p(sddir);
    snprintf(path, sizeof(path), "%s/%s", sddir, name);
    snprintf(tmp, sizeof(tmp), "%s.part", path);

    FILE *f = fopen(tmp, "wb");
    if (!f)
    {
        reply_text(fd, 500, "Kan bestand niet aanmaken op SD");
        return;
    }
    setvbuf(f, NULL, _IOFBF, 32 * 1024);

    snprintf(st->current, sizeof(st->current), "%s", name);
    st->total = content_length;
    st->done = 0;
    st->receiving = true;

    bool ok = true;
    unsigned long got = 0;
    if (body0len > 0)
    {
        ok = fwrite(body0, 1, body0len, f) == body0len;
        got = body0len;
    }

    char *buf = malloc(16 * 1024);
    if (!buf)
        ok = false;

    while (ok && got < content_length)
    {
        if (st->stop)
        {
            ok = false;
            break;
        }
        size_t want = content_length - got;
        if (want > 16 * 1024)
            want = 16 * 1024;
        int r = recv_wait(fd, buf, want, st, 20000);
        if (r <= 0)
        {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r)
        {
            ok = false;
            break;
        }
        got += r;
        st->done = got;
        cothread_yield(); // keep the screen and buttons responsive
    }
    free(buf);

    if (fclose(f) != 0)
        ok = false;
    st->receiving = false;

    if (!ok)
    {
        remove(tmp);
        snprintf(st->last_msg, sizeof(st->last_msg), "Mislukt: %.70s", name);
        reply_text(fd, 500, "Ontvangen of opslaan mislukt");
        return;
    }

    remove(path); // replace existing file
    if (rename(tmp, path) != 0)
    {
        remove(tmp);
        reply_text(fd, 500, "Hernoemen mislukt");
        return;
    }

    st->files_ok++;
    snprintf(st->last_msg, sizeof(st->last_msg), "Ontvangen: %.70s", name);
    reply_text(fd, 200, "OK");
}


// ---------------------------------------------------------------------------
// File browser API

// Converts a web path ("/roms/nds/x.nds") into a full path ("sd:/roms/nds/x.nds")
static bool resolve(const char *query, const char *key, char *out, size_t outlen,
                    char *clean, size_t cleanlen)
{
    char raw[256];
    if (!query_param(query, key, raw, sizeof(raw)))
        return false;
    if (!sanitize_dir(raw, clean, cleanlen))
        return false;
    snprintf(out, outlen, "%s%s", drive, clean);
    return true;
}

typedef struct {
    char *p;
    size_t len, cap;
} sbuf_t;

static bool sb_add(sbuf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np)
            return false;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return true;
}

static void sb_str(sbuf_t *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static void sb_json_str(sbuf_t *b, const char *s)
{
    sb_add(b, "\"", 1);
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            char e[2] = { '\\', (char)c };
            sb_add(b, e, 2);
        }
        else if (c < 32)
        {
            char e[8];
            snprintf(e, sizeof(e), "\\u%04x", c);
            sb_str(b, e);
        }
        else
        {
            sb_add(b, (const char *)&c, 1);
        }
    }
    sb_add(b, "\"", 1);
}

static void api_list(int fd, server_state_t *st, const char *query)
{
    char full[300], clean[256];
    if (!resolve(query, "path", full, sizeof(full), clean, sizeof(clean)))
    {
        reply_text(fd, 400, "Ongeldig pad");
        return;
    }
    // opendir() on a drive root wants the trailing slash
    if (strcmp(clean, "/") == 0)
        snprintf(full, sizeof(full), "%s/", drive);

    DIR *d = opendir(full);
    if (!d)
    {
        reply_text(fd, 404, "Map niet gevonden");
        return;
    }

    sbuf_t b = { 0 };
    sb_str(&b, "[");
    bool first = true;
    struct dirent *de;
    int n = 0;
    while ((de = readdir(d)) != NULL)
    {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char path[560];
        snprintf(path, sizeof(path), "%s%s%s", drive, strcmp(clean, "/") == 0 ? "/" : clean,
                 strcmp(clean, "/") == 0 ? "" : "/");
        size_t pl = strlen(path);
        snprintf(path + pl, sizeof(path) - pl, "%s", de->d_name);

        bool is_dir = de->d_type == DT_DIR;
        unsigned long size = 0;
        if (!is_dir)
        {
            struct stat stt;
            if (stat(path, &stt) == 0)
                size = stt.st_size;
        }
        int attr = FAT_getAttr(path);
        bool hidden = attr >= 0 && (attr & ATTR_HIDDEN);

        char num[48];
        if (!first)
            sb_str(&b, ",");
        first = false;
        sb_str(&b, "{\"n\":");
        sb_json_str(&b, de->d_name);
        snprintf(num, sizeof(num), ",\"d\":%d,\"s\":%lu,\"h\":%d}", is_dir, size, hidden);
        sb_str(&b, num);

        if ((++n % 16) == 0)
            cothread_yield();
    }
    closedir(d);
    sb_str(&b, "]");

    if (!b.p)
        reply_text(fd, 500, "Geen geheugen");
    else
        reply(fd, 200, "application/json; charset=utf-8", b.p, b.len);
    free(b.p);
    (void)st;
}

static void api_download(int fd, server_state_t *st, const char *query)
{
    char full[300], clean[256];
    if (!resolve(query, "path", full, sizeof(full), clean, sizeof(clean)))
    {
        reply_text(fd, 400, "Ongeldig pad");
        return;
    }
    struct stat stt;
    FILE *f = fopen(full, "rb");
    if (!f || stat(full, &stt) != 0)
    {
        if (f)
            fclose(f);
        reply_text(fd, 404, "Bestand niet gevonden");
        return;
    }

    const char *name = strrchr(clean, '/');
    name = name ? name + 1 : clean;
    char safe[128];
    snprintf(safe, sizeof(safe), "%s", name);
    for (char *c = safe; *c; c++)
        if (*c == '"' || (unsigned char)*c < 32)
            *c = '_';

    char inl[4] = "0";
    query_param(query, "inline", inl, sizeof(inl));
    const char *ctype = "application/octet-stream";
    const char *dot = strrchr(safe, '.');
    if (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")))
        ctype = "image/jpeg";
    else if (dot && !strcasecmp(dot, ".png"))
        ctype = "image/png";
    else if (dot && !strcasecmp(dot, ".bmp"))
        ctype = "image/bmp";
    else if (dot && !strcasecmp(dot, ".gif"))
        ctype = "image/gif";

    char hdr[400];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Content-Disposition: %s; filename=\"%s\"\r\n"
                     "Cache-Control: max-age=3600\r\n"
                     "Connection: close\r\n\r\n", inl[0] == '1' ? ctype : "application/octet-stream",
                     (unsigned long)stt.st_size, inl[0] == '1' ? "inline" : "attachment", safe);
    if (!send_all(fd, hdr, n))
    {
        fclose(f);
        return;
    }

    snprintf(st->current, sizeof(st->current), "%s", safe);
    st->total = stt.st_size;
    st->done = 0;
    st->sending = true;

    char *buf = malloc(16 * 1024);
    while (buf && !st->stop)
    {
        size_t r = fread(buf, 1, 16 * 1024, f);
        if (r == 0)
            break;
        if (!send_all(fd, buf, r))
            break;
        st->done += r;
        cothread_yield();
    }
    free(buf);
    fclose(f);
    st->sending = false;
    if (st->done == st->total)
        snprintf(st->last_msg, sizeof(st->last_msg), "Verstuurd: %.70s", safe);
}

// Copies one file, updating progress. Returns true on success.
static bool copy_file(const char *src, const char *dst, server_state_t *st)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out)
    {
        fclose(in);
        return false;
    }
    setvbuf(out, NULL, _IOFBF, 32 * 1024);

    struct stat stt;
    st->total = stat(src, &stt) == 0 ? (unsigned long)stt.st_size : 0;
    st->done = 0;
    const char *nm = strrchr(src, '/');
    snprintf(st->current, sizeof(st->current), "%s", nm ? nm + 1 : src);

    bool ok = true;
    char *buf = malloc(32 * 1024);
    if (!buf)
        ok = false;
    while (ok && !st->stop)
    {
        size_t r = fread(buf, 1, 32 * 1024, in);
        if (r == 0)
            break;
        if (fwrite(buf, 1, r, out) != r)
            ok = false;
        st->done += r;
        cothread_yield();
    }
    if (st->stop)
        ok = false;
    free(buf);
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    if (!ok)
        remove(dst);
    return ok;
}

// Recursively copies a file or folder
static bool copy_any(const char *src, const char *dst, server_state_t *st, int depth)
{
    struct stat stt;
    if (stat(src, &stt) != 0 || depth > 16)
        return false;
    if (!S_ISDIR(stt.st_mode))
        return copy_file(src, dst, st);

    if (mkdir(dst, 0777) != 0 && !file_exists(dst))
        return false;
    DIR *d = opendir(src);
    if (!d)
        return false;
    bool ok = true;
    struct dirent *de;
    while (ok && (de = readdir(d)) != NULL)
    {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char *s2 = malloc(600), *d2 = malloc(600);
        if (!s2 || !d2)
            ok = false;
        else
        {
            snprintf(s2, 600, "%s/%s", src, de->d_name);
            snprintf(d2, 600, "%s/%s", dst, de->d_name);
            ok = copy_any(s2, d2, st, depth + 1);
        }
        free(s2);
        free(d2);
    }
    closedir(d);
    return ok;
}

// Picks "name.ext", "name (2).ext", ... that doesn't exist yet in dir
static void unique_path(const char *dir, const char *name, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s/%s", dir, name);
    if (!file_exists(out))
        return;
    char base[160], ext[32] = "";
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base)
    {
        snprintf(ext, sizeof(ext), "%s", dot);
        *dot = '\0';
    }
    for (int i = 2; i < 1000; i++)
    {
        snprintf(out, outlen, "%s/%s (%d)%s", dir, base, i, ext);
        if (!file_exists(out))
            return;
    }
}

static void api_copy_move(int fd, server_state_t *st, bool move, const char *query)
{
    char full[300], clean[256], dfull[300], dclean[256];
    if (!resolve(query, "path", full, sizeof(full), clean, sizeof(clean)) ||
        !resolve(query, "to", dfull, sizeof(dfull), dclean, sizeof(dclean)) ||
        strcmp(clean, "/") == 0)
    {
        reply_text(fd, 400, "Ongeldig pad");
        return;
    }
    if (strcmp(dclean, "/") == 0)
        snprintf(dfull, sizeof(dfull), "%s", drive);

    bool protect = strcasecmp(clean, "/_nds") == 0 || strcasecmp(clean, "/_dsidl") == 0;
    if (move && protect)
    {
        reply_text(fd, 403, "Deze map is nodig voor je DSi-software en wordt niet verplaatst");
        return;
    }

    // Not into itself or one of its own sub folders
    size_t cl = strlen(clean);
    if (strncmp(dclean, clean, cl) == 0 && (dclean[cl] == '\0' || dclean[cl] == '/'))
    {
        reply_text(fd, 400, "Kan een map niet in zichzelf plaatsen");
        return;
    }

    struct stat stt;
    if (stat(full, &stt) != 0)
    {
        reply_text(fd, 404, "Niet gevonden");
        return;
    }

    const char *name = strrchr(clean, '/');
    name = name ? name + 1 : clean;

    // Source folder of the item
    char srcdir[256];
    snprintf(srcdir, sizeof(srcdir), "%s", clean);
    char *sl = strrchr(srcdir, '/');
    if (sl == srcdir)
        strcpy(srcdir, "/");
    else if (sl)
        *sl = '\0';
    if (move && strcmp(srcdir, dclean) == 0)
    {
        reply_text(fd, 400, "Het staat al in deze map");
        return;
    }

    char dst[420];
    unique_path(dfull, name, dst, sizeof(dst));

    if (move)
    {
        if (rename(full, dst) != 0)
        {
            reply_text(fd, 500, "Verplaatsen mislukt");
            return;
        }
        snprintf(st->last_msg, sizeof(st->last_msg), "Verplaatst: %.68s", name);
        reply_text(fd, 200, "OK");
        return;
    }

    st->receiving = true;
    bool ok = copy_any(full, dst, st, 0);
    st->receiving = false;
    if (!ok)
    {
        reply_text(fd, 500, st->stop ? "Gestopt" : "Kopiëren mislukt (SD vol?)");
        return;
    }
    snprintf(st->last_msg, sizeof(st->last_msg), "Gekopieerd: %.66s", name);
    reply_text(fd, 200, "OK");
}

// ---------------------------------------------------------------------------
// Storage info, photo list, inline files and zip downloads

static void api_info(int fd)
{
    unsigned long long fr = 0, tot = 0;
    disk_space(drive, &fr, &tot);
    char b[96];
    snprintf(b, sizeof(b), "{\"free\":%llu,\"total\":%llu}", fr, tot);
    reply(fd, 200, "application/json", b, strlen(b));
}

static bool is_image(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg") || !strcasecmp(d, ".png") ||
                 !strcasecmp(d, ".bmp") || !strcasecmp(d, ".gif"));
}

static void photo_walk(sbuf_t *b, const char *full, const char *web, int depth, bool *first, int *n)
{
    if (depth > 4 || *n > 2000)
        return;
    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;
        char f2[400], w2[400];
        snprintf(f2, sizeof(f2), "%s/%s", full, de->d_name);
        snprintf(w2, sizeof(w2), "%s/%s", web, de->d_name);
        if (de->d_type == DT_DIR)
        {
            photo_walk(b, f2, w2, depth + 1, first, n);
            continue;
        }
        if (!is_image(de->d_name))
            continue;
        struct stat st;
        unsigned long sz = stat(f2, &st) == 0 ? (unsigned long)st.st_size : 0;
        if (!*first)
            sb_str(b, ",");
        *first = false;
        sb_str(b, "{\"p\":");
        sb_json_str(b, w2);
        char num[32];
        snprintf(num, sizeof(num), ",\"s\":%lu}", sz);
        sb_str(b, num);
        if ((++*n & 15) == 0)
            cothread_yield();
    }
    closedir(d);
}

static void api_photos(int fd)
{
    sbuf_t b = { 0 };
    sb_str(&b, "[");
    bool first = true;
    int n = 0;
    // DSi camera photos, plus the "photo" folder some tools use
    static const char *roots[] = { "/DCIM", "/photo", "/screenshots", NULL };
    for (int i = 0; roots[i]; i++)
    {
        char full[64];
        snprintf(full, sizeof(full), "%s%s", drive, roots[i]);
        photo_walk(&b, full, roots[i], 0, &first, &n);
    }
    sb_str(&b, "]");
    if (!b.p)
        reply_text(fd, 500, "Geen geheugen");
    else
        reply(fd, 200, "application/json; charset=utf-8", b.p, b.len);
    free(b.p);
}

static uint32_t zcrc_table[256];

static uint32_t zcrc(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!zcrc_table[1])
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            zcrc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = zcrc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

typedef struct {
    char *name;
    uint32_t crc, size, off;
} zent_t;

typedef struct {
    int fd;
    zent_t *e;
    int n, cap;
    uint32_t off;
    bool ok;
    server_state_t *st;
    char *buf;
} zip_t;

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool zsend(zip_t *z, const void *d, size_t n)
{
    if (!z->ok)
        return false;
    if (!send_all(z->fd, d, n))
        z->ok = false;
    z->off += n;
    return z->ok;
}

static void zip_file(zip_t *z, const char *full, const char *name)
{
    FILE *f = fopen(full, "rb");
    if (!f)
        return;
    if (z->n == z->cap)
    {
        z->cap = z->cap ? z->cap * 2 : 64;
        zent_t *nw = realloc(z->e, sizeof(zent_t) * z->cap);
        if (!nw)
        {
            fclose(f);
            z->ok = false;
            return;
        }
        z->e = nw;
    }
    zent_t *e = &z->e[z->n++];
    e->name = strdup(name);
    e->off = z->off;
    e->crc = 0;
    e->size = 0;
    size_t nl = strlen(name);

    uint8_t h[30] = { 0 };
    put32(h, 0x04034b50);
    put16(h + 4, 20);
    put16(h + 6, 0x0808); // data descriptor + UTF-8 names
    put16(h + 26, nl);
    zsend(z, h, 30);
    zsend(z, name, nl);

    struct stat st;
    snprintf(z->st->current, sizeof(z->st->current), "%s", name);
    z->st->total = stat(full, &st) == 0 ? st.st_size : 0;
    z->st->done = 0;
    while (z->ok && !z->st->stop)
    {
        size_t r = fread(z->buf, 1, 16 * 1024, f);
        if (r == 0)
            break;
        e->crc = zcrc(e->crc, (uint8_t *)z->buf, r);
        e->size += r;
        zsend(z, z->buf, r);
        z->st->done += r;
        cothread_yield();
    }
    fclose(f);
    if (z->st->stop)
        z->ok = false;

    uint8_t dd[16];
    put32(dd, 0x08074b50);
    put32(dd + 4, e->crc);
    put32(dd + 8, e->size);
    put32(dd + 12, e->size);
    zsend(z, dd, 16);
}

static void zip_walk(zip_t *z, const char *full, const char *rel, int depth)
{
    if (depth > 12 || !z->ok)
        return;
    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *de;
    while (z->ok && (de = readdir(d)) != NULL)
    {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char *f2 = malloc(600), *r2 = malloc(600);
        if (!f2 || !r2)
        {
            free(f2);
            free(r2);
            z->ok = false;
            break;
        }
        snprintf(f2, 600, "%s/%s", full, de->d_name);
        snprintf(r2, 600, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
        if (de->d_type == DT_DIR)
            zip_walk(z, f2, r2, depth + 1);
        else
            zip_file(z, f2, r2);
        free(f2);
        free(r2);
    }
    closedir(d);
}

static void api_zip(int fd, server_state_t *st, const char *query)
{
    char full[300], clean[256];
    if (!resolve(query, "path", full, sizeof(full), clean, sizeof(clean)))
    {
        reply_text(fd, 400, "Ongeldig pad");
        return;
    }
    if (strcmp(clean, "/") == 0)
        snprintf(full, sizeof(full), "%s", drive);
    const char *base = strrchr(clean, '/');
    base = (base && base[1]) ? base + 1 : "sd-kaart";

    char hdr[300];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 200 OK\r\nContent-Type: application/zip\r\n"
                     "Content-Disposition: attachment; filename=\"%s.zip\"\r\n"
                     "Connection: close\r\n\r\n", base);
    if (!send_all(fd, hdr, n))
        return;

    zip_t z = { fd, NULL, 0, 0, 0, true, st, malloc(16 * 1024) };
    if (!z.buf)
        return;
    st->sending = true;
    // Entries are stored inside a folder named like the one downloaded
    zip_walk(&z, full, base, 0);

    // Central directory
    uint32_t cd_start = z.off;
    for (int i = 0; i < z.n && z.ok; i++)
    {
        zent_t *e = &z.e[i];
        size_t nl = strlen(e->name);
        uint8_t c[46] = { 0 };
        put32(c, 0x02014b50);
        put16(c + 4, 20);
        put16(c + 6, 20);
        put16(c + 8, 0x0808);
        put32(c + 16, e->crc);
        put32(c + 20, e->size);
        put32(c + 24, e->size);
        put16(c + 28, nl);
        put32(c + 42, e->off);
        zsend(&z, c, 46);
        zsend(&z, e->name, nl);
    }
    uint8_t eocd[22] = { 0 };
    put32(eocd, 0x06054b50);
    put16(eocd + 8, z.n);
    put16(eocd + 10, z.n);
    put32(eocd + 12, z.off - cd_start);
    put32(eocd + 16, cd_start);
    zsend(&z, eocd, 22);

    for (int i = 0; i < z.n; i++)
        free(z.e[i].name);
    free(z.e);
    free(z.buf);
    st->sending = false;
    if (z.ok)
        snprintf(st->last_msg, sizeof(st->last_msg), "Zip verstuurd: %.60s", base);
}

static void api_drive(int fd, const char *method, const char *query)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/_dsidl/drive.txt", drive);
    if (strcmp(method, "POST") == 0)
    {
        char link[400] = "";
        query_param(query, "link", link, sizeof(link));
        FILE *f = fopen(path, "w");
        if (!f)
        {
            reply_text(fd, 500, "Opslaan mislukt");
            return;
        }
        fprintf(f, "%s\n", link);
        fclose(f);
        reply_text(fd, 200, "OK");
        return;
    }
    char link[400] = "";
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fgets(link, sizeof(link), f))
            link[strcspn(link, "\r\n")] = '\0';
        fclose(f);
    }
    reply_text(fd, 200, link);
}

static void api_action(int fd, server_state_t *st, const char *target, const char *query)
{
    char full[300], clean[256];
    if (!resolve(query, "path", full, sizeof(full), clean, sizeof(clean)) ||
        strcmp(clean, "/") == 0)
    {
        reply_text(fd, 400, "Ongeldig pad");
        return;
    }
    bool protect = strcasecmp(clean, "/_nds") == 0 || strcasecmp(clean, "/_dsidl") == 0;

    if (strcmp(target, "/api/mkdir") == 0)
    {
        if (mkdir(full, 0777) != 0)
            reply_text(fd, 409, "Map maken mislukt (bestaat hij al?)");
        else
            reply_text(fd, 200, "OK");
    }
    else if (strcmp(target, "/api/delete") == 0)
    {
        if (protect)
        {
            reply_text(fd, 403, "Deze map is nodig voor je DSi-software en wordt niet verwijderd");
            return;
        }
        struct stat stt;
        if (stat(full, &stt) != 0)
            reply_text(fd, 404, "Niet gevonden");
        else if (S_ISDIR(stt.st_mode) ? rmdir(full) != 0 : remove(full) != 0)
            reply_text(fd, 409, S_ISDIR(stt.st_mode) ? "Map is niet leeg" : "Verwijderen mislukt");
        else
        {
            snprintf(st->last_msg, sizeof(st->last_msg), "Verwijderd: %.68s", clean);
            reply_text(fd, 200, "OK");
        }
    }
    else if (strcmp(target, "/api/rename") == 0)
    {
        char to[128];
        if (protect || !query_param(query, "to", to, sizeof(to)))
        {
            reply_text(fd, 400, "Ongeldige naam");
            return;
        }
        sanitize_filename(to);
        if (to[0] == '\0')
        {
            reply_text(fd, 400, "Ongeldige naam");
            return;
        }
        char dst[420];
        snprintf(dst, sizeof(dst), "%s", full);
        char *slash = strrchr(dst, '/');
        snprintf(slash + 1, sizeof(dst) - (slash + 1 - dst), "%s", to);
        if (file_exists(dst))
            reply_text(fd, 409, "Er bestaat al iets met die naam");
        else if (rename(full, dst) != 0)
            reply_text(fd, 500, "Hernoemen mislukt");
        else
            reply_text(fd, 200, "OK");
    }
    else if (strcmp(target, "/api/move") == 0 || strcmp(target, "/api/copy") == 0)
    {
        api_copy_move(fd, st, target[5] == 'm', query);
    }
    else if (strcmp(target, "/api/hide") == 0)
    {
        char on[4] = "1";
        query_param(query, "on", on, sizeof(on));
        int attr = FAT_getAttr(full);
        if (attr < 0)
        {
            reply_text(fd, 404, "Niet gevonden");
            return;
        }
        attr = on[0] == '1' ? (attr | ATTR_HIDDEN) : (attr & ~ATTR_HIDDEN);
        if (FAT_setAttr(full, attr & 0xFF) < 0)
            reply_text(fd, 500, "Kenmerk wijzigen mislukt");
        else
            reply_text(fd, 200, "OK");
    }
    else
    {
        reply_text(fd, 404, "Onbekende actie");
    }
}

static void handle_client(int fd, server_state_t *st, const char *default_dir)
{
    (void)default_dir;
    char *req = malloc(4096);
    if (!req)
        return;

    size_t len = 0;
    char *hend = NULL;
    while (len < 4095)
    {
        // Browsers often open spare connections that never send anything
        int r = recv_wait(fd, req + len, 4095 - len, st, 3000);
        if (r <= 0)
            break;
        len += r;
        req[len] = '\0';
        hend = strstr(req, "\r\n\r\n");
        if (hend)
            break;
    }
    if (!hend)
    {
        free(req);
        return;
    }
    *hend = '\0';
    char *body0 = hend + 4;
    size_t body0len = len - (body0 - req);

    char method[8] = { 0 };
    char target[1024] = { 0 };
    if (sscanf(req, "%7s %1023s", method, target) != 2)
    {
        reply_text(fd, 400, "Bad request");
        free(req);
        return;
    }

    unsigned long clen = 0;
    for (char *line = strstr(req, "\r\n"); line; line = strstr(line, "\r\n"))
    {
        line += 2;
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            clen = strtoul(line + 15, NULL, 10);
    }

    char *query = strchr(target, '?');
    if (query)
        *query++ = '\0';

    if (strcmp(method, "GET") == 0 && (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0))
    {
        reply(fd, 200, "text/html; charset=utf-8", (const char *)webui_bin, webui_bin_size);
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/unrar.js") == 0)
    {
        reply(fd, 200, "text/javascript", (const char *)unrarjs_bin, unrarjs_bin_size);
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/unrar.wasm") == 0)
    {
        reply(fd, 200, "application/wasm", (const char *)unrarwasm_bin, unrarwasm_bin_size);
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/task") == 0)
    {
        char js[600], escd[520];
        size_t k = 0;
        for (const char *c = st->task_unrar; *c && k < sizeof(escd) - 2; c++)
        {
            if (*c == '"' || *c == '\\')
                escd[k++] = '\\';
            escd[k++] = *c;
        }
        escd[k] = '\0';
        if (st->task_unrar[0])
            snprintf(js, sizeof(js), "{\"unrar\":\"%s\"}", escd);
        else
            snprintf(js, sizeof(js), "{}");
        reply(fd, 200, "application/json", js, strlen(js));
    }
    else if (strcmp(method, "POST") == 0 && strcmp(target, "/api/taskdone") == 0)
    {
        char ok[8] = "0";
        query_param(query ? query : "", "ok", ok, sizeof(ok));
        if (st->task_unrar[0])
        {
            const char *nm = strrchr(st->task_unrar, '/');
            snprintf(st->last_msg, sizeof(st->last_msg), "%s %.60s",
                     ok[0] == '1' ? "Uitgepakt via telefoon:" : "Uitpakken mislukt:", nm ? nm + 1 : st->task_unrar);
            st->task_unrar[0] = '\0';
            st->task_done = ok[0] == '1';
        }
        reply_text(fd, 200, "OK");
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/list") == 0)
    {
        api_list(fd, st, query ? query : "");
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/dl") == 0)
    {
        api_download(fd, st, query ? query : "");
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/zip") == 0)
    {
        api_zip(fd, st, query ? query : "");
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/info") == 0)
    {
        api_info(fd);
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/photos") == 0)
    {
        api_photos(fd);
    }
    else if (strcmp(target, "/api/drive") == 0)
    {
        api_drive(fd, method, query ? query : "");
    }
    else if (strcmp(method, "POST") == 0 && strncmp(target, "/api/", 5) == 0)
    {
        api_action(fd, st, target, query ? query : "");
    }
    else if ((strcmp(method, "PUT") == 0 || strcmp(method, "POST") == 0) &&
             strcmp(target, "/upload") == 0)
    {
        if (body0len > clen)
            body0len = clen;
        handle_upload(fd, st, query ? query : "", clen, body0, body0len);
    }
    else
    {
        reply_text(fd, 404, "Niet gevonden");
    }
    free(req);
}

bool server_run(server_state_t *st, const char *default_dir)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0)
        return false;

    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 2) != 0)
    {
        closesocket(ls);
        return false;
    }

    while (!st->stop)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        struct timeval tv = { 0, 0 }; // poll so that B (stop) is noticed
        int r = select(ls + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0)
        {
            cothread_yield_irq(IRQ_VBLANK);
            continue;
        }

        int c = accept(ls, NULL, NULL);
        if (c < 0)
            continue;

        handle_client(c, st, default_dir);
        shutdown(c, SHUT_RDWR);
        closesocket(c);
    }

    closesocket(ls);
    return true;
}
