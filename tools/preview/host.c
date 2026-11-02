// Host preview: renders DSiDL screens to PPM so the design can be checked
// without a DSi.  cc -Isource tools/preview/host.c source/gfx.c source/ui.c
//    source/font_data.c source/qrcodegen.c -o preview
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "ui.h"

static color_t top_px[SCR_W * SCR_H], bot_px[SCR_W * SCR_H];

static void save(const char *name)
{
    FILE *f = fopen(name, "wb");
    fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H * 2);
    color_t *bufs[2] = { top_px, bot_px };
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < SCR_W * SCR_H; i++)
        {
            color_t c = bufs[b][i];
            unsigned char rgb[3] = { (c & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, ((c >> 10) & 31) * 255 / 31 };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

int main(void)
{
    scr_top.px = top_px; scr_bot.px = bot_px;
    gfx_noclip(&scr_top); gfx_noclip(&scr_bot);

    ui_item_t it = { "Pixel Doodle", "Thomas Armstrong", "v0.24.0", "app",
        "An offline pixel-art editor and animation tool for DS, DSi and 3DS. Supports layers, frames, onion skin and PNG export.",
        "PixelDoodle.nds", 403968, 0, 2 };
    ui_row_t rows[] = {
        { "A5200DS", "AlekMaul - 444 KB", "emulator", NULL },
        { "Bazado-DS", "game - 5.4 MB", "game", NULL },
        { "dsi-wifi-manager", "tasken - 172 KB", "utility", NULL },
        { "Pixel Doodle", "Thomas Armstrong - 394 KB", "app", NULL },
        { "TerrariaDS", "15.2 MB", "game", NULL },
    };
    const char *tabs[] = { "Alles", "Games", "Emu", "Tools", "Apps" };
    ui_browser_top(&it, true, 3, "Ziggo-5G-Thuis");
    ui_browser_bottom(rows, 5, 3, 0, "", tabs, 5, 0, NULL);
    save("p1_browser.ppm");

    const char *lab[] = { "Downloaden", "Andere map" }, *keys[] = { "A", "Y" };
    ui_browser_bottom(rows, 5, 3, 0, "pix", tabs, 5, 4, NULL);
    ui_dialog("Pixel Doodle", "PixelDoodle.nds (394 KB)\nOpslaan in: sd:/roms/nds", lab, keys, 2, 0);
    save("p2_dialog.ppm");

    ui_progress_screen("Pixel Doodle", "PixelDoodle.nds", "Downloaden...", 180000, 403968, 52000, 4, NULL);
    save("p3_progress.ppm");

    ui_tile_t tiles[] = {
        { "Telefoon", 0, RGB(38,190,110) }, { "Wifi", 1, RGB(18,158,230) },
        { "Lijst", 2, RGB(255,140,48) }, { "Updaten", 3, RGB(140,100,242) },
        { "Over", 4, RGB(120,132,150) }, { "Terug", 5, RGB(238,72,82) } };
    ui_menu(tiles, 6, 1, "Wifi-netwerken", "Zoek netwerken, kies er een en typ het wachtwoord. DSiDL onthoudt ze op je SD-kaart.", true, 4, "Ziggo-5G-Thuis");
    save("p4_menu.ppm");

    ui_server_top("http://10.112.19.151");
    ui_server_bottom("http://10.112.19.151", true, "Ontvangen", "TerrariaDS.nds", 5200000, 16017408, 180000, 60, "Ontvangen: dsidl.nds", 2);
    save("p5_server.ppm");

    char buf[65] = "Wachtwoord1";
    ui_kbd_t k = { "Wachtwoord voor Ziggo-5G-Thuis", "Let op hoofdletters", buf, sizeof(buf), false, false, false, 15 };
    const char *info[] = { "DSi-modus: WPA2, WPA, WEP en open", "", "!Tik op een toets of gebruik de pijltjes en A.", "START: wachtwoord tonen/verbergen" };
    ui_info_top("Wachtwoord invoeren", info, 4, false, 0, NULL);
    ui_keyboard(&k);
    save("p6_keyboard.ppm");

    ui_row_t nets[] = { { "Ziggo-5G-Thuis", "lock WPA2 - Opgeslagen", NULL, "4" }, { "TP-Link_23", "lock WPA2", NULL, "3" },
        { "Gast", "Open", NULL, "2" }, { "Buren-wifi", "lock WPA2 - DSi-instellingen", NULL, "1" } };
    const char *wl[] = { "Verbinden", "Wachtwoord", "Zoeken" }, *wk[] = { "A", "Y", "R" };
    const char *winfo[] = { "Verbonden met Ziggo-5G-Thuis", "IP 192.168.1.23", "", "Netwerken die je hier invoert worden bewaard op je SD-kaart." };
    ui_info_top("Wifi-netwerken", winfo, 4, true, 4, "Ziggo-5G-Thuis");
    ui_list_screen("Netwerken in de buurt", "DSi-modus - WPA2 ondersteund", nets, 4, 0, 0, 2, wl, wk, 3, NULL);
    save("p7_wifi.ppm");

    ui_row_t dirs[] = { { ".. (omhoog)", NULL, NULL, NULL }, { "gb", NULL, NULL, NULL }, { "gba", NULL, NULL, NULL }, { "nds", NULL, NULL, NULL }, { "snes", NULL, NULL, NULL } };
    const char *fl[] = { "Hier opslaan", "Nieuwe map", "Annuleren" }, *fk[] = { "X", "Y", "B" };
    ui_list_screen("Map kiezen", "sd:/roms", dirs, 5, 3, 0, 1, fl, fk, 3, NULL);
    save("p8_folder.ppm");

    const char *lines[] = { "Certificaten laden", "Wifi starten", "Verbinden met Ziggo-5G-Thuis" };
    ui_splash(lines, 3, 5);
    save("p9_splash.ppm");
    return 0;
}
