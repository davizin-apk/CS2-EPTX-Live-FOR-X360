// A partida 5v5 (match.gd): rodadas, tempo de compra, bomba (plantar, desarmar, explodir), economia, meio-tempo,
// objetivos dos bots, efeitos e sons.
#include <stdlib.h>
#include <string.h>

#include "game.h"

void build_update_and_draw(float dt);
void build_clear(void);
void portals_draw(void);

static float now(void)
{
	return (float)G.time;
}

static float db(float v)
{
	return powf(10.0f, v / 20.0f);
}

void sound_at(const char *id, Vec3 p, float vol)
{
	netev_sound(id, p, vol, false);
	audio_play(assets_sound(&G.as, id), AUDIO_WORLD, p, db(vol), 15.0f, frand_range(0.97f, 1.03f), false);
}

void sound_ui(const char *id, float vol)
{
	audio_play(assets_sound(&G.as, id), AUDIO_UI, v3(0, 0, 0), db(vol), 1, 1, false);
}

void music_play(const char *id, float vol)
{
	netev_music(id, vol);
	if (G.m.music_voice >= 0)
		audio_stop(G.m.music_voice);
	G.m.music_voice = audio_play(assets_sound(&G.as, id), AUDIO_MUSIC, v3(0, 0, 0), db(vol), 1, 1, false);
}

void match_notice(const char *text, float dur)
{
	snprintf(G.m.notice, sizeof G.m.notice, "%s", text);
	G.m.notice_until = G.time + dur;
}

static int alive_count(int team)
{
	int n = 0;
	for (int i = 0; i < G.m.count; i++)
		n += G.m.f[i].team == team && G.m.f[i].alive;
	return n;
}

// ---------------------------------------------------------------- comeco

void match_start(int mode, int side, int difficulty, bool autoplay)
{
	Match *m = &G.m;
	// as malhas das pecas ficam de uma partida para outra
	int meshes[MAX_PIECES];
	for (int i = 0; i < MAX_PIECES; i++)
		meshes[i] = m->active ? m->pieces[i].mesh : -1;
	Skin skins[MAX_FIGHTERS];
	for (int i = 0; i < MAX_FIGHTERS; i++)
		skins[i] = m->active ? m->f[i].skin : (Skin){ 0 };
	memset(m, 0, sizeof *m);
	for (int i = 0; i < MAX_PIECES; i++)
		m->pieces[i].mesh = meshes[i];
	for (int i = 0; i < MAX_PORTALS; i++)
		m->portals[i].twin = -1;
	m->active = true;
	m->mode = mode;
	m->player_team = side == 2 ? (frand() < 0.5f ? TEAM_CT : TEAM_T) : side;
	m->difficulty = difficulty;
	m->music_voice = -1;
	m->player = -1;
	m->planter = m->defuser = -1;
	const ModesDef *md = &MODES[mode];
	for (int team = 0; team < 2; team++)
		for (int i = 0; i < md->team_size; i++) {
			Fighter *f = &m->f[m->count];
			f->skin = skins[m->count];
			f->used = true;
			f->index = m->count;
			f->team = team;
			f->money = md->start_money;
			f->build_piece = -1;
			f->editing = -1;
			f->remote = -1;
			if (team == m->player_team && i == 0 && !autoplay) {
				f->is_player = true;
				m->player = m->count;
				snprintf(f->name, sizeof f->name, "You");
			} else {
				bot_setup(f, difficulty);
				snprintf(f->name, sizeof f->name, "%s", TEAMS[team].bot_names[i % 5]);
				if (team == m->player_team && i == 0) {
					f->bot.autopilot = true;
					snprintf(f->name, sizeof f->name, "You (autopilot)");
					// BS_FP=1: a camera fica no olho dele, com a arma em primeira pessoa (teste no PC)
					const char *fp = getenv("BS_FP");
					if (fp && *fp == '1')
						m->player = m->count;
				}
			}
			m->count++;
		}
	m->round = 1;
	m->wind_voice = audio_play(assets_sound(&G.as, "amb_wind"), AUDIO_MUSIC, v3(0, 0, 0), db(-14), 1, 1, true);
	plat_log("partida: %s, lado %s, bots %s", md->name, m->player_team ? "T" : "CT", BOT_SKILLS[difficulty].name);
	extern void match_start_round(void);
	match_start_round();
	plat_capture_mouse(!autoplay);
}

void match_end(void)
{
	netgame_end();
	audio_stop_kind(AUDIO_MUSIC);
	audio_stop_kind(AUDIO_WORLD);
	build_clear();
	G.m.active = false;
}

