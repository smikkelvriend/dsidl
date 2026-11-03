// DSiDL - homebrew downloader for Nintendo DSi
//
// - Browse/search the Universal-DB homebrew catalog and download .nds/.dsi
// - Receive files from a phone/PC browser over WiFi
// - Update itself from GitHub releases

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#include <nds.h>
#include <dswifi9.h>
#include <fat.h>

#include "catalog.h"
#include "http.h"
#include "server.h"
#include "util.h"
#include "wifi.h"
#include "gfx.h"
#include "ui.h"
#include "appui.h"
#include "unzip.h"
#include "rarx.h"
#include "img.h"
#include "sound.h"
#include "ndsscan.h"
#include "fileops.h"
#include "gdrive.h"

#define CATALOG_URL   "https://db.universal-team.net/data/full.json"
#define UPDATE_BASE   "https://github.com/smikkelvriend/dsidl/releases/latest/download/"
// TWiLight Menu++ repacked as a DSi-friendly zip by .github/workflows/twilight.yml
#define TWL_BASE      "https://github.com/smikkelvriend/dsidl/releases/download/twilight/"
// "sd:" on the DSi internal SD card, "fat:" on a flashcard
char drive[8] = "sd:";
static char DATA_DIR[32], CATALOG_FILE[64], SETTINGS_FILE[64];


static catalog_t cat;
static int *view;          // indices into cat.entries after filtering
static int view_count;
static int sel, scroll;
static int dl_choice;      // which download of the selected entry
static char search[32];
static int category;       // 0 = all
static const char *categories[] = { "alles", "game", "emulator", "utility", "app" };
#define NUM_CATS 5

static char self_path[256] = "";
static char save_dir[128];
static char update_base[200] = UPDATE_BASE;
static bool set_dark = false, set_sound = true, set_show_hidden = true;
static int set_accent = 0;
#define wifi_ok wifi_connected()
static char WIFI_FILE[64];

// ---------------------------------------------------------------------------
// Background jobs (run in a cothread with a big stack, UI keeps running)

typedef enum { JOB_NONE, JOB_CATALOG, JOB_DOWNLOAD, JOB_UPDATE_CHECK,
               JOB_UPDATE_INSTALL, JOB_SERVER, JOB_TWL_CHECK, JOB_TWL_DOWNLOAD,
               JOB_TWL_EXTRACT, JOB_COPY, JOB_BOXART, JOB_SCAN, JOB_DRIVE_LIST, JOB_UNZIP, JOB_UNRAR } job_t;

static volatile bool job_running = false;
static volatile int job_result;
static job_t job_type;
static http_progress_t prog;
static server_state_t srv;
static char job_err[300];
static char job_msg[512];
static char job_url[2048];
static char job_path[300];
static char remote_version[32];
static char twl_zip[96];
static int twl_files;
static ndslist_t installed;          // programs found on the SD card
static int box_found, box_ok, box_skip, box_known_missing, box_missing_new;
static bool box_retry_missing;
static char *drive_html;
static int unzip_files;


// ---------------------------------------------------------------------------
// Settings

static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "save_dir=", 9) == 0 && line[9])
            snprintf(save_dir, sizeof(save_dir), "%s", line + 9);
        else if (strncmp(line, "update_url=", 11) == 0 && line[11])
            snprintf(update_base, sizeof(update_base), "%s", line + 11);
        else if (strncmp(line, "theme=", 6) == 0)
            set_dark = strcmp(line + 6, "dark") == 0;
        else if (strncmp(line, "accent=", 7) == 0)
            set_accent = atoi(line + 7);
        else if (strncmp(line, "sound=", 6) == 0)
            set_sound = atoi(line + 6) != 0;
        else if (strncmp(line, "show_hidden=", 12) == 0)
            set_show_hidden = atoi(line + 12) != 0;
    }
    fclose(f);
}

static void write_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "# DSiDL instellingen\n");
    fprintf(f, "save_dir=%s\n", save_dir);
    fprintf(f, "update_url=%s\n", update_base);
    fprintf(f, "theme=%s\n", set_dark ? "dark" : "light");
    fprintf(f, "accent=%d\n", set_accent);
    fprintf(f, "sound=%d\n", set_sound ? 1 : 0);
    fprintf(f, "show_hidden=%d\n", set_show_hidden ? 1 : 0);
    fclose(f);
}

static void save_default_settings(void)
{
    if (file_exists(SETTINGS_FILE))
        return;
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "# DSiDL instellingen\n");
    fprintf(f, "save_dir=%s\n", save_dir);
    fprintf(f, "update_url=%s\n", update_base);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Catalog handling

static void rebuild_view(void)
{
    free(view);
    view = malloc(sizeof(int) * (cat.count + 1));
    view_count = 0;
    for (int i = 0; i < cat.count; i++)
    {
        cat_entry_t *e = &cat.entries[i];
        if (category != 0 && (!e->category || strcasecmp(e->category, categories[category]) != 0))
            continue;
        if (search[0] && !contains_ci(e->title, search) && !contains_ci(e->author, search) &&
            !contains_ci(e->description, search))
            continue;
        view[view_count++] = i;
    }
    sel = 0;
    scroll = 0;
    dl_choice = 0;
}

static void icons_reset(void);
static char load_err[64] = "";

static bool load_catalog_file(void)
{
    FILE *f = fopen(CATALOG_FILE, "rb");
    if (!f)
    {
        snprintf(load_err, sizeof(load_err), "geen bestand");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0)
    {
        fclose(f);
        snprintf(load_err, sizeof(load_err), "leeg bestand");
        return false;
    }
    char *buf = malloc(n + 1);
    if (!buf)
    {
        fclose(f);
        snprintf(load_err, sizeof(load_err), "te weinig geheugen (%ld KB)", n / 1024);
        return false;
    }
    // Read in chunks; some storage drivers dislike huge single reads
    size_t r = 0;
    while (r < (size_t)n)
    {
        size_t want = (size_t)n - r;
        if (want > 16 * 1024)
            want = 16 * 1024;
        size_t got = fread(buf + r, 1, want, f);
        if (got == 0)
            break;
        r += got;
    }
    fclose(f);
    buf[r] = '\0';

    catalog_free(&cat);
    int c = catalog_parse(&cat, buf, r);
    if (c <= 0)
    {
        long o = catalog_error_offset;
        catalog_free(&cat);
        snprintf(load_err, sizeof(load_err), "lijst beschadigd (%ld)", o);
        free(buf);
        return false;
    }
    free(buf);
    rebuild_view();
    icons_reset();
    return true;
}

static bool catalog_is_old(void)
{
    struct stat st;
    if (stat(CATALOG_FILE, &st) != 0)
        return true;
    time_t now = time(NULL);
    return now - st.st_mtime > 24 * 60 * 60 || st.st_mtime > now + 60;
}

// ---------------------------------------------------------------------------
// Jobs

// ---------------------------------------------------------------------------
// Boxart for TWiLight Menu++ (covers from GameTDB)

static const char *box_regions(char c)
{
    switch (c)
    {
        case 'E': case 'O': return "US,EN";
        case 'J': return "JA,EN";
        case 'K': return "KO,EN";
        case 'D': return "DE,EN";
        case 'F': return "FR,EN";
        case 'S': return "ES,EN";
        case 'I': return "IT,EN";
        case 'H': return "NL,EN";
        case 'U': return "AU,EN";
        default: return "EN,US";
    }
}

// Remembered between runs (sd:/_dsidl/boxart_cache.txt):
//   "C <hash> <size> <code>"  game code of a file, so its header is not read again
//   "M <code>"                GameTDB has no cover for this code (skipped next time)
typedef struct { uint32_t key; char code[5]; bool seen; } box_code_t;
static bool box_prune;
static box_code_t *box_codes;
static int box_ncodes, box_capcodes;
static char (*box_miss)[5];
static int box_nmiss, box_capmiss;

static uint32_t box_key(const ndsfile_t *f)
{
    uint32_t h = 2166136261u;
    for (const char *c = f->path; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    return h ^ (uint32_t)f->size * 2654435761u;
}

static void box_add_code(uint32_t key, const char *code)
{
    if (box_ncodes == box_capcodes)
    {
        int nc = box_capcodes ? box_capcodes * 2 : 256;
        box_code_t *n = realloc(box_codes, sizeof(box_code_t) * nc);
        if (!n)
            return;
        box_codes = n;
        box_capcodes = nc;
    }
    box_codes[box_ncodes].key = key;
    box_codes[box_ncodes].seen = true;
    snprintf(box_codes[box_ncodes].code, 5, "%.4s", code);
    box_ncodes++;
}

static const char *box_find_code(uint32_t key)
{
    for (int i = 0; i < box_ncodes; i++)
        if (box_codes[i].key == key)
        {
            box_codes[i].seen = true;
            return box_codes[i].code;
        }
    return NULL;
}

static bool box_is_missing(const char *code)
{
    for (int i = 0; i < box_nmiss; i++)
        if (!strcmp(box_miss[i], code))
            return true;
    return false;
}

static void box_add_missing(const char *code)
{
    if (box_is_missing(code))
        return;
    if (box_nmiss == box_capmiss)
    {
        int nc = box_capmiss ? box_capmiss * 2 : 64;
        char (*n)[5] = realloc(box_miss, 5 * nc);
        if (!n)
            return;
        box_miss = n;
        box_capmiss = nc;
    }
    snprintf(box_miss[box_nmiss++], 5, "%.4s", code);
}

static void box_cache_load(void)
{
    box_ncodes = box_nmiss = 0;
    char path[96], line[64];
    snprintf(path, sizeof(path), "%s/boxart_cache.txt", DATA_DIR);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        unsigned long key;
        char code[8];
        if (line[0] == 'C' && sscanf(line + 2, "%lx %7s", &key, code) == 2 && strlen(code) == 4)
        {
            box_add_code((uint32_t)key, code);
            if (box_ncodes)
                box_codes[box_ncodes - 1].seen = false;
        }
        else if (line[0] == 'M' && sscanf(line + 2, "%7s", code) == 1 && strlen(code) == 4)
            box_add_missing(code);
    }
    fclose(f);
}

static void box_cache_save(void)
{
    char path[96], tmp[100];
    snprintf(path, sizeof(path), "%s/boxart_cache.txt", DATA_DIR);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < box_ncodes; i++)
        if (!box_prune || box_codes[i].seen) // forget files that are gone
            fprintf(f, "C %08lx %s\n", (unsigned long)box_codes[i].key, box_codes[i].code);
    for (int i = 0; i < box_nmiss; i++)
        fprintf(f, "M %s\n", box_miss[i]);
    if (fclose(f) == 0)
    {
        remove(path);
        rename(tmp, path);
    }
}

static int box_missing_count(void)
{
    box_cache_load();
    return box_nmiss;
}

static int boxart_run(void)
{
    char dir[96];
    snprintf(dir, sizeof(dir), "%s/_nds/TWiLightMenu/boxart", drive);
    mkdir_p(dir);
    box_found = box_ok = box_skip = box_known_missing = box_missing_new = 0;
    box_cache_load();
    box_prune = false;
    if (box_retry_missing)
        box_nmiss = 0;

    snprintf(prog.status, sizeof(prog.status), "Spellen zoeken...");
    nds_free(&installed);
    nds_scan(&installed, drive, 1000);

    // Cancel in the UI must also stop the cover downloads below
    http_set_cancel_flag(&prog.cancel);

    // Collect game codes (skip homebrew without a real code)
    int n = installed.count;
    prog.counts = true;
    prog.total = n;
    for (int i = 0; i < n && !prog.cancel; i++)
    {
        ndsfile_t *f = &installed.items[i];
        prog.done = i;
        uint32_t key = box_key(f);
        const char *known = box_find_code(key);
        if (known)
            snprintf(f->code, sizeof(f->code), "%s", known);
        else
        {
            nds_read_banner(f);
            box_add_code(key, strlen(f->code) == 4 ? f->code : "----");
        }
        if (strlen(f->code) != 4 || strcmp(f->code, "####") == 0 || strcmp(f->code, "----") == 0 ||
            strncmp(f->code, "HB", 2) == 0)
            continue;
        box_found++;
        char out[140];
        snprintf(out, sizeof(out), "%s/%s.png", dir, f->code);
        if (file_exists(out))
        {
            box_skip++;
            continue;
        }
        if (box_is_missing(f->code))
        {
            box_known_missing++;
            continue;
        }
        if (known)
            nds_read_banner(f); // title for the status line
        if (!prog.cancel)
            snprintf(prog.status, sizeof(prog.status), "%.40s", f->title);

        char regs[16];
        snprintf(regs, sizeof(regs), "%s", box_regions(f->code[3]));
        bool got = false, all404 = true;
        for (char *reg = strtok(regs, ","); reg && !got && !prog.cancel; reg = strtok(NULL, ","))
        {
            char url[160], err[96], tmp[150];
            snprintf(url, sizeof(url), "https://art.gametdb.com/ds/coverS/%s/%s.png", reg, f->code);
            snprintf(tmp, sizeof(tmp), "%s.part", out);
            http_progress_t pp = { 0 };
            int code = http_get_file(url, tmp, &pp, err, sizeof(err));
            if (code != 404)
                all404 = false;
            if (code == 200)
            {
                // TWiLight shows up to 208x143; shrink bigger covers to 128x115
                int w, h;
                if (img_info(tmp, &w, &h) && (w > 208 || h > 143))
                {
                    int sw, sh;
                    uint8_t *rgba = img_load_rgba(tmp, &sw, &sh);
                    uint8_t *small = rgba ? img_resize_rgba(rgba, sw, sh, 128, 115) : NULL;
                    free(rgba);
                    if (small)
                    {
                        img_write_png(out, 128, 115, 4, small);
                        free(small);
                    }
                    remove(tmp);
                }
                else
                {
                    remove(out);
                    rename(tmp, out);
                }
                got = file_exists(out);
            }
            else
                remove(tmp);
        }
        if (got)
            box_ok++;
        else if (all404 && !prog.cancel)
        {
            box_add_missing(f->code);
            box_missing_new++;
        }
        if ((i & 31) == 31)
            box_cache_save(); // keep progress if the DSi is switched off
    }
    box_prune = !prog.cancel;
    box_cache_save();
    http_set_cancel_flag(NULL);
    prog.done = n;
    if (prog.cancel)
        snprintf(job_err, sizeof(job_err), "Gestopt");
    return 200;
}

