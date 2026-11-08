// A plataforma no 360: controle pelo XAM, relogio, arquivos (game:, Hdd1:\BotStrike, Usb0:\BotStrike), log do xsdk e
// o audio numa thread propria, que mistura e entrega quadros de 256 amostras ao driver do kernel.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "plat.h"
#include "xsdk/audio.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

void gfx_xbox_fatal(const char *msg);

static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t audio_thread;
static volatile bool audio_running;
static PlatMixer mixer;
static double t0;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

bool plat_open(char *err, uint32_t eb)
{
	(void)err;
	(void)eb;
	t0 = now_s();
	xsdk_log_write("botstrike: porte completo (menu, partida, bots, Build Fight, Portal Strike)");
	return true;
}

void plat_close(void)
{
	plat_audio_stop();
	xsdk_log_write("botstrike: fim");
}

bool plat_quit_requested(void)
{
	return false; // sai pelo menu (QUIT) ou pelo Guide
}

double plat_seconds(void)
{
	return now_s() - t0;
}

void plat_capture_mouse(bool on)
{
	(void)on;
}

static float stick(int16_t v, int dead)
{
	if (v > -dead && v < dead)
		return 0.0f;
	const float s = v > 0 ? (float)(v - dead) / (32767.0f - dead) : (float)(v + dead) / (32768.0f - dead);
	return s < -1 ? -1 : (s > 1 ? 1 : s);
}

void plat_poll(PadState *pad)
{
	memset(pad, 0, sizeof *pad);
	INPUT_STATE in = { 0 };
	if (XamInputGetState(0, 0, &in) != ERROR_SUCCESS)
		return;
	pad->connected = true;
	pad->lx = stick(in.gamepad.thumb_lx, 7849);
	pad->ly = stick(in.gamepad.thumb_ly, 7849);
	pad->rx = stick(in.gamepad.thumb_rx, 8689);
	pad->ry = stick(in.gamepad.thumb_ry, 8689);
	pad->lt = in.gamepad.left_trigger / 255.0f;
	pad->rt = in.gamepad.right_trigger / 255.0f;
	pad->buttons = in.gamepad.buttons;
	pad->mouse_x = 640;
	pad->mouse_y = 360;
}

const char *plat_find_file(const char *name)
{
	static char path[256];
	const char *dirs[] = { "game:/", "hdd:/BotStrike/", "usb:/BotStrike/" };
	for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
		snprintf(path, sizeof path, "%s%s", dirs[i], name);
		FILE *f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return path;
		}
	}
	snprintf(path, sizeof path, "game:/%s", name);
	return path;
}

void plat_log(const char *fmt, ...)
{
	char line[512];
	va_list a;
	va_start(a, fmt);
	vsnprintf(line, sizeof line, fmt, a);
	va_end(a);
	xsdk_log_write("%s", line);
}

void plat_fatal(const char *msg)
{
	xsdk_log_write("botstrike: ERRO: %s", msg);
	gfx_xbox_fatal(msg);
}

// ---------------------------------------------------------------- audio

static void *audio_main(void *unused)
{
	(void)unused;
	static float stereo[XSDK_AUDIO_FRAME_SAMPLES * 2];
	while (audio_running) {
		float *frame;
		const NTSTATUS st = xsdk_audio_next_frame(100, &frame);
		if (st == STATUS_INVALID_HANDLE)
			break;
		if (!NT_SUCCESS(st))
			continue;
		mixer(stereo, XSDK_AUDIO_FRAME_SAMPLES);
		// os 6 canais vem um depois do outro: frente esquerda, frente direita, e o resto em silencio
		for (int i = 0; i < XSDK_AUDIO_FRAME_SAMPLES; i++) {
			frame[XSDK_AUDIO_FRONT_LEFT * XSDK_AUDIO_FRAME_SAMPLES + i] = stereo[i * 2];
			frame[XSDK_AUDIO_FRONT_RIGHT * XSDK_AUDIO_FRAME_SAMPLES + i] = stereo[i * 2 + 1];
		}
		for (int c = 2; c < XSDK_AUDIO_CHANNELS; c++)
			memset(frame + c * XSDK_AUDIO_FRAME_SAMPLES, 0, XSDK_AUDIO_FRAME_SAMPLES * sizeof(float));
		xsdk_audio_queue_frame();
	}
	return NULL;
}

bool plat_audio_start(PlatMixer m)
{
	mixer = m;
	if (!NT_SUCCESS(xsdk_audio_open(0)))
		return false;
	audio_running = true;
	if (pthread_create(&audio_thread, NULL, audio_main, NULL) != 0) {
		audio_running = false;
		xsdk_audio_close();
		return false;
	}
	return true;
}

void plat_audio_stop(void)
{
	if (!audio_running)
		return;
	audio_running = false;
	xsdk_audio_close(); // a thread esperando no next_frame volta
	pthread_join(audio_thread, NULL);
}

void plat_audio_lock(void)
{
	pthread_mutex_lock(&audio_mutex);
}

void plat_audio_unlock(void)
{
	pthread_mutex_unlock(&audio_mutex);
}

// ---------------------------------------------------------------- rede

#include <errno.h>
#include <stdlib.h>

#include "xsdk/net.h"

typedef struct {
	PlatThreadFn fn;
	void *arg;
} ThreadStart;

static void *thread_main(void *p)
{
	ThreadStart s = *(ThreadStart *)p;
	free(p);
	s.fn(s.arg);
	return NULL;
}

bool plat_thread(PlatThreadFn fn, void *arg)
{
	ThreadStart *s = malloc(sizeof *s);
	if (!s)
		return false;
	s->fn = fn;
	s->arg = arg;
	pthread_t t;
	if (pthread_create(&t, NULL, thread_main, s) != 0) {
		free(s);
		return false;
	}
	pthread_detach(t);
	return true;
}

static pthread_mutex_t net_mutex = PTHREAD_MUTEX_INITIALIZER;

void plat_net_lock(void)
{
	pthread_mutex_lock(&net_mutex);
}

void plat_net_unlock(void)
{
	pthread_mutex_unlock(&net_mutex);
}

void plat_sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

// A rede do xsdk so sobe na primeira conexao, e o console leva uns segundos para ter IP: espera ate 10 s, como a dash
// do EPTX.
bool plat_net_ready(char *ip, int ip_bytes)
{
	struct in_addr address;
	int net = EAGAIN;
	for (int i = 0; i < 40 && (net = xsdk_net_address(&address)) == EAGAIN; i++)
		plat_sleep_ms(250);
	if (net != 0) {
		snprintf(ip, (size_t)ip_bytes, "%s", net == EAGAIN ? "sem IP ainda" : "sem rede");
		return false;
	}
	const uint8_t *b = (const uint8_t *)&address.s_addr;
	snprintf(ip, (size_t)ip_bytes, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	return true;
}

const char *const *plat_eptx_dirs(int *count)
{
	static const char *const dirs[] = { "hdd:/EPTX", "usb:/EPTX", "game:/EPTX" };
	*count = 3;
	return dirs;
}