void match_start_round(void)
{
	Match *m = &G.m;
	m->phase = PHASE_FREEZE;
	m->phase_end = now() + MODES[m->mode].freeze_time;
	m->round_start = now();
	build_clear();
	portals_clear();
	for (int i = 0; i < MAX_PORTALS; i++)
		m->portals[i].twin = -1;
	m->tacmap_open = false;
	m->buy_open = false;
	m->bomb_planted = false;
	m->bomb_dropped = false;
	m->planter = m->defuser = -1;
	m->plant_progress = m->defuse_progress = 0;
	m->plan_site = frand() < 0.5f ? 0 : 1;
	memset(m->fx, 0, sizeof m->fx);
	memset(m->shells, 0, sizeof m->shells);
	int idx[2] = { 0, 0 };
	// spawns por time, na ordem das entidades
	for (int i = 0; i < m->count; i++) {
		Fighter *f = &m->f[i];
		const BsSpawn *sp = NULL;
		int seen = 0;
		for (uint32_t s = 0; s < G.map.spawn_count; s++)
			if ((int)G.map.spawns[s].team == f->team) {
				if (seen == idx[f->team]) {
					sp = &G.map.spawns[s];
					break;
				}
				seen++;
			}
		if (!sp)
			sp = &G.map.spawns[0];
		idx[f->team]++;
		if (!f->alive)
			memset(f->has, 0, sizeof f->has);
		f->bomb = false;
		fighter_respawn(f, sp->position, sp->yaw);
		f->frozen = true;
		for (int k = 0; k < BUILD_MATERIALS_COUNT; k++)
			f->mats[k] = MODES[m->mode].building ? BUILD_MATERIALS[k].start_amount : 0;
		f->build_mat = 0;
		if (f->is_bot) {
			bot_buy(f);
			bot_round_start(f);
		}
	}
	int ts[MAX_FIGHTERS], tn = 0;
	for (int i = 0; i < m->count; i++)
		if (m->f[i].team == TEAM_T)
			ts[tn++] = i;
	m->bomb_carrier = tn ? ts[rand() % tn] : -1;
	if (m->bomb_carrier >= 0)
		m->f[m->bomb_carrier].bomb = true;
	m->spectate = m->player >= 0 ? m->player : 0;
	music_play("mus_start", -8);
	hud_round_started();
	netev_banner(3, 0, "");
}

bool match_buy_allowed(const Fighter *f)
{
	const Match *m = &G.m;
	if (G.net_role == NET_ROLE_CLIENT)
		return f->alive && m->net_can_buy;
	if (!f->alive)
		return false;
	const ModesDef *md = &MODES[m->mode];
	const bool in_time = m->phase == PHASE_FREEZE ||
		(m->phase == PHASE_LIVE && now() - m->round_start < md->freeze_time + md->buy_time);
	for (uint32_t s = 0; s < G.map.spawn_count; s++)
		if ((int)G.map.spawns[s].team == f->team)
			return in_time && v3_length(v3_sub(f->body.position, G.map.spawns[s].position)) < 18.0f;
	return in_time;
}

// ---------------------------------------------------------------- fim de rodada

static void end_round(int winner, const char *why)
{
	Match *m = &G.m;
	if (m->phase == PHASE_OVER || m->phase == PHASE_MATCH_OVER)
		return;
	const ModesDef *md = &MODES[m->mode];
	m->score[winner]++;
	const int loser = 1 - winner;
	m->loss_streak[winner] = 0;
	m->loss_streak[loser] = m->loss_streak[loser] + 1 < 5 ? m->loss_streak[loser] + 1 : 5;
	for (int i = 0; i < m->count; i++) {
		Fighter *f = &m->f[i];
		int add = f->team == winner ? md->win_bonus : md->loss_bonus + 500 * (m->loss_streak[loser] - 1);
		f->money = f->money + add > md->max_money ? md->max_money : f->money + add;
	}
	if (m->player >= 0)
		music_play(winner == m->f[m->player].team ? "mus_won" : "mus_lost", -6);
	hud_round_over(winner, why);
	plat_log("rodada %d: vitoria %s (%s), CT %d x T %d", m->round, winner ? "T" : "CT", why, m->score[0], m->score[1]);
	m->planter = m->defuser = -1;
	if (m->score[winner] >= md->rounds_to_win || m->round >= md->max_rounds) {
		m->phase = PHASE_MATCH_OVER;
		m->phase_end = now() + 10.0;
		hud_match_over(m->score[winner] != m->score[loser] ? winner : -1);
	} else {
		m->phase = PHASE_OVER;
		m->phase_end = now() + 6.0;
	}
}

