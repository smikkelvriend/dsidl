// DSiDL - streaming zip extractor
//
// Reads local file headers one after another (the archives are made by our
// own GitHub Action, so sizes are always in the local headers). Deflate data
// is inflated with miniz's tinfl using only a 32 KB window.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <nds.h>

#include "miniz/miniz.h"
#include "unzip.h"
#include "util.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool safe_name(const char *n)
{
    if (n[0] == '/' || strstr(n, "..") || strchr(n, ':') || strchr(n, '\\'))
        return false;
    return true;
}

// Finds the central directory (works for zips made by any tool, also ones
// that write sizes after the data, like Windows, macOS and phones do)
typedef struct {
    uint32_t crc, csize, usize, lofs;
    uint16_t method, flags;
    char *name;
} zentry_t;

static int read_central(FILE *z, long fsize, zentry_t **out)
{
    *out = NULL;
    long scan = fsize < 66000 ? fsize : 66000;
    uint8_t *tail = malloc(scan);
    if (!tail)
        return -1;
    fseek(z, fsize - scan, SEEK_SET);
    if (fread(tail, 1, scan, z) != (size_t)scan)
    {
        free(tail);
        return -1;
    }
    long eocd = -1;
    for (long i = scan - 22; i >= 0; i--)
        if (rd32(tail + i) == 0x06054b50)
        {
            eocd = i;
            break;
        }
    if (eocd < 0)
    {
        free(tail);
        return -1;
    }
    uint16_t count = rd16(tail + eocd + 10);
    uint32_t cd_size = rd32(tail + eocd + 12), cd_off = rd32(tail + eocd + 16);
    free(tail);
    if (cd_off == 0xFFFFFFFF || count == 0xFFFF)
        return -2; // zip64 not supported

    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    zentry_t *e = calloc(count + 1, sizeof(zentry_t));
    if (!cd || !e)
    {
        free(cd);
        free(e);
        return -1;
    }
    fseek(z, cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, z) != cd_size)
    {
        free(cd);
        free(e);
        return -1;
    }
    int n = 0;
    uint32_t p = 0;
    for (int i = 0; i < count && p + 46 <= cd_size; i++)
    {
        if (rd32(cd + p) != 0x02014b50)
            break;
        uint16_t nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        if (p + 46 + nl > cd_size)
            break;
        e[n].flags = rd16(cd + p + 8);
        e[n].method = rd16(cd + p + 10);
        e[n].crc = rd32(cd + p + 16);
        e[n].csize = rd32(cd + p + 20);
        e[n].usize = rd32(cd + p + 24);
        e[n].lofs = rd32(cd + p + 42);
        e[n].name = malloc(nl + 1);
        if (e[n].name)
        {
            memcpy(e[n].name, cd + p + 46, nl);
            e[n].name[nl] = '\0';
            n++;
        }
        p += 46 + nl + xl + cl;
    }
    free(cd);
    *out = e;
    return n;
}

