#include "app.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "xsdk/log.h"
#ifdef __XSDK__
#include "xsdk/net.h"
#endif

#include "eptx_client.h"
#include "eptx_http.h"
#include "eptx_json.h"
#include "image.h"

#ifdef EPTX_PC
/*
 * Versao de PC (client/pc): o "HD" e a pasta pc-hdd ao lado do .exe, e o eptx.ini
 * fica junto do .exe.
 */
static const char *g_root = "pc-hdd";
static const char *const CONFIG_PATHS[] = { "eptx.ini", "pc-hdd/EPTX-NXE/eptx.ini", "pc-hdd/EPTX/eptx.ini" };
#else
/*
 * Instalado como XBLA, game: e o proprio pacote, so de leitura: o que a dash
 * guarda fica em Hdd1:\EPTX-NXE, onde da para trocar por FTP.
 *
 * Esta edicao convive com o EPTX Live no mesmo console: grava so na pasta dela
 * (EPTX-NXE). Do Hdd1:\EPTX do EPTX Live ela so le o eptx.ini, se a dela nao tiver,
 * para achar o servidor; a sessao do EPTX Live nunca e tocada.
 */
static const char *g_root = "hdd:";
static const char *const CONFIG_PATHS[] = { "hdd:/EPTX-NXE/eptx.ini", "usb:/EPTX-NXE/eptx.ini", "game:/eptx.ini",
	"hdd:/EPTX/eptx.ini", "usb:/EPTX/eptx.ini" };
#endif

/* O xsdk nao tem sleep/usleep; nanosleep existe nos dois lados. */
static void pause_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* O disco: o que tem a pasta desta edicao (ou a do EPTX Live); sem nenhuma, o HD interno se houver, senao o USB. */
static void pick_root(void)
{
#ifndef EPTX_PC
	struct stat st;
	if (stat("hdd:/EPTX-NXE", &st) == 0)
		g_root = "hdd:";
	else if (stat("usb:/EPTX-NXE", &st) == 0)
		g_root = "usb:";
	else if (stat("hdd:/EPTX", &st) == 0)
		g_root = "hdd:";
	else if (stat("usb:/EPTX", &st) == 0)
		g_root = "usb:";
	else
		g_root = stat("hdd:/", &st) == 0 ? "hdd:" : "usb:";
	xsdk_log_write("eptx: disco do EPTX: %s", g_root);
#endif
}

/* g_root + rest, como "usb:/EPTX/session.ini". */
static const char *on_root(char *out, size_t cap, const char *rest)
{
	snprintf(out, cap, "%s%s", g_root, rest);
	return out;
}
#define DATA_REST "/EPTX-NXE"
/* Uma sessao por perfil do Xbox: profiles/<XUID>.ini (veja session_file). */
#define PROFILES_REST DATA_REST "/profiles"
#define CONTENT_REST "/Content/0000000000000000"

#ifdef _WIN32
#define make_dir(path) mkdir(path)
#else
#define make_dir(path) mkdir(path, 0777)
#endif
/* "EPXN": esta dash; "EPTX": o EPTX Live. Nenhuma das duas precisa aparecer na biblioteca. */
#define OWN_TITLE_ID "4550584E"
#define LIVE_TITLE_ID "45505458"

enum {
	RESPONSE_CAP = 256 * 1024,
	/* O GIF do avatar: ate 8 MB no servidor; cada quadro cai para 192 px e ficam no maximo 48. */
	GIF_CAP = 8 * 1024 * 1024 + 1024,
	GIF_SIDE = 192,
	GIF_FRAMES = 48,
	MAX_TOKENS = 4096,
	DOWNLOAD_RETRIES = 5,
	QUEUE_SIZE = 64,
	/* O servidor derruba a presenca depois de presenceTimeoutSeconds; 60 s fica bem abaixo. */
	HEARTBEAT_SECONDS = 60,
	/* O servidor tira da sala quem ficou 60 s sem aparecer. */
	ROOM_SECONDS = 20,
	WATCH_SECONDS = 5,
	STFS_NAMES_END = 0x1711,
	STFS_THUMB_SIZE = 0x1712,
	STFS_TITLE_THUMB_SIZE = 0x1716,
	STFS_THUMB = 0x171A,
	STFS_TITLE_THUMB = 0x571A,
	STFS_THUMB_MAX = 0x4000,
	CONTENT_GOD = 0x00007000,
	CONTENT_ARCADE = 0x000D0000,
};

app_state g_app;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;

void app_lock(void) { pthread_mutex_lock(&g_lock); }
void app_unlock(void) { pthread_mutex_unlock(&g_lock); }

/* --- fila de tarefas -------------------------------------------------------- */

typedef enum {
	TASK_START,
	TASK_SIGN_IN,
	TASK_SIGN_OUT,
	TASK_REFRESH,
	TASK_ART,
	TASK_LIBRARY_ART,
	TASK_FRIEND_AVATAR,
	TASK_INSTALL,
	TASK_SAVE_AVATAR,
	TASK_SEARCH,
	TASK_LOOKUP,
	TASK_SEND_REQUEST,
	TASK_ACCEPT_REQUEST,
	TASK_DECLINE_REQUEST,
	TASK_REMOVE_FRIEND,
	TASK_ROOM_CREATE,
	TASK_ROOM_INVITE,
	TASK_ROOM_JOIN,
	TASK_ROOM_LEAVE,
	TASK_PROFILE,
} task_type;

typedef struct {
	task_type type;
	int64_t arg;
	char gamertag[40];
	char password[64];
	char text[64]; /* pesquisa, jogo da sala */
	char mode[33];
	avatar_look look;
	uint8_t *png;
	int png_len;
} task;

static task g_queue[QUEUE_SIZE];
static int g_queue_head, g_queue_count;

static void push(const task *t)
{
	pthread_mutex_lock(&g_queue_lock);
	if (g_queue_count < QUEUE_SIZE) {
		g_queue[(g_queue_head + g_queue_count) % QUEUE_SIZE] = *t;
		g_queue_count++;
		pthread_cond_signal(&g_queue_cond);
	} else {
		xsdk_log_write("eptx: fila cheia, tarefa %d descartada", (int)t->type);
	}
	pthread_mutex_unlock(&g_queue_lock);
}

static void push_simple(task_type type, int64_t arg)
{
	task t;
	memset(&t, 0, sizeof t);
	t.type = type;
	t.arg = arg;
	push(&t);
}

void app_sign_in(const char *gamertag, const char *password)
{
	task t;
	memset(&t, 0, sizeof t);
	t.type = TASK_SIGN_IN;
	snprintf(t.gamertag, sizeof t.gamertag, "%s", gamertag);
	snprintf(t.password, sizeof t.password, "%s", password);
	push(&t);
}

void app_sign_out(void) { push_simple(TASK_SIGN_OUT, 0); }

static void push_text(task_type type, int64_t arg, const char *text, const char *mode);
void app_set_profile(const char *gamertag) { push_text(TASK_PROFILE, 0, gamertag, NULL); }
void app_refresh(void) { push_simple(TASK_REFRESH, 0); }
void app_install(int64_t title_id) { push_simple(TASK_INSTALL, title_id); }

/* Marca o painel como esperando o servidor antes de a tarefa entrar na fila. */
static void action_busy(void)
{
	app_lock();
	g_app.action = ACTION_BUSY;
	g_app.action_message[0] = '\0';
	g_app.action_serial++;
	app_unlock();
}

static void push_text(task_type type, int64_t arg, const char *text, const char *mode)
{
	task t;
	memset(&t, 0, sizeof t);
	t.type = type;
	t.arg = arg;
	snprintf(t.text, sizeof t.text, "%s", text ? text : "");
	snprintf(t.mode, sizeof t.mode, "%s", mode ? mode : "");
	push(&t);
}

void app_search_store(const char *query)
{
	app_lock();
	snprintf(g_app.search_query, sizeof g_app.search_query, "%s", query);
	g_app.search_loading = true;
	g_app.search_count = 0;
	g_app.search_serial++;
	app_unlock();
	push_text(TASK_SEARCH, 0, query, NULL);
}

void app_lookup_gamertag(const char *gamertag)
{
	app_lock();
	snprintf(g_app.lookup_gamertag, sizeof g_app.lookup_gamertag, "%s", gamertag);
	g_app.lookup = LOOKUP_LOADING;
	g_app.lookup_serial++;
	app_unlock();
	push_text(TASK_LOOKUP, 0, gamertag, NULL);
}

void app_send_friend_request(const char *gamertag)
{
	action_busy();
	push_text(TASK_SEND_REQUEST, 0, gamertag, NULL);
}

void app_accept_request(int64_t request_id)
{
	action_busy();
	push_simple(TASK_ACCEPT_REQUEST, request_id);
}

void app_decline_request(int64_t request_id)
{
	action_busy();
	push_simple(TASK_DECLINE_REQUEST, request_id);
}

void app_remove_friend(int64_t friendship_id)
{
	action_busy();
	push_simple(TASK_REMOVE_FRIEND, friendship_id);
}

void app_room_create(const char *game, const char *mode)
{
	action_busy();
	push_text(TASK_ROOM_CREATE, 0, game, mode);
}

void app_room_invite(int64_t user_id)
{
	action_busy();
	push_simple(TASK_ROOM_INVITE, user_id);
}

void app_room_join(int64_t room_id)
{
	action_busy();
	push_simple(TASK_ROOM_JOIN, room_id);
}

void app_room_leave(void)
{
	action_busy();
	push_simple(TASK_ROOM_LEAVE, 0);
}

static volatile bool g_watch_fast;

void app_watch(bool fast)
{
	if (g_watch_fast == fast)
		return;
	/* Acorda a thread: ela pode estar no meio de uma espera de um minuto. */
	pthread_mutex_lock(&g_queue_lock);
	g_watch_fast = fast;
	pthread_cond_signal(&g_queue_cond);
	pthread_mutex_unlock(&g_queue_lock);
}

