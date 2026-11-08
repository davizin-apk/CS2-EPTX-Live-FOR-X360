// A partida em rede: o host roda tudo (regras, bots, tiros) e manda o estado; os clientes mandam o proprio movimento
// e as escolhas. Todos os numeros vao big-endian, escritos byte a byte, entao o 360 e o PC se entendem.
//
// Pacotes (o primeiro byte diz o tipo):
//   'C' cliente -> host, 30 por segundo: corpo (posicao, velocidade, mira), botoes seguros, escolhas ainda sem
//       confirmacao (cada uma com numero, para chegar uma vez so mesmo com pacote perdido)
//   'S' host -> cada cliente, 20 por segundo: a rodada, os 10 lutadores, o que e so dele (armas, dinheiro), e os
//       eventos que ele ainda nao confirmou (tiros, impactos, sons, mortes, faixas)
//   'N' host -> todos, 1 por segundo: os nomes
//   'W' host -> todos, 10 por segundo: um pedaco da tabela de pecas de construcao (a tabela inteira em ~1 s)
//   'P' host -> todos, 6 por segundo: os portais
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "net.h"

void shells_update_public(float dt);

static const char *const SOUNDS[] = { "snd_knife", "snd_glock", "snd_usp", "snd_ak47", "snd_m4a1s", "snd_awp", "snd_c4_beep",
	"snd_c4_explode", "snd_c4_disarm", "mus_start", "mus_won", "mus_lost", "mus_planted", "mus_menu", "amb_wind" };
static const char *const WHYS[] = { "Target saved", "Counter-Terrorists eliminated", "Terrorists eliminated", "Bomb defused",
	"Target bombed" };
static const char *const NOTICES[] = { "THE BOMB HAS BEEN PLANTED", "HALFTIME - SWITCHING SIDES" };
static const float PCOLORS[4][4] = { { 0.5f, 0.05f, 0.03f, 1 }, { 0.75f, 0.65f, 0.5f, 1 }, { 0.6f, 0.42f, 0.22f, 1 },
	{ 0.6f, 0.42f, 0.22f, 1 } };

enum {
	EV_TRACER = 1,
	EV_IMPACT,
	EV_PARTICLES,
	EV_SOUND,
	EV_UI_SOUND,
	EV_MUSIC,
	EV_FEED,
	EV_ROUND_OVER,
	EV_MATCH_OVER,
	EV_NOTICE,
	EV_ROUND_START,
};

enum {
	CH_SLOT = 1,
	CH_NEXT,
	CH_PREV,
	CH_RELOAD,
	CH_BUILD,
	CH_BUILD_CYCLE,
	CH_MATERIAL,
	CH_EDIT,
	CH_DROP,
	CH_ALT,
	CH_BUY,
	CH_ORANGE,
};

typedef struct {
	uint32_t seq;
	uint8_t type, a, b, c;
	float p[3], q[3];
} Event;

typedef struct {
	uint16_t seq;
	uint8_t type, arg;
	float v[4];
} Choice;

// ---------------------------------------------------------------- escrita e leitura big-endian

typedef struct {
	uint8_t *p;
	int n, cap;
} W;
static void w8(W *w, uint32_t v)
{
	if (w->n < w->cap)
		w->p[w->n++] = (uint8_t)v;
}
static void w16(W *w, uint32_t v)
{
	w8(w, v >> 8);
	w8(w, v);
}
static void w32(W *w, uint32_t v)
{
	w16(w, v >> 16);
	w16(w, v);
}
static void wf(W *w, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	w32(w, u);
}
static void wstr(W *w, const char *s, int max)
{
	int n = (int)strlen(s);
	if (n > max)
		n = max;
	w8(w, (uint32_t)n);
	for (int i = 0; i < n; i++)
		w8(w, (uint8_t)s[i]);
}

typedef struct {
	const uint8_t *p;
	int n, at;
	bool bad;
} R;
static uint32_t r8(R *r)
{
	if (r->at >= r->n) {
		r->bad = true;
		return 0;
	}
	return r->p[r->at++];
}
static uint32_t r16(R *r)
{
	const uint32_t a = r8(r);
	return a << 8 | r8(r);
}
static uint32_t r32(R *r)
{
	const uint32_t a = r16(r);
	return a << 16 | r16(r);
}
static float rf(R *r)
{
	const uint32_t u = r32(r);
	float f;
	memcpy(&f, &u, 4);
	return isfinite(f) ? f : 0.0f;
}
static void rstr(R *r, char *out, int cap)
{
	const int n = (int)r8(r);
	int k = 0;
	for (int i = 0; i < n; i++) {
		const char c = (char)r8(r);
		if (k < cap - 1 && c >= 32)
			out[k++] = c;
	}
	out[k] = 0;
}

static int sound_index(const char *id)
{
	for (int i = 0; i < (int)(sizeof SOUNDS / sizeof SOUNDS[0]); i++)
		if (!strcmp(SOUNDS[i], id))
			return i;
	return -1;
}

// ---------------------------------------------------------------- host

enum {
	EVENTS = 256,
	MAX_EV_PER_PACKET = 14,
};