static void check_elimination(void)
{
	Match *m = &G.m;
	if (m->phase == PHASE_OVER || m->phase == PHASE_MATCH_OVER)
		return;
	if (alive_count(TEAM_CT) == 0)
		end_round(TEAM_T, "Counter-Terrorists eliminated");
	else if (alive_count(TEAM_T) == 0 && m->phase != PHASE_PLANTED)
		end_round(TEAM_CT, "Terrorists eliminated");
}

static void next_round(void)
{
	Match *m = &G.m;
	const ModesDef *md = &MODES[m->mode];
	m->round++;
	if (m->round == md->max_rounds / 2 + 1) {
		// meio-tempo: troca de lado
		const int s = m->score[0];
		m->score[0] = m->score[1];
		m->score[1] = s;
		for (int i = 0; i < m->count; i++) {
			Fighter *f = &m->f[i];
			f->team = 1 - f->team;
			f->money = md->start_money;
			memset(f->has, 0, sizeof f->has);
			f->alive = false;
		}
		m->player_team = 1 - m->player_team;
		match_notice("HALFTIME - SWITCHING SIDES", 3.0f);
		netev_banner(2, 0, "HALFTIME - SWITCHING SIDES");
	}
	match_start_round();
}

static void restart_match(void)
{
	Match *m = &G.m;
	m->score[0] = m->score[1] = 0;
	m->round = 0;
	for (int i = 0; i < m->count; i++) {
		m->f[i].money = MODES[m->mode].start_money;
		memset(m->f[i].has, 0, sizeof m->f[i].has);
		m->f[i].alive = false;
	}
	next_round();
}

void match_died(Fighter *v, Fighter *k, int w, bool headshot)
{
	Match *m = &G.m;
	const ModesDef *md = &MODES[m->mode];
	if (k && k != v) {
		if (k->team != v->team) {
			k->kills++;
			k->money = k->money + md->kill_reward > md->max_money ? md->max_money : k->money + md->kill_reward;
			if (md->building)
				for (int i = 0; i < BUILD_MATERIALS_COUNT; i++)
					k->mats[i] = k->mats[i] + BUILD_MATERIALS[i].per_kill > 999 ? 999 : k->mats[i] + BUILD_MATERIALS[i].per_kill;
		} else {
			k->money = k->money > 300 ? k->money - 300 : 0;
		}
	}
	if (v->bomb) {
		v->bomb = false;
		m->bomb_dropped = true;
		m->bomb_drop_pos = v->body.position;
	}
	if (m->planter == v->index)
		match_plant_cancel(v);
	if (m->defuser == v->index)
		match_defuse_cancel(v);
	hud_feed(k, v, w, headshot);
	if (v->index == m->player) {
		// espectador: o proximo vivo do time
		for (int i = 1; i <= m->count; i++) {
			const int j = (v->index + i) % m->count;
			if (m->f[j].alive && m->f[j].team == v->team) {
				m->spectate = j;
				break;
			}
		}
	}
	check_elimination();
}

// ---------------------------------------------------------------- bomba

bool match_is_busy(const Fighter *f)
{
	return G.m.planter == f->index || G.m.defuser == f->index;
}

void match_plant_tick(Fighter *f, float dt)
{
	Match *m = &G.m;
	if (m->phase != PHASE_LIVE || !f->bomb || !f->body.on_ground)
		return;
	int s;
	if (!world_site_at(f->body.position, &s)) {
		if (f->is_player)
			match_notice("You must be in a bomb site to plant", 1.0f);
		return;
	}
	if (m->planter != f->index) {
		m->planter = f->index;
		m->plant_progress = 0;
		if (f->is_player)
			player_viewmodel_play("shoot");
	}
	m->plant_progress += dt;
	if (m->plant_progress >= MODES[m->mode].plant_time) {
		m->planter = -1;
		m->plant_progress = 0;
		f->bomb = false;
		if (f->slot == 5)
			fighter_switch(f, f->has[1] ? 1 : 2);
		m->bomb_pos = f->body.position;
		m->bomb_site = s;
		m->bomb_planted = true;
		m->phase = PHASE_PLANTED;
		m->bomb_explode_at = now() + MODES[m->mode].bomb_time;
		m->next_beep = now();
		f->money += 300;
		music_play("mus_planted", -6);
		match_notice("THE BOMB HAS BEEN PLANTED", 3.0f);
		netev_banner(2, 0, "THE BOMB HAS BEEN PLANTED");
		for (int i = 0; i < m->count; i++)
			if (m->f[i].is_bot)
				m->f[i].bot.next_repath = 0;
		plat_log("bomba plantada no %c por %s", s ? 'B' : 'A', f->name);
	}
}

