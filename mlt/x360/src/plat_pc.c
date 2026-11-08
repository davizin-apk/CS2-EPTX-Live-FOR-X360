// A plataforma no PC: janela, teclado e mouse (os comandos do Bot Strike original), controle XInput se houver, e
// audio pelo waveOut. Serve para testar o jogo inteiro antes de ir para o console.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plat.h"

typedef struct {
	WORD buttons;
	BYTE lt, rt;
	SHORT lx, ly, rx, ry;
} XPad;
typedef struct {
	DWORD packet;
	XPad pad;
} XState;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XState *);

static HWND hwnd;
static HDC hdc;
static bool quit;
static bool keys[256];
static bool captured;
static bool mouse_l, mouse_r;
static float wheel;
static char typed[32];
static int typed_len;
static int typed_back;
static float mouse_x, mouse_y;
static XInputGetStateFn xinput;
static LARGE_INTEGER freq, t0;
static FILE *log_file;
static CRITICAL_SECTION net_cs;

HDC plat_pc_dc(void)
{
	return hdc;
}

void plat_pc_size(uint32_t *w, uint32_t *h)
{
	RECT r;
	GetClientRect(hwnd, &r);
	*w = (uint32_t)(r.right > 1 ? r.right : 1);
	*h = (uint32_t)(r.bottom > 1 ? r.bottom : 1);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	switch (msg) {
	case WM_CLOSE:
		quit = true;
		return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		keys[w & 0xFF] = true;
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		keys[w & 0xFF] = false;
		return 0;
	case WM_LBUTTONDOWN:
		mouse_l = true;
		return 0;
	case WM_LBUTTONUP:
		mouse_l = false;
		return 0;
	case WM_RBUTTONDOWN:
		mouse_r = true;
		return 0;
	case WM_RBUTTONUP:
		mouse_r = false;
		return 0;
	case WM_CHAR:
		if (w == 8)
			typed_back++;
		else if (w >= 32 && w < 127 && typed_len < (int)sizeof typed - 1)
			typed[typed_len++] = (char)w;
		return 0;
	case WM_MOUSEWHEEL:
		wheel += (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
		return 0;
	case WM_KILLFOCUS:
		memset(keys, 0, sizeof keys);
		mouse_l = mouse_r = false;
		return 0;
	}
	return DefWindowProcA(h, msg, w, l);
}

bool plat_open(char *err, uint32_t eb)
{
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	InitializeCriticalSection(&net_cs);
	log_file = fopen("botstrike_pc.log", "w");
	WNDCLASSA wc = { CS_OWNDC, wndproc, 0, 0, GetModuleHandleA(NULL), 0, LoadCursor(NULL, IDC_ARROW), 0, 0, "bs360" };
	RegisterClassA(&wc);
	RECT rc = { 0, 0, 1280, 720 };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	hwnd = CreateWindowA("bs360", "Bot Strike 360 (PC)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
		rc.right - rc.left, rc.bottom - rc.top, 0, 0, wc.hInstance, 0);
	if (!hwnd) {
		snprintf(err, eb, "a janela nao abriu");
		return false;
	}
	hdc = GetDC(hwnd);
	HMODULE x = LoadLibraryA("xinput1_4.dll");
	if (!x)
		x = LoadLibraryA("xinput9_1_0.dll");
	if (x)
		xinput = (XInputGetStateFn)(void *)GetProcAddress(x, "XInputGetState");
	return true;
}

void plat_close(void)
{
	plat_audio_stop();
	if (captured)
		ShowCursor(TRUE);
	if (hwnd)
		DestroyWindow(hwnd);
	if (log_file)
		fclose(log_file);
}

bool plat_quit_requested(void)
{
	return quit;
}

double plat_seconds(void)
{
	LARGE_INTEGER n;
	QueryPerformanceCounter(&n);
	return (double)(n.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
}

void plat_capture_mouse(bool on)
{
	if (on == captured)
		return;
	captured = on;
	ShowCursor(on ? FALSE : TRUE);
}

static float stick(SHORT v, int dead)
{
	if (v > -dead && v < dead)
		return 0.0f;
	const float s = v > 0 ? (float)(v - dead) / (32767.0f - dead) : (float)(v + dead) / (32768.0f - dead);
	return s < -1 ? -1 : (s > 1 ? 1 : s);
}

void plat_poll(PadState *pad)
{
	MSG msg;
	while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
	memset(pad, 0, sizeof *pad);
	XState xs;
	if (xinput && xinput(0, &xs) == 0) {
		pad->connected = true;
		pad->lx = stick(xs.pad.lx, 7849);
		pad->ly = stick(xs.pad.ly, 7849);
		pad->rx = stick(xs.pad.rx, 8689);
		pad->ry = stick(xs.pad.ry, 8689);
		pad->lt = xs.pad.lt / 255.0f;
		pad->rt = xs.pad.rt / 255.0f;
		pad->buttons = xs.pad.buttons;
	}
	const bool focus = GetForegroundWindow() == hwnd;
	pad->keyboard = true;
	if (focus)
		memcpy(pad->keys, keys, sizeof keys);
	RECT r;
	GetClientRect(hwnd, &r);
	POINT c = { (r.right) / 2, (r.bottom) / 2 }, m;
	GetCursorPos(&m);
	ScreenToClient(hwnd, &m);
	if (focus && captured) {
		pad->look_x = (float)(m.x - c.x) * 0.0022f;
		pad->look_y = -(float)(m.y - c.y) * 0.0022f;
		POINT s = c;
		ClientToScreen(hwnd, &s);
		SetCursorPos(s.x, s.y);
		mouse_x = 640.0f;
		mouse_y = 360.0f;
	} else if (r.right > 0 && r.bottom > 0) {
		mouse_x = (float)m.x * 1280.0f / (float)r.right;
		mouse_y = (float)m.y * 720.0f / (float)r.bottom;
	}
	pad->mouse_x = mouse_x;
	pad->mouse_y = mouse_y;
	pad->mouse_left = focus && mouse_l;
	pad->mouse_right = focus && mouse_r;
	pad->wheel = wheel;
	wheel = 0.0f;
	memcpy(pad->text, typed, (size_t)typed_len);
	pad->text[typed_len] = 0;
	pad->text_len = typed_len;
	pad->backspace = typed_back > 0;
	typed_len = 0;
	typed_back = 0;
}

const char *plat_find_file(const char *name)
{
	static char path[512];
	const char *dirs[] = { "out/", "", "../out/", "C:/mlt/x360/out/" };
	for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
		snprintf(path, sizeof path, "%s%s", dirs[i], name);
		FILE *f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return path;
		}
	}
	snprintf(path, sizeof path, "out/%s", name);
	return path;
}

