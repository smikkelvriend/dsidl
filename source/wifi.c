// DSiDL - WiFi manager
//
// Networks entered here are saved in a text file on the SD card
// (sd:/_dsidl/wifi.txt), not in the DSi's internal memory, so nothing on the
// console itself is ever modified.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "wifi.h"
#include "gfx.h"
#include "ui.h"
#include "appui.h"

#define MAX_SAVED 24
#define MAX_APS   32

typedef struct {
    char ssid[33];
    char pass[65];
} saved_net_t;

static saved_net_t saved[MAX_SAVED];
static int num_saved;
static char saved_path[96];

static Wifi_AccessPoint aps[MAX_APS];
static int num_aps;

static bool connected;
static char cur_ssid[33];
static int cur_bars;
static void (*status_cb)(const char *text);

void wifi_set_status_callback(void (*cb)(const char *text))
{
    status_cb = cb;
}

static void status(const char *fmt, const char *arg)
{
    static char line[80];
    snprintf(line, sizeof(line), fmt, arg);
    if (status_cb)
        status_cb(line);
}

bool wifi_connected(void)
{
    return connected && Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED;
}

const char *wifi_ssid(void)
{
    return cur_ssid;
}

int wifi_bars(void)
{
    return wifi_connected() ? (cur_bars > 0 ? cur_bars : 3) : 0;
}

// ---------------------------------------------------------------------------
// Saved networks (tab separated: ssid<TAB>password)

static void load_saved(void)
{
    num_saved = 0;
    FILE *f = fopen(saved_path, "r");
    if (!f)
        return;
    char line[160];
    while (num_saved < MAX_SAVED && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        snprintf(saved[num_saved].ssid, sizeof(saved[0].ssid), "%s", line);
        snprintf(saved[num_saved].pass, sizeof(saved[0].pass), "%s", tab + 1);
        num_saved++;
    }
    fclose(f);
}

static void write_saved(void)
{
    FILE *f = fopen(saved_path, "w");
    if (!f)
        return;
    fprintf(f, "# DSiDL wifi-netwerken: naam<TAB>wachtwoord\n");
    for (int i = 0; i < num_saved; i++)
        fprintf(f, "%s\t%s\n", saved[i].ssid, saved[i].pass);
    fclose(f);
}

static saved_net_t *find_saved(const char *ssid)
{
    for (int i = 0; i < num_saved; i++)
        if (strcmp(saved[i].ssid, ssid) == 0)
            return &saved[i];
    return NULL;
}

static void remember(const char *ssid, const char *pass)
{
    saved_net_t *s = find_saved(ssid);
    if (!s)
    {
        if (num_saved == MAX_SAVED)
        {
            memmove(&saved[0], &saved[1], sizeof(saved_net_t) * (MAX_SAVED - 1));
            num_saved--;
        }
        s = &saved[num_saved++];
    }
    snprintf(s->ssid, sizeof(s->ssid), "%s", ssid);
    snprintf(s->pass, sizeof(s->pass), "%s", pass ? pass : "");
    write_saved();
}