void match_plant_cancel(Fighter *f)
{
	if (G.m.planter == f->index) {
		G.m.planter = -1;
		G.m.plant_progress = 0;
	}
}

void match_defuse_tick(Fighter *f, float dt)
{
	Match *m = &G.m;
	if (m->phase != PHASE_PLANTED || f->team != TEAM_CT || v3_length(v3_sub(f->body.position, m->bomb_pos)) > 1.8f)
		return;
	if (m->defuser != f->index) {
		if (m->defuser >= 0)
			return;
		m->defuser = f->index;
		m->defuse_progress = 0;
	}
	m->defuse_progress += dt;
	const float need = f->kit ? MODES[m->mode].defuse_kit_time : MODES[m->mode].defuse_time;
	if (m->defuse_progress >= need) {
		m->defuser = -1;
		sound_ui("snd_c4_disarm", -4);
		f->money += 300;
		end_round(TEAM_CT, "Bomb defused");
	}
}

void match_defuse_cancel(Fighter *f)
{
	if (G.m.defuser == f->index) {
		G.m.defuser = -1;
		G.m.defuse_progress = 0;
	}
}

void match_drop_bomb(Fighter *f)
{
	Match *m = &G.m;
	if (!f->bomb)
		return;
	f->bomb = false;
	fighter_switch(f, f->has[1] ? 1 : 2);
	const Vec3 a = fighter_aim(f);
	m->bomb_drop_pos = nav_closest(v3_add(f->body.position, v3(a.x * 1.5f, 0, a.z * 1.5f)));
	m->bomb_dropped = true;
}

static void check_bomb_pickup(void)
{
	Match *m = &G.m;
	if (!m->bomb_dropped || m->phase == PHASE_PLANTED)
		return;
	for (int i = 0; i < m->count; i++) {
		Fighter *f = &m->f[i];
		if (f->alive && f->team == TEAM_T && v3_length(v3_sub(f->body.position, m->bomb_drop_pos)) < 1.2f) {
			f->bomb = true;
			m->bomb_carrier = i;
			m->bomb_dropped = false;
			break;
		}
	}
}

static void explode(void)
{
	Match *m = &G.m;
	m->phase = PHASE_LIVE; // para o end_round aceitar
	sound_at("snd_c4_explode", m->bomb_pos, 6);
	for (int i = 0; i < MAX_FX; i++)
		if (!m->fx[i].used) {
			m->fx[i] = (Fx){ true, FX_EXPLOSION, v3_add(m->bomb_pos, v3(0, 2, 0)), { 0, 0, 0 }, { 0, 0, 0 }, { 1, 0.7f, 0.3f, 1 },
				now(), 1.2f, 30.0f, 0, false };
			break;
		}
	for (int i = 0; i < m->count; i++) {
		Fighter *f = &m->f[i];
		if (f->alive && v3_length(v3_sub(f->body.position, m->bomb_pos)) < 20.0f) {
			f->hp = 0;
			fighter_die(f, NULL, WEAPONS_KNIFE, false);
		}
	}
	m->bomb_planted = false;
	end_round(TEAM_T, "Target bombed");
}

// ---------------------------------------------------------------- objetivos dos bots (chamado do bot)

static int nearest_alive(int team, Vec3 p)
{
	int best = -1;
	float bd = 1e30f;
	for (int i = 0; i < G.m.count; i++) {
		const Fighter *f = &G.m.f[i];
		if (f->team == team && f->alive) {
			const float d = v3_length(v3_sub(f->body.position, p));
			if (d < bd) {
				bd = d;
				best = i;
			}
		}
	}
	return best;
}

static void set_goal(Fighter *b, Vec3 p, int kind)
{
	if (v3_length(v3_sub(b->bot.goal, p)) > 1.0f || b->bot.goal_kind != kind) {
		b->bot.goal = p;
		b->bot.goal_kind = kind;
		b->bot.next_repath = 0;
	}
}

