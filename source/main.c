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

#define CATALOG_URL   "https://db.universal-team.net/data/full.json"
#define UPDATE_BASE   "https://github.com/smikkelvriend/dsidl/releases/latest/download/"
// "sd:" on the DSi internal SD card, "fat:" on a flashcard
char drive[8] = "sd:";
static char DATA_DIR[32], CATALOG_FILE[64], SETTINGS_FILE[64];

#define LIST_ROWS 19
#define COLS 32

PrintConsole top, bottom;
static Keyboard *kbd;

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

static void color(int c, int bright)
{
    printf("\x1b[%d;%dm", c, bright);
}

static void reset_color(void)
{
    printf("\x1b[39;0m");
}

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
// Drawing

static void print_wrapped(const char *text, int width, int max_lines)
{
    if (!text)
        return;
    int lines = 0;
    const char *p = text;
    while (*p && lines < max_lines)
    {
        while (*p == ' ' || *p == '\n')
        {
            if (*p == '\n' && lines > 0)
                ; // collapse newlines
            p++;
        }
        if (!*p)
            break;
        int len = strlen(p);
        int take = len < width ? len : width;
        const char *nl = memchr(p, '\n', take);
        if (nl)
            take = nl - p;
        else if (take < len)
        {
            int t = take;
            while (t > 0 && p[t] != ' ')
                t--;
            if (t > width / 2)
                take = t;
        }
        printf("%.*s", take, p);
        if (take < width)
            printf("\n");
        p += take;
        lines++;
    }
}

// Speed / remaining time measurement. g_frames counts screen refreshes
// (60 per second) while a job runs.
static u32 g_frames;

typedef struct {
    u32 last_frame;
    size_t last_done;
    u32 speed;     // bytes per second, smoothed; 0 = not known yet
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
        // Started over (new file or reconnect from scratch)
        rate.last_done = done;
        rate.last_frame = g_frames;
        rate.speed = 0;
        return;
    }
    u32 frames = g_frames - rate.last_frame;
    if (frames < 60) // measure over at least one second
        return;
    u32 inst = (u32)((unsigned long long)(done - rate.last_done) * 60 / frames);
    // Smooth it so the time doesn't jump around too much
    rate.speed = rate.speed == 0 ? inst : (rate.speed * 3 + inst) / 4;
    if (rate.speed == 0 && inst == 0)
        rate.speed = 0;
    rate.last_done = done;
    rate.last_frame = g_frames;
}

static void format_time(unsigned long secs, char *out, size_t outlen)
{
    if (secs >= 3600)
        snprintf(out, outlen, "%lu:%02lu:%02lu", secs / 3600, (secs / 60) % 60, secs % 60);
    else
        snprintf(out, outlen, "%lu:%02lu", secs / 60, secs % 60);
}

static void draw_progress_bar(size_t done, size_t total)
{
    char a[16], b[16];
    rate_update(done);
    format_size(done, a, sizeof(a));
    if (total > 0)
    {
        int pct = (int)((unsigned long long)done * 100 / total);
        int fill = pct * 30 / 100;
        printf("[");
        color(32, 1);
        for (int i = 0; i < 30; i++)
            printf("%c", i < fill ? '#' : ' ');
        reset_color();
        printf("]\n");
        format_size(total, b, sizeof(b));
        printf("%s / %s  (%d%%)\n\n", a, b, pct);
    }
    else
    {
        printf("%s ontvangen\n\n", a);
    }

    char spd[16], eta[16];
    format_size(rate.speed, spd, sizeof(spd));
    if (rate.speed > 0 && total > done)
        format_time((unsigned long)((total - done) / rate.speed) + 1, eta, sizeof(eta));
    else if (total > 0 && done >= total)
        snprintf(eta, sizeof(eta), "0:00");
    else
        snprintf(eta, sizeof(eta), "--:--");

    printf("Snelheid: ");
    color(36, 1);
    printf("%s/s\n", rate.speed > 0 ? spd : "...");
    reset_color();
    if (total > 0)
    {
        printf("Resterend: ");
        color(33, 1);
        printf("%s\n", eta);
        reset_color();
    }
}