static void forget(const char *ssid)
{
    for (int i = 0; i < num_saved; i++)
    {
        if (strcmp(saved[i].ssid, ssid) == 0)
        {
            memmove(&saved[i], &saved[i + 1], sizeof(saved_net_t) * (num_saved - i - 1));
            num_saved--;
            write_saved();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Scanning

static int signal_bars(const Wifi_AccessPoint *ap)
{
    int r = ap->rssi;
    if (isDSiMode())
    {
        // dBm
        if (r == 0) return 0;
        if (r > -55) return 4;
        if (r > -65) return 3;
        if (r > -75) return 2;
        return 1;
    }
    // DS mode: 0..255
    if (r > 170) return 4;
    if (r > 120) return 3;
    if (r > 70) return 2;
    return r > 0 ? 1 : 0;
}

static int cmp_ap(const void *a, const void *b)
{
    const Wifi_AccessPoint *x = a, *y = b;
    return signal_bars(y) - signal_bars(x);
}

// Collects visible networks (one entry per name, strongest first)
static void collect_aps(void)
{
    num_aps = 0;
    int n = Wifi_GetNumAP();
    for (int i = 0; i < n && num_aps < MAX_APS; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK)
            continue;
        if (ap.ssid_len == 0 || ap.ssid[0] == '\0')
            continue;
        if (ap.flags & WFLAG_APDATA_NINTENDO_TAG)
            continue; // local DS multiplayer hosts

        int dup = -1;
        for (int j = 0; j < num_aps; j++)
            if (strcmp(aps[j].ssid, ap.ssid) == 0)
                dup = j;
        if (dup >= 0)
        {
            if (signal_bars(&ap) > signal_bars(&aps[dup]))
                aps[dup] = ap;
            continue;
        }
        aps[num_aps++] = ap;
    }
    qsort(aps, num_aps, sizeof(Wifi_AccessPoint), cmp_ap);
}

static void scan_for(int frames)
{
    Wifi_ScanMode();
    for (int i = 0; i < frames; i++)
        cothread_yield_irq(IRQ_VBLANK);
    collect_aps();
}

// ---------------------------------------------------------------------------
// Connecting

static const char *status_text(int s)
{
    switch (s)
    {
        case ASSOCSTATUS_DISCONNECTED: return "niet verbonden";
        case ASSOCSTATUS_SEARCHING: return "netwerk zoeken";
        case ASSOCSTATUS_AUTHENTICATING: return "wachtwoord controleren";
        case ASSOCSTATUS_ASSOCIATING: return "verbinden";
        case ASSOCSTATUS_ACQUIRINGDHCP: return "IP-adres ophalen";
        case ASSOCSTATUS_ASSOCIATED: return "verbonden";
        case ASSOCSTATUS_CANNOTCONNECT: return "kan niet verbinden";
        default: return "...";
    }
}

// Connects to 'ap'. pass == NULL means: use the DSi settings for this network.
// Shows progress on the current console. B cancels.
static bool connect_ap(Wifi_AccessPoint *ap, const char *pass)
{
    Wifi_SetIP(0, 0, 0, 0, 0); // DHCP

    int r;
    if (pass == NULL)
        r = Wifi_ConnectWfcAP(ap);
    else if (pass[0] == '\0')
        r = Wifi_ConnectSecureAP(ap, NULL, 0);
    else
        r = Wifi_ConnectSecureAP(ap, pass, strlen(pass));
    if (r != 0)
        return false;

    int last = -1;
    for (int frame = 0; frame < 60 * 30; frame++)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        if (keysDown() & KEY_B)
            break;

        int s = Wifi_AssocStatus();
        if (s != last)
        {
            char t[80];
            snprintf(t, sizeof(t), "%s: %s", ap->ssid, status_text(s));
            if (status_cb)
                status_cb(t);
            last = s;
        }
        if (s == ASSOCSTATUS_ASSOCIATED)
        {
            connected = true;
            snprintf(cur_ssid, sizeof(cur_ssid), "%s", ap->ssid);
            cur_bars = signal_bars(ap);
            return true;
        }
        if (s == ASSOCSTATUS_CANNOTCONNECT)
            break;
    }
    Wifi_DisconnectAP();
    connected = false;
    return false;
}

// Tries saved networks in range, then the DSi's own settings.
static bool auto_connect(void)
{
    connected = false;
    cur_ssid[0] = '\0';

    // 1. Networks saved in DSiDL, strongest first
    for (int i = 0; i < num_aps; i++)
    {
        saved_net_t *s = find_saved(aps[i].ssid);
        if (!s || !(aps[i].flags & WFLAG_APDATA_COMPATIBLE))
            continue;
        status("Verbinden met %.24s", aps[i].ssid);
        if (connect_ap(&aps[i], s->pass))
            return true;
    }

    // 2. Networks from the DSi settings (connection 1-6)
    for (int i = 0; i < num_aps; i++)
    {
        if (!(aps[i].flags & WFLAG_APDATA_CONFIG_IN_WFC))
            continue;
        status("Verbinden met %.24s", aps[i].ssid);
        if (connect_ap(&aps[i], NULL))
            return true;
    }

    // 3. Last resort: let the library pick a DSi-settings network
    if (Wifi_GetData(WIFIGETDATA_NUMWFCAPS, 0, NULL) > 0)
    {
        Wifi_AutoConnect();
        for (int frame = 0; frame < 60 * 20; frame++)
        {
            cothread_yield_irq(IRQ_VBLANK);
            int s = Wifi_AssocStatus();
            if (s == ASSOCSTATUS_ASSOCIATED)
            {
                connected = true;
                return true;
            }
            if (s == ASSOCSTATUS_CANNOTCONNECT)
                break;
        }
    }
    return false;
}

bool wifi_start(const char *saved_file, bool autoconnect)
{
    snprintf(saved_path, sizeof(saved_path), "%s", saved_file);
    load_saved();

    if (!Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE))
        return false;
    Wifi_EnableWifi();
    if (!autoconnect)
        return false;

    status("%s", "Netwerken zoeken");
    scan_for(150);
    return auto_connect();
}

