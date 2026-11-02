// DSiDL - small helpers
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "util.h"

void sanitize_filename(char *name)
{
    // Keep only the last path component
    char *slash = strrchr(name, '/');
    char *bslash = strrchr(name, '\\');
    if (bslash > slash)
        slash = bslash;
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);

    char *o = name;
    for (char *p = name; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || strchr("<>:\"/\\|?*", c))
            continue;
        *o++ = c;
    }
    *o = '\0';

    // No names made only of dots
    bool only_dots = true;
    for (char *p = name; *p; p++)
        if (*p != '.')
            only_dots = false;
    if (only_dots)
        name[0] = '\0';
}

void mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0)
        return;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    // Skip the drive prefix ("sd:/")
    char *start = strchr(tmp, ':');
    start = start ? start + 1 : tmp;
    if (*start == '/')
        start++;
    if (*start == '\0')
        return; // root of the drive always exists

    for (char *p = start; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

void format_size(unsigned long bytes, char *out, size_t outlen)
{
    if (bytes >= 1024UL * 1024UL)
        snprintf(out, outlen, "%lu.%lu MB", bytes / (1024UL * 1024UL),
                 (bytes % (1024UL * 1024UL)) * 10 / (1024UL * 1024UL));
    else if (bytes >= 1024)
        snprintf(out, outlen, "%lu KB", bytes / 1024);
    else
        snprintf(out, outlen, "%lu B", bytes);
}

int version_compare(const char *a, const char *b)
{
    while (*a == 'v' || *a == 'V')
        a++;
    while (*b == 'v' || *b == 'V')
        b++;
    while (*a || *b)
    {
        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);
        if (x != y)
            return x < y ? -1 : 1;
        while (*a && !isdigit((unsigned char)*a))
            a++;
        while (*b && !isdigit((unsigned char)*b))
            b++;
    }
    return 0;
}

bool contains_ci(const char *haystack, const char *needle)
{
    if (!haystack)
        return false;
    if (!needle || !*needle)
        return true;
    size_t n = strlen(needle);
    for (const char *h = haystack; *h; h++)
    {
        size_t i = 0;
        while (i < n && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n)
            return true;
    }
    return false;
}

bool disk_space(const char *drive, unsigned long long *free_b, unsigned long long *total_b)
{
    char root[16];
    snprintf(root, sizeof(root), "%s/", drive);
    struct statvfs st;
    if (statvfs(root, &st) != 0)
        return false;
    if (free_b)
        *free_b = (unsigned long long)st.f_bavail * st.f_frsize;
    if (total_b)
        *total_b = (unsigned long long)st.f_blocks * st.f_frsize;
    return true;
}

void format_size64(unsigned long long b, char *out, size_t outlen)
{
    if (b >= 1024ULL * 1024 * 1024)
        snprintf(out, outlen, "%llu.%llu GB", b >> 30, ((b & ((1ULL << 30) - 1)) * 10) >> 30);
    else
        format_size((unsigned long)b, out, outlen);
}

uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}