void match_bot_objective(Fighter *b)
{
	Match *m = &G.m;
	if (m->phase == PHASE_FREEZE)
		return;
	Brain *br = &b->bot;
	if (b->team == TEAM_T) {
		int s;
		if (b->bomb) {
			if (world_site_at(b->body.position, &s) && m->phase == PHASE_LIVE && br->target < 0) {
				set_goal(b, b->body.position, GOAL_PLANT);
				if (b->slot != 5)
					fighter_switch(b, 5);
				match_plant_tick(b, 0.12f);
			} else {
				match_plant_cancel(b);
				set_goal(b, br->goal_kind != GOAL_SITE ? world_random_in_site(m->plan_site) : br->goal, GOAL_SITE);
				if (b->slot == 5 && br->target >= 0)
					fighter_switch(b, b->has[1] ? 1 : 2);
			}
		} else if (m->bomb_dropped && m->phase == PHASE_LIVE) {
			if (nearest_alive(TEAM_T, m->bomb_drop_pos) == b->index)
				set_goal(b, m->bomb_drop_pos, GOAL_BOMB);
		} else if (m->phase == PHASE_PLANTED) {
			if (br->goal_kind != GOAL_GUARD) {
				set_goal(b, nav_closest(v3_add(m->bomb_pos, v3(frand_range(-6, 6), 0, frand_range(-6, 6)))), GOAL_GUARD);
				if (MODES[m->mode].building && frand() < BOT_SKILLS[br->skill].build_chance) {
					const Placement p = build_placement(b, BUILD_PIECES_WALL, b->body.yaw, 0.0f);
					build_try(b, BUILD_PIECES_WALL, &p);
				}
			}
		}
	} else {
		if (m->phase == PHASE_PLANTED) {
			const float d = v3_length(v3_sub(b->body.position, m->bomb_pos));
			if (nearest_alive(TEAM_CT, m->bomb_pos) == b->index || alive_count(TEAM_T) == 0) {
				set_goal(b, m->bomb_pos, GOAL_DEFUSE);
				if (d < 1.6f && (br->target < 0 || alive_count(TEAM_T) == 0))
					match_defuse_tick(b, 0.12f);
				else
					match_defuse_cancel(b);
			} else if (br->goal_kind != GOAL_RETAKE) {
				set_goal(b, nav_closest(v3_add(m->bomb_pos, v3(frand_range(-5, 5), 0, frand_range(-5, 5)))), GOAL_RETAKE);
			}
		} else if (br->goal_kind == GOAL_SITE && v3_length(v3_sub(b->body.position, br->goal)) < 1.5f && now() > br->hold_until) {
			br->hold_until = now() + frand_range(4, 9);
			set_goal(b, world_random_in_site(br->site), GOAL_SITE);
		}
	}
	// o assento do jogador no modo automatico constroi assim que pode, para mostrar que a construcao funciona
	if (br->autopilot && MODES[m->mode].building && m->phase == PHASE_LIVE && frand() < 0.02f) {
		const Placement p = build_placement(b, BUILD_PIECES_WALL, b->body.yaw, 0.0f);
		build_try(b, BUILD_PIECES_WALL, &p);
	}
	// portal: quem tem a Portal Gun abre um atalho para o objetivo uma vez por rodada
	if (MODES[m->mode].portals && b->has[4] && !br->portal_done && m->phase == PHASE_LIVE &&
		now() - m->round_start > MODES[m->mode].freeze_time + br->portal_delay && br->target < 0)
		portal_bot_open(b);
}

// ---------------------------------------------------------------- efeitos

static Fx *fx_new(void)
{
	Fx *oldest = &G.m.fx[0];
	for (int i = 0; i < MAX_FX; i++) {
		if (!G.m.fx[i].used)
			return &G.m.fx[i];
		if (G.m.fx[i].born < oldest->born)
			oldest = &G.m.fx[i];
	}
	return oldest;
}

void fx_tracer(Vec3 a, Vec3 b)
{
	netev_tracer(a, b);
	*fx_new() = (Fx){ true, FX_TRACER, a, b, { 0, 0, 0 }, { 1, 0.85f, 0.5f, 0.6f }, now(), 0.05f, 0.012f, 0, false };
}

void fx_flash(Vec3 at, bool viewmodel)
{
	*fx_new() = (Fx){ true, FX_FLASH, at, { 0, 0, 0 }, { 0, 0, 0 }, { 1, 0.7f, 0.3f, 1 }, now(), 0.045f, 0.14f * frand_range(0.8f, 1.2f),
		frand() * 6.28f, viewmodel };
}

static void particles(Vec3 p, const float c[4], int n);

void fx_impact(Vec3 p, Vec3 n, bool wood)
{
	netev_impact(p, n, wood);
	const float c[4] = { wood ? 0.35f : 0.1f, wood ? 0.22f : 0.08f, wood ? 0.1f : 0.06f, 1 };
	*fx_new() = (Fx){ true, FX_DECAL, v3_add(p, v3_scale(n, 0.01f)), n, { 0, 0, 0 }, { c[0], c[1], c[2], 1 }, now(), 12.0f, 0.06f, 0, false };
	const float puff[4] = { wood ? 0.6f : 0.75f, wood ? 0.42f : 0.65f, wood ? 0.22f : 0.5f, 1 };
	particles(p, puff, 6);
}