typedef struct {
	bool used;
	Vec3 pos, vel;
	float yaw, pitch;
	bool ground, crouch, fire, use, walk;
	uint8_t epoch;
	uint16_t last_choice; // a ultima escolha aplicada
	uint32_t ev_ack; // o ultimo evento que ele confirmou
	double last_at;
	bool prev_fire;
} Remote;

static Event events[EVENTS];
static uint32_t ev_seq;
static Remote remotes[NET_MAX_MEMBERS];
static double next_state, next_names, next_world, next_portals;
static int world_cursor;
static uint32_t snap_seq;

static void ev_push(const Event *e)
{
	if (G.net_role != NET_ROLE_HOST)
		return;
	Event *d = &events[++ev_seq % EVENTS];
	*d = *e;
	d->seq = ev_seq;
}

void netev_tracer(Vec3 a, Vec3 b)
{
	ev_push(&(Event){ 0, EV_TRACER, 0, 0, 0, { a.x, a.y, a.z }, { b.x, b.y, b.z } });
}

void netev_impact(Vec3 p, Vec3 n, bool wood)
{
	ev_push(&(Event){ 0, EV_IMPACT, wood, 0, 0, { p.x, p.y, p.z }, { n.x, n.y, n.z } });
}

void netev_particles(Vec3 p, const float c[4], int n)
{
	int k = 1;
	if (c[0] < 0.55f && c[1] < 0.1f)
		k = 0;
	else if (c[1] < 0.5f)
		k = 2;
	ev_push(&(Event){ 0, EV_PARTICLES, (uint8_t)k, (uint8_t)n, 0, { p.x, p.y, p.z }, { 0, 0, 0 } });
}

void netev_sound(const char *id, Vec3 p, float vol, bool ui)
{
	const int i = sound_index(id);
	if (i >= 0)
		ev_push(&(Event){ 0, ui ? EV_UI_SOUND : EV_SOUND, (uint8_t)i, 0, 0, { p.x, p.y, p.z }, { vol, 0, 0 } });
}

void netev_music(const char *id, float vol)
{
	const int i = sound_index(id);
	if (i >= 0)
		ev_push(&(Event){ 0, EV_MUSIC, (uint8_t)i, 0, 0, { 0, 0, 0 }, { vol, 0, 0 } });
}

void netev_feed(Fighter *k, Fighter *v, int weapon, bool hs)
{
	ev_push(&(Event){ 0, EV_FEED, (uint8_t)(k ? k->index : 255), (uint8_t)v->index, (uint8_t)weapon, { 0, 0, 0 },
		{ hs ? 1.0f : 0.0f, 0, 0 } });
}

// kind 0: fim de rodada (winner, why); 1: fim de partida (winner ou -1); 2: aviso (why = texto)
void netev_banner(int kind, int winner, const char *why)
{
	if (kind == 0) {
		int w = 0;
		for (int i = 0; i < (int)(sizeof WHYS / sizeof WHYS[0]); i++)
			if (!strcmp(WHYS[i], why))
				w = i;
		ev_push(&(Event){ 0, EV_ROUND_OVER, (uint8_t)winner, (uint8_t)w, 0, { 0 }, { 0 } });
	} else if (kind == 1) {
		ev_push(&(Event){ 0, EV_MATCH_OVER, (uint8_t)(winner + 1), 0, 0, { 0 }, { 0 } });
	} else if (kind == 2) {
		for (int i = 0; i < (int)(sizeof NOTICES / sizeof NOTICES[0]); i++)
			if (!strcmp(NOTICES[i], why))
				ev_push(&(Event){ 0, EV_NOTICE, (uint8_t)i, 0, 0, { 0 }, { 0 } });
	} else {
		ev_push(&(Event){ 0, EV_ROUND_START, 0, 0, 0, { 0 }, { 0 } });
	}
}

void netgame_host_begin(void)
{
	NetView v;
	net_view(&v);
	G.net_role = NET_ROLE_HOST;
	memset(remotes, 0, sizeof remotes);
	memset(events, 0, sizeof events);
	ev_seq = 0;
	next_state = next_names = next_world = next_portals = 0;
	for (int i = 0; i < G.m.count; i++)
		G.m.f[i].remote = -1;
	// cada outro membro da sala ocupa um lugar de bot do time do host
	const int team = G.m.player >= 0 ? G.m.f[G.m.player].team : TEAM_CT;
	for (int k = 0; k < v.room.member_count; k++) {
		const NetMember *mb = &v.room.members[k];
		if (mb->user_id == v.user_id)
			continue;
		for (int i = 0; i < G.m.count; i++) {
			Fighter *f = &G.m.f[i];
			if (f->team == team && f->is_bot && f->remote < 0) {
				f->is_bot = false;
				f->remote = mb->slot;
				// o bot deste lugar ja tinha comprado com o dinheiro dele: o jogador comeca do zero
				f->money = MODES[G.m.mode].start_money;
				memset(f->has, 0, sizeof f->has);
				fighter_reset_loadout(f);
				snprintf(f->name, sizeof f->name, "%s", mb->gamertag);
				remotes[mb->slot & 3].used = true;
				plat_log("rede: %s joga no lugar %d (slot %d)", mb->gamertag, i, mb->slot);
				break;
			}
		}
	}
}

static Fighter *fighter_of_slot(int slot)
{
	for (int i = 0; i < G.m.count; i++)
		if (G.m.f[i].remote == slot)
			return &G.m.f[i];
	return NULL;
}

