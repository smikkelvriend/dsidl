// DSiDL - small helpers
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