static void draw_list(void)
{
    consoleSelect(&top);
    consoleClear();
    color(31, 1);
    printf("DSiDL " APP_VERSION);
    reset_color();
    if (wifi_ok)
        printf("  %.13s\n", wifi_ssid()[0] ? wifi_ssid() : "wifi aan");
    else
        printf("  GEEN WIFI\n");

    printf("Zoek: ");
    color(33, 1);
    printf("%.12s", search[0] ? search : "-");
    reset_color();
    printf("  Soort: ");
    color(36, 1);
    printf("%s\n", categories[category]);
    reset_color();
    printf("--------------------------------");

    if (view_count == 0)
    {
        printf("\n");
        if (cat.count == 0)
            printf(" Geen lijst geladen.\n Druk X > Lijst vernieuwen.\n\n %.30s\n", load_err);
        else
            printf(" Niets gevonden.\n Druk B om zoeken te wissen.\n");
        return;
    }

    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + LIST_ROWS)
        scroll = sel - LIST_ROWS + 1;

    for (int row = 0; row < LIST_ROWS && scroll + row < view_count; row++)
    {
        int idx = scroll + row;
        cat_entry_t *e = &cat.entries[view[idx]];
        if (idx == sel)
        {
            color(32, 1);
            printf(">%-31.31s", e->title);
            reset_color();
        }
        else
        {
            printf(" %-31.31s", e->title);
        }
    }
    printf("\x1b[23;0H");
    printf("%d / %d", sel + 1, view_count);
}

static void draw_details(void)
{
    consoleSelect(&bottom);
    consoleClear();

    if (view_count == 0)
    {
        printf("\n Y      zoeken\n X      menu\n SELECT soort wisselen\n START  afsluiten\n");
        return;
    }

    cat_entry_t *e = &cat.entries[view[sel]];
    color(33, 1);
    printf("%.32s", e->title);
    if (strlen(e->title) < 32)
        printf("\n");
    reset_color();
    printf("door %.24s\n", e->author ? e->author : "?");
    printf("versie %.12s  [%s]\n", e->version ? e->version : "?",
           e->category ? e->category : "?");
    printf("\n");
    print_wrapped(e->description, 32, 8);

    printf("\x1b[16;0H");
    if (dl_choice >= e->num_dl)
        dl_choice = 0;
    char sz[16];
    format_size(e->dl[dl_choice].size, sz, sizeof(sz));
    color(36, 1);
    printf("%.24s", e->dl[dl_choice].name);
    reset_color();
    printf(" %s\n", sz);
    if (e->num_dl > 1)
        printf("L/R: ander bestand (%d/%d)\n", dl_choice + 1, e->num_dl);
    else
        printf("\n");

    printf("\x1b[19;0H");
    printf("A downloaden    Y zoeken\n");
    printf("B zoeken wissen X menu\n");
    printf("SELECT soort    START stoppen\n");
}

static void draw_job(const char *title)
{
    consoleSelect(&bottom);
    consoleClear();
    color(33, 1);
    printf("%s\n\n", title);
    reset_color();
    printf("%.32s\n\n", prog.status);
    draw_progress_bar(prog.done, prog.total);
    printf("\x1b[22;0HB: annuleren");
}

static void draw_server(void)
{
    consoleSelect(&bottom);
    consoleClear();
    color(33, 1);
    printf("Ontvangen via wifi\n\n");
    reset_color();
    printf("Open op je telefoon (zelfde\nwifi-netwerk) in de browser om\nbestanden te bekijken en sturen:\n\n");
    color(32, 1);
    printf("  http://%s\n\n", srv.ip);
    reset_color();
    if (srv.receiving || srv.sending)
    {
        printf("%s: %.20s\n", srv.sending ? "Naar telefoon" : "Ontvangen", srv.current);
        draw_progress_bar(srv.done, srv.total);
    }
    else
    {
        rate_reset(); // start measuring fresh for the next file
        printf("Wachten op bestanden...\n\n");
    }
    printf("\n%.64s\n", srv.last_msg);
    printf("Ontvangen: %d bestand(en)\n", srv.files_ok);
    printf("\x1b[22;0HB: stoppen");
}