static int job_main(void *arg)
{
    (void)arg;
    int r = -1;

    switch (job_type)
    {
        case JOB_CATALOG:
        {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s.part", CATALOG_FILE);
            r = http_get_file(CATALOG_URL, tmp, &prog, job_err, sizeof(job_err));
            if (r == 200)
            {
                remove(CATALOG_FILE);
                rename(tmp, CATALOG_FILE);
            }
            break;
        }
        case JOB_DOWNLOAD:
        {
            char tmp[310];
            snprintf(tmp, sizeof(tmp), "%s.part", job_path);
            r = http_get_file(job_url, tmp, &prog, job_err, sizeof(job_err));
            if (r == 200)
            {
                remove(job_path);
                if (rename(tmp, job_path) != 0)
                {
                    snprintf(job_err, sizeof(job_err), "Hernoemen mislukt");
                    r = -1;
                }
            }
            break;
        }
        case JOB_UPDATE_CHECK:
        {
            char url[256];
            char *data = NULL;
            snprintf(url, sizeof(url), "%sversion.txt", update_base);
            r = http_get_mem(url, &data, NULL, 256, &prog, job_err, sizeof(job_err));
            if (r == 200 && data)
            {
                char *p = data;
                while (*p == ' ' || *p == '\n' || *p == '\r')
                    p++;
                p[strcspn(p, " \r\n")] = '\0';
                snprintf(remote_version, sizeof(remote_version), "%s", p);
            }
            else if (r == 404)
            {
                snprintf(job_err, sizeof(job_err), "Nog geen release gevonden");
            }
            free(data);
            break;
        }
        case JOB_UPDATE_INSTALL:
        {
            char url[256], tmp[300];
            snprintf(url, sizeof(url), "%sdsidl.nds", update_base);
            snprintf(tmp, sizeof(tmp), "%s.new", self_path);
            r = http_get_file(url, tmp, &prog, job_err, sizeof(job_err));
            if (r == 200)
            {
                // Sanity check: the new file must look like a DS ROM
                FILE *f = fopen(tmp, "rb");
                unsigned char hdr[0x200] = { 0 };
                size_t n = f ? fread(hdr, 1, sizeof(hdr), f) : 0;
                if (f)
                    fclose(f);
                // 0x15C: logo CRC 0xCF56 on every valid DS ROM
                if (n < sizeof(hdr) || hdr[0x15C] != 0x56 || hdr[0x15D] != 0xCF)
                {
                    remove(tmp);
                    snprintf(job_err, sizeof(job_err), "Gedownload bestand is geen DS-rom");
                    r = -1;
                }
                else
                {
                    char bak[310];
                    snprintf(bak, sizeof(bak), "%s.old", self_path);
                    remove(bak);
                    rename(self_path, bak);
                    if (rename(tmp, self_path) != 0)
                    {
                        rename(bak, self_path); // put the old version back
                        snprintf(job_err, sizeof(job_err), "Vervangen mislukt");
                        r = -1;
                    }
                    else
                    {
                        remove(bak);
                    }
                }
            }
            break;
        }
        case JOB_TWL_CHECK:
        {
            char *data = NULL;
            r = http_get_mem(TWL_BASE "twilight-version.txt", &data, NULL, 256, &prog, job_err, sizeof(job_err));
            if (r == 200 && data)
            {
                char *p = data;
                while (*p == ' ' || *p == '\n' || *p == '\r')
                    p++;
                p[strcspn(p, " \r\n")] = '\0';
                snprintf(remote_version, sizeof(remote_version), "%s", p);
            }
            else if (r == 404)
                snprintf(job_err, sizeof(job_err), "TWiLight is nog niet klaargezet op GitHub");
            free(data);
            break;
        }
        case JOB_TWL_DOWNLOAD:
            r = http_get_file(TWL_BASE "twilight-dsi.zip", twl_zip, &prog, job_err, sizeof(job_err));
            break;
        case JOB_TWL_EXTRACT:
            r = unzip_all(twl_zip, drive, &prog, &twl_files, job_err, sizeof(job_err)) ? 200 : -1;
            break;
        case JOB_COPY:
            // job_path = source, job_url = destination
            r = fo_copy(job_path, job_url, &prog) ? 200 : -1;
            if (r != 200)
                snprintf(job_err, sizeof(job_err), prog.cancel ? "Gestopt" : "Kopiëren mislukt (SD vol?)");
            break;
        case JOB_DRIVE_LIST:
            free(drive_html);
            drive_html = NULL;
            r = http_get_mem(job_url, &drive_html, NULL, 4 * 1024 * 1024, &prog, job_err, sizeof(job_err));
            break;
        case JOB_UNZIP:
            // job_path = zip, job_url = destination folder
            r = unzip_all(job_path, job_url, &prog, &unzip_files, job_err, sizeof(job_err)) ? 200 : -1;
            break;
        case JOB_UNRAR:
            // job_path = rar, job_url = destination folder
            r = rar_extract_all(job_path, job_url, &prog, &unzip_files, job_err, sizeof(job_err)) ? 200 : -1;
            break;
        case JOB_SCAN:
            nds_free(&installed);
            snprintf(prog.status, sizeof(prog.status), "SD-kaart doorzoeken...");
            nds_scan(&installed, drive, 1000);
            r = 200;
            break;
        case JOB_BOXART:
            r = boxart_run();
            break;
        case JOB_SERVER:
        {
            r = server_run(&srv, save_dir) ? 200 : -1;
            if (r != 200)
                snprintf(job_err, sizeof(job_err), "Kan poort 80 niet openen");
            break;
        }
        default:
            break;
    }

    job_result = r;
    job_running = false;
    return 0;
}

// ---------------------------------------------------------------------------
// Background downloads (icons, screenshots) - never run together with a job

static volatile bool bg_busy;
static volatile int bg_result;
static char bg_url[512], bg_path[160];
static http_progress_t bg_prog;

static int bg_main(void *arg)
{
    (void)arg;
    char err[96];
    char tmp[170];
    snprintf(tmp, sizeof(tmp), "%s.part", bg_path);
    bg_result = http_get_file(bg_url, tmp, &bg_prog, err, sizeof(err));
    if (bg_result == 200)
    {
        remove(bg_path);
        rename(tmp, bg_path);
    }
    bg_busy = false;
    return 0;
}

static bool bg_fetch(const char *url, const char *path)
{
    if (bg_busy || job_running || !wifi_connected())
        return false;
    snprintf(bg_url, sizeof(bg_url), "%s", url);
    snprintf(bg_path, sizeof(bg_path), "%s", path);
    memset((void *)&bg_prog, 0, sizeof(bg_prog));
    bg_busy = true;
    if (cothread_create(bg_main, NULL, 80 * 1024, COTHREAD_DETACHED) < 0)
    {
        bg_busy = false;
        return false;
    }
    return true;
}

static void bg_stop(void)
{
    if (!bg_busy)
        return;
    bg_prog.cancel = true;
    while (bg_busy)
        app_wait_frame();
}

// Icon cache: one state per catalog entry
enum { ICON_UNKNOWN, ICON_LOADED, ICON_NONE, ICON_FETCHING };
static uint8_t *icon_state;
static img_t *icon_small, *icon_big;
static int icon_count;
static int icon_fetch_idx = -1;
static char ICON_DIR[48], SHOT_DIR[48];

static void icons_reset(void)
{
    for (int i = 0; i < icon_count; i++)
    {
        img_free(&icon_small[i]);
        img_free(&icon_big[i]);
    }
    free(icon_state);
    free(icon_small);
    free(icon_big);
    icon_count = cat.count;
    icon_state = calloc(icon_count + 1, 1);
    icon_small = calloc(icon_count + 1, sizeof(img_t));
    icon_big = calloc(icon_count + 1, sizeof(img_t));
    icon_fetch_idx = -1;
}

static void cache_path(const char *dir, const char *url, char *out, size_t len)
{
    snprintf(out, len, "%s/%08lx.png", dir, (unsigned long)fnv1a(url));
}

// Loads the icon of entry i from the SD cache. Returns true if loaded.
static bool icon_load(int i)
{
    cat_entry_t *e = &cat.entries[i];
    if (!e->icon)
    {
        icon_state[i] = ICON_NONE;
        return false;
    }
    char path[96];
    cache_path(ICON_DIR, e->icon, path, sizeof(path));
    img_t full;
    if (!file_exists(path) || !img_load(path, &full, 64, 64))
        return false;
    bool ok = img_scaled(&full, &icon_big[i], 44, 44) && img_scaled(&full, &icon_small[i], 22, 22);
    img_free(&full);
    icon_state[i] = ok ? ICON_LOADED : ICON_NONE;
    return ok;
}

// Called every frame from the browser: loads cached icons of visible rows
// (max one per frame) and starts downloads of missing ones when idle.
static bool icons_tick(int idle_frames)
{
    bool changed = false;
    if (!icon_state || icon_count != cat.count)
        return false;

    // A download finished?
    if (icon_fetch_idx >= 0 && !bg_busy)
    {
        int i = icon_fetch_idx;
        icon_fetch_idx = -1;
        if (bg_result != 200 || !icon_load(i))
            icon_state[i] = ICON_NONE;
        changed = true;
    }

    // Visible rows first, then the rest of the list
    int first = scroll, last = scroll + BROWSER_ROWS;
    for (int pass = 0; pass < 2; pass++)
    {
        int from = pass == 0 ? first : 0, to = pass == 0 ? last : view_count;
        for (int v = from; v < to && v < view_count; v++)
        {
            int i = view[v];
            if (icon_state[i] != ICON_UNKNOWN)
                continue;
            if (icon_load(i))
                return true; // one decode per frame
            if (icon_state[i] == ICON_NONE)
                continue;
            // Not cached: download when the user is not busy with the UI
            if (idle_frames > (pass == 0 ? 20 : 90) && icon_fetch_idx < 0)
            {
                char path[96];
                cache_path(ICON_DIR, cat.entries[i].icon, path, sizeof(path));
                if (bg_fetch(cat.entries[i].icon, path))
                {
                    icon_state[i] = ICON_FETCHING;
                    icon_fetch_idx = i;
                }
            }
            return changed;
        }
    }
    return changed;
}

static const img_t *icon_of(int i, bool big)
{
    if (!icon_state || i >= icon_count || icon_state[i] != ICON_LOADED)
        return NULL;
    return big ? &icon_big[i] : &icon_small[i];
}

static void start_job(job_t type)
{
    bg_stop();
    memset((void *)&prog, 0, sizeof(prog));
    job_err[0] = '\0';
    job_type = type;
    job_result = 0;
    job_running = true;
    if (cothread_create(job_main, NULL, 96 * 1024, COTHREAD_DETACHED) < 0)
    {
        job_running = false;
        job_result = -1;
        snprintf(job_err, sizeof(job_err), "Kan taak niet starten");
    }
}

// ---------------------------------------------------------------------------
// Speed / remaining time measurement (g_frames counts 60 Hz frames)

typedef struct {
    u32 last_frame;
    size_t last_done;
    u32 speed; // bytes per second, smoothed; 0 = unknown
} rate_t;

static rate_t rate;

static void rate_reset(void)
{
    rate.last_frame = g_frames;
    rate.last_done = 0;
    rate.speed = 0;
}

static void rate_update(size_t done)
{
    if (done < rate.last_done)
    {
        rate.last_done = done;
        rate.last_frame = g_frames;
        rate.speed = 0;
        return;
    }
    u32 frames = g_frames - rate.last_frame;
    if (frames < 60)
        return;
    u32 inst = (u32)((unsigned long long)(done - rate.last_done) * 60 / frames);
    rate.speed = rate.speed == 0 ? inst : (rate.speed * 3 + inst) / 4;
    rate.last_done = done;
    rate.last_frame = g_frames;
}

static long rate_eta(size_t done, size_t total)
{
    if (!rate.speed || !total || done >= total)
        return total && done >= total ? 0 : -1;
    return (long)((total - done) / rate.speed) + 1;
}

// ---------------------------------------------------------------------------
// Browser screen