void plat_log(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	char line[1024];
	vsnprintf(line, sizeof line, fmt, a);
	va_end(a);
	printf("%s\n", line);
	if (log_file) {
		fprintf(log_file, "[%8.2f] %s\n", plat_seconds(), line);
		fflush(log_file);
	}
}

// ---------------------------------------------------------------- audio: waveOut, 4 blocos de 1024 quadros

enum {
	AUDIO_BLOCKS = 4,
	AUDIO_FRAMES = 1024,
};

static CRITICAL_SECTION audio_cs;
static bool audio_cs_ready;
static HWAVEOUT wave;
static WAVEHDR headers[AUDIO_BLOCKS];
static int16_t blocks[AUDIO_BLOCKS][AUDIO_FRAMES * 2];
static float mixbuf[AUDIO_FRAMES * 2];
static PlatMixer mixer;
static HANDLE audio_thread, audio_event;
static volatile bool audio_running;

static void fill(int i)
{
	mixer(mixbuf, AUDIO_FRAMES);
	for (int k = 0; k < AUDIO_FRAMES * 2; k++)
		blocks[i][k] = (int16_t)(mixbuf[k] * 32767.0f);
	headers[i].dwFlags &= ~WHDR_DONE;
	waveOutWrite(wave, &headers[i], sizeof headers[i]);
}