static void message(const char *title, const char *text)
{
    consoleSelect(&bottom);
    consoleClear();
    color(33, 1);
    printf("%s\n\n", title);
    reset_color();
    print_wrapped(text, 32, 16);
    printf("\x1b[22;0HA: verder");
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        if (keysDown() & (KEY_A | KEY_B))
            break;
    }
}

static bool confirm(const char *title, const char *text)
{
    consoleSelect(&bottom);
    consoleClear();
    color(33, 1);
    printf("%s\n\n", title);
    reset_color();
    print_wrapped(text, 32, 16);
    printf("\x1b[22;0HA: ja    B: nee");
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        u32 k = keysDown();
        if (k & KEY_A)
            return true;
        if (k & KEY_B)
            return false;
    }
}

// Runs a job while showing progress. Returns job result.
static int run_job_ui(job_t type, const char *title)
{
    start_job(type);
    rate_reset();
    int frame = 0;
    while (job_running)
    {
        g_frames++;
        if ((frame++ % 10) == 0)
            draw_job(title);
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        if (keysDown() & KEY_B)
        {
            prog.cancel = true;
            snprintf(prog.status, sizeof(prog.status), "Annuleren...");
        }
    }
    return job_result;
}

// ---------------------------------------------------------------------------
// Actions

static bool ensure_wifi(void)
{
    if (wifi_ok)
        return true;
    if (confirm("Geen wifi", "Er is geen wifi-verbinding.\n\nWifi-netwerken openen om een netwerk te kiezen?"))
        return wifi_manager();
    return false;
}

