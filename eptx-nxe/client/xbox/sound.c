#include "sound.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xsdk/audio.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	MAX_CLIPS = 48,
	/* Uma espera por slot livre desiste depois disto, para tentar de novo. */
	FEED_WAIT_MS = 100,
	/* Um efeito da skin tem fracao de segundo; isto deixa de fora a musica dos creditos. */
	MAX_CLIP_BYTES = 4 * 1024 * 1024,
};

static const float VOLUME = 0.7f;

typedef struct {
	char name[40];
	int16_t *samples; /* estereo intercalado */
	int frames;       /* amostras por canal */
	bool missing;
} clip;

typedef struct {
	const clip *c;
	int pos;
} voice;

static char g_dir[128];
static clip g_clips[MAX_CLIPS];
static int g_clip_count;
static voice g_voices[SOUND_VOICES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_open;

/* Carrega na primeira vez; chamado com g_lock travado. */
static const clip *find_clip(const char *name)
{
	for (int i = 0; i < g_clip_count; i++)
		if (strcmp(g_clips[i].name, name) == 0)
			return g_clips[i].missing ? NULL : &g_clips[i];
	if (g_clip_count >= MAX_CLIPS)
		return NULL;
	clip *c = &g_clips[g_clip_count++];
	memset(c, 0, sizeof *c);
	snprintf(c->name, sizeof c->name, "%s", name);
	c->missing = true;

	char path[200];
	snprintf(path, sizeof path, "%s/Audio/%s.pcm", g_dir, name);
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		xsdk_log_write("eptx: som faltando: %s", path);
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	const long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *bytes = size > 4 && size <= MAX_CLIP_BYTES ? (uint8_t *)malloc((size_t)size) : NULL;
	if (bytes && fread(bytes, 1, (size_t)size, fp) == (size_t)size) {
		/* Little-endian no arquivo; montado byte a byte, vale no PowerPC e no PC. */
		const int count = (int)(size / 2);
		c->samples = (int16_t *)malloc((size_t)count * sizeof(int16_t));
		if (c->samples) {
			for (int i = 0; i < count; i++)
				c->samples[i] = (int16_t)(bytes[2 * i] | bytes[2 * i + 1] << 8);
			c->frames = count / 2;
			c->missing = false;
		}
	}
	free(bytes);
	fclose(fp);
	return c->missing ? NULL : c;
}

void sound_play(const char *name)
{
	if (!g_open || !name || !name[0])
		return;
	pthread_mutex_lock(&g_lock);
	const clip *c = find_clip(name);
	if (c) {
		/* Uma voz livre; sem nenhuma, a que esta tocando ha mais tempo. */
		int pick = 0;
		for (int i = 0; i < SOUND_VOICES; i++) {
			if (!g_voices[i].c) {
				pick = i;
				break;
			}
			if (g_voices[i].pos > g_voices[pick].pos)
				pick = i;
		}
		g_voices[pick].c = c;
		g_voices[pick].pos = 0;
	}
	pthread_mutex_unlock(&g_lock);
}

/* Um quadro: 256 amostras de cada um dos 6 canais, um canal depois do outro (xsdk/audio.h). */
static void fill(float *frame)
{
	volatile float *const out = frame;
	float left[XSDK_AUDIO_FRAME_SAMPLES] = { 0 }, right[XSDK_AUDIO_FRAME_SAMPLES] = { 0 };
	pthread_mutex_lock(&g_lock);
	for (int v = 0; v < SOUND_VOICES; v++) {
		voice *vc = &g_voices[v];
		if (!vc->c)
			continue;
		for (int i = 0; i < XSDK_AUDIO_FRAME_SAMPLES && vc->pos < vc->c->frames; i++, vc->pos++) {
			left[i] += (float)vc->c->samples[2 * vc->pos] * (VOLUME / 32768.0f);
			right[i] += (float)vc->c->samples[2 * vc->pos + 1] * (VOLUME / 32768.0f);
		}
		if (vc->pos >= vc->c->frames)
			vc->c = NULL;
	}
	pthread_mutex_unlock(&g_lock);
	for (int i = 0; i < XSDK_AUDIO_FRAME_SAMPLES; i++) {
		const float l = left[i] > 1 ? 1 : left[i] < -1 ? -1 : left[i];
		const float r = right[i] > 1 ? 1 : right[i] < -1 ? -1 : right[i];
		for (int ch = 0; ch < XSDK_AUDIO_CHANNELS; ch++)
			out[ch * XSDK_AUDIO_FRAME_SAMPLES + i] =
				ch == XSDK_AUDIO_FRONT_LEFT ? l : ch == XSDK_AUDIO_FRONT_RIGHT ? r : 0.0f;
	}
}

/* Espera o kernel levar um quadro e enche o proximo, ate o audio fechar. Silencio quando nada toca. */
static void *feed(void *unused)
{
	(void)unused;
	for (;;) {
		float *frame;
		const NTSTATUS status = xsdk_audio_next_frame(FEED_WAIT_MS, &frame);
		if (status == STATUS_IO_TIMEOUT)
			continue;
		if (!NT_SUCCESS(status))
			return NULL;
		fill(frame);
		xsdk_audio_queue_frame();
	}
}

void sound_start(const char *dir)
{
	snprintf(g_dir, sizeof g_dir, "%s", dir);
	const NTSTATUS status = xsdk_audio_open(0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("eptx: audio nao abriu: 0x%08X", (unsigned)status);
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, feed, NULL) != 0) {
		xsdk_log_write("eptx: thread do audio nao subiu");
		xsdk_audio_close();
		return;
	}
	pthread_detach(thread);
	g_open = true;
}