void app_save_avatar(const avatar_look *look, uint8_t *png, int png_len)
{
	task t;
	memset(&t, 0, sizeof t);
	t.type = TASK_SAVE_AVATAR;
	t.look = *look;
	t.png = png;
	t.png_len = png_len;
	app_lock();
	g_app.avatar_saving = true;
	app_unlock();
	push(&t);
}

void app_load_art(int store_index)
{
	app_lock();
	const bool wanted = store_index >= 0 && store_index < g_app.store_count && g_app.store[store_index].art == ART_NONE;
	if (wanted)
		g_app.store[store_index].art = ART_LOADING;
	app_unlock();
	if (wanted)
		push_simple(TASK_ART, store_index);
}

void app_load_library_art(int library_index)
{
	app_lock();
	const bool wanted =
		library_index >= 0 && library_index < g_app.library_count && g_app.library[library_index].art == ART_NONE;
	if (wanted)
		g_app.library[library_index].art = ART_LOADING;
	app_unlock();
	if (wanted)
		push_simple(TASK_LIBRARY_ART, library_index);
}

void app_load_friend_avatar(int friend_index)
{
	app_lock();
	const bool wanted = friend_index >= 0 && friend_index < g_app.friend_count &&
		g_app.friends[friend_index].art == ART_NONE && !g_app.friends[friend_index].has_look &&
		g_app.friends[friend_index].avatar_updated > 0;
	if (wanted)
		g_app.friends[friend_index].art = ART_LOADING;
	app_unlock();
	if (wanted)
		push_simple(TASK_FRIEND_AVATAR, friend_index);
}

/* --- utilitarios -------------------------------------------------------------- */

static eptx_client g_client;
static char g_host[64];
static int g_port = 8360;
static char g_ini_user[40], g_ini_pass[64];
static char g_response[RESPONSE_CAP];
static eptx_json_tok g_tokens[MAX_TOKENS];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char text[200];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof text, fmt, args);
	va_end(args);
	app_lock();
	snprintf(g_app.message, sizeof g_app.message, "%s", text);
	g_app.message_serial++;
	app_unlock();
	xsdk_log_write("eptx: %s", text);
}

static void set_session(session_state state)
{
	app_lock();
	g_app.session = state;
	app_unlock();
}

static void trim(char *s)
{
	size_t len = strlen(s);
	while (len && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ' || s[len - 1] == '\t'))
		s[--len] = '\0';
	size_t start = 0;
	while (s[start] == ' ' || s[start] == '\t')
		start++;
	memmove(s, s + start, len - start + 1);
}

/* Le chave=valor de um arquivo, chamando on_pair para cada linha valida. */
static bool read_ini(const char *path, void (*on_pair)(const char *key, const char *value))
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return false;
	char line[256];
	while (fgets(line, sizeof line, fp)) {
		trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *value = eq + 1;
		trim(line);
		trim(value);
		on_pair(line, value);
	}
	fclose(fp);
	return true;
}

static void config_pair(const char *key, const char *value)
{
	if (strcmp(key, "host") == 0)
		snprintf(g_host, sizeof g_host, "%s", value);
	else if (strcmp(key, "port") == 0)
		g_port = atoi(value);
	else if (strcmp(key, "user") == 0)
		snprintf(g_ini_user, sizeof g_ini_user, "%s", value);
	else if (strcmp(key, "pass") == 0)
		snprintf(g_ini_pass, sizeof g_ini_pass, "%s", value);
}

/*
 * O perfil do Xbox logado. g_profile_key nomeia a sessao dele (o XUID, a pasta do
 * pacote do perfil; sem o pacote, "gt_" e a gamertag), e g_profile_png e a foto do
 * jogador, como esta no cabecalho do pacote. So a thread de rede mexe nestes.
 */
static char g_profile_key[40];
static char g_profile_tag[16];
static uint8_t *g_profile_png;
static int g_profile_png_len;
static uint32_t g_saved_pic; /* a foto que ja foi para o servidor por esta conta (hash), da sessao salva */
static bool g_server_ready;  /* start() chegou no servidor */

static void session_pair(const char *key, const char *value)
{
	if (strcmp(key, "token") == 0)
		snprintf(g_client.token, sizeof g_client.token, "%s", value);
	else if (strcmp(key, "secret") == 0)
		snprintf(g_client.secret, sizeof g_client.secret, "%s", value);
	else if (strcmp(key, "gamertag") == 0)
		snprintf(g_client.gamertag, sizeof g_client.gamertag, "%s", value);
	else if (strcmp(key, "pic") == 0)
		g_saved_pic = (uint32_t)strtoul(value, NULL, 16);
}

/* EPTX-NXE/profiles/<chave do perfil>.ini (com suffix, o .part da gravacao). Falso sem perfil. */
static bool session_file(char *out, size_t cap, const char *suffix)
{
	if (!g_profile_key[0])
		return false;
	char rest[96];
	snprintf(rest, sizeof rest, PROFILES_REST "/%s.ini%s", g_profile_key, suffix);
	on_root(out, cap, rest);
	return true;
}

/*
 * Guarda token e segredo, nao a senha: o servidor pode revogar a sessao, e quem
 * pegar o HD nao leva a senha que a pessoa talvez use em outro lugar.
 */
static void save_session(void)
{
	char dir[48], path[112], part[120];
	if (!session_file(path, sizeof path, "") || !session_file(part, sizeof part, ".part"))
		return;
	make_dir(g_root); /* so faz diferenca no PC, onde o "disco" e uma pasta */
	make_dir(on_root(dir, sizeof dir, DATA_REST));
	make_dir(on_root(dir, sizeof dir, PROFILES_REST));
	FILE *fp = fopen(part, "w");
	if (!fp) {
		xsdk_log_write("eptx: nao gravei a sessao: errno %d", errno);
		return;
	}
	fprintf(fp, "gamertag=%s\ntoken=%s\nsecret=%s\npic=%08x\n", g_client.gamertag, g_client.token, g_client.secret,
		(unsigned)g_saved_pic);
	fflush(fp);
#ifndef _WIN32
	fsync(fileno(fp));
#endif
	fclose(fp);
#ifdef _WIN32
	remove(path); /* no Windows o rename nao troca um arquivo que ja existe */
#endif
	rename(part, path);
}