static void refresh_catalog(void)
{
    if (!ensure_wifi())
        return;
    int r = run_job_ui(JOB_CATALOG, "Lijst ophalen");
    if (r != 200)
    {
        message("Lijst ophalen mislukt", job_err);
        return;
    }
    consoleSelect(&bottom);
    consoleClear();
    printf("Lijst verwerken...\n");
    cothread_yield_irq(IRQ_VBLANK);
    if (!load_catalog_file())
        message("Fout", "De lijst kon niet gelezen worden.");
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
    mkdir_p(save_dir);
    snprintf(job_path, sizeof(job_path), "%s/%s", save_dir, name);
    snprintf(job_url, sizeof(job_url), "%s", d->url);

    char q[200];
    char sz[16];
    format_size(d->size, sz, sizeof(sz));
    snprintf(q, sizeof(q), "%s (%s) downloaden naar %s?%s", name, sz, save_dir,
             file_exists(job_path) ? "\n\nHet bestaat al en wordt vervangen." : "");
    if (!confirm(e->title, q))
        return;

    int r = run_job_ui(JOB_DOWNLOAD, e->title);
    if (r == 200)
    {
        snprintf(job_msg, sizeof(job_msg), "Opgeslagen als\n%s\n\nJe vindt hem in je loader (bv. TWiLight Menu++).", job_path);
        message("Klaar!", job_msg);
    }
    else
    {
        message("Download mislukt", job_err);
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
    snprintf(srv.last_msg, sizeof(srv.last_msg), " ");

    start_job(JOB_SERVER);
    rate_reset();
    int frame = 0;
    while (job_running)
    {
        g_frames++;
        if ((frame++ % 15) == 0)
            draw_server();
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        if (keysDown() & KEY_B)
            srv.stop = true;
    }
    if (job_result != 200)
        message("Server-fout", job_err);
}

static void self_update(void)
{
    if (!ensure_wifi())
        return;
    remote_version[0] = '\0';
    int r = run_job_ui(JOB_UPDATE_CHECK, "Zoeken naar update");
    if (r != 200 || !remote_version[0])
    {
        message("Update", job_err[0] ? job_err : "Kon versie niet ophalen.");
        return;
    }
    if (version_compare(remote_version, APP_VERSION) <= 0)
    {
        snprintf(job_msg, sizeof(job_msg), "Je hebt al de nieuwste versie (%s).", APP_VERSION);
        message("Update", job_msg);
        return;
    }
    snprintf(job_msg, sizeof(job_msg), "Nieuwe versie %s gevonden (je hebt %s).\n\nInstalleren in:\n%s",
             remote_version, APP_VERSION, self_path);
    if (!confirm("Update", job_msg))
        return;

    r = run_job_ui(JOB_UPDATE_INSTALL, "Update installeren");
    if (r == 200)
    {
        message("Update klaar", "DSiDL is bijgewerkt. Hij sluit nu af; start hem opnieuw vanuit je loader.");
        exit(0);
    }
    message("Update mislukt", job_err);
}

// Text input with the on-screen keyboard. Returns false if cancelled.
bool ui_text_input(const char *title, const char *hint, char *buf, size_t size)
{
    keyboardShow();
    int len = strlen(buf);
    bool masked = false;
    bool ok = false;
    bool dirty = true;

    while (1)
    {
        if (dirty)
        {
            consoleSelect(&bottom);
            consoleClear();
            color(33, 1);
            printf("%.32s\n", title);
            reset_color();
            printf("> ");
            for (int i = 0; i < len; i++)
                printf("%c", masked ? '*' : buf[i]);
            printf("_\n\n");
            if (hint)
                printf("%.64s\n", hint);
            printf("ENTER/A: ok   B: annuleer");
            dirty = false;
        }

        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        int c = keyboardUpdate();
        u32 k = keysDown();

        if (c == '\n' || (k & KEY_A))
        {
            ok = true;
            break;
        }
        if (k & KEY_B)
            break;
        if (k & KEY_START)
        {
            masked = !masked;
            dirty = true;
        }
        if (c == '\b')
        {
            if (len > 0)
                buf[--len] = '\0';
            dirty = true;
        }
        else if (c >= 32 && c < 127 && len < (int)size - 1)
        {
            buf[len++] = c;
            buf[len] = '\0';
            dirty = true;
        }
    }
    keyboardHide();
    return ok;
}

static void do_search(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", search);
    if (ui_text_input("Zoeken", NULL, buf, sizeof(buf)))
        snprintf(search, sizeof(search), "%s", buf);
    rebuild_view();
}

static void menu(void)
{
    static const char *items[] = {
        "Telefoon: bestanden beheren",
        "Wifi-netwerken",
        "Lijst vernieuwen",
        "DSiDL updaten",
        "Over DSiDL",
        "Terug",
    };
    const int n = 6;
    int cur = 0;

    while (1)
    {
        consoleSelect(&bottom);
        consoleClear();
        color(33, 1);
        printf("Menu\n\n");
        reset_color();
        for (int i = 0; i < n; i++)
        {
            if (i == cur)
            {
                color(32, 1);
                printf("> %s\n", items[i]);
                reset_color();
            }
            else
            {
                printf("  %s\n", items[i]);
            }
        }
        printf("\x1b[20;0HOpslaan in:\n%.32s", save_dir);

        u32 k;
        do
        {
            cothread_yield_irq(IRQ_VBLANK);
            scanKeys();
            k = keysDown();
        } while (!(k & (KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_X)));

        if (k & KEY_UP)
            cur = (cur + n - 1) % n;
        if (k & KEY_DOWN)
            cur = (cur + 1) % n;
        if (k & (KEY_B | KEY_X))
            return;
        if (k & KEY_A)
        {
            switch (cur)
            {
                case 0: run_server(); break;
                case 1: wifi_manager(); break;
                case 2: refresh_catalog(); return;
                case 3: self_update(); break;
                case 4:
                    snprintf(job_msg, sizeof(job_msg),
                             "DSiDL %s\n\nHomebrew uit Universal-DB.\n\nInstellingen:\n%s\n\nProgramma:\n%s",
                             APP_VERSION, SETTINGS_FILE, self_path);
                    message("Over", job_msg);
                    break;
                default: return;
            }
        }
    }
}

// ---------------------------------------------------------------------------

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

int main(int argc, char **argv)
{
    defaultExceptionHandler();

    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    consoleInit(&top, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottom, 0, BgType_Text4bpp, BgSize_T_256x256, 22, 3, false, true);
    kbd = keyboardDemoInit();
    keyboardHide();

    consoleSelect(&top);
    color(31, 1);
    printf("DSiDL " APP_VERSION "\n\n");
    reset_color();

    if (!fatInitDefault())
    {
        printf("SD-kaart niet gevonden!\n");
        while (1)
            swiWaitForVBlank();
    }
    // Prefer the drive we were started from
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

    printf("Certificaten laden...\n");
    cothread_yield_irq(IRQ_VBLANK);
    bool tls = http_init();
    if (!tls)
        printf("Let op: HTTPS niet beschikbaar\n");

    printf("Wifi starten (%s)\n", isDSiMode() ? "DSi-modus, WPA2 ok" : "DS-modus, alleen WEP/open");
    cothread_yield_irq(IRQ_VBLANK);
    scanKeys();
    snprintf(WIFI_FILE, sizeof(WIFI_FILE), "%s/wifi.txt", DATA_DIR);
#ifdef SKIP_WIFI
    bool autoconnect = false;
#else
    bool autoconnect = !(keysHeld() & KEY_B);
#endif
    if (!autoconnect)
        printf("B ingedrukt: niet automatisch verbinden.\n");
    if (wifi_start(WIFI_FILE, autoconnect))
        printf("Verbonden met %s\n", wifi_ssid()[0] ? wifi_ssid() : "wifi");
    else if (autoconnect)
        printf("Geen verbinding. Kies een netwerk\nvia X > Wifi-netwerken.\n");

    printf("Lijst laden...\n");
    cothread_yield_irq(IRQ_VBLANK);
    bool have = load_catalog_file();
    if (wifi_ok && tls && (!have || catalog_is_old()))
        refresh_catalog();

    bool dirty = true;
    int repeat = 0;

    while (1)
    {
        if (dirty)
        {
            draw_list();
            draw_details();
            dirty = false;
        }

        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        u32 down = keysDown();
        u32 held = keysHeld();

        // Key repeat for scrolling
        u32 nav = down;
        if (held & (KEY_UP | KEY_DOWN))
        {
            if (++repeat > 20 && (repeat % 4) == 0)
                nav |= held & (KEY_UP | KEY_DOWN);
        }
        else
        {
            repeat = 0;
        }

        if (view_count > 0)
        {
            if (nav & KEY_DOWN) { sel = (sel + 1) % view_count; dl_choice = 0; dirty = true; }
            if (nav & KEY_UP) { sel = (sel + view_count - 1) % view_count; dl_choice = 0; dirty = true; }
            if (down & KEY_RIGHT) { sel += LIST_ROWS; if (sel >= view_count) sel = view_count - 1; dl_choice = 0; dirty = true; }
            if (down & KEY_LEFT) { sel -= LIST_ROWS; if (sel < 0) sel = 0; dl_choice = 0; dirty = true; }

            cat_entry_t *e = &cat.entries[view[sel]];
            if ((down & KEY_R) && e->num_dl > 1) { dl_choice = (dl_choice + 1) % e->num_dl; dirty = true; }
            if ((down & KEY_L) && e->num_dl > 1) { dl_choice = (dl_choice + e->num_dl - 1) % e->num_dl; dirty = true; }
        }

        if (down & KEY_A) { download_selected(); dirty = true; }
        if (down & KEY_Y) { do_search(); dirty = true; }
        if (down & KEY_B) { search[0] = '\0'; rebuild_view(); dirty = true; }
        if (down & KEY_SELECT) { category = (category + 1) % NUM_CATS; rebuild_view(); dirty = true; }
        if (down & KEY_X) { menu(); dirty = true; }
        if (down & KEY_START) break;
    }

    Wifi_DisconnectAP();
    Wifi_DisableWifi();
    return 0;
}