static void host_choice(Fighter *f, const Choice *c)
{
	Actions a;
	memset(&a, 0, sizeof a);
	a.build_select = -1;
	switch (c->type) {
	case CH_SLOT:
		a.slot_pressed = c->arg;
		break;
	case CH_NEXT:
		a.next_weapon = true;
		break;
	case CH_PREV:
		a.prev_weapon = true;
		break;
	case CH_RELOAD:
		a.reload = true;
		break;
	case CH_BUILD:
		a.build_select = c->arg < BUILD_PIECES_COUNT ? c->arg : -1;
		break;
	case CH_BUILD_CYCLE:
		a.build_cycle = true;
		break;
	case CH_MATERIAL:
		a.material_cycle = true;
		break;
	case CH_EDIT:
		a.edit_pressed = true;
		break;
	case CH_DROP:
		a.drop_pressed = true;
		break;
	case CH_ALT:
		a.alt_pressed = true;
		break;
	case CH_BUY:
		if (match_buy_allowed(f)) {
			const char *id = c->arg < WEAPONS_COUNT ? WEAPONS[c->arg].id
				: (c->arg >= 100 && c->arg - 100 < EQUIPMENT_COUNT ? EQUIPMENT[c->arg - 100].id : NULL);
			if (id)
				fighter_buy(f, id);
		}
		return;
	case CH_ORANGE:
		if (f->has[4] && f->guns[4].id == WEAPONS_PORTALGUN)
			portal_place_orange(f, v3(c->v[0], c->v[1], c->v[2]), c->v[3]);
		return;
	default:
		return;
	}
	player_remote_choice(f, &a);
}

static void host_receive(const uint8_t *buf, int n, uint8_t slot)
{
	R r = { buf, n, 0, false };
	if (r8(&r) != 'C')
		return;
	Remote *rm = &remotes[slot & 3];
	Fighter *f = fighter_of_slot(slot);
	if (!f)
		return;
	r32(&r); // numero do pacote
	const uint16_t ev_ack = (uint16_t)r16(&r);
	const uint8_t epoch = (uint8_t)r8(&r);
	Vec3 pos = v3(rf(&r), rf(&r), rf(&r));
	Vec3 vel = v3(rf(&r), rf(&r), rf(&r));
	const float yaw = rf(&r), pitch = rf(&r);
	const uint8_t flags = (uint8_t)r8(&r);
	const int nc = (int)r8(&r);
	Choice ch[16];
	int k = 0;
	for (int i = 0; i < nc && i < 16; i++) {
		ch[k].seq = (uint16_t)r16(&r);
		ch[k].type = (uint8_t)r8(&r);
		ch[k].arg = (uint8_t)r8(&r);
		for (int j = 0; j < 4; j++)
			ch[k].v[j] = rf(&r);
		k++;
	}
	if (r.bad)
		return;
	// o ack de eventos vem em 16 bits: estende para o numero inteiro mais perto
	const uint32_t base = ev_seq & ~0xFFFFu;
	uint32_t full = base | ev_ack;
	if (full > ev_seq)
		full -= 0x10000;
	if (full > rm->ev_ack || rm->ev_ack > ev_seq)
		rm->ev_ack = full;
	rm->last_at = G.time;
	// o corpo: so se ele ja esta onde o host o pos por ultimo (nascer, portal)
	if (epoch == f->net_epoch && f->alive) {
		const float jump = v3_length(v3_sub(pos, f->body.position));
		if (jump < 6.0f || !rm->used) {
			f->body.position = pos;
			f->body.velocity = vel;
		}
		f->body.yaw = yaw;
		f->body.pitch = clampf(pitch, -1.553f, 1.553f);
		f->body.on_ground = flags & 1;
		f->body.crouching = (flags & 2) != 0;
		f->body.height = f->body.crouching ? 1.3f : 1.83f;
		f->body.eye = f->body.crouching ? 1.18f : 1.64f;
	}
	rm->fire = flags & 4;
	rm->use = flags & 8;
	rm->walk = flags & 16;
	rm->used = true;
	for (int i = 0; i < k; i++) {
		const int16_t d = (int16_t)(ch[i].seq - rm->last_choice);
		if (d <= 0)
			continue;
		rm->last_choice = ch[i].seq;
		host_choice(f, &ch[i]);
	}
}

// O lugar de um jogador remoto, a cada quadro do host: o que ele segura (tiro, desarmar) e o resto do jogador.
void netgame_remote(Fighter *f, float dt)
{
	Remote *rm = &remotes[f->remote & 3];
	Actions a;
	memset(&a, 0, sizeof a);
	a.build_select = -1;
	a.fire = rm->fire;
	a.fire_pressed = rm->fire && !rm->prev_fire;
	a.use = rm->use;
	rm->prev_fire = rm->fire;
	player_remote(f, &a, dt);
}

