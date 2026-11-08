#include "audio.h"

#include <string.h>

#include "plat.h"

enum {
	VOICES = 32,
	OUT_RATE = 48000,
};

typedef struct {
	const Sound *s;
	int kind;
	Vec3 pos;
	float volume, unit;
	double cursor, step;
	bool loop, active;
	uint32_t serial;
} Voice;

static Voice voices[VOICES];
static uint32_t serial;
static Vec3 listener_pos;
static float listener_yaw;
static float master = 0.5f, music = 0.35f;

void audio_init(void)
{
	memset(voices, 0, sizeof voices);
}

void audio_shutdown(void)
{
	plat_audio_lock();
	memset(voices, 0, sizeof voices);
	plat_audio_unlock();
}

int audio_play(const Sound *s, int kind, Vec3 pos, float volume, float unit, float pitch, bool loop)
{
	if (!s || !s->pcm || s->frames == 0)
		return -1;
	plat_audio_lock();
	int pick = -1;
	for (int i = 0; i < VOICES; i++)
		if (!voices[i].active) {
			pick = i;
			break;
		}
	if (pick < 0) {
		// sem voz livre: rouba a mais antiga que nao seja musica
		uint32_t oldest = UINT32_MAX;
		for (int i = 0; i < VOICES; i++)
			if (voices[i].kind != AUDIO_MUSIC && voices[i].serial < oldest) {
				oldest = voices[i].serial;
				pick = i;
			}
	}
	if (pick >= 0)
		voices[pick] = (Voice){ s, kind, pos, volume, unit > 0.1f ? unit : 10.0f, 0.0,
			(double)s->rate / OUT_RATE * (pitch > 0.1f ? pitch : 1.0f), loop, true, ++serial };
	plat_audio_unlock();
	return pick;
}

void audio_stop(int v)
{
	if (v < 0 || v >= VOICES)
		return;
	plat_audio_lock();
	voices[v].active = false;
	plat_audio_unlock();
}

void audio_stop_kind(int kind)
{
	plat_audio_lock();
	for (int i = 0; i < VOICES; i++)
		if (voices[i].kind == kind)
			voices[i].active = false;
	plat_audio_unlock();
}

void audio_listener(Vec3 pos, float yaw)
{
	listener_pos = pos;
	listener_yaw = yaw;
}

void audio_volumes(float m, float mu)
{
	master = clampf(m, 0.0f, 1.0f);
	music = clampf(mu, 0.0f, 1.0f);
}

void audio_mix(float *out, uint32_t frames)
{
	memset(out, 0, frames * 2 * sizeof(float));
	const Vec3 right = v3(cosf(listener_yaw), 0.0f, -sinf(listener_yaw));
	plat_audio_lock();
	for (int i = 0; i < VOICES; i++) {
		Voice *v = &voices[i];
		if (!v->active)
			continue;
		float gl = v->volume * master, gr = gl;
		if (v->kind == AUDIO_MUSIC) {
			gl *= music;
			gr *= music;
		} else if (v->kind == AUDIO_WORLD) {
			const Vec3 d = v3_sub(v->pos, listener_pos);
			const float dist = v3_length(d);
			const float att = dist <= v->unit ? 1.0f : v->unit / dist;
			const float side = dist > 0.01f ? v3_dot(d, right) / dist : 0.0f;
			gl *= att * (0.5f - 0.4f * side) * 2.0f * 0.7f;
			gr *= att * (0.5f + 0.4f * side) * 2.0f * 0.7f;
		}
		const int16_t *pcm = v->s->pcm;
		const uint32_t n = v->s->frames;
		for (uint32_t f = 0; f < frames; f++) {
			uint32_t k = (uint32_t)v->cursor;
			if (k >= n) {
				if (!v->loop) {
					v->active = false;
					break;
				}
				v->cursor -= n;
				k = (uint32_t)v->cursor;
			}
			const float frac = (float)(v->cursor - k);
			const float a = pcm[k], b = pcm[k + 1 < n ? k + 1 : (v->loop ? 0 : k)];
			const float s = (a + (b - a) * frac) * (1.0f / 32768.0f);
			out[f * 2] += s * gl;
			out[f * 2 + 1] += s * gr;
			v->cursor += v->step;
		}
	}
	plat_audio_unlock();
	for (uint32_t i = 0; i < frames * 2; i++)
		out[i] = clampf(out[i], -1.0f, 1.0f);
}
