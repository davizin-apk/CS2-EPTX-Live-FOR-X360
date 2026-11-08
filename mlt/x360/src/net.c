#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eptx_client.h"
#include "eptx_http.h"
#include "eptx_json.h"
#include "plat.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesock close
#endif

static const char TITLE_ID[] = "42533336";
static const char TITLE_NAME[] = "Bot Strike 360";

// ---------------------------------------------------------------- estado dividido (sempre sob plat_net_lock)

static NetView view;
static char pack_hash[40];
static char want_detail[64] = "Menu";
// pedidos pendentes
static bool want_login, want_create, want_leave, want_state, want_state_playing;
static int64_t want_invite, want_join;
static char want_user[40], want_pass[64], want_mode[24], want_state_mode[24];
// recebidos
enum {
	RING = 64,
};
static uint8_t ring_data[RING][NET_MAX_PACKET];
static int ring_len[RING];
static uint8_t ring_from[RING];
static int ring_head, ring_tail;

// so da thread de HTTP
static eptx_client client;
static char host[64];
static int port = 8360;
static char eptx_dir[260];
static char response[48 * 1024];
static eptx_json_tok tokens[4096];

// UDP
static sock_t udp = BAD_SOCK;
static struct sockaddr_in relay;
static bool relay_known;

static void set_status(int status, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(int status, const char *fmt, ...)
{
	char text[160];
	va_list a;
	va_start(a, fmt);
	vsnprintf(text, sizeof text, fmt, a);
	va_end(a);
	plat_net_lock();
	view.status = status;
	snprintf(view.message, sizeof view.message, "%s", text);
	plat_net_unlock();
	plat_log("eptx: %s", text);
}

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = 0;
	size_t a = 0;
	while (s[a] == ' ' || s[a] == '\t')
		a++;
	memmove(s, s + a, n - a + 1);
}

static bool read_ini(const char *path, void (*pair)(const char *, const char *))
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		trim(line);
		char *v = eq + 1;
		trim(v);
		pair(line, v);
	}
	fclose(f);
	return true;
}

static void config_pair(const char *k, const char *v)
{
	if (!strcmp(k, "host"))
		snprintf(host, sizeof host, "%s", v);
	else if (!strcmp(k, "port"))
		port = atoi(v);
}

static void session_pair(const char *k, const char *v)
{
	if (!strcmp(k, "token"))
		snprintf(client.token, sizeof client.token, "%s", v);
	else if (!strcmp(k, "secret"))
		snprintf(client.secret, sizeof client.secret, "%s", v);
	else if (!strcmp(k, "gamertag"))
		snprintf(client.gamertag, sizeof client.gamertag, "%s", v);
}

// A sessao fica no mesmo session.ini da dash do EPTX: entrar aqui tambem entra la, e vice-versa.
static void save_session(void)
{
	char path[300], part[310];
	snprintf(path, sizeof path, "%s/session.ini", eptx_dir);
	snprintf(part, sizeof part, "%s.part", path);
	FILE *f = fopen(part, "w");
	if (!f)
		return;
	fprintf(f, "gamertag=%s\ntoken=%s\nsecret=%s\n", client.gamertag, client.token, client.secret);
	fclose(f);
	remove(path);
	rename(part, path);
}

// ---------------------------------------------------------------- API

static int parse(int len)
{
	return eptx_json_parse(response, (size_t)len, tokens, (int)(sizeof tokens / sizeof tokens[0]));
}

static bool refresh_me(void)
{
	int len = 0;
	const int r = eptx_client_get(&client, "/api/v1/me", response, sizeof response, &len);
	if (r != EPTX_OK) {
		plat_log("eptx: /api/v1/me recusou (%d, http %d): %s", r, client.last_status, client.last_error);
		return false;
	}
	const int n = parse(len);
	int64_t id = 0;
	char tag[40] = "";
	if (n > 0) {
		eptx_json_get_int(response, tokens, n, 0, "id", &id);
		eptx_json_get_string(response, tokens, n, 0, "gamertag", tag, sizeof tag);
	}
	plat_net_lock();
	view.user_id = id;
	snprintf(view.gamertag, sizeof view.gamertag, "%s", tag[0] ? tag : client.gamertag);
	plat_net_unlock();
	return true;
}

