// DSiDL - entropy and time hooks required by Mbed TLS
#include <string.h>
#include <time.h>

#include <nds.h>
#include <mbedtls/platform_time.h>

static u32 mix_state = 0x9E3779B9;

static u32 mix(u32 x)
{
    mix_state ^= x + 0x7F4A7C15 + (mix_state << 6) + (mix_state >> 2);
    mix_state = (mix_state << 13) | (mix_state >> 19);
    mix_state *= 0x85EBCA6B;
    return mix_state;
}

// Gathers jitter from the video counter, the clock and input state. Not a
// true hardware RNG, but this is only used to seed a CTR-DRBG for TLS client
// connections.
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen)
{
    (void)data;
    for (size_t i = 0; i < len; i++)
    {
        u32 spins = 0;
        u16 v = REG_VCOUNT;
        while (REG_VCOUNT == v && spins < 4000)
            spins++;

        touchPosition t;
        touchRead(&t);
        u32 x = mix(spins ^ ((u32)REG_VCOUNT << 16) ^ (u32)time(NULL) ^
                    ((u32)t.rawx << 8) ^ t.rawy ^ keysHeld() ^ (u32)(uintptr_t)&x);
        output[i] = (unsigned char)(x ^ (x >> 8) ^ (x >> 16) ^ (x >> 24));
    }
    *olen = len;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)time(NULL) * 1000;
}
