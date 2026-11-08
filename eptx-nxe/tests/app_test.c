/*
 * Teste de integracao de app.c contra o servidor EPTX de verdade (uma instancia de
 * teste, com banco proprio; veja run-integration.ps1). O mesmo app.c da dash, na
 * versao de PC (EPTX_PC): sessao em pc-hdd/EPTX-NXE, servidor do eptx.ini da pasta.
 *
 *   app_test <passo>
 *
 * Cada passo roda como uma conta, numa pasta propria, e confere o que o servidor
 * respondeu atraves de g_app, como a tela faria.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "app.h"

static int g_failures;

/* O pacote de perfil falso que o script poe no "HD" de alice (pc-hdd/Content/<XUID>/...). */
#define ALICE_XUID "E000000000000A11"
#define ALICE_SESSION "pc-hdd/EPTX-NXE/profiles/" ALICE_XUID ".ini"

static void pause_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* Espera expr ficar verdadeira (lida sob o lock), ate seconds segundos. */
#define WAIT(expr, seconds)                                                                                            \
	do {                                                                                                               \
		for (int i_ = 0; i_ < (seconds) * 20; i_++) {                                                                  \
			app_lock();                                                                                                \
			const int ok_ = (expr);                                                                                    \
			app_unlock();                                                                                              \
			if (ok_)                                                                                                   \
				break;                                                                                                 \
			pause_ms(50);                                                                                              \
		}                                                                                                              \
	} while (0)

#define CHECK(expr, ...)                                                                                               \
	do {                                                                                                               \
		app_lock();                                                                                                    \
		const int ok_ = (expr);                                                                                        \
		app_unlock();                                                                                                  \
		printf("%s  ", ok_ ? "ok  " : "FALHOU");                                                                       \
		printf(__VA_ARGS__);                                                                                           \
		printf("\n");                                                                                                  \
		if (!ok_)                                                                                                      \
			g_failures++;                                                                                              \
	} while (0)

/* Espera a acao do painel terminar (o servidor respondeu). */
static action_state g_last;

static action_state finish(uint32_t before)
{
	WAIT(g_app.action_serial != before && g_app.action != ACTION_BUSY, 15);
	app_lock();
	const action_state s = g_app.action;
	printf("      servidor: \"%s\"\n", g_app.action_message);
	app_unlock();
	g_last = s;
	return s;
}

static uint32_t serial(void)
{
	app_lock();
	const uint32_t s = g_app.action_serial;
	app_unlock();
	return s;
}

static int find_request_in(const char *gamertag)
{
	for (int i = 0; i < g_app.requests_in_count; i++)
		if (strcmp(g_app.requests_in[i].gamertag, gamertag) == 0)
			return i;
	return -1;
}

static int find_friend(const char *gamertag)
{
	for (int i = 0; i < g_app.friend_count; i++)
		if (strcmp(g_app.friends[i].gamertag, gamertag) == 0)
			return i;
	return -1;
}

/* O perfil do Xbox "logado": o passo roda como a conta de mesmo nome (alice-send: alice). */
static char g_profile[16];

static void sign_in(void)
{
	app_start();
	app_set_profile(g_profile);
	WAIT(g_app.session == SESSION_SIGNED_IN || g_app.session == SESSION_NO_SERVER, 20);
	CHECK(g_app.session == SESSION_SIGNED_IN, "entrou como %s (servidor %s)", g_app.gamertag, g_app.server);
	CHECK(g_app.server_ok, "indicador de servidor aceso");
	WAIT(g_app.friends_loaded && g_app.requests_loaded, 10);
}

static void step_alice_send(void)
{
	app_lookup_gamertag("bob");
	WAIT(g_app.lookup != LOOKUP_LOADING, 10);
	CHECK(g_app.lookup == LOOKUP_FOUND && g_app.lookup_image.width > 0, "busca de \"bob\" achou a conta e o avatar (%dx%d)",
		g_app.lookup_image.width, g_app.lookup_image.height);

	app_lookup_gamertag("ninguem-com-esse-nome");
	WAIT(g_app.lookup != LOOKUP_LOADING, 10);
	CHECK(g_app.lookup == LOOKUP_NOT_FOUND, "busca de gamertag inexistente diz que nao existe");

	uint32_t s = serial();
	app_send_friend_request("bob");
	finish(s);
	CHECK(g_last == ACTION_OK, "pedido para bob confirmado pelo servidor");

	s = serial();
	app_send_friend_request("bob");
	finish(s);
	CHECK(g_last == ACTION_FAILED && strstr(g_app.action_message, "ja mandou"), "pedido repetido recusado");

	s = serial();
	app_send_friend_request("alice");
	finish(s);
	CHECK(g_last == ACTION_FAILED && strstr(g_app.action_message, "propria conta"), "pedido para si mesma recusado");

	s = serial();
	app_send_friend_request("ninguem-com-esse-nome");
	finish(s);
	CHECK(g_last == ACTION_FAILED && strstr(g_app.action_message, "nao existe"), "pedido para gamertag inexistente");

	s = serial();
	app_send_friend_request("carol");
	finish(s);
	CHECK(g_last == ACTION_OK, "pedido para carol confirmado");
	CHECK(g_app.requests_out_count == 2, "dois pedidos enviados pendentes (%d)", g_app.requests_out_count);
}

