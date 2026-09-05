#ifndef AUDIO_H
#define AUDIO_H
#include "estados.h"
#include "miniaudio.h"

static ma_engine engine;
static ma_sound backgroundSound;
void playBackground(const char *arquivo);
int audioInit();
void playAudio();
void audioClose();
Estado radio();

#endif