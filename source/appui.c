// DSiDL - interactive UI helpers on the DS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "appui.h"
#include "wifi.h"

volatile uint32_t g_frames;

static bool touch_active, touch_moved;
static int touch_x0, touch_y0, touch_last_y;

void app_wait_frame(void)
{
    cothread_yield_irq(IRQ_VBLANK);
    g_frames++;
}

input_t app_input(void)
{
    input_t in = { 0 };
    in.hit = -1;
    scanKeys();
    in.down = keysDown();
    in.held = keysHeld();

    if (in.held & KEY_TOUCH)
    {
        touchPosition t;
        touchRead(&t);
        if (!touch_active)
        {
            touch_active = true;
            touch_moved = false;
            touch_x0 = t.px;
            touch_y0 = t.py;
            touch_last_y = t.py;
        }
        else
        {
            if (abs(t.py - touch_y0) > 8)
                touch_moved = true;
            if (touch_moved)
            {
                in.drag_dy = t.py - touch_last_y;
                touch_last_y = t.py;
            }
        }
        in.touching = true;
    }
    else if (touch_active)
    {
        touch_active = false;
        if (!touch_moved)
            in.hit = ui_hit_test(touch_x0, touch_y0);
    }
    return in;
}

static uint32_t key_mask(const char *k)
{
    if (!k)
        return 0;
    if (strcmp(k, "A") == 0) return KEY_A;
    if (strcmp(k, "B") == 0) return KEY_B;
    if (strcmp(k, "X") == 0) return KEY_X;
    if (strcmp(k, "Y") == 0) return KEY_Y;
    if (strcmp(k, "L") == 0) return KEY_L;
    if (strcmp(k, "R") == 0) return KEY_R;
    if (strcmp(k, "START") == 0) return KEY_START;
    if (strcmp(k, "SELECT") == 0) return KEY_SELECT;
    return 0;
}

int app_dialog(const char *title, const char *text, const char *const *labels,
               const char *const *keys, int n, int primary)
{
    // Keep what's behind so the dialog can be redrawn on the same backdrop
    color_t *backup = malloc(SCR_W * SCR_H * 2);
    if (backup)
        memcpy(backup, scr_bot.px, SCR_W * SCR_H * 2);

    ui_dialog(title, text, labels, keys, n, primary);
    gfx_present();

    bool b_used = false;
    for (int i = 0; i < n; i++)
        if (keys && keys[i] && strcmp(keys[i], "B") == 0)
            b_used = true;

    int result = -1;
    // Ignore keys still held from before the dialog opened
    for (int i = 0; i < 2; i++)
        app_wait_frame();
    while (1)
    {
        app_wait_frame();
        input_t in = app_input();
        for (int i = 0; i < n; i++)
        {
            if ((keys && (in.down & key_mask(keys[i]))) || in.hit == HIT_BTN + i)
            {
                result = i;
                goto done;
            }
        }
        if (!b_used && (in.down & KEY_B))
            goto done;
    }
done:
    if (backup)
    {
        memcpy(scr_bot.px, backup, SCR_W * SCR_H * 2);
        free(backup);
    }
    return result;
}

void app_message(const char *title, const char *text)
{
    const char *labels[] = { "OK" };
    const char *keys[] = { "A" };
    app_dialog(title, text, labels, keys, 1, 0);
}

bool app_confirm(const char *title, const char *text, const char *yes, const char *no)
{
    const char *labels[] = { no ? no : "Nee", yes ? yes : "Ja" };
    const char *keys[] = { "B", "A" };
    return app_dialog(title, text, labels, keys, 2, 1) == 1;
}

void app_status(const char *title, const char *text)
{
    ui_background(&scr_bot);
    ui_dialog(title, text, NULL, NULL, 0, 0);
    gfx_present();
}

void app_wifi_info(bool *connected, int *bars, const char **ssid)
{
    *connected = wifi_connected();
    *bars = wifi_bars();
    *ssid = wifi_ssid();
}

bool app_text_input(const char *title, const char *hint, char *buf, size_t size,
                    bool password)
{
    ui_kbd_t k = { title, hint, buf, size, false, false, false, 11 };
    bool ok = false;

    bool c;
    int bars;
    const char *ssid;
    app_wifi_info(&c, &bars, &ssid);
    const char *lines_pw[] = {
        "Tik op de toetsen of gebruik de pijltjes en A.",
        "",
        "!B: wissen   START: klaar",
        "!SELECT: hoofdletters   X: tonen/verbergen",
        "",
        "Let op hoofdletters in wachtwoorden.",
    };
    const char *lines[] = {
        "Tik op de toetsen of gebruik de pijltjes en A.",
        "",
        "!B: wissen (leeg = terug)",
        "!START: klaar   SELECT: hoofdletters",
    };
    ui_info_top(title, password ? lines_pw : lines, password ? 6 : 4, c, bars, ssid);

    bool dirty = true;
    int repeat = 0;
    for (int i = 0; i < 2; i++)
        app_wait_frame();
    while (1)
    {
        if (dirty)
        {
            ui_keyboard(&k);
            gfx_present();
            dirty = false;
        }
        app_wait_frame();
        input_t in = app_input();

        u32 nav = in.down;
        if (in.held & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))
        {
            if (++repeat > 18 && (repeat % 4) == 0)
                nav |= in.held & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        }
        else
            repeat = 0;

        if (nav & KEY_LEFT) { ui_kbd_move(&k, -1, 0); dirty = true; }
        if (nav & KEY_RIGHT) { ui_kbd_move(&k, 1, 0); dirty = true; }
        if (nav & KEY_UP) { ui_kbd_move(&k, 0, -1); dirty = true; }
        if (nav & KEY_DOWN) { ui_kbd_move(&k, 0, 1); dirty = true; }
        if (in.down & KEY_SELECT) { k.shift = !k.shift; dirty = true; }
        if ((in.down & KEY_X) && password) { k.masked = !k.masked; dirty = true; }

        int code = 0;
        if (in.down & KEY_A)
            code = ui_kbd_key(&k, k.cursor_key);
        if (in.hit >= HIT_KEY && in.hit < HIT_KEY + 100)
        {
            k.cursor_key = in.hit - HIT_KEY;
            code = ui_kbd_key(&k, k.cursor_key);
        }
        if (in.down & KEY_START)
            code = KB_OK;
        if (in.down & KEY_B)
        {
            if (buf[0] == '\0')
                break; // cancel
            code = KB_BACK;
        }

        size_t len = strlen(buf);
        if (code == KB_OK)
        {
            ok = true;
            break;
        }
        else if (code == KB_BACK)
        {
            if (len > 0)
                buf[len - 1] = '\0';
            dirty = true;
        }
        else if (code == KB_SHIFT)
        {
            k.shift = !k.shift;
            dirty = true;
        }
        else if (code == KB_SYMS)
        {
            k.symbols = !k.symbols;
            dirty = true;
        }
        else if (code >= 32 && code < 256)
        {
            if (len + 1 < size)
            {
                buf[len] = (char)code;
                buf[len + 1] = '\0';
            }
            // Shift applies to one letter, like a phone keyboard
            if (k.shift && !k.symbols && code >= 'A' && code <= 'Z')
                k.shift = false;
            dirty = true;
        }
    }
    return ok;
}