static void write_fighter(W *w, const Fighter *f)
{
	const uint32_t flags = (f->used ? 1 : 0) | (f->alive ? 2 : 0) | (f->team ? 4 : 0) | (f->bomb ? 8 : 0) | (f->kit ? 16 : 0) |
		(f->helmet ? 32 : 0) | (f->body.crouching ? 64 : 0) | (f->is_bot ? 128 : 0);
	w8(w, flags);
	wf(w, f->body.position.x);
	wf(w, f->body.position.y);
	wf(w, f->body.position.z);
	wf(w, f->body.yaw);
	wf(w, f->body.pitch);
	w16(w, (uint32_t)(int16_t)clampf(f->body.velocity.x * 100, -32000, 32000));
	w16(w, (uint32_t)(int16_t)clampf(f->body.velocity.y * 100, -32000, 32000));
	w16(w, (uint32_t)(int16_t)clampf(f->body.velocity.z * 100, -32000, 32000));
	w8(w, (uint32_t)(f->hp < 0 ? 0 : f->hp));
	w8(w, (uint32_t)f->armor);
	w8(w, (uint32_t)f->slot);
	const Gun *g = f->slot >= 1 && f->slot <= 4 && f->has[f->slot] ? &f->guns[f->slot] : NULL;
	w8(w, g ? (uint32_t)g->id : 255);
	w8(w, (uint32_t)(f->anim_kind < 0 ? 0 : f->anim_kind));
	w8(w, (uint32_t)(f->topple * 255));
	w16(w, (uint32_t)f->money);
	w8(w, (uint32_t)f->kills);
	w8(w, (uint32_t)f->deaths);
	w8(w, f->net_epoch);
}

static void send_state(int slot)
{
	Match *m = &G.m;
	Remote *rm = &remotes[slot & 3];
	Fighter *you = fighter_of_slot(slot);
	if (!you)
		return;
	uint8_t buf[NET_MAX_PACKET];
	W w = { buf, 0, sizeof buf };
	w8(&w, 'S');
	w32(&w, ++snap_seq);
	w16(&w, rm->last_choice);
	w8(&w, (uint32_t)you->index);
	w8(&w, (uint32_t)m->mode);
	w8(&w, (uint32_t)m->phase);
	w8(&w, (uint32_t)m->round);
	w8(&w, (uint32_t)m->score[0]);
	w8(&w, (uint32_t)m->score[1]);
	wf(&w, m->phase == PHASE_PLANTED ? (float)(m->bomb_explode_at - G.time) : (float)(m->phase_end - G.time));
	w8(&w, (m->bomb_planted ? 1 : 0) | (m->bomb_dropped ? 2 : 0) | (match_buy_allowed(you) ? 4 : 0) | (you->frozen ? 8 : 0));
	const Vec3 bp = m->bomb_planted ? m->bomb_pos : m->bomb_drop_pos;
	wf(&w, bp.x);
	wf(&w, bp.y);
	wf(&w, bp.z);
	wf(&w, m->plant_progress);
	wf(&w, m->defuse_progress);
	w8(&w, (uint32_t)(m->planter < 0 ? 255 : m->planter));
	w8(&w, (uint32_t)(m->defuser < 0 ? 255 : m->defuser));
	w8(&w, (uint32_t)m->count);
	for (int i = 0; i < m->count; i++)
		write_fighter(&w, &m->f[i]);
	// so dele: armas, materiais, construcao
	uint32_t has = 0;
	for (int s = 1; s <= 4; s++)
		has |= you->has[s] ? 1u << s : 0;
	w8(&w, has);
	for (int s = 1; s <= 4; s++) {
		w8(&w, you->has[s] ? (uint32_t)you->guns[s].id : 255);
		w8(&w, (uint32_t)you->guns[s].mag);
		w16(&w, (uint32_t)you->guns[s].reserve);
	}
	for (int k = 0; k < 3; k++)
		w16(&w, (uint32_t)you->mats[k]);
	w8(&w, (uint32_t)you->build_mat);
	w8(&w, (uint32_t)(you->build_piece < 0 ? 255 : you->build_piece));
	w8(&w, (uint32_t)(you->editing < 0 ? 255 : you->editing));
	w8(&w, you->scoped ? 1 : 0);
	wf(&w, (float)fmax(0, you->reload_until - G.time));
	// eventos que ele ainda nao confirmou
	uint32_t from = rm->ev_ack + 1;
	if (ev_seq - from + 1 > EVENTS - 8)
		from = ev_seq - (EVENTS - 8) + 1;
	int count = 0;
	for (uint32_t s = from; s <= ev_seq && count < MAX_EV_PER_PACKET; s++)
		count++;
	w8(&w, (uint32_t)count);
	for (uint32_t s = from; s < from + (uint32_t)count; s++) {
		const Event *e = &events[s % EVENTS];
		w16(&w, e->seq & 0xFFFF);
		w8(&w, e->type);
		w8(&w, e->a);
		w8(&w, e->b);
		w8(&w, e->c);
		for (int j = 0; j < 3; j++)
			wf(&w, e->p[j]);
		for (int j = 0; j < 3; j++)
			wf(&w, e->q[j]);
	}
	net_send((uint8_t)slot, buf, w.n);
}

static void send_names(void)
{
	uint8_t buf[NET_MAX_PACKET];
	W w = { buf, 0, sizeof buf };
	w8(&w, 'N');
	w8(&w, (uint32_t)G.m.count);
	for (int i = 0; i < G.m.count; i++) {
		const Fighter *f = &G.m.f[i];
		wstr(&w, f->name, 23);
		w8(&w, f->is_bot ? 0 : 1);
	}
	net_send(255, buf, w.n);
}

