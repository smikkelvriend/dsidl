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

extern char drive[8];

// Web page (web/index.html, embedded at build time)
#include "webui_bin.h"

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    int idle = 0;
    while (len > 0)
    {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { 0, 20 * 1000 };
        int s = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (s < 0)
            return false;
        if (s == 0)
        {
            cothread_yield();
            idle += 20;
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
        struct timeval tv = { 0, 20 * 1000 };
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0)
            return recv(fd, buf, len, 0);
        if (r < 0)
            return -1;
        cothread_yield();
        waited += 20;
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

    char hdr[400];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                     "Content-Length: %lu\r\n"
                     "Content-Disposition: attachment; filename=\"%s\"\r\n"
                     "Connection: close\r\n\r\n", (unsigned long)stt.st_size, safe);
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
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/list") == 0)
    {
        api_list(fd, st, query ? query : "");
    }
    else if (strcmp(method, "GET") == 0 && strcmp(target, "/dl") == 0)
    {
        api_download(fd, st, query ? query : "");
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
        struct timeval tv = { 0, 100 * 1000 };
        int r = select(ls + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0)
        {
            cothread_yield();
            continue;
        }

        int c = accept(ls, NULL, NULL);
        if (c < 0)
            continue;

        struct timeval to = { 30, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        handle_client(c, st, default_dir);
        shutdown(c, SHUT_RDWR);
        closesocket(c);
    }

    closesocket(ls);
    return true;
}