/* UTF-16BE, terminado em zero ou no fim do campo, para UTF-8. */
static void utf16be_to_utf8(const uint8_t *src, size_t bytes, char *out, size_t cap)
{
	size_t w = 0;
	for (size_t i = 0; i + 1 < bytes; i += 2) {
		const uint32_t c = (uint32_t)src[i] << 8 | src[i + 1];
		if (c == 0)
			break;
		if (c < 0x80) {
			if (w + 1 >= cap)
				break;
			out[w++] = (char)c;
		} else if (c < 0x800) {
			if (w + 2 >= cap)
				break;
			out[w++] = (char)(0xC0 | c >> 6);
			out[w++] = (char)(0x80 | (c & 0x3F));
		} else {
			if (w + 3 >= cap)
				break;
			out[w++] = (char)(0xE0 | c >> 12);
			out[w++] = (char)(0x80 | (c >> 6 & 0x3F));
			out[w++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[w < cap ? w : cap - 1] = '\0';
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Para caminho e query: o que nao for letra, numero ou -_. vira %XX. */
static bool url_encode(const char *s, char *out, size_t cap)
{
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (w + 4 >= cap)
			return false;
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
			*p == '_' || *p == '.')
			out[w++] = (char)*p;
		else
			w += (size_t)snprintf(out + w, cap - w, "%%%02X", *p);
	}
	out[w] = '\0';
	return true;
}

/* String JSON com aspas, para montar corpos com o que a pessoa digitou. */
static void json_string(const char *s, char *out, size_t cap)
{
	size_t w = 0;
	if (cap < 3)
		return;
	out[w++] = '"';
	for (const unsigned char *p = (const unsigned char *)s; *p && w + 8 < cap; p++) {
		if (*p == '"' || *p == '\\') {
			out[w++] = '\\';
			out[w++] = (char)*p;
		} else if (*p < 0x20) {
			w += (size_t)snprintf(out + w, cap - w, "\\u%04x", *p);
		} else {
			out[w++] = (char)*p;
		}
	}
	out[w++] = '"';
	out[w] = '\0';
}

/* --- requisicoes ao servidor ------------------------------------------------------ */

static void forget_session(void);

/*
 * Todas as requisicoes assinadas passam por aqui. Sem resposta, o indicador de
 * servidor apaga; com 401, a sessao salva nao vale mais (expirou em 30 dias sem uso
 * ou foi revogada) e a dash volta para "entrar".
 */
typedef enum { API_GET, API_POST, API_DELETE, API_PUT_PNG } api_method;

/* body e JSON; no API_PUT_PNG, os bytes do PNG, com png_len. */
static int api_any(api_method method, const char *path, const void *body, size_t png_len, char *out, size_t cap,
	int *len)
{
	int rc;
	/* A sessao caiu no meio de uma sequencia (um 401 logo antes): o resto para em silencio. */
	if (!g_client.token[0])
		return EPTX_ERR_NO_SESSION;
	if (method == API_GET)
		rc = eptx_client_get(&g_client, path, out, cap, len);
	else if (method == API_POST)
		rc = eptx_client_post(&g_client, path, body ? (const char *)body : "{}", out, cap, len);
	else if (method == API_PUT_PNG)
		rc = eptx_client_put_bytes(&g_client, path, "image/png", body, png_len, out, cap, len);
	else
		rc = eptx_client_delete(&g_client, path, out, cap, len);
	app_lock();
	g_app.server_ok = rc != EPTX_ERR_HTTP;
	app_unlock();
	if (rc == EPTX_ERR_STATUS && g_client.last_status == 401) {
		xsdk_log_write("eptx: %s respondeu 401, sessao encerrada", path);
		forget_session();
		say("Sua sessao no EPTX expirou. Entre de novo pelo Perfil.");
	}
	return rc;
}

static int api_get(const char *path, char *out, size_t cap, int *len)
{
	return api_any(API_GET, path, NULL, 0, out, cap, len);
}

static int api_post(const char *path, const char *body, char *out, size_t cap, int *len)
{
	return api_any(API_POST, path, body, 0, out, cap, len);
}

static int api_delete(const char *path, char *out, size_t cap, int *len)
{
	return api_any(API_DELETE, path, NULL, 0, out, cap, len);
}

static int api_put_png(const char *path, const uint8_t *png, int png_len, char *out, size_t cap, int *len)
{
	return api_any(API_PUT_PNG, path, png, (size_t)png_len, out, cap, len);
}

/* O erro da ultima requisicao, em portugues: o codigo que o servidor manda vira frase. */
static const char *server_error(void)
{
	static const struct {
		const char *code, *text;
	} KNOWN[] = {
		{ "gamertag_nao_encontrada", "Essa gamertag nao existe no servidor EPTX." },
		{ "nao_pode_adicionar_a_si_mesmo", "Essa e a sua propria conta." },
		{ "ja_sao_amigos", "Voces ja sao amigos." },
		{ "pedido_ja_enviado", "Voce ja mandou um pedido para essa pessoa." },
		{ "bloqueado", "Essa pessoa nao recebe pedidos seus." },
		{ "pedido_nao_encontrado", "Esse pedido nao existe mais." },
		{ "pedido_ja_resolvido", "Esse pedido ja foi respondido." },
		{ "amizade_nao_encontrada", "Essa amizade nao existe mais." },
		{ "sala_nao_encontrada", "A sala nao existe mais." },
		{ "sem_convite", "Voce nao tem convite para essa sala." },
		{ "sala_cheia", "A sala esta cheia." },
		{ "nao_sao_amigos", "So da para convidar amigos." },
		{ "so_o_host", "So quem criou a sala pode fazer isso." },
		{ "campos_faltando", "O servidor recusou o pedido (campos faltando)." },
		{ "sessao_invalida", "Sua sessao no EPTX expirou." },
	};
	static char text[160];
	if (g_client.last_status == 0 || !g_client.last_error[0] || g_client.last_error[0] != '{') {
		snprintf(text, sizeof text, "Sem resposta do servidor (%s).",
			g_client.last_error[0] ? g_client.last_error : "rede");
		return text;
	}
	char code[64] = "";
	const size_t len = strlen(g_client.last_error);
	const int n = eptx_json_parse(g_client.last_error, len, g_tokens, MAX_TOKENS);
	if (n > 0)
		eptx_json_get_string(g_client.last_error, g_tokens, n, 0, "error", code, sizeof code);
	for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
		if (strcmp(code, KNOWN[i].code) == 0)
			return KNOWN[i].text;
	snprintf(text, sizeof text, "O servidor recusou (%d %s).", g_client.last_status, code[0] ? code : "erro");
	return text;
}

/* O resultado da acao do painel, confirmado (ou recusado) pelo servidor. Vai tambem para o aviso. */
static void finish_action(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish_action(bool ok, const char *fmt, ...)
{
	char text[160];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof text, fmt, args);
	va_end(args);
	app_lock();
	g_app.action = ok ? ACTION_OK : ACTION_FAILED;
	snprintf(g_app.action_message, sizeof g_app.action_message, "%s", text);
	g_app.action_serial++;
	app_unlock();
	say("%s", text);
}

/* --- servidor ------------------------------------------------------------------ */

static void load_catalog(void)
{
	int len = 0;
	const int rc = api_get("/api/v1/catalog?limit=200", g_response, RESPONSE_CAP, &len);
	if (rc != EPTX_OK) {
		if (rc != EPTX_ERR_NO_SESSION)
			say("Nao consegui abrir a loja: %s", server_error());
		return;
	}
	const int n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
	const int items = n > 0 ? eptx_json_get(g_response, g_tokens, n, 0, "items") : -1;
	if (items < 0) {
		say("A loja respondeu num formato inesperado.");
		return;
	}

	app_lock();
	for (int i = 0; i < g_app.store_count; i++)
		image_free(&g_app.store[i].image);
	g_app.store_count = 0;
	for (int i = 0; g_app.store_count < (int)APP_MAX_STORE; i++) {
		const int item = eptx_json_at(g_tokens, n, items, i);
		if (item < 0)
			break;
		store_item *s = &g_app.store[g_app.store_count++];
		int installable = 0, shows = 0;
		memset(s, 0, sizeof *s);
		eptx_json_get_int(g_response, g_tokens, n, item, "id", &s->id);
		eptx_json_get_int(g_response, g_tokens, n, item, "totalSize", &s->size);
		eptx_json_get_string(g_response, g_tokens, n, item, "name", s->name, sizeof s->name);
		eptx_json_get_string(g_response, g_tokens, n, item, "kind", s->kind, sizeof s->kind);
		eptx_json_get_string(g_response, g_tokens, n, item, "titleId", s->title_id, sizeof s->title_id);
		eptx_json_get_string(g_response, g_tokens, n, item, "publisher", s->publisher, sizeof s->publisher);
		eptx_json_get_string(g_response, g_tokens, n, item, "description", s->description, sizeof s->description);
		eptx_json_get_bool(g_response, g_tokens, n, item, "installable", &installable);
		eptx_json_get_bool(g_response, g_tokens, n, item, "showsInOriginalDash", &shows);
		s->installable = installable;
		s->shows_in_dash = shows;
		int has_art = 0;
		eptx_json_get_bool(g_response, g_tokens, n, item, "hasArt", &has_art);
		s->art = has_art ? ART_NONE : ART_FAILED;
	}
	g_app.store_loaded = true;
	g_app.store_serial++;
	app_unlock();
}

/*
 * O avatar mora em /avatar/<gamertag>/avatar-body.png (ou .gif), o mesmo formato
 * do servico antigo do Xbox LIVE, so que no servidor EPTX. A rota e publica; mandar
 * a assinatura junto nao atrapalha e reaproveita o cliente que ja existe.
 *
 * O GIF do 360sona pode ter megas: ele vem num buffer proprio, e cada quadro e
 * reduzido para o tamanho em que a dash mostra o avatar.
 */
static bool fetch_avatar(const char *gamertag, bool gif, app_image *image)
{
	char path[160];
	size_t w = (size_t)snprintf(path, sizeof path, "/avatar/");
	for (const unsigned char *p = (const unsigned char *)gamertag; *p && w + 4 < sizeof path; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
			*p == '_' || *p == '.')
			path[w++] = (char)*p;
		else
			w += (size_t)snprintf(path + w, sizeof path - w, "%%%02X", *p);
	}
	if ((size_t)snprintf(path + w, sizeof path - w, gif ? "/avatar-body.gif" : "/avatar-body.png") >= sizeof path - w)
		return false;
	int len = 0;
	if (!gif) {
		if (api_get(path, g_response, RESPONSE_CAP, &len) != EPTX_OK)
			return false; /* 404: a conta nao tem avatar */
		return image_decode_png((const uint8_t *)g_response, (size_t)len, image);
	}
	char *buffer = (char *)malloc(GIF_CAP);
	if (!buffer)
		return false;
	const bool ok = api_get(path, buffer, GIF_CAP, &len) == EPTX_OK &&
		image_decode_gif((const uint8_t *)buffer, (size_t)len, GIF_SIDE, GIF_FRAMES, image);
	free(buffer);
	return ok;
}

/* "avatarType": o que a conta mostra, "look" (o desenhado), "png" ou "gif" (a foto). */
static bool read_shows_look(int n, int object)
{
	char type[8] = "";
	eptx_json_get_string(g_response, g_tokens, n, object, "avatarType", type, sizeof type);
	return strcmp(type, "look") == 0;
}

/* "avatarType": "gif" quando o avatar e o GIF animado do 360sona. */
static bool read_gif(int n, int object)
{
	char type[8] = "";
	eptx_json_get_string(g_response, g_tokens, n, object, "avatarType", type, sizeof type);
	return strcmp(type, "gif") == 0;
}

/* "avatarLook": [16 numeros]. Falso se nao veio (null) ou veio torto; os numeros sao limitados as opcoes. */
static bool read_look(int n, int object, avatar_look *out)
{
	const int array = eptx_json_get(g_response, g_tokens, n, object, "avatarLook");
	if (array < 0 || g_tokens[array].type != EPTX_JSON_ARRAY)
		return false;
	for (int i = 0; i < LOOK_FIELDS; i++) {
		const int item = eptx_json_at(g_tokens, n, array, i);
		int64_t v = 0;
		if (item < 0 || eptx_json_int(g_response, g_tokens, item, &v) != 0)
			return false;
		out->v[i] = (uint8_t)(v >= 0 && v < AVATAR_OPTIONS[i] ? v : 0);
	}
	return true;
}

/*
 * O proprio avatar. Com look a dash desenha e nao baixa nada; sem, /me diz quando
 * a imagem mudou, e so entao ela e baixada de novo.
 */
static void load_me(void)
{
	int len = 0;
	if (api_get("/api/v1/me", g_response, RESPONSE_CAP, &len) != EPTX_OK)
		return;
	const int n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
	int64_t updated = 0;
	avatar_look look;
	bool has_look = false, gif = false;
	if (n > 0) {
		eptx_json_get_int(g_response, g_tokens, n, 0, "avatarUpdated", &updated);
		has_look = read_look(n, 0, &look) && read_shows_look(n, 0);
		gif = read_gif(n, 0);
	}

	app_lock();
	if (has_look) {
		const bool changed = !g_app.has_look || memcmp(&look, &g_app.look, sizeof look) != 0;
		g_app.has_look = true;
		g_app.look = look;
		g_app.avatar_updated = updated;
		if (g_app.avatar_art != ART_FAILED || changed) {
			image_free(&g_app.avatar_image);
			g_app.avatar_art = ART_FAILED;
			g_app.avatar_serial++;
		}
		app_unlock();
		return;
	}
	const bool changed = g_app.has_look || updated != g_app.avatar_updated || g_app.avatar_art == ART_NONE;
	g_app.has_look = false;
	app_unlock();
	if (!changed)
		return;

	app_image image = { 0 };
	const bool ok = updated > 0 && fetch_avatar(g_client.gamertag, gif, &image);
	app_lock();
	image_free(&g_app.avatar_image);
	g_app.avatar_image = image;
	g_app.avatar_gif = gif;
	g_app.avatar_updated = updated;
	g_app.avatar_art = ok ? ART_READY : ART_FAILED;
	g_app.avatar_serial++;
	app_unlock();
}

static void load_friend_avatar(int index)
{
	char gamertag[40];
	bool gif = false;
	app_lock();
	const bool valid = index < g_app.friend_count;
	if (valid) {
		snprintf(gamertag, sizeof gamertag, "%s", g_app.friends[index].gamertag);
		gif = g_app.friends[index].avatar_gif;
	}
	const uint32_t serial = g_app.friends_serial;
	app_unlock();
	if (!valid)
		return;

	app_image image = { 0 };
	const bool ok = fetch_avatar(gamertag, gif, &image);
	app_lock();
	if (serial == g_app.friends_serial && index < g_app.friend_count && g_app.friends[index].avatar_gif == gif) {
		image_free(&g_app.friends[index].image);
		g_app.friends[index].image = image;
		g_app.friends[index].art = ok ? ART_READY : ART_FAILED;
	} else {
		image_free(&image);
	}
	app_unlock();
}

static void load_friends(void)
{
	int len = 0;
	const int rc = api_get("/api/v1/friends", g_response, RESPONSE_CAP, &len);
	if (rc != EPTX_OK) {
		if (rc != EPTX_ERR_NO_SESSION)
			say("Nao consegui carregar os amigos: %s", server_error());
		return;
	}
	const int n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
	const int items = n > 0 ? eptx_json_get(g_response, g_tokens, n, 0, "items") : -1;
	if (items < 0)
		return;

	/* Monta fora do lock e so depois compara com a lista que a tela esta mostrando. */
	static friend_item found[APP_MAX_FRIENDS];
	int count = 0;
	for (int i = 0; count < (int)APP_MAX_FRIENDS; i++) {
		const int item = eptx_json_at(g_tokens, n, items, i);
		if (item < 0)
			break;
		friend_item *f = &found[count++];
		char state[16] = "";
		memset(f, 0, sizeof *f);
		eptx_json_get_int(g_response, g_tokens, n, item, "friendshipId", &f->friendship_id);
		eptx_json_get_int(g_response, g_tokens, n, item, "userId", &f->user_id);
		eptx_json_get_string(g_response, g_tokens, n, item, "gamertag", f->gamertag, sizeof f->gamertag);
		eptx_json_get_string(g_response, g_tokens, n, item, "motto", f->motto, sizeof f->motto);
		eptx_json_get_int(g_response, g_tokens, n, item, "avatarUpdated", &f->avatar_updated);
		f->has_look = read_look(n, item, &f->look) && read_shows_look(n, item);
		f->avatar_gif = read_gif(n, item);
		const int presence = eptx_json_get(g_response, g_tokens, n, item, "presence");
		if (presence >= 0) {
			eptx_json_get_string(g_response, g_tokens, n, presence, "state", state, sizeof state);
			eptx_json_get_string(g_response, g_tokens, n, presence, "titleName", f->title_name, sizeof f->title_name);
		}
		f->online = strcmp(state, "online") == 0 || strcmp(state, "away") == 0;
		f->art = !f->has_look && f->avatar_updated > 0 ? ART_NONE : ART_FAILED;
	}

	app_lock();
	bool same = count == g_app.friend_count;
	for (int i = 0; same && i < count; i++)
		same = strcmp(found[i].gamertag, g_app.friends[i].gamertag) == 0;
	if (same) {
		/* Mesma gente na mesma ordem: so a presenca muda, e o avatar so se a pessoa trocou. */
		for (int i = 0; i < count; i++) {
			friend_item *f = &g_app.friends[i];
			memcpy(f->motto, found[i].motto, sizeof f->motto);
			memcpy(f->title_name, found[i].title_name, sizeof f->title_name);
			f->online = found[i].online;
			f->has_look = found[i].has_look;
			f->look = found[i].look;
			if (f->avatar_updated != found[i].avatar_updated) {
				f->avatar_updated = found[i].avatar_updated;
				f->avatar_gif = found[i].avatar_gif;
				if (f->art != ART_LOADING)
					f->art = !f->has_look && f->avatar_updated > 0 ? ART_NONE : ART_FAILED;
			}
		}
	} else {
		for (int i = 0; i < g_app.friend_count; i++)
			image_free(&g_app.friends[i].image);
		memcpy(g_app.friends, found, sizeof(friend_item) * (size_t)count);
		g_app.friend_count = count;
		g_app.friends_serial++;
	}
	g_app.friends_loaded = true;
	app_unlock();
}

static void send_presence(const char *state)
{
	char body[160];
	char out[256];
	int len = 0;
	snprintf(body, sizeof body,
		"{\"state\":\"%s\",\"titleId\":\"" OWN_TITLE_ID "\",\"titleName\":\"EPTX NXE Edition\",\"detail\":\"Na dash\"}",
		state);
	if (api_post("/api/v1/presence", body, out, sizeof out, &len) != EPTX_OK)
		xsdk_log_write("eptx: presenca nao foi: %s", g_client.last_error);
}

/* --- pedidos de amizade ----------------------------------------------------------- */

static int read_requests(int n, int array, request_item *out)
{
	int count = 0;
	for (int i = 0; array >= 0 && count < (int)APP_MAX_REQUESTS; i++) {
		const int item = eptx_json_at(g_tokens, n, array, i);
		if (item < 0)
			break;
		request_item *r = &out[count++];
		memset(r, 0, sizeof *r);
		eptx_json_get_int(g_response, g_tokens, n, item, "id", &r->id);
		eptx_json_get_int(g_response, g_tokens, n, item, "user_id", &r->user_id);
		eptx_json_get_string(g_response, g_tokens, n, item, "gamertag", r->gamertag, sizeof r->gamertag);
	}
	return count;
}

static void load_requests(void)
{
	int len = 0;
	if (api_get("/api/v1/friends/requests", g_response, RESPONSE_CAP, &len) != EPTX_OK)
		return;
	const int n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
	if (n <= 0)
		return;
	static request_item in[APP_MAX_REQUESTS], out[APP_MAX_REQUESTS];
	const int in_count = read_requests(n, eptx_json_get(g_response, g_tokens, n, 0, "incoming"), in);
	const int out_count = read_requests(n, eptx_json_get(g_response, g_tokens, n, 0, "outgoing"), out);

	app_lock();
	const bool changed = in_count != g_app.requests_in_count || out_count != g_app.requests_out_count ||
		memcmp(in, g_app.requests_in, sizeof(request_item) * (size_t)in_count) != 0 ||
		memcmp(out, g_app.requests_out, sizeof(request_item) * (size_t)out_count) != 0;
	if (changed) {
		memcpy(g_app.requests_in, in, sizeof(request_item) * (size_t)in_count);
		memcpy(g_app.requests_out, out, sizeof(request_item) * (size_t)out_count);
		g_app.requests_in_count = in_count;
		g_app.requests_out_count = out_count;
		g_app.requests_serial++;
	}
	g_app.requests_loaded = true;
	app_unlock();
}

/* Quem mandou o pedido id, para a mensagem de confirmacao. */
static void request_gamertag(int64_t id, char *out, size_t cap)
{
	app_lock();
	snprintf(out, cap, "%s", "a pessoa");
	for (int i = 0; i < g_app.requests_in_count; i++)
		if (g_app.requests_in[i].id == id)
			snprintf(out, cap, "%s", g_app.requests_in[i].gamertag);
	app_unlock();
}

static void send_request(const task *t)
{
	char tag[96], body[128], out[256];
	int len = 0;
	json_string(t->text, tag, sizeof tag);
	snprintf(body, sizeof body, "{\"gamertag\":%s}", tag);
	if (api_post("/api/v1/friends/requests", body, out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		return;
	}
	/* Se a outra pessoa ja tinha me adicionado, o servidor fecha a amizade na hora. */
	char status[16] = "";
	const int n = eptx_json_parse(out, (size_t)len, g_tokens, MAX_TOKENS);
	if (n > 0)
		eptx_json_get_string(out, g_tokens, n, 0, "status", status, sizeof status);
	load_requests();
	if (strcmp(status, "accepted") == 0) {
		load_friends();
		finish_action(true, "%s ja tinha te adicionado: agora voces sao amigos.", t->text);
	} else {
		finish_action(true, "Pedido de amizade enviado para %s.", t->text);
	}
}

static void accept_request(int64_t id)
{
	char who[40], path[80], out[128];
	int len = 0;
	request_gamertag(id, who, sizeof who);
	snprintf(path, sizeof path, "/api/v1/friends/requests/%lld/accept", (long long)id);
	if (api_post(path, "{}", out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		load_requests();
		return;
	}
	load_requests();
	load_friends();
	finish_action(true, "Agora voce e %s sao amigos.", who);
}

static void decline_request(int64_t id)
{
	char who[40], path[64], out[128];
	int len = 0;
	request_gamertag(id, who, sizeof who);
	snprintf(path, sizeof path, "/api/v1/friends/%lld", (long long)id);
	if (api_delete(path, out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		load_requests();
		return;
	}
	load_requests();
	finish_action(true, "Pedido de %s recusado.", who);
}

static void remove_friend(int64_t friendship_id)
{
	char who[40] = "O amigo", path[64], out[128];
	int len = 0;
	app_lock();
	for (int i = 0; i < g_app.friend_count; i++)
		if (g_app.friends[i].friendship_id == friendship_id)
			snprintf(who, sizeof who, "%s", g_app.friends[i].gamertag);
	app_unlock();
	snprintf(path, sizeof path, "/api/v1/friends/%lld", (long long)friendship_id);
	if (api_delete(path, out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		return;
	}
	load_friends();
	finish_action(true, "%s saiu da sua lista de amigos.", who);
}

/*
 * Procura a gamertag pela rota publica do avatar: ela da 404 para quem nao existe e
 * o PNG do avatar (o padrao, se a pessoa nunca mexeu) para quem existe. Assim a dash
 * mostra quem vai receber o pedido sem o servidor precisar de uma rota de busca.
 */
static void lookup(const task *t)
{
	app_image image = { 0 };
	const bool ok = fetch_avatar(t->text, false, &image);
	const int status = g_client.last_status;
	app_lock();
	if (strcmp(g_app.lookup_gamertag, t->text) != 0) {
		/* a pessoa ja procurou outra */
		app_unlock();
		image_free(&image);
		return;
	}
	image_free(&g_app.lookup_image);
	if (ok) {
		g_app.lookup = LOOKUP_FOUND;
		g_app.lookup_image = image;
		g_app.lookup_art = ART_READY;
	} else {
		g_app.lookup = status == 404 && g_app.server_ok ? LOOKUP_NOT_FOUND : LOOKUP_FAILED;
		g_app.lookup_art = ART_FAILED;
		image_free(&image);
	}
	g_app.lookup_serial++;
	app_unlock();
}

/* --- pesquisa na loja ------------------------------------------------------------------ */

static void search(const task *t)
{
	char query[200], path[260];
	int len = 0;
	if (!url_encode(t->text, query, sizeof query))
		return;
	snprintf(path, sizeof path, "/api/v1/catalog?limit=200&q=%s", query);
	const int rc = api_get(path, g_response, RESPONSE_CAP, &len);
	static int64_t ids[APP_MAX_STORE];
	int count = 0;
	if (rc == EPTX_OK) {
		const int n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
		const int items = n > 0 ? eptx_json_get(g_response, g_tokens, n, 0, "items") : -1;
		for (int i = 0; items >= 0 && count < (int)APP_MAX_STORE; i++) {
			const int item = eptx_json_at(g_tokens, n, items, i);
			if (item < 0)
				break;
			eptx_json_get_int(g_response, g_tokens, n, item, "id", &ids[count++]);
		}
	} else {
		say("A pesquisa nao foi: %s", server_error());
	}
	app_lock();
	if (strcmp(g_app.search_query, t->text) == 0) {
		g_app.search_count = 0;
		/* A loja ja tem o catalogo; a pesquisa so diz quais dele mostrar, na ordem do servidor. */
		for (int k = 0; k < count; k++)
			for (int i = 0; i < g_app.store_count; i++)
				if (g_app.store[i].id == ids[k]) {
					g_app.search_index[g_app.search_count++] = i;
					break;
				}
		g_app.search_loading = false;
		g_app.search_serial++;
	}
	app_unlock();
}

/* --- salas ---------------------------------------------------------------------------- */

static void read_room(int n, int object, room_info *r)
{
	memset(r, 0, sizeof *r);
	if (object < 0 || g_tokens[object].type != EPTX_JSON_OBJECT)
		return;
	int64_t host_id = 0, relay = 0;
	r->active = true;
	eptx_json_get_int(g_response, g_tokens, n, object, "id", &r->id);
	eptx_json_get_int(g_response, g_tokens, n, object, "hostId", &host_id);
	eptx_json_get_int(g_response, g_tokens, n, object, "relayPort", &relay);
	r->relay_port = (int)relay;
	eptx_json_get_string(g_response, g_tokens, n, object, "game", r->game, sizeof r->game);
	eptx_json_get_string(g_response, g_tokens, n, object, "mode", r->mode, sizeof r->mode);
	eptx_json_get_string(g_response, g_tokens, n, object, "state", r->state, sizeof r->state);
	eptx_json_get_string(g_response, g_tokens, n, object, "hostGamertag", r->host, sizeof r->host);
	r->is_host = host_id == g_client.user_id || strcasecmp(r->host, g_client.gamertag) == 0;
	const int members = eptx_json_get(g_response, g_tokens, n, object, "members");
	for (int i = 0; members >= 0 && r->member_count < (int)APP_ROOM_MEMBERS; i++) {
		const int m = eptx_json_at(g_tokens, n, members, i);
		if (m < 0)
			break;
		room_member *rm = &r->members[r->member_count++];
		int64_t slot = 0;
		int udp = 0;
		eptx_json_get_string(g_response, g_tokens, n, m, "gamertag", rm->gamertag, sizeof rm->gamertag);
		eptx_json_get_int(g_response, g_tokens, n, m, "slot", &slot);
		eptx_json_get_bool(g_response, g_tokens, n, m, "udp", &udp);
		rm->slot = (int)slot;
		rm->udp = udp;
	}
}

/* A sala atual e os convites. Consultar a sala tambem diz ao servidor que a conta segue nela. */
static void load_rooms(void)
{
	int len = 0;
	room_info room;
	static invite_item invites[APP_MAX_INVITES];
	int invite_count = 0;
	if (api_get("/api/v1/rooms/current", g_response, RESPONSE_CAP, &len) != EPTX_OK)
		return;
	int n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
	read_room(n, n > 0 ? eptx_json_get(g_response, g_tokens, n, 0, "room") : -1, &room);
	if (api_get("/api/v1/rooms/invites", g_response, RESPONSE_CAP, &len) == EPTX_OK) {
		n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
		const int items = n > 0 ? eptx_json_get(g_response, g_tokens, n, 0, "items") : -1;
		for (int i = 0; items >= 0 && invite_count < (int)APP_MAX_INVITES; i++) {
			const int item = eptx_json_at(g_tokens, n, items, i);
			if (item < 0)
				break;
			invite_item *iv = &invites[invite_count++];
			int64_t members = 0;
			memset(iv, 0, sizeof *iv);
			eptx_json_get_int(g_response, g_tokens, n, item, "roomId", &iv->room_id);
			eptx_json_get_int(g_response, g_tokens, n, item, "members", &members);
			iv->members = (int)members;
			eptx_json_get_string(g_response, g_tokens, n, item, "game", iv->game, sizeof iv->game);
			eptx_json_get_string(g_response, g_tokens, n, item, "mode", iv->mode, sizeof iv->mode);
			eptx_json_get_string(g_response, g_tokens, n, item, "hostGamertag", iv->host, sizeof iv->host);
		}
	}
	app_lock();
	const bool changed = memcmp(&room, &g_app.room, sizeof room) != 0 || invite_count != g_app.invite_count ||
		memcmp(invites, g_app.invites, sizeof(invite_item) * (size_t)invite_count) != 0;
	if (changed) {
		g_app.room = room;
		memcpy(g_app.invites, invites, sizeof(invite_item) * (size_t)invite_count);
		g_app.invite_count = invite_count;
		g_app.rooms_serial++;
	}
	app_unlock();
}

static void room_create(const task *t)
{
	char game[80], mode[80], body[200], out[1024];
	int len = 0;
	json_string(t->text, game, sizeof game);
	json_string(t->mode, mode, sizeof mode);
	snprintf(body, sizeof body, "{\"game\":%s,\"mode\":%s}", game, mode);
	if (api_post("/api/v1/rooms", body, out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		return;
	}
	load_rooms();
	finish_action(true, "Sala criada. Convide amigos e abra o jogo para jogar.");
}

static void room_invite(int64_t user_id)
{
	char path[64], body[64], out[128], who[40] = "o amigo";
	int len = 0;
	app_lock();
	const int64_t room = g_app.room.active ? g_app.room.id : 0;
	for (int i = 0; i < g_app.friend_count; i++)
		if (g_app.friends[i].user_id == user_id)
			snprintf(who, sizeof who, "%s", g_app.friends[i].gamertag);
	app_unlock();
	if (!room) {
		finish_action(false, "Crie uma sala antes de convidar.");
		return;
	}
	snprintf(path, sizeof path, "/api/v1/rooms/%lld/invite", (long long)room);
	snprintf(body, sizeof body, "{\"userId\":%lld}", (long long)user_id);
	if (api_post(path, body, out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		return;
	}
	finish_action(true, "Convite enviado para %s.", who);
}

static void room_join(int64_t room_id)
{
	char path[64], out[1024];
	int len = 0;
	snprintf(path, sizeof path, "/api/v1/rooms/%lld/join", (long long)room_id);
	if (api_post(path, "{}", out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		load_rooms();
		return;
	}
	load_rooms();
	finish_action(true, "Voce entrou na sala. A partida comeca dentro do jogo.");
}

static void room_leave(void)
{
	char path[64], out[128];
	int len = 0;
	app_lock();
	const int64_t room = g_app.room.active ? g_app.room.id : 0;
	app_unlock();
	if (!room) {
		finish_action(false, "Voce nao esta em nenhuma sala.");
		return;
	}
	snprintf(path, sizeof path, "/api/v1/rooms/%lld/leave", (long long)room);
	if (api_post(path, "{}", out, sizeof out, &len) != EPTX_OK) {
		finish_action(false, "%s", server_error());
		return;
	}
	load_rooms();
	finish_action(true, "Voce saiu da sala.");
}

static void signed_in(void)
{
	app_lock();
	g_app.session = SESSION_SIGNED_IN;
	snprintf(g_app.gamertag, sizeof g_app.gamertag, "%s", g_client.gamertag);
	app_unlock();
	send_presence("online");
	load_me();
	load_catalog();
	load_friends();
	load_requests();
	load_rooms();
}

/*
 * Sai da conta na memoria. Com delete_file, apaga tambem a sessao salva do perfil
 * (sair do EPTX, sessao expirada); trocar de perfil so solta a conta, e o perfil
 * anterior volta a ela da proxima vez.
 */
static void drop_account(bool delete_file)
{
	memset(g_client.token, 0, sizeof g_client.token);
	memset(g_client.secret, 0, sizeof g_client.secret);
	g_saved_pic = 0;
	char path[112];
	if (delete_file && session_file(path, sizeof path, ""))
		unlink(path);
	app_lock();
	g_app.session = SESSION_SIGNED_OUT;
	g_app.gamertag[0] = '\0';
	for (int i = 0; i < g_app.friend_count; i++)
		image_free(&g_app.friends[i].image);
	g_app.friend_count = 0;
	g_app.friends_loaded = false;
	g_app.friends_serial++;
	image_free(&g_app.avatar_image);
	g_app.has_look = false;
	g_app.avatar_updated = 0;
	g_app.avatar_art = ART_NONE;
	g_app.avatar_serial++;
	/* O que era da conta: pedidos, salas, a gamertag procurada. */
	g_app.requests_in_count = g_app.requests_out_count = 0;
	g_app.requests_loaded = false;
	g_app.requests_serial++;
	memset(&g_app.room, 0, sizeof g_app.room);
	g_app.invite_count = 0;
	g_app.rooms_serial++;
	image_free(&g_app.lookup_image);
	g_app.lookup = LOOKUP_NONE;
	g_app.lookup_serial++;
	app_unlock();
}

static void forget_session(void) { drop_account(true); }

/* O editor salvou: as escolhas e o corpo desenhado, em base64 dentro do JSON. */
static void save_avatar(const task *t)
{
	static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const size_t cap = 256 + (size_t)(t->png_len + 2) / 3 * 4;
	char *body = (char *)malloc(cap);
	bool ok = false;
	if (body) {
		size_t w = (size_t)snprintf(body, cap, "{\"look\":[");
		for (int i = 0; i < LOOK_FIELDS; i++)
			w += (size_t)snprintf(body + w, cap - w, i ? ",%u" : "%u", (unsigned)t->look.v[i]);
		w += (size_t)snprintf(body + w, cap - w, "],\"png\":\"");
		for (int i = 0; i < t->png_len; i += 3) {
			const int left = t->png_len - i;
			const uint32_t v = (uint32_t)t->png[i] << 16 | (left > 1 ? (uint32_t)t->png[i + 1] << 8 : 0) |
				(left > 2 ? t->png[i + 2] : 0);
			body[w++] = B64[v >> 18 & 63];
			body[w++] = B64[v >> 12 & 63];
			body[w++] = left > 1 ? B64[v >> 6 & 63] : '=';
			body[w++] = left > 2 ? B64[v & 63] : '=';
		}
		snprintf(body + w, cap - w, "\"}");
		char out[256];
		int len = 0;
		ok = api_post("/api/v1/me/avatar/look", body, out, sizeof out, &len) == EPTX_OK;
		if (ok) {
			const int n = eptx_json_parse(out, (size_t)len, g_tokens, MAX_TOKENS);
			int64_t updated = 0;
			if (n > 0)
				eptx_json_get_int(out, g_tokens, n, 0, "updatedAt", &updated);
			app_lock();
			g_app.has_look = true;
			g_app.look = t->look;
			g_app.avatar_updated = updated;
			image_free(&g_app.avatar_image);
			g_app.avatar_art = ART_FAILED;
			g_app.avatar_serial++;
			app_unlock();
		}
		free(body);
	}
	free(t->png);
	app_lock();
	g_app.avatar_saving = false;
	app_unlock();
	if (ok)
		say("Avatar salvo na sua conta.");
	else if (body)
		say("Nao consegui salvar o avatar: %s", g_client.last_error);
	else
		say("Nao consegui salvar o avatar: sem memoria.");
}

/* --- biblioteca no HD ------------------------------------------------------------ */

static bool is_hex8(const char *s)
{
	if (strlen(s) != 8)
		return false;
	for (int i = 0; i < 8; i++) {
		const char c = s[i];
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

static void scan_type(const char *title_id, uint32_t content_type, library_item *items, int *count)
{
	char dir_path[160];
	snprintf(dir_path, sizeof dir_path, "%s" CONTENT_REST "/%s/%08X", g_root, title_id, (unsigned)content_type);
	DIR *dir = opendir(dir_path);
	if (!dir)
		return;
	struct dirent *entry;
	while ((entry = readdir(dir)) && *count < (int)APP_MAX_LIBRARY) {
		if (entry->d_name[0] == '.')
			continue;
		library_item *it = &items[*count];
		memset(it, 0, sizeof *it);
		if ((size_t)snprintf(it->path, sizeof it->path, "%s/%s", dir_path, entry->d_name) >= sizeof it->path)
			continue; /* FATX nao passa de 42 caracteres por nome; isto nao deve acontecer */

		uint8_t head[STFS_NAMES_END];
		FILE *fp = fopen(it->path, "rb");
		if (!fp)
			continue; /* uma pasta .data de GOD, por exemplo */
		const size_t got = fread(head, 1, sizeof head, fp);
		fclose(fp);
		if (got < sizeof head || (memcmp(head, "CON ", 4) && memcmp(head, "LIVE", 4) && memcmp(head, "PIRS", 4)))
			continue;

		utf16be_to_utf8(head + 0x411, 0x80, it->name, sizeof it->name);
		if (!it->name[0])
			utf16be_to_utf8(head + 0x1691, 0x80, it->name, sizeof it->name);
		if (!it->name[0])
			snprintf(it->name, sizeof it->name, "%s", title_id);
		snprintf(it->title_id, sizeof it->title_id, "%s", title_id);
		it->content_type = content_type;
		(*count)++;
	}
	closedir(dir);
}

static void scan_library(void)
{
	/* Monta fora do lock: varrer o HD leva tempo e a tela nao pode esperar. */
	static library_item found[APP_MAX_LIBRARY];
	int count = 0;
	char content[48];
	DIR *root = opendir(on_root(content, sizeof content, CONTENT_REST));
	if (root) {
		struct dirent *entry;
		while ((entry = readdir(root)) && count < (int)APP_MAX_LIBRARY) {
			if (!is_hex8(entry->d_name) || strcasecmp(entry->d_name, OWN_TITLE_ID) == 0 ||
				strcasecmp(entry->d_name, LIVE_TITLE_ID) == 0)
				continue;
			scan_type(entry->d_name, CONTENT_GOD, found, &count);
			scan_type(entry->d_name, CONTENT_ARCADE, found, &count);
		}
		closedir(root);
	} else {
		xsdk_log_write("eptx: nao abri %s: errno %d", content, errno);
	}

	app_lock();
	for (int i = 0; i < g_app.library_count; i++)
		image_free(&g_app.library[i].image);
	memcpy(g_app.library, found, sizeof(library_item) * (size_t)count);
	g_app.library_count = count;
	g_app.library_loaded = true;
	g_app.library_serial++;
	app_unlock();
	xsdk_log_write("eptx: biblioteca com %d titulo(s)", count);
}

/* O icone mora no proprio cabecalho: o do titulo, ou o do pacote se aquele faltar. */
static void load_library_art(int index)
{
	char path[300];
	app_lock();
	const bool valid = index < g_app.library_count;
	if (valid)
		snprintf(path, sizeof path, "%s", g_app.library[index].path);
	const uint32_t serial = g_app.library_serial;
	app_unlock();
	if (!valid)
		return;

	static uint8_t head[STFS_TITLE_THUMB + STFS_THUMB_MAX];
	app_image image = { 0 };
	FILE *fp = fopen(path, "rb");
	if (fp) {
		const size_t got = fread(head, 1, sizeof head, fp);
		fclose(fp);
		if (got >= STFS_THUMB) {
			const uint32_t title_size = be32(head + STFS_TITLE_THUMB_SIZE);
			const uint32_t size = be32(head + STFS_THUMB_SIZE);
			if (title_size && title_size <= STFS_THUMB_MAX && got >= STFS_TITLE_THUMB + title_size)
				image_decode_png(head + STFS_TITLE_THUMB, title_size, &image);
			if (!image.rgba && size && size <= STFS_THUMB_MAX && got >= STFS_THUMB + size)
				image_decode_png(head + STFS_THUMB, size, &image);
		}
	}

	app_lock();
	if (serial == g_app.library_serial && index < g_app.library_count) {
		g_app.library[index].image = image;
		g_app.library[index].art = image.rgba ? ART_READY : ART_FAILED;
	} else {
		image_free(&image);
	}
	app_unlock();
}

static void load_store_art(int index)
{
	char path[64];
	app_lock();
	const bool valid = index < g_app.store_count;
	if (valid)
		snprintf(path, sizeof path, "/api/v1/catalog/%ld/art", (long)g_app.store[index].id);
	const uint32_t serial = g_app.store_serial;
	app_unlock();
	if (!valid)
		return;

	app_image image = { 0 };
	int len = 0;
	if (api_get(path, g_response, RESPONSE_CAP, &len) == EPTX_OK)
		image_decode_png((const uint8_t *)g_response, (size_t)len, &image);

	app_lock();
	if (serial == g_app.store_serial && index < g_app.store_count) {
		g_app.store[index].image = image;
		g_app.store[index].art = image.rgba ? ART_READY : ART_FAILED;
	} else {
		image_free(&image);
	}
	app_unlock();
}

/* --- instalacao --------------------------------------------------------------------- */

/*
 * "Hdd1:\Content\0000000000000000\..." vira "hdd:\Content\0000000000000000\..." (ou
 * usb:, se o EPTX mora num HD USB): hdd: e usb: sao como o xsdk chama os discos. O
 * servidor so manda Hdd1.
 */
static bool console_path(const char *install_path, char *out, size_t cap)
{
	static const char prefix[] = "Hdd1:";
	if (strncmp(install_path, prefix, sizeof prefix - 1) != 0)
		return false;
	return (size_t)snprintf(out, cap, "%s%s", g_root, install_path + sizeof prefix - 1) < cap;
}

typedef struct {
	int64_t id;
	int64_t size;
	char path[300];
} install_file;

static int64_t g_bytes_before_file;

static void on_progress(void *user, int64_t done, int64_t total)
{
	(void)user;
	(void)total;
	app_lock();
	g_app.install_done = g_bytes_before_file + done;
	app_unlock();
}

static void install(int64_t id)
{
	char path[64];
	int len = 0;
	snprintf(path, sizeof path, "/api/v1/catalog/%ld", (long)id);
	if (api_get(path, g_response, RESPONSE_CAP, &len) != EPTX_OK) {
		say("Nao consegui abrir o jogo: %s", g_client.last_error);
		return;
	}
	const int n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
	const int files = n > 0 ? eptx_json_get(g_response, g_tokens, n, 0, "files") : -1;
	if (files < 0) {
		say("O jogo veio sem lista de arquivos.");
		return;
	}
	char name[96] = "";
	int shows = 0;
	eptx_json_get_string(g_response, g_tokens, n, 0, "name", name, sizeof name);
	eptx_json_get_bool(g_response, g_tokens, n, 0, "showsInOriginalDash", &shows);

	int count = 0;
	while (eptx_json_at(g_tokens, n, files, count) >= 0)
		count++;
	install_file *list = calloc((size_t)(count ? count : 1), sizeof *list);
	if (!list) {
		say("Sem memoria para a lista de arquivos.");
		return;
	}
	int64_t total = 0;
	for (int i = 0; i < count; i++) {
		const int file = eptx_json_at(g_tokens, n, files, i);
		char install_path[300] = "";
		eptx_json_get_int(g_response, g_tokens, n, file, "id", &list[i].id);
		eptx_json_get_int(g_response, g_tokens, n, file, "size", &list[i].size);
		eptx_json_get_string(g_response, g_tokens, n, file, "installPath", install_path, sizeof install_path);
		if (!console_path(install_path, list[i].path, sizeof list[i].path)) {
			say("Destino que o console nao conhece: %s", install_path);
			free(list);
			return;
		}
		total += list[i].size;
	}

	app_lock();
	g_app.installing = true;
	snprintf(g_app.install_name, sizeof g_app.install_name, "%s", name);
	g_app.install_files = count;
	g_app.install_total = total;
	g_app.install_done = 0;
	app_unlock();
	xsdk_log_write("eptx: instalando \"%s\", %d arquivo(s), %lld bytes", name, count, (long long)total);

	/*
	 * Do ultimo para o primeiro: num GOD o primeiro arquivo e o cabecalho, e e ele
	 * que faz a dash listar o jogo. Gravando-o por ultimo, um download que cai no
	 * meio nao deixa na biblioteca um jogo sem as partes de dados.
	 */
	bool ok = true;
	g_bytes_before_file = 0;
	for (int k = 0; k < count && ok; k++) {
		const install_file *f = &list[count - 1 - k];
		app_lock();
		g_app.install_file = k + 1;
		app_unlock();
		if (eptx_client_download(&g_client, f->id, f->path, f->size, DOWNLOAD_RETRIES, on_progress, NULL) != EPTX_OK) {
			const char *slash = strrchr(f->path, '\\');
			say("%s parou em %s: %s. Instale de novo para retomar.", name, slash ? slash + 1 : f->path,
				g_client.last_error);
			ok = false;
		}
		g_bytes_before_file += f->size;
	}
	free(list);

	app_lock();
	g_app.installing = false;
	app_unlock();
	if (ok) {
		say(shows ? "%s instalado. Ja esta na biblioteca." : "%s instalado em Hdd1:\\Games.", name);
		scan_library();
	}
}

/* --- o perfil do Xbox ------------------------------------------------------------------ */

#ifdef EPTX_PC
static const char *const PROFILE_ROOTS[] = { "pc-hdd" };
#else
/* Os discos que o xsdk monta: o perfil pode estar no HD, num USB ou numa unidade de memoria. */
static const char *const PROFILE_ROOTS[] = { "hdd:", "usb:", "usb1:", "usb2:", "mu:", "mu1:" };
#endif

static bool is_hex16(const char *s)
{
	if (strlen(s) != 16)
		return false;
	for (int i = 0; i < 16; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}

static uint32_t fnv1a(const uint8_t *p, int len)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < len; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/*
 * O pacote do perfil e Content/<XUID>/FFFE07D1/00010000/<XUID>, um STFS com a
 * gamertag no nome (0x411) e a foto do jogador na miniatura do cabecalho (0x171A).
 * Acha o da gamertag logada e devolve o XUID e a foto (PNG, malloc).
 */
static bool find_profile(const char *gamertag, char *xuid, size_t xuid_cap, uint8_t **png, int *png_len)
{
	static uint8_t head[STFS_THUMB + STFS_THUMB_MAX];
	*png = NULL;
	*png_len = 0;
	for (size_t r = 0; r < sizeof PROFILE_ROOTS / sizeof PROFILE_ROOTS[0]; r++) {
		char dir_path[64];
		snprintf(dir_path, sizeof dir_path, "%s/Content", PROFILE_ROOTS[r]);
		DIR *dir = opendir(dir_path);
		if (!dir)
			continue;
		struct dirent *entry;
		bool found = false;
		while (!found && (entry = readdir(dir))) {
			if (!is_hex16(entry->d_name) || strcmp(entry->d_name, "0000000000000000") == 0)
				continue;
			char path[160];
			snprintf(path, sizeof path, "%s/%s/FFFE07D1/00010000/%s", dir_path, entry->d_name, entry->d_name);
			FILE *fp = fopen(path, "rb");
			if (!fp)
				continue;
			const size_t got = fread(head, 1, sizeof head, fp);
			fclose(fp);
			if (got < STFS_THUMB || (memcmp(head, "CON ", 4) && memcmp(head, "LIVE", 4) && memcmp(head, "PIRS", 4)))
				continue;
			char name[40];
			utf16be_to_utf8(head + 0x411, 0x80, name, sizeof name);
			if (strcasecmp(name, gamertag) != 0)
				continue;
			found = true;
			snprintf(xuid, xuid_cap, "%s", entry->d_name);
			const uint32_t size = be32(head + STFS_THUMB_SIZE);
			if (size > 8 && size <= STFS_THUMB_MAX && got >= STFS_THUMB + size && (*png = (uint8_t *)malloc(size))) {
				memcpy(*png, head + STFS_THUMB, size);
				*png_len = (int)size;
			}
			xsdk_log_write("eptx: perfil %s em %s, foto de %d bytes", gamertag, path, *png_len);
		}
		closedir(dir);
		if (found)
			return true;
	}
	xsdk_log_write("eptx: pacote do perfil %s nao encontrado", gamertag);
	return false;
}

/*
 * A foto do perfil vira o avatar da conta (PUT /api/v1/me/avatar), e os amigos a
 * veem em /avatar/<gamertag>/avatar-body.png. So vai quando muda: o hash da que ja
 * foi fica na sessao salva do perfil.
 */
static void upload_profile_pic(void)
{
	if (!g_profile_png || g_profile_png_len <= 0)
		return;
	const uint32_t hash = fnv1a(g_profile_png, g_profile_png_len);
	if (hash == g_saved_pic)
		return;
	char out[512];
	int len = 0;
	if (api_put_png("/api/v1/me/avatar", g_profile_png, g_profile_png_len, out, sizeof out, &len) != EPTX_OK) {
		say("Nao mandei a foto do perfil para o EPTX: %s", server_error());
		return;
	}
	g_saved_pic = hash;
	save_session();
	load_me();
	say("A foto do perfil %s agora e o seu avatar no EPTX.", g_profile_tag);
}

static void sign_in(const task *t);

/* Entra na conta EPTX do perfil logado: a sessao salva dele, ou o eptx.ini se for da mesma gamertag. */
static void profile_session(void)
{
	if (!g_profile_key[0]) {
		set_session(SESSION_SIGNED_OUT);
		return;
	}
	memset(g_client.token, 0, sizeof g_client.token);
	memset(g_client.secret, 0, sizeof g_client.secret);
	g_saved_pic = 0;
	char path[112];
	if (session_file(path, sizeof path, "") && read_ini(path, session_pair) && g_client.token[0] && g_client.secret[0]) {
		int len = 0;
		if (eptx_client_get(&g_client, "/api/v1/me", g_response, RESPONSE_CAP, &len) == EPTX_OK) {
			signed_in();
			upload_profile_pic();
			return;
		}
		xsdk_log_write("eptx: sessao salva do perfil recusada (%d), pedindo login", g_client.last_status);
		forget_session();
	}
	/* O user= do eptx.ini so entra sozinho no perfil da mesma gamertag, nunca em outro. */
	if (g_ini_user[0] && g_ini_pass[0] && strcasecmp(g_ini_user, g_profile_tag) == 0) {
		task t;
		memset(&t, 0, sizeof t);
		snprintf(t.gamertag, sizeof t.gamertag, "%s", g_ini_user);
		snprintf(t.password, sizeof t.password, "%s", g_ini_pass);
		sign_in(&t);
		return;
	}
	set_session(SESSION_SIGNED_OUT);
}

/* O perfil logado no Xbox mudou ("" sem ninguem). */
static void use_profile(const char *gamertag)
{
	if (strcmp(gamertag, g_profile_tag) == 0 && (g_profile_key[0] || !gamertag[0]))
		return;
	/* Solta a conta do perfil anterior; a sessao salva dele continua para a volta. */
	app_lock();
	const session_state before = g_app.session;
	app_unlock();
	if (before == SESSION_SIGNED_IN) {
		send_presence("offline");
		drop_account(false);
	}
	free(g_profile_png);
	g_profile_png = NULL;
	g_profile_png_len = 0;
	snprintf(g_profile_tag, sizeof g_profile_tag, "%.15s", gamertag); /* uma gamertag tem ate 15 */
	g_profile_key[0] = '\0';

	char xuid[17] = "";
	app_image image = { 0 };
	if (gamertag[0]) {
		find_profile(gamertag, xuid, sizeof xuid, &g_profile_png, &g_profile_png_len);
		if (xuid[0]) {
			snprintf(g_profile_key, sizeof g_profile_key, "%s", xuid);
		} else {
			/* Sem o pacote, a chave sai da gamertag: so letras e numeros, minusculas. */
			size_t w = (size_t)snprintf(g_profile_key, sizeof g_profile_key, "gt_");
			for (const char *p = gamertag; *p && w + 1 < sizeof g_profile_key; p++)
				if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
					g_profile_key[w++] = *p;
				else if (*p >= 'A' && *p <= 'Z')
					g_profile_key[w++] = (char)(*p - 'A' + 'a');
			g_profile_key[w] = '\0';
		}
		if (g_profile_png)
			image_decode_png(g_profile_png, (size_t)g_profile_png_len, &image);
	}

	app_lock();
	g_app.profile = gamertag[0] ? PROFILE_READY : PROFILE_NONE;
	snprintf(g_app.profile_gamertag, sizeof g_app.profile_gamertag, "%.15s", gamertag);
	snprintf(g_app.profile_xuid, sizeof g_app.profile_xuid, "%s", xuid);
	image_free(&g_app.profile_image);
	g_app.profile_image = image;
	g_app.profile_art = image.rgba ? ART_READY : ART_FAILED;
	g_app.profile_serial++;
	const session_state now_state = g_app.session;
	app_unlock();

	if (!gamertag[0]) {
		if (g_server_ready)
			set_session(SESSION_SIGNED_OUT);
		return;
	}
	/* Sem servidor ainda, start() entra na conta do perfil quando chegar nele. */
	if (g_server_ready && now_state != SESSION_NO_SERVER)
		profile_session();
}

/* --- a thread ------------------------------------------------------------------------- */

static void start(void)
{
	g_server_ready = false;
	pick_root();
	scan_library();

	bool configured = false;
	for (size_t i = 0; !configured && i < sizeof CONFIG_PATHS / sizeof CONFIG_PATHS[0]; i++)
		configured = read_ini(CONFIG_PATHS[i], config_pair);
	if (!configured || !g_host[0]) {
		set_session(SESSION_NO_SERVER);
		say("Falta Hdd1:\\EPTX\\eptx.ini com o IP do servidor (host=...).");
		return;
	}
	app_lock();
	snprintf(g_app.server, sizeof g_app.server, "%s:%d", g_host, g_port);
	app_unlock();

	if (eptx_http_init() != EPTX_HTTP_OK) {
		set_session(SESSION_NO_SERVER);
		say("A rede do console nao subiu. Tem cabo ou Wi-Fi?");
		return;
	}
	/*
	 * A rede do xsdk so sobe na primeira conexao, e o console leva uns segundos para
	 * ter IP (xsdk_net_address da EAGAIN ate la): conectar antes disso falha sem
	 * nenhum pacote sair. Espera o IP, ate 10 s, e mostra qual e, para conferir a rede.
	 */
	char console_ip[20] = "?";
#ifdef __XSDK__
	struct in_addr address;
	int net = EAGAIN;
	for (int i = 0; i < 40 && (net = xsdk_net_address(&address)) == EAGAIN; i++)
		pause_ms(250);
	if (net == 0) {
		const uint8_t *b = (const uint8_t *)&address.s_addr;
		snprintf(console_ip, sizeof console_ip, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	} else {
		set_session(SESSION_NO_SERVER);
		say("O console nao pegou IP (%s). Confira a rede e aperte Y.",
			net == EAGAIN ? "ainda esperando" : net == EADDRNOTAVAIL ? "sem endereco" : "rede desligada");
		return;
	}
	xsdk_log_write("eptx: IP do console %s, servidor %s:%d", console_ip, g_host, g_port);
#endif
	eptx_client_init(&g_client, g_host, g_port);
	/* Algumas tentativas: a primeira conexao logo depois de a rede subir as vezes se perde. */
	int synced = EPTX_ERR_HTTP;
	for (int attempt = 0; attempt < 3 && synced != EPTX_OK; attempt++) {
		if (attempt)
			pause_ms(1000);
		synced = eptx_client_sync_clock(&g_client);
		xsdk_log_write("eptx: tentativa %d em %s:%d: %s", attempt + 1, g_host, g_port,
			synced == EPTX_OK ? "ok" : g_client.last_error);
	}
	if (synced != EPTX_OK) {
		set_session(SESSION_NO_SERVER);
		say("Servidor %s:%d: %s. IP do console: %s. Y tenta de novo.", g_host, g_port,
			g_client.last_error[0] ? g_client.last_error : "nao respondeu", console_ip);
		return;
	}

	/* A conta e a do perfil do Xbox: entra nela se o perfil ja estiver logado. */
	g_server_ready = true;
	profile_session();
}

static void sign_in(const task *t)
{
	if (!g_profile_key[0]) {
		say("Entre num perfil do Xbox antes de entrar no EPTX.");
		return;
	}
	set_session(SESSION_SIGNING_IN);
	if (eptx_client_login(&g_client, t->gamertag, t->password, "XBOX360") != EPTX_OK) {
		set_session(SESSION_SIGNED_OUT);
		say("Nao entrou: %s", g_client.last_error);
		return;
	}
	/* Uma conta nova no perfil: a foto dele ainda nao foi para ela. */
	g_saved_pic = 0;
	save_session();
	say("Bem-vindo, %s. A conta fica salva no perfil %s.", g_client.gamertag, g_profile_tag);
	signed_in();
	upload_profile_pic();
}

static void run(const task *t)
{
	app_lock();
	const session_state session = g_app.session;
	app_unlock();
	const bool online = session == SESSION_SIGNED_IN;

	switch (t->type) {
	case TASK_START:
		start();
		break;
	case TASK_PROFILE:
		use_profile(t->text);
		break;
	case TASK_SIGN_IN:
		if (session != SESSION_NO_SERVER)
			sign_in(t);
		break;
	case TASK_SIGN_OUT:
		if (online)
			send_presence("offline");
		forget_session();
		say("Voce saiu do EPTX.");
		break;
	case TASK_REFRESH:
		/* Sem servidor, Atualizar e tentar conectar de novo. */
		if (session == SESSION_NO_SERVER) {
			set_session(SESSION_STARTING);
			start();
			break;
		}
		if (online) {
			load_me();
			load_catalog();
			load_friends();
			load_requests();
			load_rooms();
		}
		scan_library();
		break;
	case TASK_ART:
		if (online)
			load_store_art((int)t->arg);
		break;
	case TASK_LIBRARY_ART:
		load_library_art((int)t->arg);
		break;
	case TASK_FRIEND_AVATAR:
		if (online)
			load_friend_avatar((int)t->arg);
		break;
	case TASK_INSTALL:
#ifdef EPTX_PC
		(void)install;
		say("Na versao de PC a instalacao fica desligada: instale pelo console.");
#else
		if (online)
			install(t->arg);
#endif
		break;
	case TASK_SAVE_AVATAR:
		if (online) {
			save_avatar(t);
		} else {
			free(t->png);
			app_lock();
			g_app.avatar_saving = false;
			app_unlock();
		}
		break;
	case TASK_SEARCH:
		if (online)
			search(t);
		break;
	case TASK_LOOKUP:
		if (online)
			lookup(t);
		break;
	case TASK_SEND_REQUEST:
	case TASK_ACCEPT_REQUEST:
	case TASK_DECLINE_REQUEST:
	case TASK_REMOVE_FRIEND:
	case TASK_ROOM_CREATE:
	case TASK_ROOM_INVITE:
	case TASK_ROOM_JOIN:
	case TASK_ROOM_LEAVE:
		if (!online) {
			finish_action(false, "Entre no EPTX antes.");
			break;
		}
		if (t->type == TASK_SEND_REQUEST)
			send_request(t);
		else if (t->type == TASK_ACCEPT_REQUEST)
			accept_request(t->arg);
		else if (t->type == TASK_DECLINE_REQUEST)
			decline_request(t->arg);
		else if (t->type == TASK_REMOVE_FRIEND)
			remove_friend(t->arg);
		else if (t->type == TASK_ROOM_CREATE)
			room_create(t);
		else if (t->type == TASK_ROOM_INVITE)
			room_invite(t->arg);
		else if (t->type == TASK_ROOM_JOIN)
			room_join(t->arg);
		else
			room_leave();
		break;
	}
}

/*
 * Quanto a thread espera entre consultas: com um painel de amigos ou de sala aberto,
 * poucos segundos; numa sala, menos que os 60 s em que o servidor tira quem sumiu.
 */
static int poll_seconds(void)
{
	app_lock();
	const bool in_room = g_app.room.active;
	app_unlock();
	if (g_watch_fast)
		return WATCH_SECONDS;
	return in_room ? ROOM_SECONDS : HEARTBEAT_SECONDS;
}

static void *worker(void *unused)
{
	(void)unused;
	time_t last_heartbeat = time(NULL), last_poll = time(NULL);
	for (;;) {
		task t;
		bool have = false;
		const int wait = poll_seconds();
		pthread_mutex_lock(&g_queue_lock);
		if (!g_queue_count) {
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += wait;
			pthread_cond_timedwait(&g_queue_cond, &g_queue_lock, &until);
		}
		if (g_queue_count) {
			t = g_queue[g_queue_head];
			g_queue_head = (g_queue_head + 1) % QUEUE_SIZE;
			g_queue_count--;
			have = true;
		}
		pthread_mutex_unlock(&g_queue_lock);

		if (have)
			run(&t);

		app_lock();
		const bool online = g_app.session == SESSION_SIGNED_IN;
		app_unlock();
		if (online && time(NULL) - last_heartbeat >= HEARTBEAT_SECONDS) {
			last_heartbeat = last_poll = time(NULL);
			send_presence("online");
			load_friends();
			load_requests();
			load_rooms();
		} else if (online && time(NULL) - last_poll >= poll_seconds()) {
			/* Pedidos novos, amigos que entraram, quem chegou na sala: sem reiniciar a dash. */
			last_poll = time(NULL);
			if (g_watch_fast) {
				load_friends();
				load_requests();
			}
			load_rooms();
		}
	}
	return NULL;
}

void app_start(void)
{
	g_app.session = SESSION_STARTING;
	push_simple(TASK_START, 0);
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, NULL) != 0) {
		g_app.session = SESSION_NO_SERVER;
		snprintf(g_app.message, sizeof g_app.message, "Nao consegui criar a thread de rede.");
		return;
	}
	pthread_detach(thread);
}
