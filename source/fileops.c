// DSiDL - file operations
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nds.h>

#include "fileops.h"
#include "util.h"

bool fo_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool copy_file(const char *src, const char *dst, http_progress_t *p)
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
    struct stat st;
    if (p)
    {
        p->total = stat(src, &st) == 0 ? (size_t)st.st_size : 0;
        p->done = 0;
        const char *n = strrchr(src, '/');
        snprintf(p->status, sizeof(p->status), "%.60s", n ? n + 1 : src);
    }
    bool ok = true;
    char *buf = malloc(32 * 1024);
    if (!buf)
        ok = false;
    while (ok)
    {
        if (p && p->cancel)
        {
            ok = false;
            break;
        }
        size_t r = fread(buf, 1, 32 * 1024, in);
        if (r == 0)
            break;
        if (fwrite(buf, 1, r, out) != r)
            ok = false;
        if (p)
            p->done += r;
        cothread_yield();
    }
    free(buf);
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    if (!ok)
        remove(dst);
    return ok;
}

static bool copy_any(const char *src, const char *dst, http_progress_t *p, int depth)
{
    if (depth > 16)
        return false;
    if (!fo_is_dir(src))
        return copy_file(src, dst, p);
    if (mkdir(dst, 0777) != 0 && !fo_is_dir(dst))
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
            ok = copy_any(s2, d2, p, depth + 1);
        }
        free(s2);
        free(d2);
    }
    closedir(d);
    return ok;
}

bool fo_copy(const char *src, const char *dst, http_progress_t *p)
{
    return copy_any(src, dst, p, 0);
}

bool fo_delete(const char *path, bool *not_empty)
{
    if (not_empty)
        *not_empty = false;
    if (fo_is_dir(path))
    {
        if (rmdir(path) == 0)
            return true;
        if (not_empty)
            *not_empty = true;
        return false;
    }
    return remove(path) == 0;
}

void fo_unique(const char *dir, const char *name, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", dir, name);
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
        snprintf(out, len, "%s/%s (%d)%s", dir, base, i, ext);
        if (!file_exists(out))
            return;
    }
}