static void send_world(void)
{
	uint8_t buf[NET_MAX_PACKET];
	W w = { buf, 0, sizeof buf };
	w8(&w, 'W');
	const int n = 24;
	w8(&w, (uint32_t)n);
	for (int k = 0; k < n; k++) {
		const int i = (world_cursor + k) % MAX_PIECES;
		const Piece *p = &G.m.pieces[i];
		w8(&w, (uint32_t)i);
		const bool live = p->used && !p->dying;
		w8(&w, live ? 1 : 0);
		if (!live)
			continue;
		w8(&w, (uint32_t)p->piece);
		w8(&w, (uint32_t)p->mat);
		w8(&w, (uint32_t)p->team);
		w8(&w, (uint32_t)p->card);
		w8(&w, (uint32_t)p->ramp_card);
		uint32_t ed = 0;
		for (int e = 0; e < 9; e++)
			ed |= p->edits[e] ? 1u << e : 0;
		w16(&w, ed);
		wf(&w, p->xf.m[3]);
		wf(&w, p->xf.m[7]);
		wf(&w, p->xf.m[11]);
		wf(&w, p->oy);
		w8(&w, (uint32_t)(clampf(p->built / fmaxf(p->build_time, 0.01f), 0, 1) * 255));
		w8(&w, (uint32_t)(clampf(p->hp / fmaxf(p->max_hp, 1), 0, 1) * 255));
	}
	world_cursor = (world_cursor + n) % MAX_PIECES;
	net_send(255, buf, w.n);
}

static void send_portals(void)
{
	uint8_t buf[NET_MAX_PACKET];
	W w = { buf, 0, sizeof buf };
	w8(&w, 'P');
	for (int i = 0; i < MAX_PORTALS; i++) {
		const Portal *p = &G.m.portals[i];
		w8(&w, p->used ? 1 : 0);
		if (!p->used)
			continue;
		w8(&w, (uint32_t)p->color);
		w8(&w, (uint32_t)p->owner);
		w8(&w, p->standing ? 1 : 0);
		w8(&w, (uint32_t)(p->twin < 0 ? 255 : p->twin));
		for (int k = 0; k < 12; k++)
			wf(&w, p->xf.m[k]);
	}
	net_send(255, buf, w.n);
}

// ---------------------------------------------------------------- cliente

static uint32_t cl_seq;
static uint16_t cl_ev_applied;
static bool cl_ev_any;
static Choice cl_choices[16];
static int cl_choice_count;
static uint16_t cl_choice_seq;
static uint8_t cl_epoch;
static double cl_next_cmd;
static int cl_you = -1;
static double cl_banner_until;

static void cl_choice(uint8_t type, uint8_t arg, float a, float b, float c, float d)
{
	if (cl_choice_count == 16) {
		memmove(cl_choices, cl_choices + 1, 15 * sizeof cl_choices[0]);
		cl_choice_count--;
	}
	cl_choices[cl_choice_count++] = (Choice){ ++cl_choice_seq, type, arg, { a, b, c, d } };
}

void netcl_buy(const char *id)
{
	for (int i = 0; i < WEAPONS_COUNT; i++)
		if (!strcmp(WEAPONS[i].id, id)) {
			cl_choice(CH_BUY, (uint8_t)i, 0, 0, 0, 0);
			return;
		}
	for (int i = 0; i < EQUIPMENT_COUNT; i++)
		if (!strcmp(EQUIPMENT[i].id, id))
			cl_choice(CH_BUY, (uint8_t)(100 + i), 0, 0, 0, 0);
}

void netcl_orange(Vec3 feet, float yaw)
{
	cl_choice(CH_ORANGE, 0, feet.x, feet.y, feet.z, yaw);
}

// As escolhas do quadro (o que o player_update faria no host).
bool netcl_choices(const Actions *a)
{
	if (a->slot_pressed)
		cl_choice(CH_SLOT, (uint8_t)a->slot_pressed, 0, 0, 0, 0);
	if (a->next_weapon)
		cl_choice(CH_NEXT, 0, 0, 0, 0, 0);
	if (a->prev_weapon)
		cl_choice(CH_PREV, 0, 0, 0, 0, 0);
	if (a->reload)
		cl_choice(CH_RELOAD, 0, 0, 0, 0, 0);
	if (a->build_select >= 0)
		cl_choice(CH_BUILD, (uint8_t)a->build_select, 0, 0, 0, 0);
	if (a->build_cycle)
		cl_choice(CH_BUILD_CYCLE, 0, 0, 0, 0, 0);
	if (a->material_cycle)
		cl_choice(CH_MATERIAL, 0, 0, 0, 0, 0);
	if (a->edit_pressed)
		cl_choice(CH_EDIT, 0, 0, 0, 0, 0);
	if (a->drop_pressed)
		cl_choice(CH_DROP, 0, 0, 0, 0, 0);
	if (a->alt_pressed)
		cl_choice(CH_ALT, 0, 0, 0, 0, 0);
	return true;
}

