// Mixer de software: ate 32 vozes mono, com volume pela distancia e panorama pelo lado, misturadas a 48 kHz estereo.
// Portavel; a saida e da plataforma (plat_audio_start).
#ifndef BS_AUDIO_H
#define BS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "assets.h"
#include "mathx.h"

enum {
	AUDIO_UI = 0, // sem posicao
	AUDIO_WORLD = 1,
	AUDIO_MUSIC = 2,
};

void audio_init(void);
void audio_shutdown(void);
// Toca um som; pos so vale com AUDIO_WORLD. unit: a distancia ate onde o som fica no volume cheio (o unit_size do
// Godot). Devolve a voz, para parar ou soltar depois, ou -1.
int audio_play(const Sound *s, int kind, Vec3 pos, float volume, float unit, float pitch, bool loop);
void audio_stop(int voice);
void audio_stop_kind(int kind);
void audio_listener(Vec3 pos, float yaw);
void audio_volumes(float master, float music);
// Chamado pela thread de audio da plataforma.
void audio_mix(float *stereo, uint32_t frames);

#endif