static void step_bob_accept(void)
{
	WAIT(find_request_in("alice") >= 0, 10);
	CHECK(find_request_in("alice") >= 0, "pedido de alice chegou (%d recebido[s])", g_app.requests_in_count);
	app_lock();
	const int i = find_request_in("alice");
	const int64_t id = i >= 0 ? g_app.requests_in[i].id : 0;
	app_unlock();
	const uint32_t s = serial();
	app_accept_request(id);
	finish(s);
	CHECK(g_last == ACTION_OK, "aceitou o pedido");
	CHECK(g_app.requests_in_count == 0, "a lista de pedidos atualizou depois da confirmacao");
	CHECK(find_friend("alice") >= 0, "alice entrou na lista de amigos");
	app_lock();
	const int f = find_friend("alice");
	if (f >= 0)
		printf("      alice: amizade %lld, usuario %lld, %s\n", (long long)g_app.friends[f].friendship_id,
			(long long)g_app.friends[f].user_id, g_app.friends[f].online ? "online" : "offline");
	app_unlock();
	CHECK(f >= 0 && g_app.friends[f].friendship_id > 0 && g_app.friends[f].user_id > 0, "ids da amizade vieram");

	const uint32_t s2 = serial();
	app_accept_request(id);
	finish(s2);
	CHECK(g_last == ACTION_FAILED, "aceitar de novo e recusado pelo servidor");
}

static void step_carol_decline(void)
{
	WAIT(find_request_in("alice") >= 0, 10);
	app_lock();
	const int i = find_request_in("alice");
	const int64_t id = i >= 0 ? g_app.requests_in[i].id : 0;
	app_unlock();
	CHECK(id > 0, "pedido de alice chegou para carol");
	const uint32_t s = serial();
	app_decline_request(id);
	finish(s);
	CHECK(g_last == ACTION_OK, "recusou o pedido");
	CHECK(g_app.requests_in_count == 0 && g_app.friend_count == 0, "sem pedidos e sem amigos depois de recusar");
}

static void step_alice_room(void)
{
	CHECK(find_friend("bob") >= 0 && find_friend("carol") < 0, "alice tem bob como amigo e nao carol");
	CHECK(g_app.requests_out_count == 0, "nenhum pedido enviado pendente (%d)", g_app.requests_out_count);

	uint32_t s = serial();
	app_room_create("botstrike", "classic");
	finish(s);
	CHECK(g_last == ACTION_OK, "sala criada");
	CHECK(g_app.room.active && g_app.room.is_host && g_app.room.member_count == 1, "alice e host, sozinha na sala %lld",
		(long long)g_app.room.id);

	app_lock();
	const int f = find_friend("bob");
	const int64_t bob = f >= 0 ? g_app.friends[f].user_id : 0;
	app_unlock();
	s = serial();
	app_room_invite(bob);
	finish(s);
	CHECK(g_last == ACTION_OK, "convite para bob enviado");

	s = serial();
	app_room_invite(999999);
	finish(s);
	CHECK(g_last == ACTION_FAILED && strstr(g_app.action_message, "amigos"), "convidar quem nao e amigo e recusado");
}

static void step_bob_join(void)
{
	WAIT(g_app.invite_count > 0, 10);
	CHECK(g_app.invite_count == 1 && strcmp(g_app.invites[0].host, "alice") == 0, "convite de alice chegou (%s, %s)",
		g_app.invites[0].game, g_app.invites[0].mode);
	app_lock();
	const int64_t room = g_app.invites[0].room_id;
	app_unlock();
	uint32_t s = serial();
	app_room_join(room);
	finish(s);
	CHECK(g_last == ACTION_OK, "entrou na sala");
	CHECK(g_app.room.active && !g_app.room.is_host && g_app.room.member_count == 2, "bob na sala de %s com %d jogadores",
		g_app.room.host, g_app.room.member_count);
	s = serial();
	app_room_leave();
	finish(s);
	CHECK(g_last == ACTION_OK, "saiu da sala");
	CHECK(!g_app.room.active, "sem sala depois de sair");
}