void netgame_client_begin(int mode)
{
	Match *m = &G.m;
	int meshes[MAX_PIECES];
	for (int i = 0; i < MAX_PIECES; i++)
		meshes[i] = m->active ? m->pieces[i].mesh : -1;
	Skin skins[MAX_FIGHTERS];
	for (int i = 0; i < MAX_FIGHTERS; i++)
		skins[i] = m->active ? m->f[i].skin : (Skin){ 0 };
	memset(m, 0, sizeof *m);
	for (int i = 0; i < MAX_PIECES; i++)
		m->pieces[i].mesh = meshes[i];
	for (int i = 0; i < MAX_FIGHTERS; i++) {
		m->f[i].skin = skins[i];
		m->f[i].index = i;
		m->f[i].remote = -1;
		m->f[i].build_piece = -1;
		m->f[i].editing = -1;
	}
	for (int i = 0; i < MAX_PORTALS; i++)
		m->portals[i].twin = -1;
	m->active = true;
	m->mode = mode;
	m->player = -1;
	m->planter = m->defuser = -1;
	m->music_voice = -1;
	m->net_last_state = G.time;
	m->wind_voice = audio_play(assets_sound(&G.as, "amb_wind"), AUDIO_MUSIC, v3(0, 0, 0), 0.2f, 1, 1, true);
	G.net_role = NET_ROLE_CLIENT;
	G.state = STATE_MATCH;
	cl_seq = 0;
	cl_ev_any = false;
	cl_choice_count = 0;
	cl_you = -1;
	cl_banner_until = 0;
	plat_capture_mouse(true);
	plat_log("rede: entrando na partida do host (%s)", MODES[mode].name);
}

static void apply_event(R *r)
{
	const uint16_t seq = (uint16_t)r16(r);
	const uint8_t type = (uint8_t)r8(r), a = (uint8_t)r8(r), b = (uint8_t)r8(r), c = (uint8_t)r8(r);
	float p[3], q[3];
	for (int j = 0; j < 3; j++)
		p[j] = rf(r);
	for (int j = 0; j < 3; j++)
		q[j] = rf(r);
	if (r->bad || (cl_ev_any && (int16_t)(seq - cl_ev_applied) <= 0))
		return;
	cl_ev_applied = seq;
	cl_ev_any = true;
	const Vec3 P = v3(p[0], p[1], p[2]), Q = v3(q[0], q[1], q[2]);
	switch (type) {
	case EV_TRACER:
		fx_tracer(P, Q);
		break;
	case EV_IMPACT:
		fx_impact(P, Q, a != 0);
		break;
	case EV_PARTICLES:
		fx_particles(P, PCOLORS[a & 3], b);
		break;
	case EV_SOUND:
		if (a < sizeof SOUNDS / sizeof SOUNDS[0])
			sound_at(SOUNDS[a], P, q[0]);
		break;
	case EV_UI_SOUND:
		if (a < sizeof SOUNDS / sizeof SOUNDS[0])
			sound_ui(SOUNDS[a], q[0]);
		break;
	case EV_MUSIC:
		if (a < sizeof SOUNDS / sizeof SOUNDS[0])
			music_play(SOUNDS[a], q[0]);
		break;
	case EV_FEED:
		if (b < G.m.count)
			hud_feed(a < G.m.count ? &G.m.f[a] : NULL, &G.m.f[b], c < WEAPONS_COUNT ? c : 0, q[0] > 0.5f);
		break;
	case EV_ROUND_OVER:
		hud_round_over(a & 1, b < sizeof WHYS / sizeof WHYS[0] ? WHYS[b] : "");
		break;
	case EV_MATCH_OVER:
		hud_match_over((int)a - 1);
		break;
	case EV_NOTICE:
		if (a < sizeof NOTICES / sizeof NOTICES[0])
			match_notice(NOTICES[a], 3.0f);
		break;
	case EV_ROUND_START:
		hud_round_started();
		break;
	}
}