bool unzip_all(const char *zip, const char *dest_root, http_progress_t *prog,
               int *files_out, char *err, size_t errlen)
{
    crc_init();
    FILE *z = fopen(zip, "rb");
    if (!z)
    {
        snprintf(err, errlen, "Kan zip niet openen");
        return false;
    }
    struct stat st;
    long fsize = stat(zip, &st) == 0 ? (long)st.st_size : 0;
    if (prog)
    {
        prog->total = fsize;
        prog->done = 0;
    }

    zentry_t *ents = NULL;
    int count = read_central(z, fsize, &ents);
    if (count < 0)
    {
        snprintf(err, errlen, count == -2 ? "Zip is te groot (zip64 niet ondersteund)" : "Geen geldig zip-bestand");
        fclose(z);
        return false;
    }

    tinfl_decompressor *inf = malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    uint8_t *in_buf = malloc(16 * 1024);
    char *path = malloc(600), *tmp = malloc(610);
    bool ok = inf && dict && in_buf && path && tmp;
    if (!ok)
        snprintf(err, errlen, "Geen geheugen");

    int files = 0;
    for (int ei = 0; ok && ei < count; ei++)
    {
        zentry_t *en = &ents[ei];
        const char *name = en->name;
        size_t nlen = strlen(name);
        // Skip macOS metadata
        if (strncmp(name, "__MACOSX/", 9) == 0 || strstr(name, "/._") || strncmp(name, "._", 2) == 0)
            continue;
        if ((en->flags & 1) || (en->method != 0 && en->method != 8) || !safe_name(name))
        {
            snprintf(err, errlen, (en->flags & 1) ? "Zip met wachtwoord niet ondersteund" :
                     "Niet ondersteund: %.40s", name);
            ok = false;
            break;
        }

        // Local header: skip name + extra field
        uint8_t h[30];
        fseek(z, en->lofs, SEEK_SET);
        if (fread(h, 1, 30, z) != 30 || rd32(h) != 0x04034b50)
        {
            snprintf(err, errlen, "Zip is beschadigd");
            ok = false;
            break;
        }
        fseek(z, rd16(h + 26) + rd16(h + 28), SEEK_CUR);

        snprintf(path, 600, "%s/%s", dest_root, name);
        if (prog)
            snprintf(prog->status, sizeof(prog->status), "%.60s", name);

        if (nlen > 0 && name[nlen - 1] == '/')
        {
            path[strlen(path) - 1] = '\0';
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
        FILE *out = fopen(tmp, "wb");
        if (!out)
        {
            snprintf(err, errlen, "Kan niet schrijven: %.40s", name);
            ok = false;
            break;
        }
        setvbuf(out, NULL, _IOFBF, 32 * 1024);

        uint32_t got_crc = 0, written = 0, remaining = en->csize;
        if (en->method == 0)
        {
            while (remaining > 0 && ok)
            {
                size_t want = remaining > 16384 ? 16384 : remaining;
                size_t r = fread(in_buf, 1, want, z);
                if (r == 0 || fwrite(in_buf, 1, r, out) != r)
                    ok = false;
                got_crc = crc_update(got_crc, in_buf, r);
                written += r;
                remaining -= r;
                if (prog)
                    prog->done += r;
                cothread_yield();
                if (prog && prog->cancel)
                    ok = false;
            }
        }
        else
        {
            tinfl_init(inf);
            size_t in_avail = 0, in_ofs = 0, dict_ofs = 0;
            while (ok)
            {
                if (in_avail == 0 && remaining > 0)
                {
                    size_t want = remaining > 16384 ? 16384 : remaining;
                    in_avail = fread(in_buf, 1, want, z);
                    if (in_avail == 0)
                    {
                        ok = false;
                        break;
                    }
                    in_ofs = 0;
                    remaining -= in_avail;
                    if (prog)
                        prog->done += in_avail;
                }
                size_t in_bytes = in_avail, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
                tinfl_status s = tinfl_decompress(inf, in_buf + in_ofs, &in_bytes, dict,
                                                  dict + dict_ofs, &out_bytes,
                                                  remaining > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);
                in_ofs += in_bytes;
                in_avail -= in_bytes;
                if (out_bytes)
                {
                    if (fwrite(dict + dict_ofs, 1, out_bytes, out) != out_bytes)
                        ok = false;
                    got_crc = crc_update(got_crc, dict + dict_ofs, out_bytes);
                    written += out_bytes;
                    dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
                }
                if (s == TINFL_STATUS_DONE)
                    break;
                if (s < 0 || (s == TINFL_STATUS_NEEDS_MORE_INPUT && remaining == 0 && in_avail == 0))
                {
                    ok = false;
                    break;
                }
                cothread_yield();
                if (prog && prog->cancel)
                    ok = false;
            }
        }

        if (fclose(out) != 0)
            ok = false;
        if (ok && (written != en->usize || got_crc != en->crc))
        {
            snprintf(err, errlen, "Controle mislukt: %.40s", name);
            ok = false;
        }
        if (!ok)
        {
            if (!err[0])
                snprintf(err, errlen, prog && prog->cancel ? "Gestopt" : "Uitpakken mislukt: %.40s (SD vol?)", name);
            remove(tmp);
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

    fclose(z);
    for (int i = 0; i < count; i++)
        free(ents[i].name);
    free(ents);
    free(inf);
    free(dict);
    free(in_buf);
    free(path);
    free(tmp);
    if (files_out)
        *files_out = files;
    if (ok && files == 0)
    {
        snprintf(err, errlen, "Zip was leeg");
        ok = false;
    }
    return ok;
}
