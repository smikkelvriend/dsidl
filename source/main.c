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

#define CATALOG_URL   "https://db.universal-team.net/data/full.json"
#define UPDATE_BASE   "https://github.com/smikkelvriend/dsidl/releases/latest/download/"
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
#define wifi_ok wifi_connected()
static char WIFI_FILE[64];

// ---------------------------------------------------------------------------
// Background jobs (run in a cothread with a big stack, UI keeps running)

typedef enum { JOB_NONE, JOB_CATALOG, JOB_DOWNLOAD, JOB_UPDATE_CHECK,
               JOB_UPDATE_INSTALL, JOB_SERVER } job_t;

static volatile bool job_running = false;
static volatile int job_result;
static job_t job_type;
static http_progress_t prog;
static server_state_t srv;
static char job_err[96];
static char job_msg[512];
static char job_url[2048];
static char job_path[300];
static char remote_version[32];


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

static void start_job(job_t type)
{
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
    }
}

static void draw_browser(void)
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
                         e->dl[dl_choice].name, e->dl[dl_choice].size, dl_choice, e->num_dl };
        ui_browser_top(&it, c, bars, ssid);
    }
    else
    {
        ui_browser_top(NULL, c, bars, ssid);
    }

    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + BROWSER_ROWS)
        scroll = sel - BROWSER_ROWS + 1;
    if (scroll < 0)
        scroll = 0;

    const char *empty = cat.count == 0 ? "Nog geen lijst - X > Lijst" : "Niets gevonden";
    ui_browser_bottom(rows, view_count, sel, scroll, search, tab_labels, NUM_CATS, category, empty);
    gfx_present();
}

// ---------------------------------------------------------------------------
// Jobs with a progress screen

// Runs a job while showing progress. Returns job result.
static int run_job_ui(job_t type, const char *title, const char *name)
{
    start_job(type);
    rate_reset();
    int frame = 0;
    while (job_running)
    {
        if ((frame++ % 6) == 0)
        {
            rate_update(prog.done);
            ui_progress_screen(title, name, prog.status, prog.done, prog.total, rate.speed,
                               rate_eta(prog.done, prog.total),
                               prog.cancel ? "Bezig..." : "Annuleren");
            gfx_present();
        }
        app_wait_frame();
        input_t in = app_input();
        if ((in.down & KEY_B) || in.hit == HIT_BTN)
        {
            prog.cancel = true;
            snprintf(prog.status, sizeof(prog.status), "Annuleren...");
        }
    }
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
    bool dirty = true, chosen = false;

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
            ui_info_top("Map kiezen", info, 6, c, bars, ssid);
            ui_list_screen("Map kiezen", shown, r, total, sel_i, scr, 1, labels, keys, 3,
                           "Geen submappen");
            gfx_present();
            dirty = false;
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
            dirty = true;
        }
    }

    for (int i = 0; i < n; i++)
        free(names[i]);

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

static void download_selected(void)
{
    if (view_count == 0 || !ensure_wifi())
        return;
    cat_entry_t *e = &cat.entries[view[sel]];
    cat_download_t *d = &e->dl[dl_choice];

    char name[128];
    snprintf(name, sizeof(name), "%s", d->name);
    sanitize_filename(name);
    snprintf(job_url, sizeof(job_url), "%s", d->url);

    char sz[16];
    format_size(d->size, sz, sizeof(sz));

    while (1)
    {
        snprintf(job_path, sizeof(job_path), "%s/%s", is_drive_root(save_dir) ? drive : save_dir, name);
        bool exists = file_exists(job_path);

        char text[400];
        snprintf(text, sizeof(text), "%s (%s)\nOpslaan in: %s%s%s", name, sz, save_dir,
                 is_drive_root(save_dir) ? "/" : "",
                 exists ? "\nBestaat al en wordt vervangen." : "");
        static const char *labels[] = { "Annuleren", "Andere map", "Downloaden" };
        static const char *keys[] = { "B", "Y", "A" };
        draw_browser();
        int ch = app_dialog(e->title, text, labels, keys, 3, 2);
        if (ch == 0 || ch < 0)
        {
            draw_browser();
            return;
        }
        if (ch == 1)
        {
            if (folder_picker(save_dir, sizeof(save_dir)))
                write_settings();
            continue;
        }
        break;
    }
    mkdir_p(save_dir);

    int r = run_job_ui(JOB_DOWNLOAD, e->title, name);
    draw_browser();
    if (r == 200)
    {
        snprintf(job_msg, sizeof(job_msg), "Opgeslagen als %s\n\nJe vindt hem in je loader, bijvoorbeeld TWiLight Menu++.", job_path);
        app_message("Klaar!", job_msg);
    }
    else
    {
        app_message("Download mislukt", job_err);
    }
}