static void refresh_friends(void)
{
	int len = 0;
	if (eptx_client_get(&client, "/api/v1/friends", response, sizeof response, &len) != EPTX_OK)
		return;
	const int n = parse(len);
	const int items = n > 0 ? eptx_json_get(response, tokens, n, 0, "items") : -1;
	NetFriend list[NET_MAX_FRIENDS];
	int count = 0;
	for (int i = 0; items >= 0 && count < NET_MAX_FRIENDS; i++) {
		const int it = eptx_json_at(tokens, n, items, i);
		if (it < 0)
			break;
		NetFriend *f = &list[count++];
		memset(f, 0, sizeof *f);
		eptx_json_get_int(response, tokens, n, it, "userId", &f->user_id);
		eptx_json_get_string(response, tokens, n, it, "gamertag", f->gamertag, sizeof f->gamertag);
		const int p = eptx_json_get(response, tokens, n, it, "presence");
		if (p >= 0) {
			eptx_json_get_string(response, tokens, n, p, "state", f->state, sizeof f->state);
			eptx_json_get_string(response, tokens, n, p, "titleName", f->title, sizeof f->title);
			eptx_json_get_string(response, tokens, n, p, "detail", f->detail, sizeof f->detail);
		}
	}
	plat_net_lock();
	memcpy(view.friends, list, sizeof list[0] * (size_t)count);
	view.friend_count = count;
	plat_net_unlock();
}

static void refresh_invites(void)
{
	int len = 0;
	if (eptx_client_get(&client, "/api/v1/rooms/invites", response, sizeof response, &len) != EPTX_OK)
		return;
	const int n = parse(len);
	const int items = n > 0 ? eptx_json_get(response, tokens, n, 0, "items") : -1;
	NetInvite list[NET_MAX_INVITES];
	int count = 0;
	for (int i = 0; items >= 0 && count < NET_MAX_INVITES; i++) {
		const int it = eptx_json_at(tokens, n, items, i);
		if (it < 0)
			break;
		NetInvite *v = &list[count++];
		memset(v, 0, sizeof *v);
		eptx_json_get_int(response, tokens, n, it, "roomId", &v->room_id);
		eptx_json_get_string(response, tokens, n, it, "hostGamertag", v->host, sizeof v->host);
		eptx_json_get_string(response, tokens, n, it, "mode", v->mode, sizeof v->mode);
	}
	plat_net_lock();
	memcpy(view.invites, list, sizeof list[0] * (size_t)count);
	view.invite_count = count;
	plat_net_unlock();
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0));
}

