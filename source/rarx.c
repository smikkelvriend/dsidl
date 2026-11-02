// DSiDL - .rar extraction using libarchive's RAR/RAR5 readers.
//
// RAR5 archives often use a 32 MB dictionary while the DSi only has 16 MB of
// RAM. The (patched) RAR5 reader keeps a smaller window in RAM and hands every
// decoded (still unfiltered) byte to rar_hist_write(), which keeps it in a
// temporary history file on the SD card. Matches that point further back than
// the RAM window are then served from that file by rar_far_read().

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <nds.h>

#include "libarchive/archive.h"
#include "libarchive/archive_entry.h"
#include "rarx.h"
extern char ppmd7v_swap_path[300];
extern size_t ppmd7v_ram_limit;
extern void (*ppmd7v_yield)(void);
extern volatile int ppmd7v_abort;
static struct archive *cur_a;
static http_progress_t *cur_prog;
// Called regularly from inside the (slow) PPMd decoder
static void rar_yield(void)
{
    if (cur_prog)
    {
        if (cur_a)
            cur_prog->done = (size_t)archive_filter_bytes(cur_a, -1);
        if (cur_prog->cancel)
            ppmd7v_abort = 1;
    }
    cothread_yield();
}
#include "util.h"

extern size_t rar5_window_cap;
extern int (*rar5_far_read)(int64_t abs_pos, uint8_t *dst, int len);
extern int (*rar5_hist_write)(int64_t abs_pos, const uint8_t *src, int len);

static FILE *cur_out;          // file being written
static int64_t cur_size;
static FILE *hist_f;           // unfiltered history (only when needed)
static char hist_path[300];
static int64_t hist_wpos = -1; // current file position of hist_f
static bool hist_dirty;

static int rar_hist_write(int64_t pos, const uint8_t *src, int len)
{
    if (!hist_f)
    {
        hist_f = fopen(hist_path, "w+b");
        if (!hist_f)
            return -1;
        setvbuf(hist_f, NULL, _IOFBF, 32 * 1024);
        hist_wpos = 0;
    }
    if (pos != hist_wpos && fseek(hist_f, (long)pos, SEEK_SET) != 0)
        return -1;
    if (fwrite(src, 1, len, hist_f) != (size_t)len)
        return -1;
    hist_wpos = pos + len;
    hist_dirty = true;
    return 0;
}

static int rar_far_read(int64_t pos, uint8_t *dst, int len)
{
    if (!hist_f || pos < 0 || pos + len > hist_wpos)
        return -1;
    if (hist_dirty)
    {
        fflush(hist_f);
        hist_dirty = false;
    }
    if (fseek(hist_f, (long)pos, SEEK_SET) != 0)
        return -1;
    size_t r = fread(dst, 1, len, hist_f);
    fseek(hist_f, (long)hist_wpos, SEEK_SET);
    return r == (size_t)len ? 0 : -1;
}

// Largest block malloc() can still give (in MB), for error messages
static unsigned free_mb(void)
{
    size_t lo = 0, hi = 32u << 20;
    while (hi - lo > (256u << 10))
    {
        size_t mid = (lo + hi) / 2;
        void *t = malloc(mid);
        if (t)
        {
            free(t);
            lo = mid;
        }
        else
            hi = mid;
    }
    return (unsigned)(lo >> 20);
}

static void set_rar_err(struct archive *a, char *err, size_t errlen)
{
    const char *m = archive_error_string(a);
    unsigned need;
    if (m && sscanf(m, "PPMd needs %u MB", &need) == 1)
        snprintf(err, errlen,
                 "Oude rar (PPMd): heeft %u MB geheugen nodig, de DSi heeft %u MB vrij. "
                 "Pak opnieuw in als RAR5 of .zip.",
                 need, free_mb());
    else
        snprintf(err, errlen, "Rar-fout: %.60s", m ? m : "onbekend");
}

static bool safe_rel(const char *n)
{
    return n[0] != '/' && !strstr(n, "..") && !strchr(n, ':');
}

static size_t pick_window(void)
{
#ifdef RARX_TEST_CAP
    return RARX_TEST_CAP;
#endif
    // Largest power of two that leaves room for everything else
    static const size_t sizes[] = { 8u << 20, 4u << 20, 2u << 20, 1u << 20 };
    for (int i = 0; i < 4; i++)
    {
        void *t = malloc(sizes[i] + (2u << 20));
        if (t)
        {
            free(t);
            return sizes[i];
        }
    }
    return 512u << 10;
}

