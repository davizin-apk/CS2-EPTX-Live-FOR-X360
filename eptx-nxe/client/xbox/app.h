/*
 * Estado da dash e a thread de rede.
 *
 * A tela (dash.cpp) nunca fala com o servidor: ela pede uma tarefa com
 * app_request() e le o resultado em g_app, sempre sob app_lock(). Uma thread so
 * executa as tarefas em ordem, porque o eptx_client nao e seguro entre threads e
 * o console nao ganha nada com dois downloads ao mesmo tempo.
 *
 * Toda chamada ao servidor passa por api_get/api_post/api_delete em app.c: e la
 * que a sessao expirada (401) vira "entre de novo" e que a falta de rede acende o
 * indicador de servidor fora do ar.
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
	APP_MAX_REQUESTS = 50,
	APP_MAX_INVITES = 16,
	APP_ROOM_MEMBERS = 4,
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
	char publisher[64];
	char description[240];
	bool installable;
	bool shows_in_dash;
	art_state art;
	app_image image;
} store_item;

typedef struct {
	int64_t friendship_id; /* DELETE /api/v1/friends/<isto> remove a amizade */
	int64_t user_id;       /* o que POST /rooms/:id/invite pede */
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

/* Pedido de amizade pendente: recebido (alguem me adicionou) ou enviado. */
typedef struct {
	int64_t id; /* aceitar: POST /friends/requests/<id>/accept; recusar: DELETE /friends/<id> */
	int64_t user_id;
	char gamertag[40];
} request_item;

typedef struct {
	char name[96];
	char title_id[12];
	uint32_t content_type;
	char path[300]; /* o container no HD, em caminho do xsdk (hdd:\...) */
	art_state art;
	app_image image;
} library_item;

typedef struct {
	char gamertag[40];
	int slot;
	bool udp; /* o relay ja recebeu pacotes dele nos ultimos segundos */
} room_member;

/* A sala em que a conta esta (GET /api/v1/rooms/current). A partida roda no jogo, nao na dash. */
typedef struct {
	bool active;
	int64_t id;
	char game[33];
	char mode[33];
	char state[12]; /* "lobby" ou "playing" */
	char host[40];
	bool is_host;
	int relay_port;
	room_member members[APP_ROOM_MEMBERS];
	int member_count;
} room_info;

typedef struct {
	int64_t room_id;
	char game[33];
	char mode[33];
	char host[40];
	int members;
} invite_item;

typedef enum {
	SESSION_STARTING,   /* lendo eptx.ini e falando com o servidor */
	SESSION_SIGNED_OUT,
	SESSION_SIGNING_IN,
	SESSION_SIGNED_IN,
	SESSION_NO_SERVER,  /* servidor nao respondeu; a dash continua, offline */
} session_state;

/* O perfil do Xbox logado no console: sem um, a dash nao entra. */
typedef enum {
	PROFILE_UNKNOWN, /* a tela ainda nao perguntou ao XAM */
	PROFILE_NONE,    /* ninguem logado */
	PROFILE_READY,
} profile_state;

/* Busca de uma gamertag antes de mandar o pedido de amizade. */
typedef enum {
	LOOKUP_NONE,
	LOOKUP_LOADING,
	LOOKUP_FOUND,
	LOOKUP_NOT_FOUND,
	LOOKUP_FAILED,
} lookup_state;

/* Uma acao que o painel aberto espera (mandar pedido, aceitar, criar sala, ...). */
typedef enum {
	ACTION_IDLE,
	ACTION_BUSY,
	ACTION_OK,
	ACTION_FAILED,
} action_state;

