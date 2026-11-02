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

int app_ease(int cur, int target)
{
    int d = target - cur;
    if (d == 0)
        return cur;
    if (d > -2 && d < 2)
        return target;
    // Move about a third of the remaining distance each frame
    int step = d / 3;
    if (step == 0)
        step = d > 0 ? 1 : -1;
    return cur + step;
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
    // Keep what's behind so the dialog can be animated over it
    color_t *backup = malloc(SCR_W * SCR_H * 2);
    if (backup)
        memcpy(backup, scr_bot.px, SCR_W * SCR_H * 2);

    // Pop in
    for (int a = 32; a <= 256; a += 32)
    {
        if (backup)
            memcpy(scr_bot.px, backup, SCR_W * SCR_H * 2);
        ui_dialog(title, text, labels, keys, n, primary, a, -1);
        app_wait_frame();
        gfx_present_bot();
        if (!backup)
            break;
    }
    if (!backup)
    {
        ui_dialog(title, text, labels, keys, n, primary, 256, -1);
        gfx_present_bot();
    }

    bool b_used = false;
    for (int i = 0; i < n; i++)
        if (keys && keys[i] && strcmp(keys[i], "B") == 0)
            b_used = true;

    int result = -1;
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
        // Show the press, then shrink away
        if (result >= 0)
        {
            memcpy(scr_bot.px, backup, SCR_W * SCR_H * 2);
            ui_dialog(title, text, labels, keys, n, primary, 256, result);
            gfx_present_bot();
            for (int i = 0; i < 4; i++)
                app_wait_frame();
        }
        for (int a = 192; a >= 0; a -= 64)
        {
            memcpy(scr_bot.px, backup, SCR_W * SCR_H * 2);
            ui_dialog(title, text, labels, keys, n, primary, a, -1);
            app_wait_frame();
            gfx_present_bot();
        }
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
    ui_dialog(title, text, NULL, NULL, 0, 0, 256, -1);
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
    ui_kbd_t k = { title, hint, buf, size, false, false, false, 11, -1, 0 };
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
    gfx_transition(TR_UP_BOT); // keyboard slides up
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
        if (k.press_t > 0 && --k.press_t == 0)
            dirty = true;

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

        if (code != 0)
        {
            int pk = k.cursor_key;
            if (in.down & KEY_START)
                pk = -1;
            if (in.down & KEY_B)
                pk = -1;
            k.pressed_key = pk;
            k.press_t = 5;
            dirty = true;
        }

        size_t len = strlen(buf);
        if (code == KB_OK)
        {
            ok = true;
            // show the OK press briefly
            ui_keyboard(&k);
            gfx_present_bot();
            for (int i = 0; i < 4; i++)
                app_wait_frame();
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
    gfx_transition(TR_DOWN_BOT); // keyboard slides away on the next frame
    return ok;
}