static void run_server(void)
{
    if (!ensure_wifi())
        return;
    memset(&srv, 0, sizeof(srv));
    u32 ip = Wifi_GetIP();
    snprintf(srv.ip, sizeof(srv.ip), "%lu.%lu.%lu.%lu", ip & 0xFF, (ip >> 8) & 0xFF,
             (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    char url[40];
    snprintf(url, sizeof(url), "http://%s", srv.ip);

    ui_server_top(url);
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

static void menu(void)
{
    static const ui_tile_t tiles[] = {
        { "Telefoon", 0, RGB(38, 190, 110) },
        { "Wifi", 1, RGB(18, 158, 230) },
        { "Lijst", 2, RGB(255, 140, 48) },
        { "Updaten", 3, RGB(140, 100, 242) },
        { "Over", 4, RGB(120, 132, 150) },
        { "Terug", 5, RGB(238, 72, 82) },
    };
    static const char *titles[] = {
        "Telefoon", "Wifi-netwerken", "Lijst vernieuwen", "DSiDL updaten", "Over DSiDL", "Terug"
    };
    static const char *texts[] = {
        "Bekijk, upload, verplaats en kopieer bestanden op je SD-kaart vanuit de browser op je telefoon.",
        "Zoek netwerken, kies er een en typ het wachtwoord. DSiDL onthoudt ze op je SD-kaart.",
        "Haal de nieuwste lijst met homebrew op uit Universal-DB.",
        "Kijk op GitHub of er een nieuwe versie van DSiDL is en installeer hem.",
        "Versie, instellingen en waar DSiDL zijn bestanden bewaart.",
        "Terug naar de lijst.",
    };
    const int n = 6;
    int cur = 0;
    bool dirty = true;

    while (1)
    {
        if (dirty)
        {
            bool c;
            int bars;
            const char *ssid;
            app_wifi_info(&c, &bars, &ssid);
            ui_menu(tiles, n, cur, titles[cur], texts[cur], c, bars, ssid);
            gfx_present();
            dirty = false;
        }
        app_wait_frame();
        input_t in = app_input();
        u32 k = in.down;
        int act = -1;

        if (k & KEY_RIGHT) { cur = (cur + 1) % n; dirty = true; }
        if (k & KEY_LEFT) { cur = (cur + n - 1) % n; dirty = true; }
        if (k & KEY_DOWN) { cur = (cur + 3) % n; dirty = true; }
        if (k & KEY_UP) { cur = (cur + n - 3) % n; dirty = true; }
        if (k & (KEY_B | KEY_X))
            return;
        if (k & KEY_A)
            act = cur;
        if (in.hit >= HIT_TILE && in.hit < HIT_TILE + n)
        {
            cur = in.hit - HIT_TILE;
            act = cur;
        }
        if (act < 0)
            continue;

        dirty = true;
        switch (act)
        {
            case 0: run_server(); break;
            case 1: wifi_manager(); break;
            case 2: refresh_catalog(); return;
            case 3: self_update(); break;
            case 4:
                snprintf(job_msg, sizeof(job_msg),
                         "DSiDL %s - homebrew uit Universal-DB.\nOpslaan in: %s\nInstellingen: %s\nProgramma: %s",
                         APP_VERSION, save_dir, SETTINGS_FILE, self_path);
                app_message("Over DSiDL", job_msg);
                break;
            default: return;
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

    find_self_path(argc, argv);
    mkdir_p(DATA_DIR);
    load_settings();
    save_default_settings();

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

    splash("Lijst laden");
    bool have = load_catalog_file();
    build_rows();
    if (wifi_ok && tls && (!have || catalog_is_old()))
        refresh_catalog();

    bool dirty = true;
    int repeat = 0, drag = 0;

    while (1)
    {
        if (dirty)
        {
            draw_browser();
            dirty = false;
        }

        app_wait_frame();
        input_t in = app_input();
        u32 down = in.down;

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
            if (nav & KEY_DOWN) { sel = (sel + 1) % view_count; dl_choice = 0; dirty = true; }
            if (nav & KEY_UP) { sel = (sel + view_count - 1) % view_count; dl_choice = 0; dirty = true; }
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

        if ((down & KEY_A) || tap_download) { download_selected(); dirty = true; }
        if ((down & KEY_Y) || in.hit == HIT_SEARCH) { do_search(); dirty = true; }
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
        if ((down & KEY_X) || in.hit == HIT_MENU) { menu(); dirty = true; }
        if (down & KEY_START)
        {
            if (app_confirm("Afsluiten", "DSiDL afsluiten?", "Afsluiten", "Nee"))
                break;
            dirty = true;
        }
    }

    Wifi_DisconnectAP();
    Wifi_DisableWifi();
    return 0;
}