static void client_state(R *r)
{
	Match *m = &G.m;
	r32(r);
	const uint16_t acked = (uint16_t)r16(r);
	const int you = (int)r8(r);
	const int mode = (int)r8(r);
	const int phase = (int)r8(r), round = (int)r8(r), s0 = (int)r8(r), s1 = (int)r8(r);
	const float left = rf(r);
	const uint8_t flags = (uint8_t)r8(r);
	const Vec3 bp = v3(rf(r), rf(r), rf(r));
	const float pp = rf(r), dp = rf(r);
	const int planter = (int)r8(r), defuser = (int)r8(r);
	const int count = (int)r8(r);
	if (r->bad || count > MAX_FIGHTERS || you >= count || mode >= MODES_COUNT)
		return;
	m->mode = mode;
	m->phase = phase;
	m->round = round;
	m->score[0] = s0;
	m->score[1] = s1;
	m->phase_end = G.time + left;
	m->bomb_explode_at = G.time + left;
	m->bomb_planted = flags & 1;
	m->bomb_dropped = (flags & 2) != 0;
	m->net_can_buy = (flags & 4) != 0;
	if (m->bomb_planted)
		m->bomb_pos = bp;
	else
		m->bomb_drop_pos = bp;
	m->plant_progress = pp;
	m->defuse_progress = dp;
	m->planter = planter == 255 ? -1 : planter;
	m->defuser = defuser == 255 ? -1 : defuser;
	m->count = count;
	m->player = you;
	m->spectate = m->spectate < count ? m->spectate : 0;
	for (int i = 0; i < count; i++) {
		Fighter *f = &m->f[i];
		const uint8_t fl = (uint8_t)r8(r);
		const Vec3 pos = v3(rf(r), rf(r), rf(r));
		const float yaw = rf(r), pitch = rf(r);
		const Vec3 vel = v3((int16_t)r16(r) / 100.0f, (int16_t)r16(r) / 100.0f, (int16_t)r16(r) / 100.0f);
		const int hp = (int)r8(r), armor = (int)r8(r), slot = (int)r8(r), gun = (int)r8(r), anim = (int)r8(r);
		const float topple = r8(r) / 255.0f;
		const int money = (int)r16(r), kills = (int)r8(r), deaths = (int)r8(r);
		const uint8_t epoch = (uint8_t)r8(r);
		if (r->bad)
			return;
		const bool was_alive = f->alive;
		f->used = fl & 1;
		f->alive = (fl & 2) != 0;
		f->team = (fl & 4) ? TEAM_T : TEAM_CT;
		f->bomb = (fl & 8) != 0;
		f->kit = (fl & 16) != 0;
		f->helmet = (fl & 32) != 0;
		f->is_bot = (fl & 128) != 0;
		f->index = i;
		f->hp = hp;
		f->armor = armor;
		f->money = money;
		f->kills = kills;
		f->deaths = deaths;
		f->net_target = pos;
		if (!was_alive && f->alive) {
			// nasceu de novo: tira o tombo e pula para la
			f->topple = 0;
			f->anim.paused = false;
			f->anim_kind = -1;
			f->body.position = pos;
		}
		if (!f->alive && was_alive)
			f->anim.paused = true;
		if (i == you) {
			f->is_player = true;
			f->slot = slot;
			if (epoch != cl_epoch || !f->body.height) {
				// o host moveu o corpo (nascer, portal): o cliente pula para la
				memset(&f->body, 0, sizeof f->body);
				f->body.position = pos;
				f->body.yaw = yaw;
				f->body.pitch = pitch;
				f->body.height = 1.83f;
				f->body.eye = 1.64f;
				f->body.on_ground = true;
				cl_epoch = epoch;
			}
		} else {
			f->is_player = false;
			f->body.yaw = yaw;
			f->body.pitch = pitch;
			f->body.velocity = vel;
			f->body.crouching = (fl & 64) != 0;
			f->body.height = f->body.crouching ? 1.3f : 1.83f;
			f->slot = slot;
			f->anim_kind = anim;
			if (v3_length(v3_sub(f->body.position, pos)) > 5.0f)
				f->body.position = pos;
			f->topple = f->alive ? 0 : fmaxf(f->topple, topple);
		}
		memset(f->has, 0, sizeof f->has);
		if (gun != 255 && slot >= 1 && slot <= 4 && gun < WEAPONS_COUNT) {
			f->has[slot] = true;
			f->guns[slot].id = gun;
		}
	}
	// so seu
	Fighter *me = &m->f[you];
	const uint32_t has = r8(r);
	for (int s = 1; s <= 4; s++) {
		const int id = (int)r8(r), mag = (int)r8(r), reserve = (int)r16(r);
		me->has[s] = (has >> s & 1) && id < WEAPONS_COUNT;
		if (me->has[s])
			me->guns[s] = (Gun){ id, mag, reserve };
	}
	for (int k = 0; k < 3; k++)
		me->mats[k] = (int)r16(r);
	me->build_mat = (int)r8(r) % BUILD_MATERIALS_COUNT;
	const int bpiece = (int)r8(r), editing = (int)r8(r);
	me->build_piece = bpiece == 255 || bpiece >= BUILD_PIECES_COUNT ? -1 : bpiece;
	me->editing = editing == 255 || editing >= MAX_PIECES ? -1 : editing;
	me->scoped = r8(r) != 0;
	const float reload = rf(r);
	me->reload_until = G.time + reload;
	me->frozen = (flags & 8) != 0;
	// as escolhas que o host ja aplicou saem da fila
	int keep = 0;
	for (int i = 0; i < cl_choice_count; i++)
		if ((int16_t)(cl_choices[i].seq - acked) > 0)
			cl_choices[keep++] = cl_choices[i];
	cl_choice_count = keep;
	const int ne = (int)r8(r);
	for (int i = 0; i < ne && !r->bad; i++)
		apply_event(r);
	m->net_last_state = G.time;
	cl_you = you;
}

static void client_names(R *r)
{
	const int n = (int)r8(r);
	for (int i = 0; i < n && i < MAX_FIGHTERS && !r->bad; i++) {
		rstr(r, G.m.f[i].name, sizeof G.m.f[i].name);
		r8(r);
	}
}

void piece_set_remote(int idx, bool live, int piece, int mat, int team, int card, int ramp, uint32_t edits, Vec3 c, float oy,
	float built, float hp);

static void client_world(R *r)
{
	const int n = (int)r8(r);
	for (int k = 0; k < n && !r->bad; k++) {
		const int i = (int)r8(r);
		const bool live = r8(r) != 0;
		if (!live) {
			if (i < MAX_PIECES)
				piece_set_remote(i, false, 0, 0, 0, 0, 0, 0, v3(0, 0, 0), 0, 0, 0);
			continue;
		}
		const int piece = (int)r8(r), mat = (int)r8(r), team = (int)r8(r), card = (int)r8(r), ramp = (int)r8(r);
		const uint32_t edits = r16(r);
		const Vec3 c = v3(rf(r), rf(r), rf(r));
		const float oy = rf(r), built = r8(r) / 255.0f, hp = r8(r) / 255.0f;
		if (!r->bad && i < MAX_PIECES && piece < BUILD_PIECES_COUNT && mat < BUILD_MATERIALS_COUNT)
			piece_set_remote(i, true, piece, mat, team & 1, card & 3, ramp & 3, edits, c, oy, built, hp);
	}
}