void fx_particles(Vec3 p, const float c[4], int n)
{
	netev_particles(p, c, n);
	particles(p, c, n);
}

static void particles(Vec3 p, const float c[4], int n)
{
	for (int i = 0; i < n; i++) {
		// para cima, num cone de 60 graus, de 1 a 3 m/s, como o CPUParticles3D do match.gd
		const float a = frand() * 6.28f, s = sinf(frand() * 1.047f);
		const Vec3 dir = v3_normalize(v3(cosf(a) * s, 1.0f, sinf(a) * s));
		*fx_new() = (Fx){ true, FX_PARTICLE, p, { 0, 0, 0 }, v3_scale(dir, frand_range(1.0f, 3.0f)), { c[0], c[1], c[2], 1 },
			now(), 0.5f, frand_range(0.03f, 0.06f), 0, false };
	}
}

void fx_shell(int kind, Vec3 at, const Xf *view)
{
	Shell *s = &G.m.shells[0];
	for (int i = 0; i < MAX_SHELLS; i++) {
		if (!G.m.shells[i].used) {
			s = &G.m.shells[i];
			break;
		}
		if (G.m.shells[i].born < s->born)
			s = &G.m.shells[i];
	}
	const Vec3 x = xf_col(view, 0), y = xf_col(view, 1), z = xf_col(view, 2);
	const Xf turn = xf_yaw(PI_F / 2, v3(0, 0, 0));
	Xf b = *view;
	b.m[3] = at.x;
	b.m[7] = at.y;
	b.m[11] = at.z;
	*s = (Shell){ true, kind, xf_mul(&b, &turn),
		v3_add(v3_add(v3_scale(x, frand_range(1.6f, 2.4f)), v3_scale(y, frand_range(1.0f, 1.8f))), v3_scale(z, frand_range(0.1f, 0.5f))),
		v3(frand_range(-20, 20), frand_range(-20, 20), frand_range(-20, 20)), now() };
}

static void billboard(FxVertex *v, Vec3 c, float size, float spin, const float col[4], const AtlasRect *r)
{
	const Xf *cam = NULL;
	(void)cam;
	// os eixos da camera saem da view (linhas 0 e 1)
	const Vec3 right = v3(G.cam.view.m[0], G.cam.view.m[4], G.cam.view.m[8]);
	const Vec3 up = v3(G.cam.view.m[1], G.cam.view.m[5], G.cam.view.m[9]);
	const float cs = cosf(spin) * size / 2, sn = sinf(spin) * size / 2;
	const Vec3 a = v3_add(v3_scale(right, cs), v3_scale(up, sn));
	const Vec3 b = v3_add(v3_scale(right, -sn), v3_scale(up, cs));
	const Vec3 q[4] = { v3_sub(v3_sub(c, a), b), v3_sub(v3_add(c, a), b), v3_add(v3_add(c, a), b), v3_add(v3_sub(c, a), b) };
	float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
	if (r) {
		u0 = (float)r->x / G.as.atlas_w;
		v0 = (float)r->y / G.as.atlas_h;
		u1 = (float)(r->x + r->w) / G.as.atlas_w;
		v1 = (float)(r->y + r->h) / G.as.atlas_h;
	}
	const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v1, v1, v0, v0 };
	static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++) {
		const int j = idx[i];
		v[i] = (FxVertex){ q[j].x, q[j].y, q[j].z, col[0], col[1], col[2], col[3], us[j], vs[j] };
	}
}