static void step_alice_cleanup(void)
{
	WAIT(g_app.room.active, 10);
	CHECK(g_app.room.active && g_app.room.member_count == 1, "bob ja saiu da sala de alice (%d jogador[es])",
		g_app.room.member_count);
	uint32_t s = serial();
	app_room_leave();
	finish(s);
	CHECK(g_last == ACTION_OK && !g_app.room.active, "alice fechou a sala");

	app_lock();
	const int f = find_friend("bob");
	const int64_t friendship = f >= 0 ? g_app.friends[f].friendship_id : 0;
	app_unlock();
	s = serial();
	app_remove_friend(friendship);
	finish(s);
	CHECK(g_last == ACTION_OK, "removeu bob");
	CHECK(find_friend("bob") < 0, "bob saiu da lista sem reiniciar");

	WAIT(g_app.store_loaded, 10);
	CHECK(g_app.store_count >= 2, "loja carregada com %d titulo(s)", g_app.store_count);
	app_search_store("Homebrew");
	WAIT(!g_app.search_loading, 10);
	CHECK(g_app.search_count >= 1 && g_app.search_count < g_app.store_count, "pesquisa \"Homebrew\": %d de %d",
		g_app.search_count, g_app.store_count);
	app_search_store("nada-com-esse-nome");
	WAIT(!g_app.search_loading, 10);
	CHECK(g_app.search_count == 0, "pesquisa sem resultado devolve zero");
}

static void step_alice_expire(void)
{
	/* O servidor revoga a sessao por fora (o script apaga as sessoes do banco de teste). */
	const char *cmd = getenv("EXPIRE_CMD");
	const int deleted = cmd ? system(cmd) : -1;
	CHECK(deleted == 0, "sessoes apagadas no banco de teste");
	app_refresh();
	WAIT(g_app.session == SESSION_SIGNED_OUT, 15);
	CHECK(g_app.session == SESSION_SIGNED_OUT, "401 voltou a dash para entrar");
	CHECK(strstr(g_app.message, "expirou") != NULL, "mensagem: \"%s\"", g_app.message);
	struct stat st;
	CHECK(stat(ALICE_SESSION, &st) != 0, "a sessao salva no perfil de alice foi apagada");
}

/*
 * Os perfis: a sessao de alice fica no arquivo do perfil dela (o XUID da pasta), outro
 * perfil sem conta pede login sem apagar a dela, e a volta entra sem login.
 */
static void step_alice_profiles(void)
{
	struct stat st;
	CHECK(strcmp(g_app.profile_xuid, ALICE_XUID) == 0 && g_app.profile_art == ART_READY,
		"pacote do perfil achado: XUID %s, foto %dx%d", g_app.profile_xuid, g_app.profile_image.width,
		g_app.profile_image.height);
	CHECK(stat(ALICE_SESSION, &st) == 0, "a sessao de alice esta salva no perfil dela");

	app_set_profile("Visitante");
	WAIT(g_app.session == SESSION_SIGNED_OUT && strcmp(g_app.profile_gamertag, "Visitante") == 0, 15);
	CHECK(g_app.session == SESSION_SIGNED_OUT && g_app.friend_count == 0, "outro perfil, sem conta: pede login");
	CHECK(g_app.profile_xuid[0] == '\0', "o perfil Visitante nao tem pacote no disco");
	CHECK(stat(ALICE_SESSION, &st) == 0, "trocar de perfil nao apagou a sessao de alice");

	app_lock();
	g_app.message[0] = '\0';
	app_unlock();
	app_set_profile("alice");
	WAIT(g_app.session == SESSION_SIGNED_IN, 15);
	CHECK(g_app.session == SESSION_SIGNED_IN && strcmp(g_app.gamertag, "alice") == 0,
		"de volta ao perfil de alice, a conta dela volta");
	CHECK(strstr(g_app.message, "Bem-vindo") == NULL && strstr(g_app.message, "foto") == NULL,
		"sem login de novo e sem reenviar a mesma foto (\"%s\")", g_app.message);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char *step = argc > 1 ? argv[1] : "";
	printf("== %s\n", step);
	const char *dash = strchr(step, '-');
	snprintf(g_profile, sizeof g_profile, "%.*s", dash ? (int)(dash - step) : (int)strlen(step), step);
	sign_in();
	if (g_app.session != SESSION_SIGNED_IN)
		return 1;
	if (strcmp(step, "alice-send") == 0)
		step_alice_send();
	else if (strcmp(step, "bob-accept") == 0)
		step_bob_accept();
	else if (strcmp(step, "carol-decline") == 0)
		step_carol_decline();
	else if (strcmp(step, "alice-room") == 0)
		step_alice_room();
	else if (strcmp(step, "bob-join") == 0)
		step_bob_join();
	else if (strcmp(step, "alice-cleanup") == 0)
		step_alice_cleanup();
	else if (strcmp(step, "alice-profiles") == 0)
		step_alice_profiles();
	else if (strcmp(step, "alice-expire") == 0)
		step_alice_expire();
	else {
		fprintf(stderr, "passo desconhecido: %s\n", step);
		return 2;
	}
	printf("%s\n", g_failures ? "-- com falhas" : "-- tudo certo");
	fflush(stdout);
	_exit(g_failures ? 1 : 0); /* a thread de rede fica presa no wait; sai sem esperar */
}