// ---------------------------------------------------------------------------
// Manager screen

static char conn_status[96];

static void conn_cb(const char *t)
{
    snprintf(conn_status, sizeof(conn_status), "%s", t);
    app_status("Verbinden...", conn_status);
}

static void draw_manager(int sel, int scroll)
{
    static ui_row_t rows[MAX_APS];
    static char subs[MAX_APS][48];
    static char bars[MAX_APS][2];

    for (int i = 0; i < num_aps; i++)
    {
        Wifi_AccessPoint *ap = &aps[i];
        bool ok = ap->flags & WFLAG_APDATA_COMPATIBLE;
        bool open = ap->security_type == AP_SECURITY_OPEN;
        const char *extra = find_saved(ap->ssid) ? " - opgeslagen" :
                            (ap->flags & WFLAG_APDATA_CONFIG_IN_WFC) ? " - DSi-instellingen" : "";
        if (!ok)
            snprintf(subs[i], sizeof(subs[i]), "lock %s - niet ondersteund",
                     Wifi_ApSecurityTypeString(ap->security_type));
        else if (open)
            snprintf(subs[i], sizeof(subs[i]), "Open netwerk%s", extra);
        else
            snprintf(subs[i], sizeof(subs[i]), "lock %s%s",
                     Wifi_ApSecurityTypeString(ap->security_type), extra);
        bars[i][0] = '0' + signal_bars(ap);
        bars[i][1] = 0;
        rows[i].title = ap->ssid;
        rows[i].subtitle = subs[i];
        rows[i].category = NULL;
        rows[i].badge = bars[i];
    }

    const char *lines[6];
    char l0[80];
    int n = 0;
    if (wifi_connected())
    {
        u32 ip = Wifi_GetIP();
        snprintf(l0, sizeof(l0), "!Verbonden met %s (%lu.%lu.%lu.%lu)", cur_ssid,
                 ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
        lines[n++] = l0;
    }
    else
        lines[n++] = "!Nu niet verbonden (zoeken verbreekt de verbinding)";
    lines[n++] = isDSiMode() ? "DSi-modus: WPA2, WPA, WEP en open netwerken."
                             : "DS-modus: alleen WEP en open netwerken.";
    lines[n++] = "";
    lines[n++] = "A: verbinden   Y: wachtwoord   X: vergeten   R: zoeken";
    lines[n++] = "";
    lines[n++] = "Wachtwoorden worden bewaard in _dsidl/wifi.txt op je SD-kaart.";
    ui_info_top("Wifi-netwerken", lines, n, wifi_connected(), wifi_bars(), cur_ssid);

    static const char *labels[] = { "Verbinden", "Wachtwoord", "Zoeken", "Terug" };
    static const char *keys[] = { "A", "Y", "R", "B" };
    ui_list_screen("Netwerken in de buurt", isDSiMode() ? "DSi-modus - WPA2 ondersteund" : "DS-modus - alleen WEP/open",
                   rows, num_aps, sel, scroll, 2, labels, keys, 4, "Zoeken naar netwerken...");
    gfx_present();
}

static bool try_connect_ui(Wifi_AccessPoint *ap, bool ask_password)
{
    char pass[65] = "";
    saved_net_t *s = find_saved(ap->ssid);
    bool wfc = ap->flags & WFLAG_APDATA_CONFIG_IN_WFC;
    bool open = ap->security_type == AP_SECURITY_OPEN;
    const char *use = NULL;

    if (!(ap->flags & WFLAG_APDATA_COMPATIBLE))
    {
        app_message("Niet ondersteund", isDSiMode()
            ? "Dit netwerk gebruikt een beveiliging die de DSi niet ondersteunt."
            : "Dit netwerk gebruikt WPA/WPA2. Start DSiDL in DSi-modus (instelling in je loader) om het te gebruiken.");
        return false;
    }

    if (open)
        use = "";
    else if (!ask_password && s)
        use = s->pass;
    else if (!ask_password && wfc)
        use = NULL; // DSi settings
    else
    {
        if (s)
            snprintf(pass, sizeof(pass), "%s", s->pass);
        char title[64];
        snprintf(title, sizeof(title), "Wachtwoord voor %.32s", ap->ssid);
        if (!app_text_input(title, "wachtwoord", pass, sizeof(pass), true))
            return false;
        use = pass;
    }

    Wifi_AccessPoint copy = *ap;
    snprintf(conn_status, sizeof(conn_status), "%s", ap->ssid);
    app_status("Verbinden...", conn_status);
    void (*old)(const char *) = status_cb;
    status_cb = conn_cb;
    bool ok = connect_ap(&copy, use);
    status_cb = old;

    if (ok)
    {
        if (use != NULL)
            remember(ap->ssid, use);
        u32 ip = Wifi_GetIP();
        char msg[96];
        snprintf(msg, sizeof(msg), "Verbonden met %s.\nIP-adres: %lu.%lu.%lu.%lu", ap->ssid,
                 ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
        app_message("Gelukt!", msg);
        return true;
    }
    app_message("Verbinden mislukt", open ? "Het netwerk reageert niet. Probeer het opnieuw of ga dichter bij de router zitten."
                                          : "Klopt het wachtwoord? Kies het netwerk en druk op Y om het opnieuw in te voeren.");
    return false;
}

bool wifi_manager(void)
{
    bool was_connected = wifi_connected();
    bool now_connected = false;

    app_status("Wifi", "Zoeken naar netwerken...");
    Wifi_DisconnectAP();
    connected = false;
    scan_for(120);

    int sel = 0, scroll = 0, drag = 0;
    int frame = 0;
    bool dirty = true;

    while (1)
    {
        if (dirty)
        {
            if (sel >= num_aps)
                sel = num_aps > 0 ? num_aps - 1 : 0;
            if (sel < scroll)
                scroll = sel;
            if (sel >= scroll + LIST_ROWS)
                scroll = sel - LIST_ROWS + 1;
            draw_manager(sel, scroll);
            dirty = false;
        }

        app_wait_frame();
        input_t in = app_input();
        u32 k = in.down;

        if (++frame % 180 == 0 && !in.touching)
        {
            char keep[33] = "";
            if (num_aps > 0)
                snprintf(keep, sizeof(keep), "%s", aps[sel].ssid);
            collect_aps();
            for (int i = 0; i < num_aps; i++)
                if (strcmp(aps[i].ssid, keep) == 0)
                    sel = i;
            dirty = true;
        }

        if (in.drag_dy)
        {
            drag += in.drag_dy;
            while (drag <= -30 && scroll + LIST_ROWS < num_aps) { scroll++; drag += 30; if (sel < scroll) sel = scroll; dirty = true; }
            while (drag >= 30 && scroll > 0) { scroll--; drag -= 30; if (sel >= scroll + LIST_ROWS) sel = scroll + LIST_ROWS - 1; dirty = true; }
        }
        if (!in.touching)
            drag = 0;

        bool connect = false, ask = false;
        if (in.hit >= HIT_ROW && in.hit < HIT_ROW + MAX_APS)
        {
            if (in.hit - HIT_ROW == sel)
                connect = true;
            else
            {
                sel = in.hit - HIT_ROW;
                dirty = true;
            }
        }
        if ((k & KEY_DOWN) && sel + 1 < num_aps) { sel++; dirty = true; }
        if ((k & KEY_UP) && sel > 0) { sel--; dirty = true; }
        if ((k & KEY_R) || in.hit == HIT_BTN + 2)
        {
            app_status("Wifi", "Opnieuw zoeken...");
            scan_for(120);
            sel = 0;
            dirty = true;
        }
        if ((k & KEY_X) && num_aps > 0 && find_saved(aps[sel].ssid))
        {
            char q[80];
            snprintf(q, sizeof(q), "Opgeslagen wachtwoord van %s vergeten?", aps[sel].ssid);
            if (app_confirm("Vergeten", q, "Vergeten", "Nee"))
                forget(aps[sel].ssid);
            dirty = true;
        }
        if ((k & KEY_A) || in.hit == HIT_BTN + 0)
            connect = true;
        if ((k & KEY_Y) || in.hit == HIT_BTN + 1)
            connect = ask = true;

        if (connect && num_aps > 0)
        {
            if (try_connect_ui(&aps[sel], ask))
            {
                now_connected = true;
                break;
            }
            Wifi_ScanMode();
            dirty = true;
        }
        if ((k & KEY_B) || in.hit == HIT_BTN + 3)
            break;
    }

    if (!now_connected && was_connected)
    {
        app_status("Wifi", "Opnieuw verbinden...");
        scan_for(90);
        now_connected = auto_connect();
    }
    return now_connected;
}