static void fx_draw(bool viewmodel)
{
	static FxVertex add[MAX_FX * 6], solid[MAX_FX * 6];
	uint32_t na = 0, ns = 0;
	const AtlasRect *flash = assets_rect(&G.as, "flash"), *white = assets_rect(&G.as, "white"),
					*disc = assets_rect(&G.as, "disc");
	const float t = now();
	for (int i = 0; i < MAX_FX; i++) {
		Fx *f = &G.m.fx[i];
		if (!f->used)
			continue;
		const float age = t - f->born;
		if (age > f->life) {
			f->used = false;
			continue;
		}
		if (f->viewmodel != viewmodel)
			continue;
		switch (f->kind) {
		case FX_TRACER: {
			// uma fita fina virada para a camera
			const Vec3 d = v3_sub(f->b, f->a);
			const Vec3 to_cam = v3_sub(G.cam.eye, f->a);
			const Vec3 side = v3_scale(v3_normalize(v3_cross(d, to_cam)), f->size);
			const Vec3 q[4] = { v3_sub(f->a, side), v3_add(f->a, side), v3_add(f->b, side), v3_sub(f->b, side) };
			static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
			for (int k = 0; k < 6; k++)
				add[na++] = (FxVertex){ q[idx[k]].x, q[idx[k]].y, q[idx[k]].z, f->color[0], f->color[1], f->color[2], f->color[3],
					(float)(white->x + 4) / G.as.atlas_w, (float)(white->y + 4) / G.as.atlas_h };
			break;
		}
		case FX_FLASH:
			billboard(add + na, f->a, f->size, f->spin, f->color, flash);
			na += 6;
			break;
		case FX_EXPLOSION: {
			const float k = 1.0f - age / f->life;
			const float c[4] = { f->color[0], f->color[1], f->color[2], k };
			billboard(add + na, f->a, 6.0f + age * 10.0f, 0, c, flash);
			na += 6;
			break;
		}
		case FX_PARTICLE: {
			const Vec3 p = v3_add(f->a, v3_add(v3_scale(f->vel, age), v3(0, -4.9f * age * age, 0)));
			billboard(solid + ns, p, f->size, 0, f->color, disc);
			ns += 6;
			break;
		}
		case FX_DECAL: {
			// um quadradinho escuro deitado na superficie
			const Vec3 n = f->b;
			const Vec3 u = v3_normalize(fabsf(n.y) < 0.99f ? v3_cross(v3(0, 1, 0), n) : v3(1, 0, 0));
			const Vec3 w = v3_cross(n, u);
			const float s = f->size / 2;
			const Vec3 q[4] = { v3_add(f->a, v3_add(v3_scale(u, -s), v3_scale(w, -s))), v3_add(f->a, v3_add(v3_scale(u, s), v3_scale(w, -s))),
				v3_add(f->a, v3_add(v3_scale(u, s), v3_scale(w, s))), v3_add(f->a, v3_add(v3_scale(u, -s), v3_scale(w, s))) };
			static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
			for (int k = 0; k < 6; k++)
				solid[ns++] = (FxVertex){ q[idx[k]].x, q[idx[k]].y, q[idx[k]].z, f->color[0], f->color[1], f->color[2], 1,
					(float)(white->x + 4) / G.as.atlas_w, (float)(white->y + 4) / G.as.atlas_h };
			break;
		}
		}
	}
	gfx_draw_fx(solid, ns, G.atlas_tex, GFX_ALPHA_TEST | GFX_UNLIT);
	gfx_draw_fx(add, na, G.atlas_tex, GFX_ADD | (viewmodel ? GFX_NO_DEPTH : 0));
}

static void shells_update(float dt)
{
	for (int i = 0; i < MAX_SHELLS; i++) {
		Shell *s = &G.m.shells[i];
		if (!s->used)
			continue;
		if (now() - s->born > 4.0f) {
			s->used = false;
			continue;
		}
		s->vel.y -= 9.8f * dt;
		const Vec3 p = xf_origin(&s->xf);
		const Vec3 np = v3_add(p, v3_scale(s->vel, dt));
		const RayHit h = world_ray(p, np, RAY_WORLD | RAY_CLIP | RAY_PIECES, -1);
		if (h.hit) {
			// quica e para
			s->vel = v3_scale(v3_sub(s->vel, v3_scale(h.normal, 1.6f * v3_dot(s->vel, h.normal))), 0.35f);
			s->spin = v3_scale(s->spin, 0.5f);
			s->xf.m[3] = h.point.x + h.normal.x * 0.01f;
			s->xf.m[7] = h.point.y + h.normal.y * 0.01f;
			s->xf.m[11] = h.point.z + h.normal.z * 0.01f;
		} else {
			s->xf.m[3] = np.x;
			s->xf.m[7] = np.y;
			s->xf.m[11] = np.z;
		}
		const Xf rx = xf_rot_x(s->spin.x * dt), rz = xf_rot_z(s->spin.z * dt);
		Xf r = xf_mul(&s->xf, &rx);
		s->xf = xf_mul(&r, &rz);
	}
}

static void shells_draw(void)
{
	static const char *names[3] = { "shell_9mm", "shell_762", "shell_50cal" };
	static const float lengths[3] = { 0.021f, 0.039f, 0.07f };
	const float tint[4] = { 1, 1, 1, 1 };
	for (int i = 0; i < MAX_SHELLS; i++) {
		const Shell *s = &G.m.shells[i];
		if (!s->used)
			continue;
		// as capsulas do CS2 saem com ~2 m: a escala do calibre de verdade
		const Model *m = assets_model(&G.as, names[s->model]);
		float size = 0.0f;
		if (m)
			for (uint32_t k = 0; k < m->vertex_count; k++)
				size = fmaxf(size, fabsf(m->pos[k * 3 + 2]) * 2.0f);
		const Xf sc = xf_scale(size > 0 ? lengths[s->model] / size : 0.01f);
		const Xf x = xf_mul(&s->xf, &sc);
		rigid_draw(names[s->model], &x, tint);
	}
}

