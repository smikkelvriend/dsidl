// DSiDL - HTTP(S) client on top of dswifi sockets + Mbed TLS
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include <nds.h>
#include <dswifi9.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "http.h"
#include "cacert_bin.h"

#define USER_AGENT "DSiDL/" APP_VERSION " (Nintendo DSi)"
#define MAX_REDIRECTS 8
#define HDR_MAX  (32 * 1024)  // github.com sends very large headers
#define PATH_MAX_LEN 4096
#define URL_MAX_LEN  4608

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;
static mbedtls_x509_crt cacert;
static mbedtls_ssl_config ssl_conf;
static bool tls_ready = false;

bool http_init(void)
{
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ssl_config_init(&ssl_conf);

    const char *pers = "dsidl";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        return false;

    // Returns number of certs that failed to parse (>0) or error (<0)
    // PEM parsing needs a NUL terminated buffer
    char *pem = malloc(cacert_bin_size + 1);
    if (!pem)
        return false;
    memcpy(pem, cacert_bin, cacert_bin_size);
    pem[cacert_bin_size] = '\0';
    int r = mbedtls_x509_crt_parse(&cacert, (const unsigned char *)pem, cacert_bin_size + 1);
    free(pem);
    if (r < 0 || cacert.version == 0)
        return false;

    if (mbedtls_ssl_config_defaults(&ssl_conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;

    mbedtls_ssl_conf_authmode(&ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&ssl_conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&ssl_conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    // TLS 1.3 needs PSA crypto init; keep it simple and robust with TLS 1.2
    mbedtls_ssl_conf_max_tls_version(&ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);

    tls_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// Connection abstraction (plain TCP or TLS)
//
// All waiting happens in small select() steps so the UI thread keeps running,
// the user can cancel with B, and a stalled connection times out instead of
// hanging forever.

#define IDLE_TIMEOUT_MS 15000

static volatile bool *g_cancel = NULL;

typedef struct {
    int fd;
    bool tls;
    mbedtls_ssl_context ssl;
} conn_t;

static bool cancelled(void)
{
    return g_cancel && *g_cancel;
}

// Waits until fd is readable (or writable). Returns 1 ready, 0 timeout or
// cancel, -1 error.
static int wait_fd(int fd, bool for_write, int timeout_ms)
{
    int waited = 0;
    while (!cancelled())
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        // Poll only (zero timeout): dswifi ignores select() timeouts when
        // nothing happens on the socket, which would block forever.
        struct timeval tv = { 0, 0 };
        int r = select(fd + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv);
        if (r > 0)
            return 1;
        if (r < 0)
            return -1;
        cothread_yield_irq(IRQ_VBLANK); // ~17 ms
        waited += 17;
        if (waited >= timeout_ms)
            return 0;
    }
    return 0;
}

// recv with timeout. Returns >0 bytes, 0 on EOF, -1 on error/timeout/cancel
static int recv_timeout(int fd, void *buf, size_t len)
{
    if (wait_fd(fd, false, IDLE_TIMEOUT_MS) != 1)
        return -1;
    int r = recv(fd, buf, len, 0);
    return r < 0 ? -1 : r;
}

static int send_timeout(int fd, const void *buf, size_t len)
{
    if (wait_fd(fd, true, IDLE_TIMEOUT_MS) != 1)
        return -1;
    int r = send(fd, buf, len, 0);
    return r < 0 ? -1 : r;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int r = send_timeout(*(int *)ctx, buf, len);
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int r = recv_timeout(*(int *)ctx, buf, len);
    if (r < 0)
        return MBEDTLS_ERR_NET_RECV_FAILED;
    if (r == 0)
        return MBEDTLS_ERR_NET_CONN_RESET; // EOF
    return r;
}

static int tcp_connect(const char *host, int port, char *err, size_t errlen)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL)
    {
        snprintf(err, errlen, "DNS mislukt: %s", host);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        closesocket(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
    {
        snprintf(err, errlen, "Kan niet verbinden met %s", host);
        return -1;
    }
    return fd;
}

static bool conn_open(conn_t *c, bool https, const char *host, int port,
                      char *err, size_t errlen)
{
    memset(c, 0, sizeof(*c));
    c->fd = tcp_connect(host, port, err, errlen);
    if (c->fd < 0)
        return false;

    c->tls = https;
    if (!https)
        return true;

    if (!tls_ready)
    {
        snprintf(err, errlen, "TLS niet beschikbaar");
        closesocket(c->fd);
        return false;
    }

    mbedtls_ssl_init(&c->ssl);
    int r = mbedtls_ssl_setup(&c->ssl, &ssl_conf);
    if (r == 0)
        r = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (r == 0)
    {
        mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);
        while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
        {
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
                break;
            cothread_yield();
        }
    }
    if (r != 0)
    {
        if (cancelled())
            snprintf(err, errlen, "Geannuleerd");
        else if (r == MBEDTLS_ERR_NET_RECV_FAILED)
            snprintf(err, errlen, "Server reageert niet (%s)", host);
        else
        {
            char e[64];
            mbedtls_strerror(r, e, sizeof(e));
            snprintf(err, errlen, "TLS fout: %s", e);
        }
        mbedtls_ssl_free(&c->ssl);
        closesocket(c->fd);
        return false;
    }
    return true;
}

static void conn_close(conn_t *c)
{
    if (c->tls)
    {
        // Don't wait for the peer here, a dead connection must close quickly
        mbedtls_ssl_free(&c->ssl);
    }
    shutdown(c->fd, SHUT_RDWR);
    closesocket(c->fd);
}

static bool conn_write_all(conn_t *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len > 0)
    {
        int r;
        if (c->tls)
        {
            r = mbedtls_ssl_write(&c->ssl, p, len);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
        }
        else
        {
            r = send_timeout(c->fd, p, len);
        }
        if (r <= 0)
            return false;
        p += r;
        len -= r;
    }
    return true;
}

// Returns >0 bytes, 0 on EOF, <0 on error/timeout/cancel
static int conn_read(conn_t *c, void *buf, size_t len)
{
    if (!c->tls)
        return recv_timeout(c->fd, buf, len);

    while (1)
    {
        int r = mbedtls_ssl_read(&c->ssl, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            cothread_yield();
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_NET_CONN_RESET)
            return 0;
        return r;
    }
}

// ---------------------------------------------------------------------------
// URL parsing

static bool parse_url(const char *url, bool *https, char *host, size_t hostlen,
                      int *port, char *path, size_t pathlen)
{
    const char *p;
    if (strncasecmp(url, "https://", 8) == 0)
    {
        *https = true;
        *port = 443;
        p = url + 8;
    }
    else if (strncasecmp(url, "http://", 7) == 0)
    {
        *https = false;
        *port = 80;
        p = url + 7;
    }
    else
    {
        return false;
    }

    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= hostlen)
        return false;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    char *colon = strchr(host, ':');
    if (colon)
    {
        *colon = '\0';
        *port = atoi(colon + 1);
    }

    snprintf(path, pathlen, "%s", slash ? slash : "/");
    return true;
}