static const char *tab_labels[NUM_CATS] = { "Alles", "Games", "Emu", "Tools", "Apps" };
static ui_row_t *rows;
static char (*row_sub)[56];

static void build_rows(void)
{
    free(rows);
    free(row_sub);
    rows = calloc(view_count + 1, sizeof(ui_row_t));
    row_sub = calloc(view_count + 1, sizeof(*row_sub));
    if (!rows || !row_sub)
        return;
    for (int i = 0; i < view_count; i++)
    {
        cat_entry_t *e = &cat.entries[view[i]];
        char sz[16];
        format_size(e->dl[0].size, sz, sizeof(sz));
        if (e->author && e->author[0])
            snprintf(row_sub[i], sizeof(row_sub[i]), "%.36s - %s", e->author, sz);
        else
            snprintf(row_sub[i], sizeof(row_sub[i]), "%s", sz);
        rows[i].title = e->title;
        rows[i].subtitle = row_sub[i];
        rows[i].category = e->category;
        rows[i].badge = NULL;
        for (int d = 0; d < e->num_dl; d++)
            if (nds_has_name(&installed, e->dl[d].name))
                rows[i].badge = "Geïnstalleerd";
    }
}

// Animation state of the browser
static int a_scroll, a_cursor, a_tab, a_slide;
static int top_sel = -1, top_dl = -1, top_count = -1;
#define TAB_W ((SCR_W - 12) / NUM_CATS)

static void draw_browser_top(void)
{
    bool c;
    int bars;
    const char *ssid;
    app_wifi_info(&c, &bars, &ssid);

    if (view_count > 0)
    {
        cat_entry_t *e = &cat.entries[view[sel]];
        if (dl_choice >= e->num_dl)
            dl_choice = 0;
        ui_item_t it = { e->title, e->author, e->version, e->category, e->description,
                         e->dl[dl_choice].name, e->dl[dl_choice].size, dl_choice, e->num_dl,
                         icon_of(view[sel], true) };
        ui_browser_top(&it, c, bars, ssid);
    }
    else
    {
        ui_browser_top(NULL, c, bars, ssid);
    }
    // New item slides in from the right inside the card
    if (a_slide)
        gfx_shift_x(&scr_top, 10, 28, 236, 130, a_slide, C_CARD);
}

static void draw_browser_bottom(void)
{
    for (int v = 0; v < view_count && rows; v++)
        rows[v].icon = icon_of(view[v], false);
    const char *empty = cat.count == 0 ? "Nog geen lijst - X > Lijst" : "Niets gevonden";
    ui_browser_bottom(rows, view_count, sel, a_scroll, a_cursor, search, tab_labels, NUM_CATS,
                      category, a_tab, empty);
}

// Draws the browser, advancing animations one step. Returns true while
// something is still moving (call again next frame).
static bool draw_browser_step(bool force)
{
    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + BROWSER_ROWS)
        scroll = sel - BROWSER_ROWS + 1;
    if (scroll < 0)
        scroll = 0;

    // Selection change -> top card slides
    bool top_changed = force || sel != top_sel || dl_choice != top_dl || view_count != top_count;
    if (sel != top_sel && top_sel >= 0 && !force)
        a_slide = 28;
    top_sel = sel;
    top_dl = dl_choice;
    top_count = view_count;

    int ns = app_ease(a_scroll, scroll * BROWSER_STEP);
    int nc = app_ease(a_cursor, sel * BROWSER_STEP);
    int nt = app_ease(a_tab, category * TAB_W);
    int nsl = app_ease(a_slide, 0);
    bool bottom_moving = ns != a_scroll || nc != a_cursor || nt != a_tab;
    bool top_moving = nsl != a_slide;
    a_scroll = ns;
    a_cursor = nc;
    a_tab = nt;
    a_slide = nsl;

    if (force || top_changed || top_moving)
        draw_browser_top();
    if (force || bottom_moving || top_changed)
        draw_browser_bottom();

    if (force || ((top_changed || top_moving) && (bottom_moving || top_changed)))
        gfx_present();
    else if (top_changed || top_moving)
        gfx_present_top();
    else if (bottom_moving)
        gfx_present_bot();

    return bottom_moving || top_moving || a_scroll != scroll * BROWSER_STEP ||
           a_cursor != sel * BROWSER_STEP || a_tab != category * TAB_W || a_slide != 0;
}

// Full redraw without animation steps (used under dialogs)
static void draw_browser(void)
{
    draw_browser_top();
    draw_browser_bottom();
    gfx_present();
}

// ---------------------------------------------------------------------------
// Jobs with a progress screen

// Runs a job while showing progress. Returns job result.
static int run_job_ui(job_t type, const char *title, const char *name)
{
    // First let the progress screen slide in completely, then start the
    // (CPU heavy) work, so the animation never has to compete with it.
    memset((void *)&prog, 0, sizeof(prog));
    snprintf(prog.status, sizeof(prog.status), "Starten...");
    gfx_transition(TR_UP_BOT);
    ui_progress_screen(title, name, prog.status, 0, 0, 0, -1, "Annuleren");
    gfx_present();
    start_job(type);
    rate_reset();
    int frame = 0;
    unsigned long shown = 0;
    while (job_running)
    {
        // The bar glides towards the real value instead of jumping
        unsigned long target = prog.done;
        if (target < shown)
            shown = target;
        unsigned long d = target - shown;
        shown += d > 8 ? d / 5 : d;
        // Redraw every 3rd frame: smooth enough, leaves CPU for the download
        if ((frame++ % 3) == 0)
        {
            rate_update(prog.done);
            ui_progress_counts = prog.counts;
            ui_progress_screen(title, name, prog.status, prog.counts ? prog.done : shown, prog.total, rate.speed,
                               rate_eta(prog.done, prog.total),
                               prog.cancel ? "Bezig..." : "Annuleren");
            gfx_present_bot();
        }
        app_wait_frame();
        input_t in = app_input();
        if ((in.down & KEY_B) || in.hit == HIT_BTN)
        {
            prog.cancel = true;
            snprintf(prog.status, sizeof(prog.status), "Annuleren...");
        }
    }
    ui_progress_counts = 0;
    return job_result;
}

// ---------------------------------------------------------------------------
// Folder picker

#define PICK_MAX 256

static int cmp_names(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static bool is_drive_root(const char *dir)
{
    const char *p = strchr(dir, ':');
    return p && (p[1] == '\0' || (p[1] == '/' && p[2] == '\0'));
}

static int list_dirs(const char *dir, char **names, int max)
{
    char path[300];
    if (is_drive_root(dir))
        snprintf(path, sizeof(path), "%s/", drive);
    else
        snprintf(path, sizeof(path), "%s", dir);

    int n = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max)
    {
        if (de->d_type != DT_DIR || de->d_name[0] == '.')
            continue;
        if (strcasecmp(de->d_name, "_nds") == 0 || strcasecmp(de->d_name, "_dsidl") == 0)
            continue;
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char *), cmp_names);
    return n;
}

static void path_join(char *out, size_t outlen, const char *dir, const char *name)
{
    if (is_drive_root(dir))
        snprintf(out, outlen, "%s/%s", drive, name);
    else
        snprintf(out, outlen, "%s/%s", dir, name);
}

static bool folder_picker(char *dir, size_t dirlen)
{
    char cur[256];
    snprintf(cur, sizeof(cur), "%s", dir);
    DIR *test = opendir(is_drive_root(cur) ? "sd:/" : cur);
    if (test)
        closedir(test);
    else
        snprintf(cur, sizeof(cur), "%s", drive);

    char *names[PICK_MAX];
    int n = list_dirs(cur, names, PICK_MAX);
    int sel_i = 0, scr = 0, drag = 0;
    int a_s = 0, a_c = 0;
    bool dirty = true, chosen = false;
    gfx_transition(TR_LEFT);

    static const char *labels[] = { "Hier opslaan", "Nieuwe map", "Annuleren" };
    static const char *keys[] = { "X", "Y", "B" };

    while (1)
    {
        bool root = is_drive_root(cur);
        int total = n + (root ? 0 : 1);

        if (dirty)
        {
            if (sel_i >= total)
                sel_i = total > 0 ? total - 1 : 0;
            if (sel_i < scr)
                scr = sel_i;
            if (sel_i >= scr + LIST_ROWS)
                scr = sel_i - LIST_ROWS + 1;

            ui_row_t r[PICK_MAX + 1];
            for (int i = 0; i < total; i++)
            {
                r[i].title = (!root && i == 0) ? ".. (omhoog)" : names[i - (root ? 0 : 1)];
                r[i].subtitle = NULL;
                r[i].category = NULL;
                r[i].badge = NULL;
            }
            char shown[300];
            snprintf(shown, sizeof(shown), "%s%s", cur, root ? "/" : "");

            bool c;
            int bars;
            const char *ssid;
            app_wifi_info(&c, &bars, &ssid);
            const char *info[] = {
                "Kies de map waarin de download komt.",
                "",
                "!A: map openen   L: omhoog",
                "!X: hier opslaan   Y: nieuwe map",
                "",
                "DSiDL onthoudt je keuze.",
            };
            int ns = app_ease(a_s, scr * LIST_STEP), nc = app_ease(a_c, sel_i * LIST_STEP);
            bool moving = ns != scr * LIST_STEP || nc != sel_i * LIST_STEP;
            a_s = ns;
            a_c = nc;
            ui_info_top("Map kiezen", info, 6, c, bars, ssid);
            ui_list_screen("Map kiezen", shown, r, total, sel_i, a_s, a_c, 1, labels, keys, 3,
                           "Geen submappen");
            gfx_present();
            dirty = moving;
        }

        app_wait_frame();
        input_t in = app_input();
        u32 k = in.down;

        if (in.drag_dy)
        {
            drag += in.drag_dy;
            while (drag <= -24 && scr + LIST_ROWS < total) { scr++; drag += 24; if (sel_i < scr) sel_i = scr; dirty = true; }
            while (drag >= 24 && scr > 0) { scr--; drag -= 24; if (sel_i >= scr + LIST_ROWS) sel_i = scr + LIST_ROWS - 1; dirty = true; }
        }
        if (!in.touching)
            drag = 0;

        if ((k & KEY_DOWN) && sel_i + 1 < total) { sel_i++; dirty = true; }
        if ((k & KEY_UP) && sel_i > 0) { sel_i--; dirty = true; }
        if (k & KEY_RIGHT) { sel_i += LIST_ROWS; if (sel_i >= total) sel_i = total - 1; if (sel_i < 0) sel_i = 0; dirty = true; }
        if (k & KEY_LEFT) { sel_i -= LIST_ROWS; if (sel_i < 0) sel_i = 0; dirty = true; }

        if ((k & KEY_B) || in.hit == HIT_BTN + 2)
            break;
        if ((k & KEY_X) || in.hit == HIT_BTN + 0)
        {
            chosen = true;
            break;
        }

        bool go_up = false;
        const char *enter = NULL;
        bool activate = (k & KEY_A) != 0;
        if (in.hit >= HIT_ROW && in.hit < HIT_ROW + 1000)
        {
            if (in.hit - HIT_ROW == sel_i)
                activate = true;
            else
            {
                sel_i = in.hit - HIT_ROW;
                dirty = true;
            }
        }
        if (activate && total > 0)
        {
            if (!root && sel_i == 0)
                go_up = true;
            else
                enter = names[sel_i - (root ? 0 : 1)];
        }
        if ((k & KEY_L) && !root)
            go_up = true;

        if ((k & KEY_Y) || in.hit == HIT_BTN + 1)
        {
            char nm[64] = "";
            if (app_text_input("Naam van de nieuwe map", "bv. spellen", nm, sizeof(nm), false))
            {
                sanitize_filename(nm);
                if (nm[0])
                {
                    char np[300];
                    path_join(np, sizeof(np), cur, nm);
                    mkdir(np, 0777);
                    snprintf(cur, sizeof(cur), "%s", np);
                }
            }
            for (int i = 0; i < n; i++)
                free(names[i]);
            n = list_dirs(cur, names, PICK_MAX);
            sel_i = scr = 0;
            dirty = true;
            continue;
        }

        if (go_up || enter)
        {
            if (go_up)
            {
                char *slash = strrchr(cur, '/');
                if (slash)
                {
                    *slash = '\0';
                    if (!strchr(cur, '/'))
                        snprintf(cur, sizeof(cur), "%s", drive);
                }
            }
            else
            {
                char np[300];
                path_join(np, sizeof(np), cur, enter);
                snprintf(cur, sizeof(cur), "%s", np);
            }
            for (int i = 0; i < n; i++)
                free(names[i]);
            n = list_dirs(cur, names, PICK_MAX);
            sel_i = scr = 0;
            a_s = a_c = 0;
            gfx_transition(go_up ? TR_RIGHT : TR_LEFT);
            dirty = true;
        }
    }

    for (int i = 0; i < n; i++)
        free(names[i]);
    gfx_transition(TR_RIGHT);

    if (chosen)
        snprintf(dir, dirlen, "%s", is_drive_root(cur) ? drive : cur);
    return chosen;
}

// ---------------------------------------------------------------------------
// Actions