static DWORD WINAPI audio_main(LPVOID unused)
{
	(void)unused;
	while (audio_running) {
		WaitForSingleObject(audio_event, 50);
		for (int i = 0; i < AUDIO_BLOCKS && audio_running; i++)
			if (headers[i].dwFlags & WHDR_DONE)
				fill(i);
	}
	return 0;
}

bool plat_audio_start(PlatMixer m)
{
	if (!audio_cs_ready) {
		InitializeCriticalSection(&audio_cs);
		audio_cs_ready = true;
	}
	mixer = m;
	WAVEFORMATEX fmt = { WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
	audio_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (waveOutOpen(&wave, WAVE_MAPPER, &fmt, (DWORD_PTR)audio_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
		return false;
	audio_running = true;
	for (int i = 0; i < AUDIO_BLOCKS; i++) {
		headers[i] = (WAVEHDR){ (LPSTR)blocks[i], sizeof blocks[i], 0, 0, 0, 0, 0, 0 };
		waveOutPrepareHeader(wave, &headers[i], sizeof headers[i]);
		fill(i);
	}
	audio_thread = CreateThread(NULL, 0, audio_main, NULL, 0, NULL);
	return true;
}

void plat_audio_stop(void)
{
	if (!audio_running)
		return;
	audio_running = false;
	SetEvent(audio_event);
	WaitForSingleObject(audio_thread, 1000);
	waveOutReset(wave);
	for (int i = 0; i < AUDIO_BLOCKS; i++)
		waveOutUnprepareHeader(wave, &headers[i], sizeof headers[i]);
	waveOutClose(wave);
}

void plat_audio_lock(void)
{
	if (!audio_cs_ready) {
		InitializeCriticalSection(&audio_cs);
		audio_cs_ready = true;
	}
	EnterCriticalSection(&audio_cs);
}

void plat_audio_unlock(void)
{
	LeaveCriticalSection(&audio_cs);
}

void plat_fatal(const char *msg)
{
	plat_log("ERRO: %s", msg);
	MessageBoxA(hwnd, msg, "Bot Strike 360", MB_OK | MB_ICONERROR);
}

// ---------------------------------------------------------------- rede

typedef struct {
	PlatThreadFn fn;
	void *arg;
} ThreadStart;

static DWORD WINAPI thread_main(LPVOID p)
{
	ThreadStart s = *(ThreadStart *)p;
	free(p);
	s.fn(s.arg);
	return 0;
}

bool plat_thread(PlatThreadFn fn, void *arg)
{
	ThreadStart *s = malloc(sizeof *s);
	if (!s)
		return false;
	s->fn = fn;
	s->arg = arg;
	HANDLE h = CreateThread(NULL, 0, thread_main, s, 0, NULL);
	if (!h) {
		free(s);
		return false;
	}
	CloseHandle(h);
	return true;
}

void plat_net_lock(void)
{
	EnterCriticalSection(&net_cs);
}

void plat_net_unlock(void)
{
	LeaveCriticalSection(&net_cs);
}

void plat_sleep_ms(int ms)
{
	Sleep((DWORD)ms);
}

bool plat_net_ready(char *ip, int ip_bytes)
{
	snprintf(ip, (size_t)ip_bytes, "PC");
	return true;
}

const char *const *plat_eptx_dirs(int *count)
{
	// a pasta do EPTX do PC: ao lado do jogo, ou a que BS_EPTX_DIR disser (para dois jogos no mesmo PC)
	static const char *dirs[3];
	static char env_dir[260];
	int n = 0;
	const char *e = getenv("BS_EPTX_DIR");
	if (e && *e) {
		snprintf(env_dir, sizeof env_dir, "%s", e);
		dirs[n++] = env_dir;
	}
	dirs[n++] = "eptx";
	dirs[n++] = "C:/mlt/x360/out/eptx";
	*count = n;
	return dirs;
}
