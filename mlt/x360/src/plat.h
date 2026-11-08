// O que muda entre o 360 e o PC alem do desenho: controle, relogio, arquivos e a saida de audio.
#ifndef BS_PLAT_H
#define BS_PLAT_H

#include <stdbool.h>
#include <stdint.h>

// Botoes no layout do controle do 360 (os mesmos bits do XInput).
enum {
	PAD_UP = 0x0001,
	PAD_DOWN = 0x0002,
	PAD_LEFT = 0x0004,
	PAD_RIGHT = 0x0008,
	PAD_START = 0x0010,
	PAD_BACK = 0x0020,
	PAD_LS = 0x0040,
	PAD_RS = 0x0080,
	PAD_LB = 0x0100,
	PAD_RB = 0x0200,
	PAD_A = 0x1000,
	PAD_B = 0x2000,
	PAD_X = 0x4000,
	PAD_Y = 0x8000,
};

typedef struct {
	bool connected;
	float lx, ly, rx, ry; // -1 a 1, zona morta ja tirada
	float lt, rt; // 0 a 1
	uint32_t buttons;
	// So no PC: o mouse gira a camera direto (radianos neste quadro) e o teclado tem as teclas do Bot Strike original.
	bool keyboard;
	float look_x, look_y;
	bool keys[256]; // codigos de tecla virtuais do Windows
	float mouse_x, mouse_y; // na grade 1280x720
	bool mouse_left, mouse_right;
	float wheel;
	// So no PC: o que foi digitado neste quadro (formularios), e Backspace.
	char text[32];
	int text_len;
	bool backspace;
} PadState;

bool plat_open(char *err, uint32_t err_bytes);
void plat_close(void);
void plat_poll(PadState *pad);
double plat_seconds(void);
// PC: prende o mouse para girar a camera (jogo) ou solta para clicar (menus, mapa). No 360 nao faz nada.
void plat_capture_mouse(bool on);
bool plat_quit_requested(void);
// Onde estao os pacotes (o primeiro que existir).
const char *plat_find_file(const char *name);
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Mostra um erro que impede o jogo de abrir e espera o jogador sair (Back no 360). Nao desenha nada do jogo.
void plat_fatal(const char *msg);
// Threads da rede (HTTP e UDP), uma trava para o que elas dividem com o jogo, e uma pausa.
typedef void (*PlatThreadFn)(void *arg);
bool plat_thread(PlatThreadFn fn, void *arg);
void plat_net_lock(void);
void plat_net_unlock(void);
void plat_sleep_ms(int ms);
// Espera a rede ter endereco (no 360 leva uns segundos depois de ligar). false se nao subir.
bool plat_net_ready(char *ip, int ip_bytes);
// As pastas onde o EPTX guarda o eptx.ini e a session.ini, na ordem de procura.
const char *const *plat_eptx_dirs(int *count);
// Liga a saida de audio: a funcao mistura frames quadros estereo (L, R intercalados) a 48 kHz.
typedef void (*PlatMixer)(float *stereo, uint32_t frames);
bool plat_audio_start(PlatMixer mixer);
void plat_audio_stop(void);
void plat_audio_lock(void);
void plat_audio_unlock(void);

#endif
