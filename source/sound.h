// DSiDL - small UI sounds, generated in code (no sound files)
#ifndef DSIDL_SOUND_H
#define DSIDL_SOUND_H

#include <stdbool.h>

enum { SND_TICK, SND_CLICK, SND_OPEN, SND_BACK, SND_SUCCESS, SND_ERROR, SND_COUNT };

void sound_init(void);
void sound_enable(bool on);
bool sound_enabled(void);
void sound_play(int which);

#endif