// Le {"room": {...}} (ou null) da resposta.
static void take_room(int len)
{
	const int n = parse(len);
	const int r = n > 0 ? eptx_json_get(response, tokens, n, 0, "room") : -1;
	NetRoom room;
	memset(&room, 0, sizeof room);
	bool in = r >= 0 && tokens[r].type == EPTX_JSON_OBJECT;
	if (in) {
		int64_t v = 0;
		eptx_json_get_int(response, tokens, n, r, "id", &room.id);
		eptx_json_get_int(response, tokens, n, r, "hostId", &room.host_id);
		eptx_json_get_string(response, tokens, n, r, "hostGamertag", room.host_tag, sizeof room.host_tag);
		eptx_json_get_string(response, tokens, n, r, "mode", room.mode, sizeof room.mode);
		char state[16] = "";
		eptx_json_get_string(response, tokens, n, r, "state", state, sizeof state);
		room.playing = !strcmp(state, "playing");
		if (eptx_json_get_int(response, tokens, n, r, "matchId", &v) == 0)
			room.match_id = (int)v;
		if (eptx_json_get_int(response, tokens, n, r, "relayPort", &v) == 0)
			room.relay_port = (int)v;
		const int ms = eptx_json_get(response, tokens, n, r, "members");
		for (int i = 0; ms >= 0 && room.member_count < NET_MAX_MEMBERS; i++) {
			const int it = eptx_json_at(tokens, n, ms, i);
			if (it < 0)
				break;
			NetMember *m = &room.members[room.member_count++];
			eptx_json_get_int(response, tokens, n, it, "userId", &m->user_id);
			eptx_json_get_string(response, tokens, n, it, "gamertag", m->gamertag, sizeof m->gamertag);
			if (eptx_json_get_int(response, tokens, n, it, "slot", &v) == 0)
				m->slot = (int)v;
			int udp_ok = 0;
			eptx_json_get_bool(response, tokens, n, it, "udp", &udp_ok);
			m->udp = udp_ok != 0;
			eptx_json_get_string(response, tokens, n, it, "packHash", m->pack, sizeof m->pack);
		}
		const int you = eptx_json_get(response, tokens, n, r, "you");
		if (you >= 0 && tokens[you].type == EPTX_JSON_OBJECT) {
			if (eptx_json_get_int(response, tokens, n, you, "slot", &v) == 0)
				room.my_slot = (int)v;
			char key[24] = "";
			eptx_json_get_string(response, tokens, n, you, "key", key, sizeof key);
			for (int k = 0; k < 8 && key[k * 2] && key[k * 2 + 1]; k++)
				room.key[k] = (uint8_t)(hexval(key[k * 2]) << 4 | hexval(key[k * 2 + 1]));
		}
	}
	plat_net_lock();
	view.in_room = in;
	view.room = room;
	plat_net_unlock();
	if (in && room.relay_port) {
		memset(&relay, 0, sizeof relay);
		relay.sin_family = AF_INET;
		relay.sin_port = htons((uint16_t)room.relay_port);
		inet_pton(AF_INET, host, &relay.sin_addr);
		relay_known = true;
	}
}

static void refresh_room(void)
{
	int len = 0;
	if (eptx_client_get(&client, "/api/v1/rooms/current", response, sizeof response, &len) == EPTX_OK)
		take_room(len);
}

static void post(const char *path, const char *body, bool room_reply)
{
	int len = 0;
	const int r = eptx_client_post(&client, path, body, response, sizeof response, &len);
	if (r == EPTX_OK && room_reply)
		take_room(len);
	else if (r != EPTX_OK)
		plat_log("eptx: %s falhou (%d, http %d): %s", path, r, client.last_status, client.last_error);
}

static void send_presence(void)
{
	char body[256], detail[64];
	plat_net_lock();
	snprintf(detail, sizeof detail, "%s", want_detail);
	plat_net_unlock();
	snprintf(body, sizeof body, "{\"state\":\"online\",\"titleId\":\"%s\",\"titleName\":\"%s\",\"detail\":\"%s\"}", TITLE_ID,
		TITLE_NAME, detail);
	post("/api/v1/presence", body, false);
}

// ---------------------------------------------------------------- thread de HTTP

static bool connect_server(void)
{
	char ip[32];
	if (!plat_net_ready(ip, sizeof ip)) {
		set_status(NET_FAILED, "Sem rede (%s): jogando so local", ip);
		return false;
	}
	if (eptx_http_init() != EPTX_HTTP_OK) {
		set_status(NET_FAILED, "A rede nao subiu: jogando so local");
		return false;
	}
	eptx_client_init(&client, host, port);
	int synced = EPTX_ERR_HTTP;
	for (int i = 0; i < 3 && synced != EPTX_OK; i++) {
		synced = eptx_client_sync_clock(&client);
		if (synced != EPTX_OK)
			plat_sleep_ms(700);
	}
	if (synced != EPTX_OK) {
		set_status(NET_FAILED, "O servidor %s:%d nao respondeu: jogando so local", host, port);
		return false;
	}
	return true;
}