static bool ensure_wifi(void)
{
    if (wifi_ok)
        return true;
    if (app_confirm("Geen wifi", "Er is geen wifi-verbinding. Wil je een netwerk kiezen?",
                    "Wifi openen", "Nee"))
    {
        bool ok = wifi_manager();
        draw_browser();
        return ok;
    }
    return false;
}

static void refresh_catalog(void)
{
    if (!ensure_wifi())
        return;
    int r = run_job_ui(JOB_CATALOG, "Lijst ophalen", "Universal-DB");
    if (r != 200)
    {
        app_message("Lijst ophalen mislukt", job_err);
        return;
    }
    app_status("Even geduld", "De lijst wordt verwerkt...");
    if (!load_catalog_file())
        app_message("Fout", "De lijst kon niet gelezen worden.");
    build_rows();
}

// Details page: screenshots on top, info + download on the bottom
static void download_selected(void)
{
    if (view_count == 0)
        return;
    int ei = view[sel];
    cat_entry_t *e = &cat.entries[ei];

    int shot = 0, loaded_shot = -1, fetching_shot = -1;
    bool shot_ok = false;
    img_t shot_img = { 0 };
    bool dirty = true;
    int frame = 0;
    gfx_transition(TR_LEFT);

    while (1)
    {
        cat_download_t *d = &e->dl[dl_choice];
        char name[128];
        snprintf(name, sizeof(name), "%s", d->name);
        sanitize_filename(name);

        // Screenshot: from cache, or download in the background
        if (e->num_shots > 0 && loaded_shot != shot)
        {
            char path[96];
            cache_path(SHOT_DIR, e->shots[shot], path, sizeof(path));
            if (fetching_shot == shot && !bg_busy)
                fetching_shot = -1;
            if (file_exists(path) && fetching_shot < 0)
            {
                img_free(&shot_img);
                shot_ok = img_load(path, &shot_img, 226, 148);
                loaded_shot = shot;
                dirty = true;
            }
            else if (fetching_shot < 0 && !bg_busy)
            {
                if (bg_fetch(e->shots[shot], path))
                    fetching_shot = shot;
                else
                {
                    loaded_shot = shot; // offline: give up for this one
                    shot_ok = false;
                    img_free(&shot_img);
                }
                dirty = true;
            }
            else if (fetching_shot >= 0 && !bg_busy && bg_result != 200)
            {
                loaded_shot = shot;
                fetching_shot = -1;
                shot_ok = false;
                dirty = true;
            }
        }
        bool loading = e->num_shots > 0 && loaded_shot != shot;

        if (dirty || (loading && (frame % 4) == 0))
        {
            ui_item_t it = { e->title, e->author, e->version, e->category, e->description,
                             d->name, d->size, dl_choice, e->num_dl, icon_of(ei, true) };
            ui_details_top(&it, shot_ok ? &shot_img : NULL, shot, e->num_shots, loading, frame);
            unsigned long long fr = 0;
            char space[32] = "?";
            bool known = disk_space(drive, &fr, NULL);
            if (known)
                format_size64(fr, space, sizeof(space));
            bool warn = known && fr < (unsigned long long)d->size + 64 * 1024;
            char dest[160];
            snprintf(dest, sizeof(dest), "%s%s", save_dir, is_drive_root(save_dir) ? "/" : "");
            ui_details_bottom(&it, dest, space, warn);
            gfx_present();
            dirty = false;
        }

        app_wait_frame();
        frame++;
        input_t in = app_input();
        u32 k = in.down;

        if ((k & KEY_B) || in.hit == HIT_BTN + 0)
            break;
        if ((k & (KEY_R | KEY_RIGHT)) && e->num_shots > 1) { shot = (shot + 1) % e->num_shots; dirty = true; }
        if ((k & (KEY_L | KEY_LEFT)) && e->num_shots > 1) { shot = (shot + e->num_shots - 1) % e->num_shots; dirty = true; }
        if (((k & KEY_SELECT) || in.hit == HIT_TAB) && e->num_dl > 1)
        {
            dl_choice = (dl_choice + 1) % e->num_dl;
            dirty = true;
        }
        if ((k & KEY_Y) || in.hit == HIT_BTN + 1)
        {
            if (folder_picker(save_dir, sizeof(save_dir)))
                write_settings();
            gfx_transition(TR_RIGHT);
            dirty = true;
        }
        if ((k & KEY_A) || in.hit == HIT_BTN + 2)
        {
            if (!ensure_wifi())
            {
                dirty = true;
                continue;
            }
            unsigned long long fr;
            if (disk_space(drive, &fr, NULL) && fr < (unsigned long long)d->size + 64 * 1024)
            {
                app_message("Te weinig ruimte", "Er is niet genoeg vrije ruimte op je SD-kaart voor deze download.");
                dirty = true;
                continue;
            }
            snprintf(job_path, sizeof(job_path), "%s/%s", is_drive_root(save_dir) ? drive : save_dir, name);
            snprintf(job_url, sizeof(job_url), "%s", d->url);
            if (file_exists(job_path))
            {
                if (!app_confirm(e->title, "Dit bestand bestaat al. Vervangen door de nieuwe download?", "Vervangen", "Nee"))
                {
                    dirty = true;
                    continue;
                }
            }
            mkdir_p(save_dir);
            int r = run_job_ui(JOB_DOWNLOAD, e->title, name);
            gfx_transition(TR_DOWN_BOT);
            dirty = true;
            ui_item_t it = { e->title, e->author, e->version, e->category, e->description,
                             d->name, d->size, dl_choice, e->num_dl, icon_of(ei, true) };
            ui_details_top(&it, shot_ok ? &shot_img : NULL, shot, e->num_shots, false, frame);
            ui_details_bottom(&it, save_dir, "", false);
            gfx_present();
            if (r == 200)
            {
                nds_free(&installed);
                nds_scan(&installed, drive, 1000);
                build_rows();
                sound_play(SND_SUCCESS);
                snprintf(job_msg, sizeof(job_msg), "Opgeslagen als %s\n\nJe vindt hem in je loader, bijvoorbeeld TWiLight Menu++.", job_path);
                app_message("Klaar!", job_msg);
            }
            else
            {
                sound_play(SND_ERROR);
                app_message("Download mislukt", job_err);
            }
            dirty = true;
        }
    }
    // Leave the screenshot download running only if it is ours; just wait
    img_free(&shot_img);
    gfx_transition(TR_RIGHT);
}

