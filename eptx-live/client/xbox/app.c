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
static const char *const CONFIG_PATHS[] = { "eptx.ini", "pc-hdd/EPTX/eptx.ini" };
#else
/*
 * Instalado como XBLA, game: e o proprio pacote, so de leitura: o que a dash
 * guarda fica em Hdd1:\EPTX, onde da para trocar por FTP.
 */
/*
 * O EPTX mora no HD interno (hdd:) ou num HD USB (usb:), como o HD FAT32 que um
 * RGH usa pela porta USB: fica no disco que tiver a pasta EPTX (pick_root).
 */
static const char *g_root = "hdd:";
static const char *const CONFIG_PATHS[] = { "hdd:/EPTX/eptx.ini", "usb:/EPTX/eptx.ini", "game:/eptx.ini" };
#endif

/* O xsdk nao tem sleep/usleep; nanosleep existe nos dois lados. */
static void pause_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* O disco do EPTX: o que tem a pasta EPTX; sem ela, o HD interno se houver, senao o USB. */
static void pick_root(void)
{
#ifndef EPTX_PC
	struct stat st;
	if (stat("hdd:/EPTX", &st) == 0)
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
#define SESSION_REST "/EPTX/session.ini"
#define CONTENT_REST "/Content/0000000000000000"

#ifdef _WIN32
#define make_dir(path) mkdir(path)
#else
#define make_dir(path) mkdir(path, 0777)
#endif
/* "EPTX": a propria dash, que nao precisa aparecer na biblioteca dela. */
#define OWN_TITLE_ID "45505458"

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
} task_type;

typedef struct {
	task_type type;
	int64_t arg;
	char gamertag[40];
	char password[64];
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
void app_refresh(void) { push_simple(TASK_REFRESH, 0); }
void app_install(int64_t title_id) { push_simple(TASK_INSTALL, title_id); }

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

static void session_pair(const char *key, const char *value)
{
	if (strcmp(key, "token") == 0)
		snprintf(g_client.token, sizeof g_client.token, "%s", value);
	else if (strcmp(key, "secret") == 0)
		snprintf(g_client.secret, sizeof g_client.secret, "%s", value);
	else if (strcmp(key, "gamertag") == 0)
		snprintf(g_client.gamertag, sizeof g_client.gamertag, "%s", value);
}

/*
 * Guarda token e segredo, nao a senha: o servidor pode revogar a sessao, e quem
 * pegar o HD nao leva a senha que a pessoa talvez use em outro lugar.
 */
static void save_session(void)
{
	char dir[32], path[48], part[56];
	make_dir(g_root); /* so faz diferenca no PC, onde o "disco" e uma pasta */
	make_dir(on_root(dir, sizeof dir, "/EPTX"));
	on_root(path, sizeof path, SESSION_REST);
	FILE *fp = fopen(on_root(part, sizeof part, SESSION_REST ".part"), "w");
	if (!fp) {
		xsdk_log_write("eptx: nao gravei a sessao: errno %d", errno);
		return;
	}
	fprintf(fp, "gamertag=%s\ntoken=%s\nsecret=%s\n", g_client.gamertag, g_client.token, g_client.secret);
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

/* --- servidor ------------------------------------------------------------------ */

static void load_catalog(void)
{
	int len = 0;
	if (eptx_client_get(&g_client, "/api/v1/catalog?limit=200", g_response, RESPONSE_CAP, &len) != EPTX_OK) {
		say("Nao consegui abrir a loja: %s", g_client.last_error);
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
		if (eptx_client_get(&g_client, path, g_response, RESPONSE_CAP, &len) != EPTX_OK)
			return false; /* 404: a conta nao tem avatar */
		return image_decode_png((const uint8_t *)g_response, (size_t)len, image);
	}
	char *buffer = (char *)malloc(GIF_CAP);
	if (!buffer)
		return false;
	const bool ok = eptx_client_get(&g_client, path, buffer, GIF_CAP, &len) == EPTX_OK &&
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
	if (eptx_client_get(&g_client, "/api/v1/me", g_response, RESPONSE_CAP, &len) != EPTX_OK)
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
	if (eptx_client_get(&g_client, "/api/v1/friends", g_response, RESPONSE_CAP, &len) != EPTX_OK) {
		say("Nao consegui carregar os amigos: %s", g_client.last_error);
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
		"{\"state\":\"%s\",\"titleId\":\"" OWN_TITLE_ID "\",\"titleName\":\"EPTX Live\",\"detail\":\"Na dash\"}",
		state);
	if (eptx_client_post(&g_client, "/api/v1/presence", body, out, sizeof out, &len) != EPTX_OK)
		xsdk_log_write("eptx: presenca nao foi: %s", g_client.last_error);
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
}

static void forget_session(void)
{
	memset(g_client.token, 0, sizeof g_client.token);
	memset(g_client.secret, 0, sizeof g_client.secret);
	char path[48];
	unlink(on_root(path, sizeof path, SESSION_REST));
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
	app_unlock();
}

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
		ok = eptx_client_post(&g_client, "/api/v1/me/avatar/look", body, out, sizeof out, &len) == EPTX_OK;
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
			if (!is_hex8(entry->d_name) || strcasecmp(entry->d_name, OWN_TITLE_ID) == 0)
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
	if (eptx_client_get(&g_client, path, g_response, RESPONSE_CAP, &len) == EPTX_OK)
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
	if (eptx_client_get(&g_client, path, g_response, RESPONSE_CAP, &len) != EPTX_OK) {
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

/* --- a thread ------------------------------------------------------------------------- */

static void start(void)
{
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

	/* Sessao salva: se o servidor ainda aceita, entra direto, como a dash faz com o perfil. */
	char session_path[48];
	if (read_ini(on_root(session_path, sizeof session_path, SESSION_REST), session_pair) && g_client.token[0] &&
		g_client.secret[0]) {
		int len = 0;
		if (eptx_client_get(&g_client, "/api/v1/me", g_response, RESPONSE_CAP, &len) == EPTX_OK) {
			signed_in();
			return;
		}
		xsdk_log_write("eptx: sessao salva recusada (%d), pedindo login", g_client.last_status);
		forget_session();
	}
	if (g_ini_user[0] && g_ini_pass[0]) {
		app_sign_in(g_ini_user, g_ini_pass);
		return;
	}
	set_session(SESSION_SIGNED_OUT);
}

static void sign_in(const task *t)
{
	set_session(SESSION_SIGNING_IN);
	if (eptx_client_login(&g_client, t->gamertag, t->password, "XBOX360") != EPTX_OK) {
		set_session(SESSION_SIGNED_OUT);
		say("Nao entrou: %s", g_client.last_error);
		return;
	}
	save_session();
	say("Bem-vindo, %s.", g_client.gamertag);
	signed_in();
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
	}
}

static void *worker(void *unused)
{
	(void)unused;
	time_t last_heartbeat = time(NULL);
	for (;;) {
		task t;
		bool have = false;
		pthread_mutex_lock(&g_queue_lock);
		if (!g_queue_count) {
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += HEARTBEAT_SECONDS;
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
			last_heartbeat = time(NULL);
			send_presence("online");
			load_friends();
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
