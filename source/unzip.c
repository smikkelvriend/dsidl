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
    if (prog)
    {
        prog->total = stat(zip, &st) == 0 ? (size_t)st.st_size : 0;
        prog->done = 0;
    }

    tinfl_decompressor *inf = malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    uint8_t *in_buf = malloc(16 * 1024);
    char *name = malloc(512), *path = malloc(600), *tmp = malloc(610);
    bool ok = inf && dict && in_buf && name && path && tmp;
    if (!ok)
        snprintf(err, errlen, "Geen geheugen");

    int files = 0;
    while (ok)
    {
        uint8_t h[30];
        if (fread(h, 1, 30, z) != 30)
            break;
        uint32_t sig = rd32(h);
        if (sig != 0x04034b50)
            break; // central directory reached: done

        uint16_t flags = rd16(h + 6), method = rd16(h + 8);
        uint32_t crc = rd32(h + 14), csize = rd32(h + 18), usize = rd32(h + 22);
        uint16_t nlen = rd16(h + 26), xlen = rd16(h + 28);
        if (nlen >= 512 || fread(name, 1, nlen, z) != nlen)
        {
            snprintf(err, errlen, "Zip is beschadigd");
            ok = false;
            break;
        }
        name[nlen] = '\0';
        fseek(z, xlen, SEEK_CUR);

        if ((flags & 8) || (flags & 1) || (method != 0 && method != 8) || !safe_name(name))
        {
            snprintf(err, errlen, "Niet ondersteund: %.40s", name);
            ok = false;
            break;
        }

        snprintf(path, 600, "%s/%s", dest_root, name);
        if (prog)
            snprintf(prog->status, sizeof(prog->status), "%.60s", name);

        if (nlen > 0 && name[nlen - 1] == '/')
        {
            path[strlen(path) - 1] = '\0';
            mkdir_p(path);
            fseek(z, csize, SEEK_CUR);
            continue;
        }

        // Make sure the folder exists
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

        uint32_t got_crc = 0, written = 0;
        uint32_t remaining = csize;

        if (method == 0)
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
            }
            // Skip any unread compressed bytes of this entry
            if (remaining > 0)
                fseek(z, remaining, SEEK_CUR);
        }

        if (fclose(out) != 0)
            ok = false;
        if (ok && (written != usize || got_crc != crc))
        {
            snprintf(err, errlen, "Controle mislukt: %.40s", name);
            ok = false;
        }
        if (!ok)
        {
            if (!err[0])
                snprintf(err, errlen, "Uitpakken mislukt: %.40s (SD vol?)", name);
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
        if (prog && prog->cancel)
        {
            snprintf(err, errlen, "Gestopt na %d bestanden", files);
            ok = false;
            break;
        }
    }

    fclose(z);
    free(inf);
    free(dict);
    free(in_buf);
    free(name);
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