// task_path: optional full SD path (sd:/...) of a .rar the phone should unpack
static bool run_server_task(const char *task_path)
{
    if (!ensure_wifi())
        return false;
    memset(&srv, 0, sizeof(srv));
    if (task_path && strncmp(task_path, drive, strlen(drive)) == 0)
        snprintf(srv.task_unrar, sizeof(srv.task_unrar), "%s", task_path + strlen(drive));
    u32 ip = Wifi_GetIP();
    snprintf(srv.ip, sizeof(srv.ip), "%lu.%lu.%lu.%lu", ip & 0xFF, (ip >> 8) & 0xFF,
             (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    char url[40];
    snprintf(url, sizeof(url), "http://%s", srv.ip);

    ui_server_top(url);
    gfx_transition(TR_LEFT);
    start_job(JOB_SERVER);
    rate_reset();
    int frame = 0;
    while (job_running)
    {
        if ((frame++ % 10) == 0)
        {
            bool busy = srv.receiving || srv.sending;
            if (busy)
                rate_update(srv.done);
            else
                rate_reset();
            ui_server_bottom(url, busy, srv.sending ? "Naar telefoon" : "Ontvangen", srv.current,
                             srv.done, srv.total, rate.speed, rate_eta(srv.done, srv.total),
                             srv.last_msg, srv.files_ok);
            gfx_present();
        }
        app_wait_frame();
        input_t in = app_input();
        if ((in.down & KEY_B) || in.hit == HIT_BTN)
            srv.stop = true;
    }
    if (job_result != 200)
        app_message("Server-fout", job_err);
    if (task_path)
    {
        nds_free(&installed);
        nds_scan(&installed, drive, 1000);
    }
    return srv.task_done;
}

static void run_server(void)
{
    run_server_task(NULL);
}

// Lets the phone unpack a .rar (much faster than the DSi, especially for old
// PPMd archives). Returns true when the phone reported success.
static bool phone_unrar(const char *full, const char *name)
{
    char msg[400];
    snprintf(msg, sizeof(msg),
             "Scan zo de QR-code met je telefoon (zelfde wifi). De telefoon haalt %s op, "
             "pakt hem uit en stuurt de bestanden terug. Daarna wordt het rar-bestand verwijderd. "
             "Laat de pagina open tot hij klaar is.", name);
    app_message("Uitpakken met telefoon", msg);
    bool ok = run_server_task(full);
    gfx_transition(TR_RIGHT);
    if (ok)
    {
        sound_play(SND_SUCCESS);
        snprintf(msg, sizeof(msg), "%s is uitgepakt door je telefoon.", name);
        app_message("Klaar", msg);
    }
    return ok;
}

static void self_update(void)
{
    if (!ensure_wifi())
        return;
    remote_version[0] = '\0';
    int r = run_job_ui(JOB_UPDATE_CHECK, "Zoeken naar update", "GitHub");
    if (r != 200 || !remote_version[0])
    {
        app_message("Update", job_err[0] ? job_err : "Kon de versie niet ophalen.");
        return;
    }
    if (version_compare(remote_version, APP_VERSION) <= 0)
    {
        snprintf(job_msg, sizeof(job_msg), "Je hebt al de nieuwste versie (%s).", APP_VERSION);
        app_message("Helemaal bij", job_msg);
        return;
    }
    snprintf(job_msg, sizeof(job_msg), "Versie %s is beschikbaar (je hebt %s). Nu installeren?",
             remote_version, APP_VERSION);
    if (!app_confirm("Nieuwe versie", job_msg, "Installeren", "Later"))
        return;

    r = run_job_ui(JOB_UPDATE_INSTALL, "Update installeren", "dsidl.nds");
    if (r == 200)
    {
        app_message("Update klaar", "DSiDL is bijgewerkt. Hij sluit nu af; start hem opnieuw vanuit je loader.");
        exit(0);
    }
    app_message("Update mislukt", job_err);
}

static void do_search(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", search);
    if (app_text_input("Zoeken", "titel, maker of omschrijving", buf, sizeof(buf), false))
        snprintf(search, sizeof(search), "%s", buf);
    rebuild_view();
    build_rows();
}

// ---------------------------------------------------------------------------
// Generic scrolling list with smooth animation. Returns the activated row
// (A or tap on selected row), or a negative value for a button / back:
//   -1 = B / back, -(10 + n) = footer button n

typedef void (*list_top_fn)(int sel);

static int run_list(const char *header, const char *sub, ui_row_t *rows, int count, int *sel_io,
                    int icon_kind, const char *const *labels, const char *const *keys, int nb,
                    const char *empty, list_top_fn top)
{
    int sel = *sel_io, scroll = 0, a_s = 0, a_c = 0, drag = 0, repeat = 0;
    if (sel >= count)
        sel = count > 0 ? count - 1 : 0;
    if (sel >= LIST_ROWS)
        scroll = sel - LIST_ROWS + 1;
    a_s = scroll * LIST_STEP;
    a_c = sel * LIST_STEP;
    bool dirty = true, top_dirty = true;
    int result = -1;

    while (1)
    {
        if (sel < scroll)
            scroll = sel;
        if (sel >= scroll + LIST_ROWS)
            scroll = sel - LIST_ROWS + 1;
        int ns = app_ease(a_s, scroll * LIST_STEP), nc = app_ease(a_c, sel * LIST_STEP);
        bool moving = ns != a_s || nc != a_c;
        if (dirty || moving || top_dirty)
        {
            a_s = ns;
            a_c = nc;
            if (top_dirty && top)
                top(sel);
            ui_list_screen(header, sub, rows, count, sel, a_s, a_c, icon_kind, labels, keys, nb, empty);
            if (top_dirty)
                gfx_present();
            else
                gfx_present_bot();
            dirty = top_dirty = false;
        }
        app_wait_frame();
        input_t in = app_input();
        u32 k = in.down, nav = k;
        if (in.held & (KEY_UP | KEY_DOWN))
        {
            if (++repeat > 18 && (repeat % 4) == 0)
                nav |= in.held & (KEY_UP | KEY_DOWN);
        }
        else
            repeat = 0;

        if (in.drag_dy)
        {
            drag += in.drag_dy;
            while (drag <= -30 && scroll + LIST_ROWS < count) { scroll++; drag += 30; if (sel < scroll) { sel = scroll; top_dirty = true; } dirty = true; }
            while (drag >= 30 && scroll > 0) { scroll--; drag -= 30; if (sel >= scroll + LIST_ROWS) { sel = scroll + LIST_ROWS - 1; top_dirty = true; } dirty = true; }
        }
        if (!in.touching)
            drag = 0;

        if ((nav & KEY_DOWN) && sel + 1 < count) { sel++; dirty = top_dirty = true; sound_play(SND_TICK); }
        if ((nav & KEY_UP) && sel > 0) { sel--; dirty = top_dirty = true; sound_play(SND_TICK); }
        if (k & KEY_RIGHT) { sel += LIST_ROWS; if (sel >= count) sel = count - 1; if (sel < 0) sel = 0; dirty = top_dirty = true; }
        if (k & KEY_LEFT) { sel -= LIST_ROWS; if (sel < 0) sel = 0; dirty = top_dirty = true; }

        if (in.hit >= HIT_ROW && in.hit < HIT_ROW + 1000)
        {
            int r = in.hit - HIT_ROW;
            if (r == sel)
            {
                result = sel;
                break;
            }
            sel = r;
            dirty = top_dirty = true;
            sound_play(SND_TICK);
        }
        if ((k & KEY_A) && count > 0)
        {
            result = sel;
            break;
        }
        bool done = false;
        for (int i = 0; i < nb; i++)
        {
            bool key = false;
            if (keys && keys[i])
            {
                if (!strcmp(keys[i], "X")) key = k & KEY_X;
                else if (!strcmp(keys[i], "Y")) key = k & KEY_Y;
                else if (!strcmp(keys[i], "B")) key = k & KEY_B;
                else if (!strcmp(keys[i], "R")) key = k & KEY_R;
                else if (!strcmp(keys[i], "L")) key = k & KEY_L;
                else if (!strcmp(keys[i], "START")) key = k & KEY_START;
            }
            if (key || in.hit == HIT_BTN + i)
            {
                result = -(10 + i);
                done = true;
                break;
            }
        }
        if (done)
            break;
        if (k & KEY_B)
        {
            result = -1;
            break;
        }
    }
    if (result != -1)
        sound_play(SND_CLICK);
    *sel_io = sel;
    return result;
}

// Simple chooser: list of options, returns index or -1
static int choose(const char *title, const char *sub, const char *const *opts, int n)
{
    ui_row_t r[16];
    for (int i = 0; i < n && i < 16; i++)
        r[i] = (ui_row_t){ opts[i], NULL, NULL, NULL, NULL, 0 };
    static const char *labels[] = { "Kiezen", "Terug" };
    static const char *keys[] = { "A", "B" };
    int sel = 0;
    gfx_transition(TR_UP_BOT);
    int res = run_list(title, sub, r, n, &sel, 0, labels, keys, 2, NULL, NULL);
    gfx_transition(TR_DOWN_BOT);
    if (res == -10)
        return sel;
    return res >= 0 ? res : -1;
}

static void rescan_installed(void)
{
    run_job_ui(JOB_SCAN, "Software zoeken", "SD-kaart");
}

// ---------------------------------------------------------------------------
// "Mijn software": programs on the SD card

static int sw_sel;

static void software_top(int sel)
{
    bool c;
    int bars;
    const char *ssid;
    app_wifi_info(&c, &bars, &ssid);
    if (installed.count == 0)
    {
        const char *l[] = { "Geen .nds- of .dsi-bestanden gevonden op je SD-kaart." };
        ui_info_top("Mijn software", l, 1, c, bars, ssid);
        return;
    }
    ndsfile_t *f = &installed.items[sel];
    nds_read_banner(f);
    img_t big = { 0 };
    if (f->icon.px)
        img_scaled(&f->icon, &big, 44, 44);
    char sz[24];
    format_size(f->size, sz, sizeof(sz));
    ui_item_t it = { f->title, f->sub[0] ? f->sub : NULL, f->code[0] ? f->code : NULL, NULL,
                     f->path, f->name, f->size, 0, 1, big.px ? &big : NULL };
    ui_browser_top(&it, c, bars, ssid);
    img_free(&big);
}

static void software_screen(void)
{
    if (installed.count == 0)
        rescan_installed();

    while (1)
    {
        int n = installed.count;
        ui_row_t *r = calloc(n + 1, sizeof(ui_row_t));
        char (*subs)[64] = calloc(n + 1, 64);
        if (!r || !subs)
        {
            free(r);
            free(subs);
            return;
        }
        for (int i = 0; i < n; i++)
        {
            ndsfile_t *f = &installed.items[i];
            nds_read_banner(f);
            char sz[24];
            format_size(f->size, sz, sizeof(sz));
            const char *folder = strchr(f->path, '/');
            char dir[48];
            snprintf(dir, sizeof(dir), "%.40s", folder ? folder : f->path);
            char *slash = strrchr(dir, '/');
            if (slash && slash != dir)
                *slash = '\0';
            else if (slash)
                slash[1] = '\0';
            snprintf(subs[i], 64, "%s - %s", dir, sz);
            r[i].title = f->title;
            r[i].subtitle = subs[i];
            r[i].icon = NULL;
        }
        // Small icons from the banners
        img_t *small = calloc(n + 1, sizeof(img_t));
        for (int i = 0; i < n && small; i++)
            if (installed.items[i].icon.px && img_scaled(&installed.items[i].icon, &small[i], 22, 22))
                r[i].icon = &small[i];

        static const char *labels[] = { "Opties", "Opnieuw zoeken", "Terug" };
        static const char *keys[] = { "A", "Y", "B" };
        char sub[48];
        snprintf(sub, sizeof(sub), "%d programma's op je SD-kaart", n);
        gfx_transition(TR_LEFT);
        int res = run_list("Mijn software", sub, r, n, &sw_sel, 0, labels, keys, 3,
                           "Niets gevonden", software_top);

        for (int i = 0; i < n && small; i++)
            img_free(&small[i]);
        free(small);
        free(r);
        free(subs);

        if (res == -1 || res == -12)
            break;
        if (res == -11)
        {
            rescan_installed();
            continue;
        }
        if (res == -10)
            res = sw_sel;
        if (res < 0 || res >= installed.count)
            continue;

        ndsfile_t *f = &installed.items[res];
        static const char *opts[] = { "Verplaatsen naar...", "Kopiëren naar...", "Naam wijzigen", "Verwijderen", "Terug" };
        int o = choose(f->title, f->path, opts, 5);
        if (o == 0 || o == 1)
        {
            char dest[128];
            snprintf(dest, sizeof(dest), "%s", save_dir);
            if (!folder_picker(dest, sizeof(dest)))
                continue;
            char to[300];
            fo_unique(is_drive_root(dest) ? drive : dest, f->name, to, sizeof(to));
            if (o == 0)
            {
                if (rename(f->path, to) != 0)
                    app_message("Mislukt", "Verplaatsen is niet gelukt.");
            }
            else
            {
                snprintf(job_path, sizeof(job_path), "%s", f->path);
                snprintf(job_url, sizeof(job_url), "%s", to);
                if (run_job_ui(JOB_COPY, "Kopiëren", f->name) != 200)
                    app_message("Mislukt", job_err);
            }
            rescan_installed();
        }
        else if (o == 2)
        {
            char nm[128];
            snprintf(nm, sizeof(nm), "%s", f->name);
            if (app_text_input("Nieuwe naam", NULL, nm, sizeof(nm), false))
            {
                sanitize_filename(nm);
                char to[300];
                snprintf(to, sizeof(to), "%s", f->path);
                char *sl = strrchr(to, '/');
                if (sl && nm[0])
                {
                    snprintf(sl + 1, sizeof(to) - (sl + 1 - to), "%s", nm);
                    if (file_exists(to) || rename(f->path, to) != 0)
                        app_message("Mislukt", "Hernoemen is niet gelukt (bestaat de naam al?).");
                }
                rescan_installed();
            }
        }
        else if (o == 3)
        {
            char q[200];
            snprintf(q, sizeof(q), "%s verwijderen van je SD-kaart?", f->name);
            if (app_confirm("Verwijderen", q, "Verwijderen", "Nee"))
            {
                if (remove(f->path) != 0)
                    app_message("Mislukt", "Verwijderen is niet gelukt.");
                rescan_installed();
                if (sw_sel >= installed.count)
                    sw_sel = installed.count - 1;
                if (sw_sel < 0)
                    sw_sel = 0;
            }
        }
    }
    gfx_transition(TR_RIGHT);
}

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

// Extracts a .zip or .rar into dest_dir. Shows the result; returns success.
static bool extract_archive(const char *arc, const char *dest_dir, const char *name, bool remove_after)
{
    const char *dest = is_drive_root(dest_dir) ? drive : dest_dir;
    bool rar = ends_with(arc, ".rar");
    char src[300];
    snprintf(src, sizeof(src), "%s", arc);
    snprintf(job_path, sizeof(job_path), "%s", src);
    snprintf(job_url, sizeof(job_url), "%s", dest);
    int r = run_job_ui(rar ? JOB_UNRAR : JOB_UNZIP, "Uitpakken", name);
    char msg[700];
    if (r != 200)
    {
        sound_play(SND_ERROR);
        snprintf(msg, sizeof(msg), "%s\n\n%s staat nog in %s%s.", job_err, name, dest_dir,
                 is_drive_root(dest_dir) ? "/" : "");
        app_message("Uitpakken mislukt", msg);
        return false;
    }
    if (remove_after)
        remove(src);
    nds_free(&installed);
    nds_scan(&installed, drive, 1000);
    sound_play(SND_SUCCESS);
    snprintf(msg, sizeof(msg), "%s is uitgepakt (%d bestand%s) in %s%s.%s", name, unzip_files,
             unzip_files == 1 ? "" : "en", dest_dir, is_drive_root(dest_dir) ? "/" : "",
             remove_after ? (rar ? " Het rar-bestand is verwijderd." : " Het zip-bestand is verwijderd.") : "");
    app_message("Klaar", msg);
    return true;
}

// ---------------------------------------------------------------------------
// File manager on the DSi

typedef struct {
    char *name;
    bool dir, hidden;
    unsigned long size;
} fentry_t;

static int cmp_fentry(const void *a, const void *b)
{
    const fentry_t *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static int list_files(const char *dir, fentry_t **out, bool show_hidden)
{
    char path[300];
    snprintf(path, sizeof(path), "%s%s", dir, is_drive_root(dir) ? "/" : "");
    DIR *d = opendir(path);
    *out = NULL;
    if (!d)
        return 0;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
    {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (n == cap)
        {
            cap = cap ? cap * 2 : 64;
            fentry_t *nw = realloc(*out, sizeof(fentry_t) * cap);
            if (!nw)
                break;
            *out = nw;
        }
        fentry_t *e = &(*out)[n];
        e->name = strdup(de->d_name);
        e->dir = de->d_type == DT_DIR;
        char full[600];
        path_join(full, sizeof(full), dir, de->d_name);
        struct stat st;
        e->size = (!e->dir && stat(full, &st) == 0) ? (unsigned long)st.st_size : 0;
        int attr = FAT_getAttr(full);
        e->hidden = (attr >= 0 && (attr & ATTR_HIDDEN)) || de->d_name[0] == '.';
        if (e->hidden && !show_hidden)
        {
            free(e->name);
            continue;
        }
        if (e->name)
            n++;
        if ((n & 15) == 0)
            cothread_yield();
    }
    closedir(d);
    if (n > 1)
        qsort(*out, n, sizeof(fentry_t), cmp_fentry);
    return n;
}

static void free_files(fentry_t *f, int n)
{
    for (int i = 0; i < n; i++)
        free(f[i].name);
    free(f);
}

static char fm_dir[256];

static void files_top(int sel)
{
    (void)sel;
    bool c;
    int bars;
    const char *ssid;
    app_wifi_info(&c, &bars, &ssid);
    unsigned long long fr = 0, tot = 0;
    char l1[64] = "", l2[300];
    if (disk_space(drive, &fr, &tot))
    {
        char a[24], b[24];
        format_size64(fr, a, sizeof(a));
        format_size64(tot, b, sizeof(b));
        snprintf(l1, sizeof(l1), "!%s vrij van %s", a, b);
    }
    snprintf(l2, sizeof(l2), "Map: %s%s", fm_dir, is_drive_root(fm_dir) ? "/" : "");
    const char *lines[] = { l1, "", l2, "", "A: openen of opties   Y: nieuwe map",
                            set_show_hidden ? "X: verborgen mappen verbergen" : "X: verborgen mappen tonen" };
    ui_info_top("Bestanden", lines, 6, c, bars, ssid);
}

static void files_screen(void)
{
    snprintf(fm_dir, sizeof(fm_dir), "%s", drive);
    int sel = 0;
    gfx_transition(TR_LEFT);
    while (1)
    {
        fentry_t *fs;
        int n = list_files(fm_dir, &fs, set_show_hidden);
        bool root = is_drive_root(fm_dir);
        int total = n + (root ? 0 : 1);
        ui_row_t *r = calloc(total + 1, sizeof(ui_row_t));
        char (*subs)[40] = calloc(total + 1, 40);
        if (!r || !subs)
        {
            free(r);
            free(subs);
            free_files(fs, n);
            return;
        }
        int off = root ? 0 : 1;
        if (!root)
            r[0] = (ui_row_t){ ".. (omhoog)", NULL, NULL, NULL, NULL, 1 };
        for (int i = 0; i < n; i++)
        {
            fentry_t *e = &fs[i];
            if (e->dir)
                snprintf(subs[i + off], 40, "Map%s", e->hidden ? " - verborgen" : "");
            else
            {
                char sz[24];
                format_size(e->size, sz, sizeof(sz));
                snprintf(subs[i + off], 40, "%s%s", sz, e->hidden ? " - verborgen" : "");
            }
            const char *dot = strrchr(e->name, '.');
            const char *cat = "other";
            if (dot && (!strcasecmp(dot, ".nds") || !strcasecmp(dot, ".dsi")))
                cat = "game";
            else if (dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".bmp")))
                cat = "utility";
            else if (dot && (!strcasecmp(dot, ".sav") || !strcasecmp(dot, ".ini") || !strcasecmp(dot, ".txt")))
                cat = "app";
            r[i + off] = (ui_row_t){ e->name, subs[i + off], cat, NULL, NULL, e->dir ? 1 : 0 };
        }
        const char *labels[] = { "Openen", "Nieuwe map", set_show_hidden ? "Verbergen" : "Verborgen", "Terug" };
        static const char *keys[] = { "A", "Y", "X", "B" };
        char shown[300];
        snprintf(shown, sizeof(shown), "%s%s%s", fm_dir, root ? "/" : "",
                 set_show_hidden ? "" : "  (verborgen uit)");
        int res = run_list("Bestanden", shown, r, total, &sel, 3, labels, keys, 4,
                           set_show_hidden ? "Lege map" : "Lege map (verborgen items uit)", files_top);
        free(r);
        free(subs);

        if (res == -12)
        {
            set_show_hidden = !set_show_hidden;
            write_settings();
            sound_play(SND_TICK);
            free_files(fs, n);
            continue;
        }
        if (res == -1 || res == -13)
        {
            if (root || res == -13)
            {
                free_files(fs, n);
                break;
            }
            res = 0; // B in a sub folder = up
            if (root)
                res = -1;
        }
        if (res == -11)
        {
            char nm[64] = "";
            if (app_text_input("Naam van de nieuwe map", NULL, nm, sizeof(nm), false))
            {
                sanitize_filename(nm);
                if (nm[0])
                {
                    char p[300];
                    path_join(p, sizeof(p), fm_dir, nm);
                    mkdir(p, 0777);
                }
            }
            free_files(fs, n);
            continue;
        }
        if (res == -10)
            res = sel;
        if (res < 0)
        {
            free_files(fs, n);
            continue;
        }
        if (!root && res == 0)
        {
            char *slash = strrchr(fm_dir, '/');
            if (slash)
            {
                *slash = '\0';
                if (!strchr(fm_dir, '/'))
                    snprintf(fm_dir, sizeof(fm_dir), "%s", drive);
            }
            sel = 0;
            gfx_transition(TR_RIGHT);
            free_files(fs, n);
            continue;
        }
        fentry_t *e = &fs[res - off];
        char full[300];
        path_join(full, sizeof(full), fm_dir, e->name);

        if (e->dir)
        {
            static const char *dopts[] = { "Openen", "Verplaatsen naar...", "Kopiëren naar...", "Naam wijzigen", "Verbergen / zichtbaar", "Verwijderen (alleen leeg)", "Terug" };
            int o = (res == sel && (keysHeld() & KEY_A)) ? 0 : 0;
            o = choose(e->name, "Map", dopts, 7);
            if (o == 0)
            {
                snprintf(fm_dir, sizeof(fm_dir), "%s", full);
                sel = 0;
                gfx_transition(TR_LEFT);
                free_files(fs, n);
                continue;
            }
            if (o < 0 || o == 6)
            {
                free_files(fs, n);
                continue;
            }
            res = o - 1; // map to file options below
        }
        else
        {
            if (ends_with(e->name, ".rar"))
            {
                static const char *ropts[] = { "Uitpakken met telefoon (snel)", "Uitpakken op de DSi", "Verplaatsen naar...", "Kopiëren naar...", "Naam wijzigen", "Verbergen / zichtbaar", "Verwijderen", "Terug" };
                res = choose(e->name, "Rar-archief", ropts, 8);
                if (res == 0 || res == 1)
                {
                    if (res == 0)
                        phone_unrar(full, e->name);
                    else
                        extract_archive(full, fm_dir, e->name, false);
                    free_files(fs, n);
                    continue;
                }
                if (res > 1)
                    res -= 2;
            }
            else if (ends_with(e->name, ".zip"))
            {
                static const char *aopts[] = { "Uitpakken hier", "Verplaatsen naar...", "Kopiëren naar...", "Naam wijzigen", "Verbergen / zichtbaar", "Verwijderen", "Terug" };
                res = choose(e->name, "Archief", aopts, 7);
                if (res == 0)
                {
                    extract_archive(full, fm_dir, e->name, false);
                    free_files(fs, n);
                    continue;
                }
                if (res > 0)
                    res--;
            }
            else
            {
                static const char *fopts[] = { "Verplaatsen naar...", "Kopiëren naar...", "Naam wijzigen", "Verbergen / zichtbaar", "Verwijderen", "Terug" };
                res = choose(e->name, "Bestand", fopts, 6);
            }
        }

        bool prot = !strcasecmp(e->name, "_nds") || !strcasecmp(e->name, "_dsidl");
        if ((res == 0 || res == 2 || res == 4) && prot && root)
        {
            app_message("Beschermd", "Deze map is nodig voor je DSi-software en kan niet verplaatst, hernoemd of verwijderd worden.");
        }
        else if (res == 0 || res == 1)
        {
            char dest[128];
            snprintf(dest, sizeof(dest), "%s", fm_dir);
            if (folder_picker(dest, sizeof(dest)))
            {
                char to[300];
                fo_unique(is_drive_root(dest) ? drive : dest, e->name, to, sizeof(to));
                if (res == 0)
                {
                    if (strncmp(dest, full, strlen(full)) == 0)
                        app_message("Mislukt", "Een map kan niet in zichzelf.");
                    else if (rename(full, to) != 0)
                        app_message("Mislukt", "Verplaatsen is niet gelukt.");
                }
                else
                {
                    snprintf(job_path, sizeof(job_path), "%s", full);
                    snprintf(job_url, sizeof(job_url), "%s", to);
                    if (run_job_ui(JOB_COPY, "Kopiëren", e->name) != 200)
                        app_message("Mislukt", job_err);
                }
            }
        }
        else if (res == 2)
        {
            char nm[128];
            snprintf(nm, sizeof(nm), "%s", e->name);
            if (app_text_input("Nieuwe naam", NULL, nm, sizeof(nm), false))
            {
                sanitize_filename(nm);
                char to[300];
                path_join(to, sizeof(to), fm_dir, nm);
                if (nm[0] && (file_exists(to) || rename(full, to) != 0))
                    app_message("Mislukt", "Hernoemen is niet gelukt (bestaat de naam al?).");
            }
        }
        else if (res == 3)
        {
            int attr = FAT_getAttr(full);
            if (attr >= 0)
                FAT_setAttr(full, (attr & ATTR_HIDDEN) ? (attr & ~ATTR_HIDDEN) : (attr | ATTR_HIDDEN));
        }
        else if (res == 4)
        {
            char q[200];
            snprintf(q, sizeof(q), "%s verwijderen?", e->name);
            if (app_confirm("Verwijderen", q, "Verwijderen", "Nee"))
            {
                bool ne;
                if (!fo_delete(full, &ne))
                    app_message("Mislukt", ne ? "De map is niet leeg." : "Verwijderen is niet gelukt.");
            }
        }
        free_files(fs, n);
    }
    gfx_transition(TR_RIGHT);
}

// ---------------------------------------------------------------------------
// Settings

static void settings_top(int sel)
{
    (void)sel;
    bool c;
    int bars;
    const char *ssid;
    app_wifi_info(&c, &bars, &ssid);
    char l1[80], l2[300], l3[300];
    snprintf(l1, sizeof(l1), "!DSiDL %s", APP_VERSION);
    snprintf(l2, sizeof(l2), "Programma: %s", self_path);
    snprintf(l3, sizeof(l3), "Instellingen: %s", SETTINGS_FILE);
    const char *lines[] = { l1, "", "Tik op een instelling of druk op A om hem te wijzigen.", "", l2, l3 };
    ui_info_top("Instellingen", lines, 6, c, bars, ssid);
}

static void settings_screen(void)
{
    int sel = 0;
    gfx_transition(TR_LEFT);
    while (1)
    {
        char v0[32], v1[32], v2[32], v3[160];
        snprintf(v0, sizeof(v0), "%s", set_dark ? "Donker" : "Licht");
        snprintf(v1, sizeof(v1), "%s", ui_accent_names[set_accent]);
        snprintf(v2, sizeof(v2), "%s", set_sound ? "Aan" : "Uit");
        snprintf(v3, sizeof(v3), "%s%s", save_dir, is_drive_root(save_dir) ? "/" : "");
        ui_row_t r[] = {
            { "Thema", v0, "app", NULL, NULL, 0 },
            { "Accentkleur", v1, "emulator", NULL, NULL, 0 },
            { "Geluid", v2, "game", NULL, NULL, 0 },
            { "Standaardmap voor downloads", v3, "utility", NULL, NULL, 0 },
        };
        static const char *labels[] = { "Wijzigen", "Terug" };
        static const char *keys[] = { "A", "B" };
        int res = run_list("Instellingen", "Uiterlijk en geluid", r, 4, &sel, 0, labels, keys, 2, NULL, settings_top);
        if (res == -1 || res == -11)
            break;
        if (res == -10)
            res = sel;
        if (res == 0)
            set_dark = !set_dark;
        else if (res == 1)
            set_accent = (set_accent + 1) % UI_ACCENTS;
        else if (res == 2)
        {
            set_sound = !set_sound;
            sound_enable(set_sound);
            sound_play(SND_SUCCESS);
        }
        else if (res == 3)
            folder_picker(save_dir, sizeof(save_dir));
        ui_set_theme(set_dark, set_accent);
        write_settings();
        gfx_transition(TR_FADE);
    }
    gfx_transition(TR_RIGHT);
}

// ---------------------------------------------------------------------------
// Boxart

static void boxart_screen(void)
{
    if (!ensure_wifi())
        return;
    int miss = box_missing_count();
    box_retry_missing = false;
    if (miss > 0)
    {
        char q[300];
        snprintf(q, sizeof(q), "DSiDL haalt hoesjes op bij GameTDB voor TWiLight Menu++. Games die al een hoesje "
                 "hebben worden overgeslagen, net als %d game%s waarvoor GameTDB de vorige keer niets had.",
                 miss, miss == 1 ? "" : "s");
        static const char *labels[] = { "Terug", "Alles opnieuw", "Starten" };
        static const char *keys[] = { "B", "Y", "A" };
        int ch = app_dialog("Boxart", q, labels, keys, 3, 2);
        if (ch <= 0)
            return;
        box_retry_missing = ch == 1;
    }
    else if (!app_confirm("Boxart", "DSiDL zoekt je games op de SD-kaart en haalt de hoesjes op bij GameTDB voor TWiLight Menu++. Games die al een hoesje hebben worden overgeslagen.", "Starten", "Nee"))
        return;
    int r = run_job_ui(JOB_BOXART, "Boxart downloaden", "GameTDB");
    gfx_transition(TR_DOWN_BOT);
    draw_browser();
    char msg[300];
    if (r == 200)
    {
        snprintf(msg, sizeof(msg), "%d games gevonden.\n%d nieuwe hoesjes, %d hadden er al een.\n"
                 "%d zonder hoesje bij GameTDB%s.",
                 box_found, box_ok, box_skip, box_missing_new + box_known_missing,
                 box_known_missing ? " (overgeslagen, eerder niet gevonden)" : "");
        sound_play(SND_SUCCESS);
        app_message("Boxart klaar", msg);
    }
    else
        app_message("Boxart", job_err[0] ? job_err : "Mislukt.");
}

// ---------------------------------------------------------------------------
// Mijn Drive: the user's own Google Drive folder (shared with link)

static bool read_drive_link(char *out, size_t len)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/drive.txt", DATA_DIR);
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fgets(out, len, f))
            out[strcspn(out, "\r\n")] = '\0';
        fclose(f);
    }
    return out[0] != '\0';
}

