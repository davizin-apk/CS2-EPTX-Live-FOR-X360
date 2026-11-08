// O EPTX Live dentro do jogo: login (a sessao que a dash do EPTX ja salvou, ou o login no PC), amigos e presenca,
// salas e convites pela API assinada, e os pacotes da partida pelo relay UDP do servidor. Duas threads (HTTP e UDP)
// fazem o trabalho; o jogo so le um retrato (net_view) e pede coisas, sem nunca esperar a rede.
#ifndef BS_NET_H
#define BS_NET_H

#include <stdbool.h>
#include <stdint.h>

enum {
	NET_OFF, // sem eptx.ini: jogo so local
	NET_CONNECTING, // "Logando..."
	NET_NEED_LOGIN, // no PC: falta entrar com gamertag e senha
	NET_ONLINE,
	NET_FAILED, // sem servidor ou sessao recusada: jogo so local
};

enum {
	NET_MAX_FRIENDS = 32,
	NET_MAX_INVITES = 8,
	NET_MAX_MEMBERS = 4,
	NET_MAX_PACKET = 1380,
};

typedef struct {
	int64_t user_id;
	char gamertag[40];
	char state[12]; // online, away, offline
	char title[48];
	char detail[64];
} NetFriend;

typedef struct {
	int64_t room_id;
	char host[40];
	char mode[24];
} NetInvite;

typedef struct {
	int64_t user_id;
	char gamertag[40];
	int slot;
	bool udp;
	char pack[40];
} NetMember;

typedef struct {
	int64_t id, host_id;
	char host_tag[40];
	char mode[24];
	bool playing;
	int match_id;
	int member_count;
	NetMember members[NET_MAX_MEMBERS];
	int my_slot;
	uint8_t key[8];
	int relay_port;
} NetRoom;

typedef struct {
	int status;
	char message[160];
	char server[64];
	char gamertag[40];
	int64_t user_id;
	NetFriend friends[NET_MAX_FRIENDS];
	int friend_count;
	NetInvite invites[NET_MAX_INVITES];
	int invite_count;
	bool in_room;
	NetRoom room;
	float udp_ms; // ida e volta de um pacote UDP ate o servidor; < 0 sem resposta
	int udp_sent, udp_replies;
} NetView;

void net_start(const char *pack_hash);
void net_view(NetView *out);
bool net_online(void);
// Pedidos (a thread de HTTP faz em seguida).
void net_login(const char *gamertag, const char *password);
void net_presence(const char *detail);
void net_room_create(const char *mode);
void net_room_invite(int64_t user_id);
void net_room_join(int64_t room_id);
void net_room_leave(void);
void net_room_state(const char *mode, bool playing);
// Partida: cliente manda para o host (target ignorado); o host manda para um slot, ou 255 para todos.
bool net_send(uint8_t target, const void *data, int len);
// O proximo pacote recebido: devolve o tamanho (0 se nao ha) e o slot de quem mandou.
int net_recv(uint8_t *from_slot, void *buf, int cap);

#endif
