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

void gfx_present(void)
{
    DC_FlushRange(scr_top.px, SCR_W * SCR_H * 2);
    DC_FlushRange(scr_bot.px, SCR_W * SCR_H * 2);
    dmaCopyWords(3, scr_top.px, vram_top, SCR_W * SCR_H * 2);
    dmaCopyWords(3, scr_bot.px, vram_bot, SCR_W * SCR_H * 2);
}