static void write_drive_link(const char *link)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/drive.txt", DATA_DIR);
    FILE *f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%s\n", link);
        fclose(f);
    }
}

static void drive_top(int sel)
{
    (void)sel;
    bool c;
    int bars;
    const char *ssid;
    app_wifi_info(&c, &bars, &ssid);
    unsigned long long fr = 0;
    char l1[64] = "", l2[200];
    if (disk_space(drive, &fr, NULL))
    {
        char a[24];
        format_size64(fr, a, sizeof(a));
        snprintf(l1, sizeof(l1), "!%s vrij op je SD-kaart", a);
    }
    snprintf(l2, sizeof(l2), "Downloads gaan naar %s%s", save_dir, is_drive_root(save_dir) ? "/" : "");
    const char *lines[] = { l1, "", "Je eigen bestanden uit je Google Drive-map.", ".zip en .rar worden automatisch uitgepakt en daarna verwijderd.", "", l2 };
    ui_info_top("Mijn Drive", lines, 6, c, bars, ssid);
}

// Downloads one Drive file to the save folder (unzipping .zip files)
static void drive_download(const gd_entry_t *e)
{
    while (1)
    {
        char text[400];
        bool zip = ends_with(e->name, ".zip") || ends_with(e->name, ".rar");
        snprintf(text, sizeof(text), "%s\nOpslaan in: %s%s%s", e->name, save_dir, is_drive_root(save_dir) ? "/" : "",
                 zip ? "\nWordt na het downloaden uitgepakt." :
                 ends_with(e->name, ".7z") ? "\n.7z kan de DSi niet uitpakken; gebruik .zip of .rar." : "");
        static const char *labels[] = { "Terug", "Andere map", "Download" };
        static const char *keys[] = { "B", "Y", "A" };
        int ch = app_dialog("Downloaden", text, labels, keys, 3, 2);
        if (ch == 1)
        {
            if (folder_picker(save_dir, sizeof(save_dir)))
                write_settings();
            gfx_transition(TR_RIGHT);
            continue;
        }
        if (ch != 2)
            return;
        break;
    }

    char name[160];
    snprintf(name, sizeof(name), "%s", e->name);
    sanitize_filename(name);
    const char *dest = is_drive_root(save_dir) ? drive : save_dir;
    mkdir_p(save_dir);
    snprintf(job_path, sizeof(job_path), "%s/%s", dest, name);
    gd_file_url(e->id, job_url, sizeof(job_url));

    int r = run_job_ui(JOB_DOWNLOAD, "Downloaden uit Drive", name);
    if (r == 200)
    {
        // A sharing problem gives an HTML page instead of the file
        FILE *f = fopen(job_path, "rb");
        char head[16] = { 0 };
        if (f)
        {
            if (fread(head, 1, 15, f)) {}
            fclose(f);
        }
        if (!strncasecmp(head, "<!DOCTYPE", 9) || !strncasecmp(head, "<html", 5))
        {
            remove(job_path);
            sound_play(SND_ERROR);
            app_message("Geen toegang", "Google gaf geen bestand terug. Zet de map (of het bestand) in Drive op 'Iedereen met de link' en probeer het opnieuw.");
            return;
        }
    }
    if (r != 200)
    {
        sound_play(SND_ERROR);
        app_message("Download mislukt", job_err);
        return;
    }

    if (ends_with(name, ".rar"))
    {
        char arc[300];
        snprintf(arc, sizeof(arc), "%s", job_path);
        static const char *labels[] = { "Op de DSi", "Met telefoon" };
        static const char *keys[] = { "Y", "A" };
        int ch = app_dialog("Rar uitpakken",
                            "Je telefoon pakt rar-bestanden veel sneller uit (seconden in plaats van "
                            "minuten). Op de DSi zelf kan ook, maar oude rar-bestanden duren daar lang.",
                            labels, keys, 2, 1);
        if (ch == 1)
            phone_unrar(arc, name);
        else if (ch == 0)
            extract_archive(arc, save_dir, name, true);
        else
        {
            char m[400];
            snprintf(m, sizeof(m), "%s staat in %s%s. Je kunt hem later uitpakken via Bestanden.",
                     name, save_dir, is_drive_root(save_dir) ? "/" : "");
            app_message("Opgeslagen", m);
        }
        build_rows();
        return;
    }
    if (ends_with(name, ".zip"))
    {
        char arc[300];
        snprintf(arc, sizeof(arc), "%s", job_path);
        extract_archive(arc, save_dir, name, true);
        build_rows();
        return;
    }
    char msg[400];
    snprintf(msg, sizeof(msg), "Opgeslagen als %s", job_path);
    nds_free(&installed);
    nds_scan(&installed, drive, 1000);
    build_rows();
    sound_play(SND_SUCCESS);
    app_message("Klaar!", msg);
}