static void http_main(void *unused)
{
	(void)unused;
	int dirs_n = 0;
	const char *const *dirs = plat_eptx_dirs(&dirs_n);
	bool configured = false;
	for (int i = 0; i < dirs_n && !configured; i++) {
		char path[300];
		snprintf(path, sizeof path, "%s/eptx.ini", dirs[i]);
		if (read_ini(path, config_pair) && host[0]) {
			configured = true;
			snprintf(eptx_dir, sizeof eptx_dir, "%s", dirs[i]);
		}
	}
	if (!configured) {
		set_status(NET_OFF, "Sem EPTX (falta EPTX/eptx.ini): jogando so local");
		return;
	}
	plat_net_lock();
	snprintf(view.server, sizeof view.server, "%s:%d", host, port);
	plat_net_unlock();
	set_status(NET_CONNECTING, "Logando...");
	if (!connect_server())
		return;
	char path[300];
	snprintf(path, sizeof path, "%s/session.ini", eptx_dir);
	read_ini(path, session_pair);
	bool online = client.token[0] && refresh_me();
	if (online) {
		set_status(NET_ONLINE, "Online como %s", view.gamertag);
	} else {
		client.token[0] = 0;
#ifdef __XSDK__
		set_status(NET_FAILED, "Sem sessao: entre no EPTX pela dash e abra o jogo de novo");
		return;
#else
		set_status(NET_NEED_LOGIN, "Entre com a sua conta do EPTX");
#endif
	}
	double next_friends = 0, next_room = 0, next_presence = 0;
	for (;;) {
		const double t = plat_seconds();
		plat_net_lock();
		const bool login = want_login, create = want_create, leave = want_leave, state = want_state, playing = want_state_playing;
		const int64_t invite = want_invite, join = want_join;
		char user[40], pass[64], mode[24], smode[24];
		snprintf(user, sizeof user, "%s", want_user);
		snprintf(pass, sizeof pass, "%s", want_pass);
		snprintf(mode, sizeof mode, "%s", want_mode);
		snprintf(smode, sizeof smode, "%s", want_state_mode);
		want_login = want_create = want_leave = want_state = false;
		want_invite = want_join = 0;
		memset(want_pass, 0, sizeof want_pass);
		plat_net_unlock();
		if (login) {
			set_status(NET_CONNECTING, "Logando...");
			if (eptx_client_login(&client, user, pass, "botstrike-pc") == EPTX_OK && refresh_me()) {
				save_session();
				online = true;
				set_status(NET_ONLINE, "Online como %s", view.gamertag);
				next_friends = next_room = next_presence = 0;
			} else {
				set_status(NET_NEED_LOGIN, "Gamertag ou senha recusada (%d)", client.last_status);
			}
			memset(pass, 0, sizeof pass);
		}
		if (!online) {
			plat_sleep_ms(200);
			continue;
		}
		char body[160];
		int64_t room_id;
		plat_net_lock();
		room_id = view.in_room ? view.room.id : 0;
		plat_net_unlock();
		if (create) {
			snprintf(body, sizeof body, "{\"game\":\"botstrike\",\"mode\":\"%s\",\"packHash\":\"%s\"}", mode, pack_hash);
			post("/api/v1/rooms", body, true);
		}
		if (invite && room_id) {
			char p[64];
			snprintf(p, sizeof p, "/api/v1/rooms/%lld/invite", (long long)room_id);
			snprintf(body, sizeof body, "{\"userId\":%lld}", (long long)invite);
			post(p, body, false);
		}
		if (join) {
			char p[64];
			snprintf(p, sizeof p, "/api/v1/rooms/%lld/join", (long long)join);
			snprintf(body, sizeof body, "{\"packHash\":\"%s\"}", pack_hash);
			post(p, body, true);
		}
		if (leave && room_id) {
			char p[64];
			snprintf(p, sizeof p, "/api/v1/rooms/%lld/leave", (long long)room_id);
			post(p, "{}", false);
			plat_net_lock();
			view.in_room = false;
			plat_net_unlock();
		}
		if (state && room_id) {
			char p[64];
			snprintf(p, sizeof p, "/api/v1/rooms/%lld/settings", (long long)room_id);
			snprintf(body, sizeof body, "{\"mode\":\"%s\",\"state\":\"%s\"}", smode, playing ? "playing" : "lobby");
			post(p, body, true);
		}
		if (t >= next_friends) {
			refresh_friends();
			next_friends = t + 8;
		}
		if (t >= next_room) {
			refresh_room();
			refresh_invites();
			next_room = t + 2;
		}
		if (t >= next_presence) {
			send_presence();
			next_presence = t + 30;
		}
		plat_sleep_ms(100);
	}
}