// ---------------------------------------------------------------- quadro

void match_update(float dt)
{
	Match *m = &G.m;
	if (!m->active)
		return;
	if (G.net_role == NET_ROLE_CLIENT) {
		// a partida e do host: aqui so o proprio movimento, as animacoes e os efeitos
		netgame_update(dt);
		if (m->player >= 0) {
			Fighter *me = &m->f[m->player];
			if (!G.paused && !m->buy_open && !m->tacmap_open)
				netcl_choices(&G.act);
			player_update(me, dt);
		}
		for (int i = 0; i < m->count; i++)
			fighter_update_anim(&m->f[i], dt);
		anim_update(&G.vm_anim, dt);
		shells_update(dt);
		if (m->hurt > 0)
			m->hurt = fmaxf(0, m->hurt - dt * 1.5f);
		netgame_flush();
		return;
	}
	if (G.net_role == NET_ROLE_HOST)
		netgame_update(dt);
	const float t = now();
	switch (m->phase) {
	case PHASE_FREEZE:
		if (t >= m->phase_end) {
			m->phase = PHASE_LIVE;
			m->phase_end = t + MODES[m->mode].round_time;
			for (int i = 0; i < m->count; i++)
				m->f[i].frozen = false;
		}
		break;
	case PHASE_LIVE:
		if (t >= m->phase_end)
			end_round(TEAM_CT, "Target saved");
		break;
	case PHASE_PLANTED:
		if (t >= m->next_beep) {
			const float left = (float)(m->bomb_explode_at - t);
			m->next_beep = t + clampf(left / 40.0f, 0.12f, 1.0f);
			sound_at("snd_c4_beep", m->bomb_pos, -4);
		}
		if (t >= m->bomb_explode_at)
			explode();
		break;
	case PHASE_OVER:
		if (t >= m->phase_end)
			next_round();
		break;
	case PHASE_MATCH_OVER:
		if (t >= m->phase_end) {
			if (G.autoplay)
				restart_match();
			else {
				match_end();
				menu_enter();
				return;
			}
		}
		break;
	}
	check_bomb_pickup();
	for (int i = 0; i < m->count; i++) {
		Fighter *f = &m->f[i];
		if (f->is_player)
			player_update(f, dt);
		else if (f->remote >= 0)
			netgame_remote(f, dt);
		else
			bot_update(f, dt);
		fighter_update_anim(f, dt);
	}
	// a arma em primeira pessoa anda no mesmo tempo do jogo (sem isto ela ficava no primeiro quadro do saque)
	anim_update(&G.vm_anim, dt);
	if (MODES[m->mode].portals)
		portals_update();
	shells_update(dt);
	if (m->hurt > 0)
		m->hurt = fmaxf(0, m->hurt - dt * 1.5f);
	if (G.net_role == NET_ROLE_HOST)
		netgame_flush();
}

void match_draw(void)
{
	Match *m = &G.m;
	build_update_and_draw(G.dt);
	for (int i = 0; i < m->count; i++)
		fighter_draw(&m->f[i]);
	const float tint[4] = { 1, 1, 1, 1 };
	if (m->bomb_planted || m->bomb_dropped) {
		const Vec3 p = m->bomb_planted ? m->bomb_pos : m->bomb_drop_pos;
		const Xf x = xf_translate(v3_add(p, v3(0, 0.05f, 0)));
		rigid_draw("w_c4", &x, tint);
		if (m->bomb_planted && fmodf(now(), 1.0f) < 0.15f) {
			const float red[4] = { 1, 0.1f, 0.1f, 0.9f };
			static FxVertex v[6];
			billboard(v, v3_add(p, v3(0, 0.2f, 0)), 0.25f, 0, red, assets_rect(&G.as, "flash"));
			gfx_draw_fx(v, 6, G.atlas_tex, GFX_ADD);
		}
	}
	shells_draw();
	if (MODES[m->mode].portals)
		portals_draw();
	fx_draw(false);
	if (m->player >= 0 && m->f[m->player].alive)
		build_draw_ghost(&m->f[m->player]);
}

void match_draw_viewmodel(void)
{
	Match *m = &G.m;
	if (m->player >= 0 && m->f[m->player].alive) {
		player_draw_viewmodel(&m->f[m->player]);
		fx_draw(true);
	}
}