// ---------------------------------------------------------------------------
// Generic GET with a body sink, redirects and automatic resume

typedef bool (*sink_fn)(void *ctx, const void *data, size_t len);

#define MAX_RESUMES 8

typedef enum { ATT_DONE, ATT_FATAL, ATT_RETRY } attempt_t;

// One request. Follows redirects (updating *url). If 'offset' > 0 asks for the
// rest of the file with a Range header. Adds bytes to *got and sets *total.
static attempt_t http_attempt(char **url, size_t offset, size_t *got, long *total,
                              sink_fn sink, void *sink_ctx, http_progress_t *prog,
                              char *path, char *hdr, char *buf,
                              int *status_out, char *err, size_t errlen)
{
    char host[128];

    for (int redirect = 0; redirect <= MAX_REDIRECTS; redirect++)
    {
        bool https;
        int port;
        if (!parse_url(*url, &https, host, sizeof(host), &port, path, PATH_MAX_LEN))
        {
            snprintf(err, errlen, "Ongeldige URL");
            return ATT_FATAL;
        }

        if (prog)
            snprintf(prog->status, sizeof(prog->status), "Verbinden: %.40s", host);

        conn_t c;
        if (!conn_open(&c, https, host, port, err, errlen))
            return cancelled() ? ATT_FATAL : ATT_RETRY;

        char range[48] = "";
        if (offset > 0)
            snprintf(range, sizeof(range), "Range: bytes=%u-\r\n", (unsigned)offset);

        int n = snprintf(hdr, HDR_MAX,
                         "GET %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "User-Agent: " USER_AGENT "\r\n"
                         "Accept: */*\r\n"
                         "%s"
                         "Connection: close\r\n"
                         "\r\n", path, host, range);
        if (!conn_write_all(&c, hdr, n))
        {
            snprintf(err, errlen, "Verzenden mislukt");
            conn_close(&c);
            return cancelled() ? ATT_FATAL : ATT_RETRY;
        }

        // Read headers
        size_t hlen = 0;
        char *hend = NULL;
        while (hlen < HDR_MAX - 1)
        {
            int r = conn_read(&c, hdr + hlen, HDR_MAX - 1 - hlen);
            if (r <= 0)
                break;
            hlen += r;
            hdr[hlen] = '\0';
            hend = strstr(hdr, "\r\n\r\n");
            if (hend)
                break;
        }
        if (!hend)
        {
            bool big = hlen >= HDR_MAX - 1;
            snprintf(err, errlen, big ? "Antwoord-headers te groot (%s)" :
                     "Geen antwoord van server (%s)", host);
            conn_close(&c);
            return (big || cancelled()) ? ATT_FATAL : ATT_RETRY;
        }
        *hend = '\0';
        char *body0 = hend + 4;
        size_t body0len = hlen - (body0 - hdr);

        int status = 0;
        if (sscanf(hdr, "HTTP/%*d.%*d %d", &status) != 1)
        {
            snprintf(err, errlen, "Ongeldige status");
            conn_close(&c);
            return ATT_FATAL;
        }
        *status_out = status;

        long content_length = -1;
        long range_start = -1, range_total = -1;
        char *location = NULL;
        for (char *line = strstr(hdr, "\r\n"); line; line = strstr(line, "\r\n"))
        {
            line += 2;
            if (strncasecmp(line, "Content-Length:", 15) == 0)
                content_length = atol(line + 15);
            else if (strncasecmp(line, "Content-Range:", 14) == 0)
                sscanf(line + 14, " bytes %ld-%*d/%ld", &range_start, &range_total);
            else if (strncasecmp(line, "Location:", 9) == 0)
                location = line + 9;
        }

        if (status >= 300 && status < 400 && location)
        {
            while (*location == ' ')
                location++;
            char *e = strstr(location, "\r\n");
            if (e)
                *e = '\0';

            char *newurl = malloc(URL_MAX_LEN);
            if (!newurl)
            {
                conn_close(&c);
                return ATT_FATAL;
            }
            if (location[0] == '/')
            {
                if ((https && port == 443) || (!https && port == 80))
                    snprintf(newurl, URL_MAX_LEN, "%s://%s%s", https ? "https" : "http",
                             host, location);
                else
                    snprintf(newurl, URL_MAX_LEN, "%s://%s:%d%s", https ? "https" : "http",
                             host, port, location);
            }
            else
            {
                snprintf(newurl, URL_MAX_LEN, "%s", location);
            }
            free(*url);
            *url = newurl;
            conn_close(&c);
            continue;
        }

        if (offset > 0)
        {
            // Resuming: the server must continue exactly where we stopped
            if (status != 206 || range_start != (long)offset)
            {
                snprintf(err, errlen, "Verbinding verbroken (hervatten niet mogelijk)");
                conn_close(&c);
                return ATT_FATAL;
            }
            if (range_total > 0)
                *total = range_total;
        }
        else
        {
            if (status != 200)
            {
                snprintf(err, errlen, "Server gaf HTTP %d", status);
                conn_close(&c);
                return ATT_FATAL;
            }
            *total = content_length;
        }

        if (prog)
        {
            prog->total = *total > 0 ? (size_t)*total : 0;
            prog->done = *got;
            snprintf(prog->status, sizeof(prog->status),
                     offset > 0 ? "Downloaden (hervat)..." : "Downloaden...");
        }

        if (body0len > 0)
        {
            if (!sink(sink_ctx, body0, body0len))
            {
                snprintf(err, errlen, "Schrijven mislukt (SD vol?)");
                conn_close(&c);
                return ATT_FATAL;
            }
            *got += body0len;
        }

        while (1)
        {
            if (*total >= 0 && *got >= (size_t)*total)
                break;
            if (cancelled())
            {
                snprintf(err, errlen, "Geannuleerd");
                conn_close(&c);
                return ATT_FATAL;
            }
            int r = conn_read(&c, buf, 8192);
            if (r == 0)
                break;
            if (r < 0)
            {
                snprintf(err, errlen, "Verbinding viel weg");
                conn_close(&c);
                return cancelled() ? ATT_FATAL : ATT_RETRY;
            }
            if (!sink(sink_ctx, buf, r))
            {
                snprintf(err, errlen, "Schrijven mislukt (SD vol?)");
                conn_close(&c);
                return ATT_FATAL;
            }
            *got += r;
            if (prog)
                prog->done = *got;
            cothread_yield();
        }

        conn_close(&c);

        if (*total >= 0 && *got < (size_t)*total)
        {
            snprintf(err, errlen, "Verbinding viel weg");
            return ATT_RETRY;
        }
        return ATT_DONE;
    }

    snprintf(err, errlen, "Te veel doorverwijzingen");
    return ATT_FATAL;
}