static void client_portals(R *r)
{
	for (int i = 0; i < MAX_PORTALS && !r->bad; i++) {
		Portal *p = &G.m.portals[i];
		if (!r8(r)) {
			p->used = false;
			continue;
		}
		p->used = true;
		p->color = (int)r8(r) & 1;
		p->owner = (int)r8(r);
		p->standing = r8(r) != 0;
		const int twin = (int)r8(r);
		p->twin = twin < MAX_PORTALS ? twin : -1;
		for (int k = 0; k < 12; k++)
			p->xf.m[k] = rf(r);
		p->surface_piece = -1;
	}
}

static void send_cmd(void)
{
	if (cl_you < 0)
		return;
	const Fighter *f = &G.m.f[cl_you];
	const Actions *a = &G.act;
	uint8_t buf[NET_MAX_PACKET];
	W w = { buf, 0, sizeof buf };
	w8(&w, 'C');
	w32(&w, ++cl_seq);
	w16(&w, cl_ev_applied);
	w8(&w, cl_epoch);
	wf(&w, f->body.position.x);
	wf(&w, f->body.position.y);
	wf(&w, f->body.position.z);
	wf(&w, f->body.velocity.x);
	wf(&w, f->body.velocity.y);
	wf(&w, f->body.velocity.z);
	wf(&w, f->body.yaw);
	wf(&w, f->body.pitch);
	const bool blocked = G.m.buy_open || G.m.tacmap_open || G.paused;
	w8(&w, (f->body.on_ground ? 1 : 0) | (f->body.crouching ? 2 : 0) | (!blocked && a->fire ? 4 : 0) | (!blocked && a->use ? 8 : 0) |
		(a->walk ? 16 : 0));
	w8(&w, (uint32_t)cl_choice_count);
	for (int i = 0; i < cl_choice_count; i++) {
		w16(&w, cl_choices[i].seq);
		w8(&w, cl_choices[i].type);
		w8(&w, cl_choices[i].arg);
		for (int j = 0; j < 4; j++)
			wf(&w, cl_choices[i].v[j]);
	}
	net_send(0, buf, w.n);
}

// ---------------------------------------------------------------- todo quadro

void netgame_update(float dt)
{
	(void)dt;
	uint8_t buf[NET_MAX_PACKET];
	uint8_t from;
	int n;
	while ((n = net_recv(&from, buf, sizeof buf)) > 0) {
		if (G.net_role == NET_ROLE_HOST) {
			host_receive(buf, n, from);
			continue;
		}
		R r = { buf, n, 1, false };
		switch (buf[0]) {
		case 'S':
			client_state(&r);
			break;
		case 'N':
			client_names(&r);
			break;
		case 'W':
			client_world(&r);
			break;
		case 'P':
			client_portals(&r);
			break;
		}
	}
	if (G.net_role == NET_ROLE_CLIENT) {
		// os outros deslizam ate onde o host diz
		for (int i = 0; i < G.m.count; i++) {
			Fighter *f = &G.m.f[i];
			if (i == G.m.player || !f->used)
				continue;
			const float k = fminf(1.0f, dt * 12.0f);
			f->body.position = v3_add(f->body.position, v3_scale(v3_sub(f->net_target, f->body.position), k));
		}
	}
}

void netgame_flush(void)
{
	const double t = G.time;
	if (G.net_role == NET_ROLE_HOST) {
		if (t >= next_state) {
			for (int s = 0; s < NET_MAX_MEMBERS; s++)
				if (remotes[s].used || fighter_of_slot(s))
					send_state(s);
			next_state = t + 0.05;
		}
		if (t >= next_names) {
			send_names();
			next_names = t + 1.0;
		}
		if (t >= next_world) {
			send_world();
			next_world = t + 0.1;
		}
		if (t >= next_portals && MODES[G.m.mode].portals) {
			send_portals();
			next_portals = t + 0.16;
		}
	} else if (G.net_role == NET_ROLE_CLIENT && t >= cl_next_cmd) {
		send_cmd();
		cl_next_cmd = t + 1.0 / 30.0;
	}
}

void netgame_end(void)
{
	G.net_role = NET_ROLE_NONE;
}

// No menu: o cliente entra quando o host comeca; volta ao menu quando o host para.
void netgame_menu_poll(void)
{
	NetView v;
	net_view(&v);
	if (!v.in_room)
		return;
	const bool host = v.room.host_id == v.user_id;
	if (!host && v.room.playing && G.state == STATE_MENU && v.room.match_id != G.net_match_id) {
		G.net_match_id = v.room.match_id;
		int mode = MODES_CLASSIC;
		for (int i = 0; i < MODES_COUNT; i++)
			if (!strcmp(MODES[i].id, v.room.mode))
				mode = i;
		audio_stop_kind(AUDIO_MUSIC);
		netgame_client_begin(mode);
	}
}