bool rar_extract_all(const char *rar, const char *dest, http_progress_t *prog,
                     int *files_out, char *err, size_t errlen)
{
    struct archive *a = archive_read_new();
    if (!a)
    {
        snprintf(err, errlen, "Geen geheugen");
        return false;
    }
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);

    rar5_window_cap = pick_window();
    rar5_far_read = rar_far_read;
    rar5_hist_write = rar_hist_write;
    snprintf(hist_path, sizeof(hist_path), "%s/.dsidl_rar.tmp", dest);
    snprintf(ppmd7v_swap_path, sizeof(ppmd7v_swap_path), "%s/.dsidl_ppmd.tmp", dest);
    ppmd7v_yield = rar_yield;
    ppmd7v_abort = 0;
    cur_a = a;
    cur_prog = prog;
    hist_f = NULL;
    hist_wpos = -1;
    hist_dirty = false;
    cur_out = NULL;

    struct stat st;
    if (prog)
    {
        prog->total = stat(rar, &st) == 0 ? (size_t)st.st_size : 0;
        prog->done = 0;
    }

    bool ok = true;
    int files = 0;
    if (archive_read_open_filename(a, rar, 32 * 1024) != ARCHIVE_OK)
    {
        snprintf(err, errlen, "Geen geldig rar-bestand");
        ok = false;
    }

    char *path = malloc(600), *tmp = malloc(610);
    if (!path || !tmp)
        ok = false;

    struct archive_entry *e;
    while (ok)
    {
        int r = archive_read_next_header(a, &e);
        if (r == ARCHIVE_EOF)
            break;
        if (r != ARCHIVE_OK && r != ARCHIVE_WARN)
        {
            set_rar_err(a, err, errlen);
            ok = false;
            break;
        }
        const char *name = archive_entry_pathname(e);
        if (!name || !safe_rel(name))
            continue;
        if (archive_entry_is_encrypted(e))
        {
            snprintf(err, errlen, "Rar met wachtwoord wordt niet ondersteund");
            ok = false;
            break;
        }
        snprintf(path, 600, "%s/%s", dest, name);
        for (char *c = path; *c; c++)
            if (*c == '\\')
                *c = '/';
        if (prog)
            snprintf(prog->status, sizeof(prog->status), "%.60s", name);

        if (archive_entry_filetype(e) == AE_IFDIR)
        {
            mkdir_p(path);
            continue;
        }
        char *slash = strrchr(path, '/');
        if (slash)
        {
            *slash = '\0';
            mkdir_p(path);
            *slash = '/';
        }
        snprintf(tmp, 610, "%s.new", path);
        cur_out = fopen(tmp, "w+b");
        if (!cur_out)
        {
            snprintf(err, errlen, "Kan niet schrijven: %.40s", name);
            ok = false;
            break;
        }
        setvbuf(cur_out, NULL, _IOFBF, 32 * 1024);
        cur_size = 0;

        const void *buf;
        size_t sz;
        la_int64_t off;
        while ((r = archive_read_data_block(a, &buf, &sz, &off)) == ARCHIVE_OK)
        {
            if (off != cur_size)
            {
                // sparse gap: fill with zeros
                fseek(cur_out, (long)off, SEEK_SET);
                cur_size = off;
            }
            if (fwrite(buf, 1, sz, cur_out) != sz)
            {
                snprintf(err, errlen, "Schrijven mislukt (SD vol?)");
                r = ARCHIVE_FATAL;
                break;
            }
            cur_size += sz;
            if (prog)
            {
                prog->done = (size_t)archive_filter_bytes(a, -1);
                if (prog->cancel)
                {
                    snprintf(err, errlen, "Gestopt");
                    r = ARCHIVE_FATAL;
                    break;
                }
            }
            cothread_yield();
        }
        if (fclose(cur_out) != 0 && r == ARCHIVE_EOF)
        {
            snprintf(err, errlen, "Schrijven mislukt (SD vol?)");
            r = ARCHIVE_FATAL;
        }
        cur_out = NULL;
        if (r != ARCHIVE_EOF)
        {
            if (!err[0] && ppmd7v_abort)
                snprintf(err, errlen, "Gestopt");
            else if (!err[0])
                set_rar_err(a, err, errlen);
            remove(tmp);
            ok = false;
            break;
        }
        remove(path);
        if (rename(tmp, path) != 0)
        {
            snprintf(err, errlen, "Vervangen mislukt: %.40s", name);
            remove(tmp);
            ok = false;
            break;
        }
        files++;
    }

    free(path);
    free(tmp);
    archive_read_free(a);
    if (hist_f)
    {
        fclose(hist_f);
        hist_f = NULL;
        remove(hist_path);
    }
    rar5_far_read = NULL;
    rar5_hist_write = NULL;
    ppmd7v_yield = NULL;
    cur_a = NULL;
    cur_prog = NULL;
    if (files_out)
        *files_out = files;
    if (ok && files == 0)
    {
        snprintf(err, errlen, "Rar was leeg");
        ok = false;
    }
    return ok;
}