static int http_get(const char *url_in, sink_fn sink, void *sink_ctx,
                    http_progress_t *prog, char *err, size_t errlen)
{
    char *url = strdup(url_in);
    char *path = malloc(PATH_MAX_LEN);
    char *hdr = malloc(HDR_MAX);
    char *buf = malloc(8192);
    int result = -1;

    g_cancel = prog ? &prog->cancel : NULL;

    if (!url || !path || !hdr || !buf)
    {
        snprintf(err, errlen, "Geen geheugen");
        goto out;
    }

    size_t got = 0;
    long total = -1;
    int status = 0;

    for (int attempt = 0; attempt <= MAX_RESUMES; attempt++)
    {
        if (attempt > 0 && prog)
        {
            snprintf(prog->status, sizeof(prog->status), "Opnieuw verbinden (%d)...", attempt);
            // short pause before retrying
            for (int i = 0; i < 60 && !cancelled(); i++)
                cothread_yield_irq(IRQ_VBLANK);
        }

        // Before any body data has arrived we can simply start over from the
        // original URL; afterwards we resume the final (redirected) URL.
        if (got == 0 && attempt > 0)
        {
            free(url);
            url = strdup(url_in);
            if (!url)
                break;
        }

        attempt_t a = http_attempt(&url, got, &got, &total, sink, sink_ctx, prog,
                                   path, hdr, buf, &status, err, errlen);
        if (a == ATT_DONE)
        {
            result = 200;
            break;
        }
        if (a == ATT_FATAL)
        {
            result = (status >= 400) ? status : -1;
            break;
        }
        // Resuming without a known size could silently truncate; stop then.
        if (got > 0 && total < 0)
            break;
    }

out:
    g_cancel = NULL;
    free(url);
    free(path);
    free(hdr);
    free(buf);
    return result;
}