// Set, change or remove the Drive link. Returns true if a link is set afterwards.
static bool drive_link_menu(bool have)
{
    char cur[400] = "";
    read_drive_link(cur, sizeof(cur));
    char text[600];
    if (have)
    {
        char id[72] = "", rk[64];
        if (!gd_parse_link(cur, id, sizeof(id), rk, sizeof(rk)))
            snprintf(id, sizeof(id), "(ongeldig)");
        snprintf(text, sizeof(text),
                 "Gekoppelde map: %.60s\n\nPlak een nieuwe link op je telefoon (Telefoon > Drive) of typ de map-ID hier.", id);
    }
    else
        snprintf(text, sizeof(text), "%s",
                 "Nog geen Drive-map gekoppeld. Zet je eigen gedumpte spellen in een Google Drive-map, deel die met 'Iedereen met de link' en plak de link op je telefoon (Telefoon > Drive).");
    const char *labels4[] = { "Terug", "Wissen", "Typen", "Telefoon" };
    const char *keys4[] = { "B", "X", "Y", "A" };
    const char *const *labels = have ? labels4 : (const char *const[]){ "Terug", "Link typen", "Via telefoon" };
    const char *const *keys = have ? keys4 : (const char *const[]){ "B", "Y", "A" };
    int nb = have ? 4 : 3;
    int ch = app_dialog(have ? "Drive-link wijzigen" : "Mijn Drive", text, labels, keys, nb, nb - 1);
    if (have && ch == 1)
    {
        if (app_confirm("Ontkoppelen", "De Drive-link verwijderen? Je bestanden in Drive blijven gewoon staan.", "Ontkoppelen", "Nee"))
            write_drive_link("");
    }
    else if (ch == nb - 1)
    {
        run_server();
        gfx_transition(TR_RIGHT);
    }
    else if (ch == nb - 2)
    {
        char buf[200] = "";
        if (app_text_input("Map-ID of link van Google Drive", "het deel na /folders/", buf, sizeof(buf), false) && buf[0])
            write_drive_link(buf);
    }
    char now[400];
    return read_drive_link(now, sizeof(now));
}

static bool drive_screen_once(void);

static void drive_screen(void)
{
    while (drive_screen_once())
        ;
}

// Returns true when the link was changed and the screen should reopen
static bool drive_screen_once(void)
{
    char link[400];
    if (!read_drive_link(link, sizeof(link)))
    {
        if (!drive_link_menu(false) || !read_drive_link(link, sizeof(link)))
            return false;
    }
    if (!ensure_wifi())
        return false;

    char root_id[72], rkey[64];
    if (!gd_parse_link(link, root_id, sizeof(root_id), rkey, sizeof(rkey)))
    {
        app_message("Ongeldige link", "De opgeslagen Drive-link wordt niet herkend. Kies een nieuwe link.");
        char before[400];
        snprintf(before, sizeof(before), "%s", link);
        if (drive_link_menu(true) && read_drive_link(link, sizeof(link)) && strcmp(link, before) != 0)
            return true;
        return false;
    }
    bool relink = false;

    // Folder stack
    char ids[16][72], names[16][80];
    int depth = 0;
    snprintf(ids[0], sizeof(ids[0]), "%s", root_id);
    snprintf(names[0], sizeof(names[0]), "Mijn Drive");
    char filter[32] = "";
    int sel = 0;

    while (1)
    {
        gd_folder_url(ids[depth], depth == 0 ? rkey : "", job_url, sizeof(job_url));
        int r = run_job_ui(JOB_DRIVE_LIST, "Drive openen", names[depth]);
        gd_entry_t *ents = NULL;
        int n = (r == 200 && drive_html) ? gd_parse_listing(drive_html, &ents) : -1;
        free(drive_html);
        drive_html = NULL;
        if (n < 0)
        {
            app_message("Drive", r == 404 || r == 403 ? "Map niet gevonden of niet gedeeld. Zet hem op 'Iedereen met de link'." : job_err);
            if (depth == 0)
                break;
            depth--;
            continue;
        }

        bool back = false;
        while (1)
        {
            // Filtered view
            int *map = calloc(n + 1, sizeof(int));
            ui_row_t *rw = calloc(n + 2, sizeof(ui_row_t));
            int m = 0;
            if (depth > 0)
                rw[m++] = (ui_row_t){ ".. (terug)", NULL, NULL, NULL, NULL, 1 };
            for (int i = 0; i < n && map && rw; i++)
            {
                if (filter[0] && !contains_ci(ents[i].name, filter))
                    continue;
                map[m - (depth > 0 ? 1 : 0)] = i;
                const char *dot = strrchr(ents[i].name, '.');
                const char *cat = (dot && (!strcasecmp(dot, ".nds") || !strcasecmp(dot, ".dsi"))) ? "game" :
                                  (dot && !strcasecmp(dot, ".zip")) ? "app" : "other";
                rw[m++] = (ui_row_t){ ents[i].name, ents[i].folder ? "Map" :
                                      (dot && !strcasecmp(dot, ".zip")) ? "Zip - wordt uitgepakt" :
                                      (dot && !strcasecmp(dot, ".rar")) ? "Rar - wordt uitgepakt" : "Bestand",
                                      cat, NULL, NULL, ents[i].folder ? 1 : 0 };
            }
            char sub[120];
            if (filter[0])
                snprintf(sub, sizeof(sub), "Zoeken: %s (%d)", filter, m - (depth > 0 ? 1 : 0));
            else
                snprintf(sub, sizeof(sub), "%s - %d items", names[depth], n);
            static const char *labels[] = { "Openen", "Zoeken", "Link", "Terug" };
            static const char *keys[] = { "A", "Y", "X", "B" };
            if (sel >= m)
                sel = 0;
            int res = run_list("Mijn Drive", sub, rw, m, &sel, 3, labels, keys, 4,
                               filter[0] ? "Niets gevonden" : "Lege map", drive_top);
            free(rw);

            if (res == -12)
            {
                free(map);
                char before[400], after[400];
                snprintf(before, sizeof(before), "%s", link);
                bool have = drive_link_menu(true);
                if (!have || !read_drive_link(after, sizeof(after)) || strcmp(after, before) != 0)
                {
                    relink = have; // reopen with the new link (or leave if removed)
                    back = true;
                    depth = 0;
                    break;
                }
                continue;
            }
            if (res == -13)
                res = -12;

            if (res == -11)
            {
                app_text_input("Zoeken in Drive", "naam van het spel", filter, sizeof(filter), false);
                sel = 0;
                free(map);
                continue;
            }
            if (res == -1 || res == -12)
            {
                free(map);
                if (filter[0])
                {
                    filter[0] = '\0';
                    sel = 0;
                    continue;
                }
                back = true;
                break;
            }
            if (res == -10)
                res = sel;
            if (res < 0)
            {
                free(map);
                continue;
            }
            if (depth > 0 && res == 0)
            {
                free(map);
                back = true;
                break;
            }
            gd_entry_t *e = &ents[map[res - (depth > 0 ? 1 : 0)]];
            free(map);
            if (e->folder)
            {
                if (depth < 15)
                {
                    depth++;
                    snprintf(ids[depth], sizeof(ids[depth]), "%s", e->id);
                    snprintf(names[depth], sizeof(names[depth]), "%.78s", e->name);
                    filter[0] = '\0';
                    sel = 0;
                    gfx_transition(TR_LEFT);
                }
                break;
            }
            drive_download(e);
        }
        free(ents);
        if (back)
        {
            if (depth == 0)
                break;
            depth--;
            sel = 0;
            filter[0] = '\0';
            gfx_transition(TR_RIGHT);
        }
    }
    gfx_transition(TR_RIGHT);
    return relink;
}

// ---------------------------------------------------------------------------
// TWiLight Menu++ updater

static void twl_installed_version(char *out, size_t len)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/twilight_version.txt", DATA_DIR);
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fgets(out, len, f))
            out[strcspn(out, "\r\n")] = '\0';
        fclose(f);
    }
}

static void twilight_update(void)
{
    if (strcmp(drive, "sd:") != 0)
    {
        app_message("TWiLight Menu++", "Bijwerken kan alleen op de SD-kaart van de DSi zelf, niet op een flashcard.");
        return;
    }
    if (!ensure_wifi())
        return;

    remote_version[0] = '\0';
    int r = run_job_ui(JOB_TWL_CHECK, "TWiLight Menu++", "Nieuwste versie opzoeken");
    gfx_transition(TR_DOWN_BOT);
    if (r != 200 || !remote_version[0])
    {
        draw_browser();
        app_message("TWiLight Menu++", job_err[0] ? job_err : "Kon de versie niet ophalen.");
        return;
    }

    char have[32];
    twl_installed_version(have, sizeof(have));
    char text[300];
    if (have[0] && version_compare(remote_version, have) <= 0)
        snprintf(text, sizeof(text), "Je hebt al %s, de nieuwste versie. Toch opnieuw installeren?", have);
    else
        snprintf(text, sizeof(text), "Versie %s is beschikbaar%s%s%s.\n\nOngeveer 35 MB downloaden en 46 MB uitpakken. Je instellingen blijven staan. Nu installeren?",
                 remote_version, have[0] ? " (je hebt " : "", have, have[0] ? ")" : "");
    draw_browser();
    if (!app_confirm("TWiLight Menu++", text, "Installeren", "Later"))
        return;

    snprintf(twl_zip, sizeof(twl_zip), "%s/twilight.zip", DATA_DIR);
    r = run_job_ui(JOB_TWL_DOWNLOAD, "TWiLight downloaden", remote_version);
    if (r != 200)
    {
        remove(twl_zip);
        draw_browser();
        app_message("Download mislukt", job_err);
        return;
    }

    r = run_job_ui(JOB_TWL_EXTRACT, "TWiLight installeren", "Bestanden uitpakken");
    remove(twl_zip); // the zip is no longer needed
    draw_browser();
    if (r != 200)
    {
        snprintf(text, sizeof(text), "%s\n\nTWiLight kan nu half bijgewerkt zijn: probeer het opnieuw voordat je je DSi herstart.", job_err);
        app_message("Installeren mislukt", text);
        return;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/twilight_version.txt", DATA_DIR);
    FILE *f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%s\n", remote_version);
        fclose(f);
    }
    snprintf(text, sizeof(text), "TWiLight Menu++ %s is geïnstalleerd (%d bestanden). Herstart je DSi om de nieuwe versie te gebruiken.",
             remote_version, twl_files);
    app_message("Klaar!", text);
}

