// DSiDL - UI sounds synthesised at start-up (short sine "blips")
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "sound.h"

#define RATE 16384

static int8_t *samples[SND_COUNT];
static uint32_t lengths[SND_COUNT];
static bool enabled = true;
static bool ready = false;

// Integer sine, input 0..1023 = one period, output -127..127
static int isin(int p)
{
    p &= 1023;
    int x = p & 511;                 // half period
    int v = (x * (512 - x)) >> 9;    // parabola 0..128
    v = v * 255 / 128;               // 0..255
    if (v > 127)
        v = 127;
    return p < 512 ? v : -v;
}

// Tone from f0 to f1 (Hz) over 'ms', with a fast attack and soft decay.
// Several segments can be appended to make little melodies.
static void tone(int8_t *buf, uint32_t *pos, int f0, int f1, int ms, int vol)
{
    int n = RATE * ms / 1000;
    uint32_t phase = 0;
    for (int i = 0; i < n; i++)
    {
        int f = f0 + (f1 - f0) * i / n;
        phase += (uint32_t)f * 1024 * 256 / RATE;
        int env = i < 40 ? i * 256 / 40 : 256 - (i - 40) * 256 / (n - 40 + 1);
        int v = isin(phase >> 8) * env / 256 * vol / 100;
        // a little second harmonic makes it sound rounder
        v += isin((phase >> 7)) * env / 256 * vol / 400;
        if (v > 127) v = 127;
        if (v < -127) v = -127;
        buf[(*pos)++] = (int8_t)v;
    }
}

static void make(int id, int total_ms, void (*fn)(int8_t *, uint32_t *))
{
    uint32_t max = RATE * total_ms / 1000 + 64;
    int8_t *b = calloc(1, (max + 3) & ~3u);
    if (!b)
        return;
    uint32_t pos = 0;
    fn(b, &pos);
    lengths[id] = (pos + 3) & ~3u;
    samples[id] = b;
    DC_FlushRange(b, lengths[id]);
}

static void s_tick(int8_t *b, uint32_t *p) { tone(b, p, 1800, 1700, 18, 35); }
static void s_click(int8_t *b, uint32_t *p) { tone(b, p, 1250, 1100, 30, 60); }
static void s_open(int8_t *b, uint32_t *p) { tone(b, p, 700, 1150, 60, 55); }
static void s_back(int8_t *b, uint32_t *p) { tone(b, p, 1100, 650, 60, 55); }
static void s_success(int8_t *b, uint32_t *p)
{
    tone(b, p, 880, 880, 90, 60);
    tone(b, p, 1175, 1175, 90, 60);
    tone(b, p, 1568, 1568, 160, 60);
}
static void s_error(int8_t *b, uint32_t *p)
{
    tone(b, p, 330, 300, 120, 70);
    tone(b, p, 260, 220, 160, 70);
}

void sound_init(void)
{
    soundEnable();
    make(SND_TICK, 18, s_tick);
    make(SND_CLICK, 30, s_click);
    make(SND_OPEN, 60, s_open);
    make(SND_BACK, 60, s_back);
    make(SND_SUCCESS, 340, s_success);
    make(SND_ERROR, 280, s_error);
    ready = true;
}

void sound_enable(bool on)
{
    enabled = on;
}

bool sound_enabled(void)
{
    return enabled;
}

void sound_play(int which)
{
    if (!ready || !enabled || which < 0 || which >= SND_COUNT || !samples[which])
        return;
    soundPlaySample(samples[which], SoundFormat_8Bit, lengths[which], RATE, 90, 64, false, 0);
}