// ---------------------------------------------------------------- UDP

enum {
	HEADER = 19,
};

static void put_header(uint8_t *p, uint32_t room, uint8_t slot, const uint8_t key[8], uint8_t type, uint8_t target)
{
	memcpy(p, "BSN1", 4);
	p[4] = (uint8_t)(room >> 24);
	p[5] = (uint8_t)(room >> 16);
	p[6] = (uint8_t)(room >> 8);
	p[7] = (uint8_t)room;
	p[8] = slot;
	memcpy(p + 9, key, 8);
	p[17] = type;
	p[18] = target;
}

static void raw_send(const uint8_t *p, int len)
{
	if (udp == BAD_SOCK || !relay_known)
		return;
	sendto(udp, (const char *)p, len, 0, (const struct sockaddr *)&relay, sizeof relay);
}

static void udp_main(void *unused)
{
	(void)unused;
	// espera a thread de HTTP achar o servidor e a porta do relay
	while (!relay_known && view.status != NET_OFF && view.status != NET_FAILED) {
		if (view.status == NET_ONLINE && host[0]) {
			memset(&relay, 0, sizeof relay);
			relay.sin_family = AF_INET;
			relay.sin_port = htons((uint16_t)(port + 1));
			inet_pton(AF_INET, host, &relay.sin_addr);
			relay_known = true;
			break;
		}
		plat_sleep_ms(200);
	}
	if (!relay_known)
		return;
	udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp == BAD_SOCK) {
		plat_log("eptx: socket UDP nao abriu");
		return;
	}
#if defined(_WIN32)
	DWORD tv = 100;
	setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
	struct timeval tv = { 0, 100000 };
	setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
	struct sockaddr_in any;
	memset(&any, 0, sizeof any);
	any.sin_family = AF_INET;
	bind(udp, (struct sockaddr *)&any, sizeof any);
	plat_log("eptx: UDP pronto, relay na porta %d", ntohs(relay.sin_port));
	double next_ping = 0, next_keep = 0;
	uint8_t buf[NET_MAX_PACKET + HEADER];
	for (;;) {
		const double t = plat_seconds();
		NetRoom room;
		bool in;
		plat_net_lock();
		room = view.room;
		in = view.in_room;
		plat_net_unlock();
		if (t >= next_ping) {
			// o teste de UDP: um ping ao servidor, sem sala, com a hora de saida
			uint8_t p[HEADER + 8];
			const uint8_t zero[8] = { 0 };
			put_header(p, 0, 0, zero, 9, 0);
			memcpy(p + HEADER, &t, 8);
			raw_send(p, sizeof p);
			plat_net_lock();
			view.udp_sent++;
			plat_net_unlock();
			next_ping = t + 2.0;
		}
		if (in && t >= next_keep) {
			// mantem o endereco do console na sala (e o NAT do roteador aberto)
			uint8_t p[HEADER];
			put_header(p, (uint32_t)room.id, (uint8_t)room.my_slot, room.key, 0, 0);
			raw_send(p, sizeof p);
			next_keep = t + 1.0;
		}
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		const int n = (int)recvfrom(udp, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < HEADER || memcmp(buf, "BSN1", 4))
			continue;
		if (buf[17] == 9) {
			double sent;
			memcpy(&sent, buf + HEADER, 8);
			plat_net_lock();
			view.udp_ms = (float)((plat_seconds() - sent) * 1000.0);
			view.udp_replies++;
			plat_net_unlock();
			continue;
		}
		if (buf[17] != 1 || n - HEADER > NET_MAX_PACKET)
			continue;
		plat_net_lock();
		const int next = (ring_head + 1) % RING;
		if (next != ring_tail) {
			memcpy(ring_data[ring_head], buf + HEADER, (size_t)(n - HEADER));
			ring_len[ring_head] = n - HEADER;
			ring_from[ring_head] = buf[8];
			ring_head = next;
		}
		plat_net_unlock();
	}
}