static void menu(void)
{
    static const ui_tile_t tiles[] = {
        { "Software", 8, RGB(255, 170, 40) },
        { "Telefoon", 0, RGB(38, 190, 110) },
        { "Wifi", 1, RGB(18, 158, 230) },
        { "Lijst", 2, RGB(255, 140, 48) },
        { "TWiLight", 6, RGB(72, 84, 190) },
        { "Boxart", 7, RGB(232, 80, 140) },
        { "Bestanden", 8, RGB(120, 132, 150) },
        { "Instellen", 9, RGB(90, 100, 118) },
        { "Updaten", 3, RGB(140, 100, 242) },
        { "Drive", 10, RGB(30, 160, 90) },
    };
    static const char *titles[] = {
        "Mijn software", "Telefoon", "Wifi-netwerken", "Lijst vernieuwen", "TWiLight Menu++ bijwerken",
        "Boxart downloaden", "Bestanden", "Instellingen", "DSiDL updaten", "Mijn Drive"
    };
    static const char *texts[] = {
        "Alle games en apps op je SD-kaart, met hun eigen icoontje. Verplaatsen, kopi\xc3\xabren, hernoemen of verwijderen.",
        "Bekijk, upload, verplaats en kopieer bestanden en foto's vanuit de browser op je telefoon.",
        "Zoek netwerken, kies er een en typ het wachtwoord. DSiDL onthoudt ze op je SD-kaart.",
        "Haal de nieuwste lijst met homebrew op uit Universal-DB.",
        "Installeer de nieuwste versie van TWiLight Menu++ op je SD-kaart. Je instellingen blijven staan.",
        "Haal hoesjes van je games op voor TWiLight Menu++.",
        "Blader door je SD-kaart: verplaatsen, kopi\xc3\xabren, hernoemen, verbergen en verwijderen.",
        "Donker of licht thema, accentkleur, geluid en de standaardmap.",
        "Kijk op GitHub of er een nieuwe versie van DSiDL is en installeer hem.",
        "Je eigen bestanden uit je Google Drive-map: zoeken, downloaden en zip automatisch uitpakken.",
    };
    const int n = 10;
    int cur = 0;
    int anim = 0;
    int rx, ry;
    ui_menu_tile_pos(0, &rx, &ry);
    bool dirty = true;
    gfx_transition(TR_LEFT);

    while (1)
    {
        int tx, ty;
        ui_menu_tile_pos(cur, &tx, &ty);
        int nx = app_ease(rx, tx), ny = app_ease(ry, ty);
        bool moving = anim < 256 || nx != rx || ny != ry;
        if (dirty || moving)
        {
            rx = nx;
            ry = ny;
            bool c;
            int bars;
            const char *ssid;
            app_wifi_info(&c, &bars, &ssid);
            ui_menu(tiles, n, cur, titles[cur], texts[cur], c, bars, ssid, anim, rx, ry);
            gfx_present();
            dirty = false;
            if (anim < 256)
                anim += 64;
            if (anim > 256)
                anim = 256;
        }
        app_wait_frame();
        input_t in = app_input();
        u32 k = in.down;
        int act = -1;

        if (k & KEY_RIGHT) { cur = (cur + 1) % n; dirty = true; }
        if (k & KEY_LEFT) { cur = (cur + n - 1) % n; dirty = true; }
        if (k & KEY_DOWN) { cur = cur + 4 < n ? cur + 4 : cur % 4; dirty = true; }
        if (k & KEY_UP) { cur = cur - 4 >= 0 ? cur - 4 : (cur + 8 < n ? cur + 8 : (cur + 4 < n ? cur + 4 : cur)); dirty = true; }
        if (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))
            sound_play(SND_TICK);
        if (k & (KEY_B | KEY_X))
        {
            sound_play(SND_BACK);
            gfx_transition(TR_RIGHT);
            return;
        }
        if (k & KEY_A)
            act = cur;
        if (in.hit >= HIT_TILE && in.hit < HIT_TILE + n)
        {
            cur = in.hit - HIT_TILE;
            act = cur;
        }
        if (act < 0)
            continue;
        anim = 256; // finish the pop-in at once
        ui_menu_tile_pos(cur, &rx, &ry);
        dirty = true;
        sound_play(SND_OPEN);
        switch (act)
        {
            case 0: software_screen(); break;
            case 1: run_server(); gfx_transition(TR_RIGHT); break;
            case 2: wifi_manager(); gfx_transition(TR_RIGHT); break;
            case 3: gfx_transition(TR_RIGHT); refresh_catalog(); return;
            case 4: twilight_update(); break;
            case 5: boxart_screen(); break;
            case 6: files_screen(); break;
            case 7: settings_screen(); break;
            case 8: self_update(); break;
            case 9: drive_screen(); break;
            default:
                gfx_transition(TR_RIGHT);
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// Startup splash

static const char *splash_lines[8];
static int splash_n;
static int splash_frame;

static void splash(const char *line)
{
    if (splash_n == 8)
    {
        memmove(splash_lines, splash_lines + 1, sizeof(splash_lines[0]) * 7);
        splash_n--;
    }
    splash_lines[splash_n++] = line;
    ui_splash(splash_lines, splash_n, splash_frame += 8);
    gfx_present();
}

static void find_self_path(int argc, char **argv)
{
    if (argc > 0 && argv[0] && (strncmp(argv[0], "sd:/", 4) == 0 || strncmp(argv[0], "fat:/", 5) == 0))
    {
        snprintf(self_path, sizeof(self_path), "%s", argv[0]);
        return;
    }
    static const char *guesses[] = {
        "/dsidl.nds", "/roms/nds/dsidl.nds", "/_nds/dsidl.nds", NULL
    };
    for (int i = 0; guesses[i]; i++)
    {
        snprintf(self_path, sizeof(self_path), "%s%s", drive, guesses[i]);
        if (file_exists(self_path))
            return;
    }
    snprintf(self_path, sizeof(self_path), "%s/roms/nds/dsidl.nds", drive);
}

static char wifi_line[64];

static void wifi_status(const char *text)
{
    snprintf(wifi_line, sizeof(wifi_line), "%s", text);
    splash(wifi_line);
}

int main(int argc, char **argv)
{
    defaultExceptionHandler();
    ui_set_theme(false, 0);
    if (isDSiMode())
        setCpuClock(true); // 133 MHz: twice as fast drawing and TLS
    gfx_init();

    splash("Opstarten...");

    if (!fatInitDefault())
    {
        splash("SD-kaart niet gevonden!");
        while (1)
            swiWaitForVBlank();
    }
    if (argc > 0 && argv[0] && strncmp(argv[0], "fat:", 4) == 0)
        strcpy(drive, "fat:");
    else
    {
        DIR *d = isDSiMode() ? opendir("sd:/") : NULL;
        if (d)
            closedir(d);
        else
            strcpy(drive, "fat:");
    }
    snprintf(DATA_DIR, sizeof(DATA_DIR), "%s/_dsidl", drive);
    snprintf(CATALOG_FILE, sizeof(CATALOG_FILE), "%s/catalog.json", DATA_DIR);
    snprintf(SETTINGS_FILE, sizeof(SETTINGS_FILE), "%s/settings.txt", DATA_DIR);
    snprintf(save_dir, sizeof(save_dir), "%s/roms/nds", drive);
    snprintf(ICON_DIR, sizeof(ICON_DIR), "%s/icons", DATA_DIR);
    snprintf(SHOT_DIR, sizeof(SHOT_DIR), "%s/shots", DATA_DIR);

    find_self_path(argc, argv);
    mkdir_p(DATA_DIR);
    mkdir_p(ICON_DIR);
    mkdir_p(SHOT_DIR);
    load_settings();
    save_default_settings();
    ui_set_theme(set_dark, set_accent);
    sound_init();
    sound_enable(set_sound);

    splash("Certificaten laden");
    bool tls = http_init();
    if (!tls)
        splash("Let op: HTTPS niet beschikbaar");

    splash(isDSiMode() ? "Wifi starten (DSi-modus)" : "Wifi starten (DS-modus)");
    scanKeys();
    snprintf(WIFI_FILE, sizeof(WIFI_FILE), "%s/wifi.txt", DATA_DIR);
#ifdef SKIP_WIFI
    bool autoconnect = false;
#else
    bool autoconnect = !(keysHeld() & KEY_B);
#endif
    wifi_set_status_callback(wifi_status);
    if (!autoconnect)
        splash("Niet automatisch verbinden");
    if (wifi_start(WIFI_FILE, autoconnect))
        splash("Verbonden!");
    else if (autoconnect)
        splash("Geen verbinding");
    wifi_set_status_callback(NULL);

    splash("Software op je SD-kaart zoeken");
    nds_scan(&installed, drive, 1000);
    splash("Lijst laden");
    bool have = load_catalog_file();
    build_rows();
    if (wifi_ok && tls && (!have || catalog_is_old()))
        refresh_catalog();

    bool dirty = true;
    int repeat = 0, drag = 0, idle = 0;

    gfx_transition(TR_FADE);
    bool force = true;
    while (1)
    {
        if (dirty || force)
        {
            dirty = draw_browser_step(force);
            force = false;
        }

        app_wait_frame();
        input_t in = app_input();
        u32 down = in.down;
        if (down || in.touching || in.held)
            idle = 0;
        else
            idle++;
        if (icons_tick(idle))
            force = true;

        u32 nav = down;
        if (in.held & (KEY_UP | KEY_DOWN))
        {
            if (++repeat > 20 && (repeat % 4) == 0)
                nav |= in.held & (KEY_UP | KEY_DOWN);
        }
        else
            repeat = 0;

        if (view_count > 0)
        {
            if (nav & KEY_DOWN) { sel = (sel + 1) % view_count; dl_choice = 0; dirty = true; sound_play(SND_TICK); }
            if (nav & KEY_UP) { sel = (sel + view_count - 1) % view_count; dl_choice = 0; dirty = true; sound_play(SND_TICK); }
            if (down & KEY_RIGHT) { sel += BROWSER_ROWS; if (sel >= view_count) sel = view_count - 1; dl_choice = 0; dirty = true; }
            if (down & KEY_LEFT) { sel -= BROWSER_ROWS; if (sel < 0) sel = 0; dl_choice = 0; dirty = true; }

            cat_entry_t *e = &cat.entries[view[sel]];
            if ((down & KEY_R) && e->num_dl > 1) { dl_choice = (dl_choice + 1) % e->num_dl; dirty = true; }
            if ((down & KEY_L) && e->num_dl > 1) { dl_choice = (dl_choice + e->num_dl - 1) % e->num_dl; dirty = true; }

            // Drag to scroll the list
            if (in.drag_dy)
            {
                drag += in.drag_dy;
                while (drag <= -30 && scroll + BROWSER_ROWS < view_count)
                {
                    scroll++;
                    drag += 30;
                    if (sel < scroll) sel = scroll;
                    dirty = true;
                }
                while (drag >= 30 && scroll > 0)
                {
                    scroll--;
                    drag -= 30;
                    if (sel >= scroll + BROWSER_ROWS) sel = scroll + BROWSER_ROWS - 1;
                    dirty = true;
                }
            }
        }
        if (!in.touching)
            drag = 0;

        bool tap_download = false;
        if (in.hit >= HIT_ROW && in.hit < HIT_ROW + 1000 && in.hit - HIT_ROW < view_count)
        {
            int idx = in.hit - HIT_ROW;
            if (idx == sel)
                tap_download = true;
            else
            {
                sel = idx;
                dl_choice = 0;
                dirty = true;
            }
        }
        if (in.hit >= HIT_TAB && in.hit < HIT_TAB + NUM_CATS)
        {
            category = in.hit - HIT_TAB;
            rebuild_view();
            build_rows();
            dirty = true;
        }

        if ((down & KEY_A) || tap_download) { download_selected(); force = true; }
        if ((down & KEY_Y) || in.hit == HIT_SEARCH) { do_search(); force = true; }
        if (down & KEY_B)
        {
            if (search[0])
            {
                search[0] = '\0';
                rebuild_view();
                build_rows();
            }
            dirty = true;
        }
        if (down & KEY_SELECT)
        {
            category = (category + 1) % NUM_CATS;
            rebuild_view();
            build_rows();
            dirty = true;
        }
        if ((down & KEY_X) || in.hit == HIT_MENU) { menu(); force = true; }
        if (down & KEY_START)
        {
            if (app_confirm("Afsluiten", "DSiDL afsluiten?", "Afsluiten", "Nee"))
                break;
            force = true;
        }
    }

    Wifi_DisconnectAP();
    Wifi_DisableWifi();
    return 0;
}
