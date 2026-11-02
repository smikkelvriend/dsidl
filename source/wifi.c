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

extern PrintConsole top, bottom;

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

bool wifi_connected(void)
{
    return connected && Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED;
}

const char *wifi_ssid(void)
{
    return cur_ssid;
}

static void color(int c, int bright)
{
    printf("\x1b[%d;%dm", c, bright);
}

static void reset_color(void)
{
    printf("\x1b[39;0m");
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
            printf("  %s...\n", status_text(s));
            last = s;
        }
        if (s == ASSOCSTATUS_ASSOCIATED)
        {
            connected = true;
            snprintf(cur_ssid, sizeof(cur_ssid), "%s", ap->ssid);
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
        printf("Verbinden met %.20s\n", aps[i].ssid);
        if (connect_ap(&aps[i], s->pass))
            return true;
    }

    // 2. Networks from the DSi settings (connection 1-6)
    for (int i = 0; i < num_aps; i++)
    {
        if (!(aps[i].flags & WFLAG_APDATA_CONFIG_IN_WFC))
            continue;
        printf("Verbinden met %.20s\n", aps[i].ssid);
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

    printf("Netwerken zoeken...\n");
    scan_for(150);
    return auto_connect();
}

// ---------------------------------------------------------------------------
// Manager screen

static void draw_manager(int sel, int scroll)
{
    consoleSelect(&top);
    consoleClear();
    color(31, 1);
    printf("Wifi-netwerken\n");
    reset_color();
    printf("--------------------------------");
    printf("%s-modus: %s\n\n", isDSiMode() ? "DSi" : "DS",
           isDSiMode() ? "WPA2, WPA, WEP, open" : "alleen WEP en open");
    printf("A  verbinden\n");
    printf("Y  wachtwoord (opnieuw) invoeren\n");
    printf("X  opgeslagen netwerk vergeten\n");
    printf("R  opnieuw zoeken\n");
    printf("B  terug\n\n");
    printf("Signaal: ");
    color(32, 1);
    printf("||||");
    reset_color();
    printf(" sterk  ");
    color(31, 0);
    printf("|");
    reset_color();
    printf(" zwak\n\n");
    color(33, 1);
    printf("*");
    reset_color();
    printf(" = opgeslagen in DSiDL\n");
    color(36, 1);
    printf("D");
    reset_color();
    printf(" = staat in DSi-instellingen\n\n");
    printf("Wachtwoorden staan in\n%.32s\n", saved_path);

    consoleSelect(&bottom);
    consoleClear();
    if (num_aps == 0)
    {
        printf("\n Zoeken naar netwerken...\n");
        return;
    }
    const int rows = 22;
    for (int i = scroll; i < num_aps && i < scroll + rows; i++)
    {
        Wifi_AccessPoint *ap = &aps[i];
        int bars = signal_bars(ap);
        bool ok = ap->flags & WFLAG_APDATA_COMPATIBLE;
        bool sv = find_saved(ap->ssid) != NULL;
        bool wfc = ap->flags & WFLAG_APDATA_CONFIG_IN_WFC;

        if (i == sel)
            color(32, 1);
        printf("%c", i == sel ? '>' : ' ');
        printf("%-19.19s", ap->ssid);
        if (i == sel)
            reset_color();

        color(33, 1);
        printf("%c", sv ? '*' : ' ');
        color(36, 1);
        printf("%c", wfc ? 'D' : ' ');
        reset_color();

        const char *sec = ok ? Wifi_ApSecurityTypeString(ap->security_type) : "n.o.";
        printf("%-5.5s", sec);

        color(bars >= 3 ? 32 : bars == 2 ? 33 : 31, bars >= 3);
        for (int b = 0; b < 4; b++)
            printf("%c", b < bars ? '|' : ' ');
        reset_color();
        printf("\n");
    }
}

static void wait_key(void)
{
    printf("\nDruk op A");
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        if (keysDown() & (KEY_A | KEY_B))
            return;
    }
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
        consoleSelect(&bottom);
        consoleClear();
        printf("Dit netwerk gebruikt beveiliging\ndie in %s-modus niet werkt.\n",
               isDSiMode() ? "DSi" : "DS");
        if (!isDSiMode())
            printf("\nStart DSiDL in DSi-modus voor\nWPA/WPA2 (instelling in je\nloader).\n");
        wait_key();
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
        snprintf(title, sizeof(title), "Wachtwoord voor %.20s", ap->ssid);
        if (!ui_text_input(title, "Let op hoofdletters. START = tonen/verbergen", pass, sizeof(pass)))
            return false;
        use = pass;
    }

    consoleSelect(&bottom);
    consoleClear();
    color(33, 1);
    printf("Verbinden met\n%.32s\n\n", ap->ssid);
    reset_color();

    Wifi_AccessPoint copy = *ap;
    bool ok = connect_ap(&copy, use);
    if (ok)
    {
        if (use != NULL)
            remember(ap->ssid, use);
        u32 ip = Wifi_GetIP();
        color(32, 1);
        printf("\nVerbonden!\n");
        reset_color();
        printf("IP: %lu.%lu.%lu.%lu\n", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
        wait_key();
        return true;
    }

    color(31, 1);
    printf("\nVerbinden mislukt.\n");
    reset_color();
    if (!open)
        printf("Klopt het wachtwoord? Probeer\nhet opnieuw met Y.\n");
    wait_key();
    return false;
}

bool wifi_manager(void)
{
    bool was_connected = wifi_connected();
    bool now_connected = false;

    consoleSelect(&bottom);
    consoleClear();
    printf("\n Zoeken naar netwerken...\n");
    // Scanning drops the current connection
    Wifi_DisconnectAP();
    connected = false;
    scan_for(120);

    int sel = 0, scroll = 0;
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
            if (sel >= scroll + 22)
                scroll = sel - 21;
            draw_manager(sel, scroll);
            dirty = false;
        }

        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        u32 k = keysDown();

        // Refresh the list every 3 seconds while scanning
        if (++frame % 180 == 0)
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

        if (k & KEY_DOWN && sel + 1 < num_aps) { sel++; dirty = true; }
        if (k & KEY_UP && sel > 0) { sel--; dirty = true; }
        if (k & KEY_R)
        {
            consoleSelect(&bottom);
            consoleClear();
            printf("\n Opnieuw zoeken...\n");
            scan_for(120);
            sel = 0;
            dirty = true;
        }
        if ((k & KEY_X) && num_aps > 0 && find_saved(aps[sel].ssid))
        {
            forget(aps[sel].ssid);
            dirty = true;
        }
        if ((k & (KEY_A | KEY_Y)) && num_aps > 0)
        {
            if (try_connect_ui(&aps[sel], (k & KEY_Y) != 0))
            {
                now_connected = true;
                break;
            }
            Wifi_ScanMode(); // back to scanning for the list
            dirty = true;
        }
        if (k & KEY_B)
            break;
    }

    if (!now_connected && was_connected)
    {
        // Leaving without choosing: restore a connection
        consoleSelect(&bottom);
        consoleClear();
        printf("Opnieuw verbinden...\n");
        scan_for(90);
        now_connected = auto_connect();
    }
    return now_connected;
}
