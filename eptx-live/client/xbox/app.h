/*
 * Estado da dash e a thread de rede.
 *
 * A tela (dash.cpp) nunca fala com o servidor: ela pede uma tarefa com
 * app_request() e le o resultado em g_app, sempre sob app_lock(). Uma thread so
 * executa as tarefas em ordem, porque o eptx_client nao e seguro entre threads e
 * o console nao ganha nada com dois downloads ao mesmo tempo.
 */
#ifndef EPTX_APP_H
#define EPTX_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "avatar_look.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	APP_MAX_STORE = 200,
	APP_MAX_FRIENDS = 100,
	APP_MAX_LIBRARY = 200,
};

/*
 * Pixels RGBA decodificados de um PNG ou GIF; a tela os sobe para uma textura e
 * libera. Num GIF animado os quadros vem um atras do outro em rgba, e delays diz
 * quantos milissegundos cada um fica na tela.
 */
typedef struct {
	uint8_t *rgba;
	int width;
	int height;
	int frames;
	uint16_t *delays;
} app_image;

typedef enum {
	ART_NONE,
	ART_LOADING,
	ART_READY, /* rgba pronto, esperando a tela subir */
	ART_SHOWN, /* a tela ja subiu a textura e liberou o rgba */
	ART_FAILED,
} art_state;

typedef struct {
	int64_t id;
	int64_t size;
	char name[96];
	char kind[12];
	char title_id[12];
	bool installable;
	bool shows_in_dash;
	art_state art;
	app_image image;
} store_item;

typedef struct {
	char gamertag[40];
	char motto[96];
	bool online;
	char title_name[64];
	int64_t avatar_updated; /* 0 com o avatar padrao; muda quando a pessoa troca */
	bool has_look;          /* o avatar e desenhado pela dash a partir de look */
	avatar_look look;
	bool avatar_gif;        /* a imagem e o GIF animado do 360sona */
	art_state art;
	app_image image;        /* sem look: o PNG (ou GIF) de /avatar/<gamertag>/avatar-body.* */
} friend_item;

typedef struct {
	char name[96];
	char title_id[12];
	uint32_t content_type;
	char path[300]; /* o container no HD, em caminho do xsdk (hdd:\...) */
	art_state art;
	app_image image;
} library_item;

typedef enum {
	SESSION_STARTING,   /* lendo eptx.ini e falando com o servidor */
	SESSION_SIGNED_OUT,
	SESSION_SIGNING_IN,
	SESSION_SIGNED_IN,
	SESSION_NO_SERVER,  /* servidor nao respondeu; a dash continua, offline */
} session_state;

typedef struct {
	session_state session;
	char server[80];      /* "192.168.0.10:8360", para mostrar */
	char gamertag[40];
	char message[200];    /* ultima noticia para o rodape: erro, "instalado", ... */
	uint32_t message_serial;

	/*
	 * Cada recarga troca a lista inteira e soma um no serial: a tela, que guarda as
	 * texturas pelo indice, ve o serial mudar e joga as dela fora.
	 */
	store_item store[APP_MAX_STORE];
	int store_count;
	bool store_loaded;
	uint32_t store_serial;

	/*
	 * O avatar da propria conta: as escolhas do editor (has_look) ou, se a conta so
	 * tem um PNG mandado de fora, a imagem. avatar_serial sobe a cada troca, para a
	 * tela refazer a textura.
	 */
	bool has_look;
	avatar_look look;
	bool avatar_gif;
	bool avatar_saving;
	int64_t avatar_updated;
	art_state avatar_art;
	app_image avatar_image;
	uint32_t avatar_serial;

	/* friends_serial sobe quando a lista muda de gente ou de ordem (nao a cada presenca). */
	friend_item friends[APP_MAX_FRIENDS];
	int friend_count;
	bool friends_loaded;
	uint32_t friends_serial;

	library_item library[APP_MAX_LIBRARY];
	int library_count;
	bool library_loaded;
	uint32_t library_serial;

	/* Instalacao em andamento. */
	bool installing;
	char install_name[96];
	int64_t install_done;
	int64_t install_total;
	int install_file;
	int install_files;
} app_state;

extern app_state g_app;

void app_lock(void);
void app_unlock(void);

/* Sobe a thread de rede e comeca: le eptx.ini, retoma a sessao salva ou espera login. */
void app_start(void);

/* Tarefas. Voltam na hora; o resultado aparece em g_app. */
void app_sign_in(const char *gamertag, const char *password);
void app_sign_out(void);
void app_refresh(void);           /* catalogo, amigos e biblioteca de novo */
void app_load_art(int store_index);
void app_load_library_art(int library_index);
void app_load_friend_avatar(int friend_index);
void app_install(int64_t title_id);
/* Salva o avatar do editor na conta. png (malloc) passa a ser da tarefa, que o libera. */
void app_save_avatar(const avatar_look *look, uint8_t *png, int png_len);

#ifdef __cplusplus
}
#endif

#endif