typedef struct {
	/*
	 * O perfil do Xbox: cada um tem a sua conta EPTX (a sessao fica em
	 * EPTX-NXE/profiles/<XUID>.ini), e a foto dele vai para o servidor como avatar.
	 */
	profile_state profile;
	char profile_gamertag[16];
	char profile_xuid[17]; /* a pasta do perfil em Content; vazio se o pacote nao foi achado */
	art_state profile_art; /* a foto do jogador, do cabecalho do pacote do perfil */
	app_image profile_image;
	uint32_t profile_serial;

	session_state session;
	char server[80];      /* "192.168.0.10:8360", para mostrar */
	bool server_ok;       /* a ultima requisicao chegou no servidor */
	char gamertag[40];
	char message[200];    /* ultima noticia para o rodape: erro, "instalado", ... */
	uint32_t message_serial;

	/* A acao do painel: o resultado que o servidor confirmou, ou o erro dele. */
	action_state action;
	char action_message[160];
	uint32_t action_serial;

	/*
	 * Cada recarga troca a lista inteira e soma um no serial: a tela, que guarda as
	 * texturas pelo indice, ve o serial mudar e joga as dela fora.
	 */
	store_item store[APP_MAX_STORE];
	int store_count;
	bool store_loaded;
	uint32_t store_serial;

	/* Pesquisa na loja: indices em store dos titulos que o servidor devolveu para search_query. */
	char search_query[64];
	int search_index[APP_MAX_STORE];
	int search_count;
	bool search_loading;
	uint32_t search_serial;

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
	char motto[96];

	/* friends_serial sobe quando a lista muda de gente ou de ordem (nao a cada presenca). */
	friend_item friends[APP_MAX_FRIENDS];
	int friend_count;
	bool friends_loaded;
	uint32_t friends_serial;

	request_item requests_in[APP_MAX_REQUESTS];
	int requests_in_count;
	request_item requests_out[APP_MAX_REQUESTS];
	int requests_out_count;
	bool requests_loaded;
	uint32_t requests_serial;

	/* A gamertag procurada e o avatar dela (a rota publica do avatar da 404 para quem nao existe). */
	char lookup_gamertag[40];
	lookup_state lookup;
	art_state lookup_art;
	app_image lookup_image;
	uint32_t lookup_serial;

	room_info room;
	invite_item invites[APP_MAX_INVITES];
	int invite_count;
	uint32_t rooms_serial;

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

/* Sobe a thread de rede e comeca: le eptx.ini e fala com o servidor. A conta espera o perfil. */
void app_start(void);

/*
 * O perfil do Xbox logado mudou ("" quando ninguem esta logado). A thread acha o
 * pacote dele, troca para a conta EPTX desse perfil (ou pede login) e manda a foto
 * do perfil como avatar quando ela mudou.
 */
void app_set_profile(const char *gamertag);

/* Tarefas. Voltam na hora; o resultado aparece em g_app. */
void app_sign_in(const char *gamertag, const char *password);
void app_sign_out(void);
void app_refresh(void);           /* catalogo, amigos, pedidos, salas e biblioteca de novo */
void app_load_art(int store_index);
void app_load_library_art(int library_index);
void app_load_friend_avatar(int friend_index);
void app_install(int64_t title_id);
/* Salva o avatar do editor na conta. png (malloc) passa a ser da tarefa, que o libera. */
void app_save_avatar(const avatar_look *look, uint8_t *png, int png_len);

/* Pesquisa na loja pelo nome ou Title ID (GET /api/v1/catalog?q=). */
void app_search_store(const char *query);

/* Amizades. O resultado de cada uma vem em action/action_message, depois que o servidor responde. */
void app_lookup_gamertag(const char *gamertag);
void app_send_friend_request(const char *gamertag);
void app_accept_request(int64_t request_id);
void app_decline_request(int64_t request_id);
void app_remove_friend(int64_t friendship_id);

/* Salas (multiplayer). */
void app_room_create(const char *game, const char *mode);
void app_room_invite(int64_t user_id);
void app_room_join(int64_t room_id);
void app_room_leave(void);

/*
 * Com um painel de amigos, pedidos ou sala aberto, a dash quer saber logo das
 * mudancas: a thread consulta a cada poucos segundos em vez de a cada minuto.
 */
void app_watch(bool fast);

#ifdef __cplusplus
}
#endif

#endif