// ---------------------------------------------------------------------------

static bool file_sink(void *ctx, const void *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len;
}

int http_get_file(const char *url, const char *path, http_progress_t *prog,
                  char *err, size_t errlen)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        snprintf(err, errlen, "Kan bestand niet maken");
        return -1;
    }
    setvbuf(f, NULL, _IOFBF, 32 * 1024);
    int r = http_get(url, file_sink, f, prog, err, errlen);
    if (fclose(f) != 0 && r == 200)
    {
        snprintf(err, errlen, "Opslaan mislukt");
        r = -1;
    }
    if (r != 200)
        remove(path);
    return r;
}

typedef struct {
    char *data;
    size_t len, cap, max;
} membuf_t;

static bool mem_sink(void *ctx, const void *data, size_t len)
{
    membuf_t *m = ctx;
    if (m->len + len + 1 > m->max)
        return false;
    if (m->len + len + 1 > m->cap)
    {
        size_t ncap = m->cap ? m->cap * 2 : 64 * 1024;
        while (ncap < m->len + len + 1)
            ncap *= 2;
        if (ncap > m->max)
            ncap = m->max;
        char *n = realloc(m->data, ncap);
        if (!n)
            return false;
        m->data = n;
        m->cap = ncap;
    }
    memcpy(m->data + m->len, data, len);
    m->len += len;
    m->data[m->len] = '\0';
    return true;
}

int http_get_mem(const char *url, char **out, size_t *outlen, size_t maxlen,
                 http_progress_t *prog, char *err, size_t errlen)
{
    membuf_t m = { NULL, 0, 0, maxlen };
    int r = http_get(url, mem_sink, &m, prog, err, errlen);
    if (r != 200 || m.data == NULL)
    {
        free(m.data);
        *out = NULL;
        if (r == 200)
        {
            snprintf(err, errlen, "Leeg antwoord");
            r = -1;
        }
        return r;
    }
    *out = m.data;
    if (outlen)
        *outlen = m.len;
    return r;
}