// ---------------------------------------------------------------- o jogo

void net_start(const char *hash)
{
	snprintf(pack_hash, sizeof pack_hash, "%s", hash);
	view.status = NET_CONNECTING;
	view.udp_ms = -1;
	snprintf(view.message, sizeof view.message, "Logando...");
	if (!plat_thread(http_main, NULL) || !plat_thread(udp_main, NULL))
		set_status(NET_FAILED, "Sem threads para a rede: jogando so local");
}

void net_view(NetView *out)
{
	plat_net_lock();
	*out = view;
	plat_net_unlock();
}

bool net_online(void)
{
	return view.status == NET_ONLINE;
}

void net_login(const char *user, const char *pass)
{
	plat_net_lock();
	snprintf(want_user, sizeof want_user, "%s", user);
	snprintf(want_pass, sizeof want_pass, "%s", pass);
	want_login = true;
	plat_net_unlock();
}

void net_presence(const char *detail)
{
	plat_net_lock();
	const bool changed = strcmp(want_detail, detail) != 0;
	snprintf(want_detail, sizeof want_detail, "%s", detail);
	plat_net_unlock();
	(void)changed;
}

void net_room_create(const char *mode)
{
	plat_net_lock();
	snprintf(want_mode, sizeof want_mode, "%s", mode);
	want_create = true;
	plat_net_unlock();
}

void net_room_invite(int64_t user_id)
{
	plat_net_lock();
	want_invite = user_id;
	plat_net_unlock();
}

void net_room_join(int64_t room_id)
{
	plat_net_lock();
	want_join = room_id;
	plat_net_unlock();
}

void net_room_leave(void)
{
	plat_net_lock();
	want_leave = true;
	plat_net_unlock();
}

void net_room_state(const char *mode, bool playing)
{
	plat_net_lock();
	snprintf(want_state_mode, sizeof want_state_mode, "%s", mode);
	want_state_playing = playing;
	want_state = true;
	plat_net_unlock();
}

bool net_send(uint8_t target, const void *data, int len)
{
	if (len > NET_MAX_PACKET)
		return false;
	NetRoom room;
	plat_net_lock();
	const bool in = view.in_room;
	room = view.room;
	plat_net_unlock();
	if (!in)
		return false;
	uint8_t p[HEADER + NET_MAX_PACKET];
	put_header(p, (uint32_t)room.id, (uint8_t)room.my_slot, room.key, 1, target);
	memcpy(p + HEADER, data, (size_t)len);
	raw_send(p, HEADER + len);
	return true;
}

int net_recv(uint8_t *from_slot, void *buf, int cap)
{
	int n = 0;
	plat_net_lock();
	if (ring_tail != ring_head) {
		n = ring_len[ring_tail] < cap ? ring_len[ring_tail] : cap;
		memcpy(buf, ring_data[ring_tail], (size_t)n);
		*from_slot = ring_from[ring_tail];
		ring_tail = (ring_tail + 1) % RING;
	}
	plat_net_unlock();
	return n;
}
