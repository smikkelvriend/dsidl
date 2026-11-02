// DSiDL - renderer: Nintendo DS(i) part (two 16-bit bitmap backgrounds)
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "gfx.h"

static u16 *vram_top, *vram_bot;

void gfx_init(void)
{
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    int bt = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    int bb = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    vram_top = bgGetGfxPtr(bt);
    vram_bot = bgGetGfxPtr(bb);

    scr_top.px = aligned_alloc(32, SCR_W * SCR_H * 2);
    scr_bot.px = aligned_alloc(32, SCR_W * SCR_H * 2);
    memset(scr_top.px, 0xFF, SCR_W * SCR_H * 2);
    memset(scr_bot.px, 0xFF, SCR_W * SCR_H * 2);
    gfx_noclip(&scr_top);
    gfx_noclip(&scr_bot);
    gfx_present();
}

static int pending_tr;
static u16 *old_top, *old_bot, *mix_buf;

void gfx_transition(int kind)
{
    if (!old_top)
    {
        old_top = malloc(SCR_W * SCR_H * 2);
        old_bot = malloc(SCR_W * SCR_H * 2);
        mix_buf = aligned_alloc(32, SCR_W * SCR_H * 2);
    }
    if (!old_top || !old_bot || !mix_buf)
        return;
    // The back buffers still hold what is on screen right now
    memcpy(old_top, scr_top.px, SCR_W * SCR_H * 2);
    memcpy(old_bot, scr_bot.px, SCR_W * SCR_H * 2);
    pending_tr = kind;
}

static void copy_to_vram(const u16 *src, u16 *vram)
{
    DC_FlushRange(src, SCR_W * SCR_H * 2);
    dmaCopyWords(3, src, vram, SCR_W * SCR_H * 2);
}

// Ease out cubic, t and result in 0..256
static int ease_out(int t)
{
    int u = 256 - t;
    return 256 - (u * u / 256) * u / 256;
}

static void compose(u16 *out, const u16 *old, const u16 *nw, int kind, int e)
{
    if (kind == TR_LEFT || kind == TR_RIGHT)
    {
        int off = SCR_W * e / 256; // how far the new screen has come in
        for (int y = 0; y < SCR_H; y++)
        {
            const u16 *o = old + y * SCR_W, *n = nw + y * SCR_W;
            u16 *d = out + y * SCR_W;
            if (kind == TR_LEFT)
            {
                // old moves left, new comes from the right
                memcpy(d, o + off, (SCR_W - off) * 2);
                memcpy(d + SCR_W - off, n, off * 2);
            }
            else
            {
                memcpy(d, n + SCR_W - off, off * 2);
                memcpy(d + off, o, (SCR_W - off) * 2);
            }
        }
    }
    else if (kind == TR_UP_BOT || kind == TR_DOWN_BOT)
    {
        int off = SCR_H * e / 256;
        if (kind == TR_UP_BOT)
        {
            // new slides up over the old one
            memcpy(out, old, (SCR_H - off) * SCR_W * 2);
            memcpy(out + (SCR_H - off) * SCR_W, nw, off * SCR_W * 2);
        }
        else
        {
            // old slides down, revealing the new one
            int keep = SCR_H - off;
            memcpy(out, nw, off * SCR_W * 2);
            memcpy(out + off * SCR_W, old, keep * SCR_W * 2);
        }
    }
    else
    {
        for (int i = 0; i < SCR_W * SCR_H; i++)
            out[i] = gfx_mix(old[i], nw[i], e);
    }
}

static void run_transition(void)
{
    int kind = pending_tr;
    pending_tr = TR_NONE;
    const int frames = kind == TR_FADE ? 6 : 8;
    bool both = kind == TR_LEFT || kind == TR_RIGHT || kind == TR_FADE;
    for (int f = 1; f < frames; f++)
    {
        int e = ease_out(f * 256 / frames);
        if (both)
        {
            compose(mix_buf, old_top, scr_top.px, kind, e);
            cothread_yield_irq(IRQ_VBLANK);
            copy_to_vram(mix_buf, vram_top);
            compose(mix_buf, old_bot, scr_bot.px, kind, e);
            copy_to_vram(mix_buf, vram_bot);
        }
        else
        {
            compose(mix_buf, old_bot, scr_bot.px, kind, e);
            cothread_yield_irq(IRQ_VBLANK);
            copy_to_vram(mix_buf, vram_bot);
            copy_to_vram(scr_top.px, vram_top);
        }
    }
}

void gfx_present(void)
{
    if (pending_tr)
        run_transition();
    copy_to_vram(scr_top.px, vram_top);
    copy_to_vram(scr_bot.px, vram_bot);
}

void gfx_present_top(void)
{
    if (pending_tr)
    {
        gfx_present();
        return;
    }
    copy_to_vram(scr_top.px, vram_top);
}

void gfx_present_bot(void)
{
    if (pending_tr)
    {
        gfx_present();
        return;
    }
    copy_to_vram(scr_bot.px, vram_bot);
}
