// DSiDL - tiny HTTP server: phone opens http://<dsi-ip>/ and uploads files
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <nds.h>
#include <dswifi9.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "server.h"
#include "util.h"

extern char drive[8];

static const char page_html[] =
"<!DOCTYPE html><html lang=nl><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>DSiDL</title><style>"
"body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:20px;max-width:560px;margin:auto}"
"h1{font-size:22px;margin:0 0 4px}p{color:#aaa;margin:0 0 18px}"
".card{background:#1d1d1d;border-radius:14px;padding:16px;margin-bottom:14px}"
"label{display:block;font-size:13px;color:#aaa;margin-bottom:6px}"
"select,input[type=text]{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #333;background:#000;color:#eee;font-size:16px}"
"input[type=file]{width:100%;color:#eee;font-size:15px}"
"button{width:100%;padding:14px;border:0;border-radius:12px;background:#d71921;color:#fff;font-size:17px;font-weight:600;margin-top:6px}"
"button:disabled{background:#444}"
".bar{height:10px;background:#333;border-radius:5px;overflow:hidden;margin-top:8px}"
".bar div{height:100%;width:0;background:#d71921;transition:width .2s}"
".log{font-size:14px;white-space:pre-wrap;color:#ccc}"
"</style></head><body>"
"<h1>DSiDL</h1><p>Bestanden naar je DSi sturen via wifi</p>"
"<div class=card><label>Map op de SD-kaart</label>"
"<select id=dir>"
"<option value='/roms/nds'>/roms/nds (DS en DSi)</option>"
"<option value='/roms/gba'>/roms/gba</option>"
"<option value='/roms/gb'>/roms/gb</option>"
"<option value='/roms/nes'>/roms/nes</option>"
"<option value='/roms/snes'>/roms/snes</option>"
"<option value='/roms/gen'>/roms/gen</option>"
"<option value='/'>/ (hoofdmap)</option>"
"<option value='_custom'>Andere map...</option>"
"</select>"
"<input type=text id=custom placeholder='/mijn/map' style='display:none;margin-top:8px'></div>"
"<div class=card><label>Bestanden</label><input type=file id=f multiple>"
"<button id=go>Versturen</button>"
"<div class=bar><div id=bar></div></div></div>"
"<div class='card log' id=log>Klaar om te versturen.</div>"
"<script>"
"const $=id=>document.getElementById(id);"
"$('dir').onchange=()=>{$('custom').style.display=$('dir').value=='_custom'?'block':'none'};"
"function up(file,dir){return new Promise((ok,bad)=>{"
"const x=new XMLHttpRequest();"
"x.open('PUT','/upload?dir='+encodeURIComponent(dir)+'&name='+encodeURIComponent(file.name));"
"x.upload.onprogress=e=>{if(e.lengthComputable)$('bar').style.width=(100*e.loaded/e.total)+'%'};"
"x.onload=()=>x.status==200?ok(x.responseText):bad(x.responseText||('HTTP '+x.status));"
"x.onerror=()=>bad('Verbinding verbroken');x.send(file);})}"
"$('go').onclick=async()=>{"
"const files=[...$('f').files];if(!files.length){$('log').textContent='Kies eerst een bestand.';return}"
"let dir=$('dir').value;if(dir=='_custom')dir=$('custom').value||'/';"
"$('go').disabled=true;let log='';"
"for(const f of files){log+=f.name+' ... ';$('log').textContent=log;$('bar').style.width='0';"
"try{await up(f,dir);log+='klaar\\n'}catch(e){log+='MISLUKT: '+e+'\\n'}"
"$('log').textContent=log}"
"$('go').disabled=false};"
"</script></body></html>";

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0)
    {
        int r = send(fd, p, len, 0);
        if (r <= 0)
            return false;
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
                      code == 400 ? "Bad Request" : "Internal Server Error";
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
        reply(fd, 200, "text/html; charset=utf-8", page_html, sizeof(page_html) - 1);
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
