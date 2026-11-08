// Lutador, jogador e bot: fighter.gd, player.gd, bot.gd e viewmodel.gd em C.
#include <stdlib.h>
#include <string.h>

#include "game.h"

static const float GRAVITY = 800.0f * U2M;
static const float JUMP_SPEED = 301.993f * U2M;
static const float EYE = 1.64f;
static const float EYE_CROUCH = 1.18f;
static const float TURBO = 0.08f;
static const float EDIT_RANGE = 7.0f;

int weapon_index(const char *id)
{
	for (int i = 0; i < WEAPONS_COUNT; i++)
		if (!strcmp(WEAPONS[i].id, id))
			return i;
	return -1;
}

static int skill_index(int d)
{
	return d >= 0 && d < BOT_SKILLS_COUNT ? d : BOT_SKILLS_NORMAL;
}

static float now(void)
{
	return (float)G.time;
}

static float angle_diff(float a, float b)
{
	float d = fmodf(b - a + PI_F, 2 * PI_F);
	if (d < 0)
		d += 2 * PI_F;
	return d - PI_F;
}

static float lerp_angle(float a, float b, float t)
{
	return a + angle_diff(a, b) * t;
}

// ---------------------------------------------------------------- armas e compra

void fighter_give(Fighter *f, int w)
{
	if (w < 0)
		return;
	const int s = WEAPONS[w].slot;
	f->has[s] = true;
	f->guns[s] = (Gun){ w, WEAPONS[w].mag, WEAPONS[w].reserve };
}

void fighter_reset_loadout(Fighter *f)
{
	memset(f->has, 0, sizeof f->has);
	fighter_give(f, WEAPONS_KNIFE);
	fighter_give(f, weapon_index(TEAMS[f->team].default_pistol));
	f->slot = 2;
	f->armor = 0;
	f->helmet = false;
	f->kit = false;
}

Gun *fighter_gun(Fighter *f)
{
	return f->slot >= 1 && f->slot <= 4 && f->has[f->slot] ? &f->guns[f->slot] : NULL;
}

const WeaponsDef *fighter_weapon(const Fighter *f)
{
	if (f->slot >= 1 && f->slot <= 4 && f->has[f->slot])
		return &WEAPONS[f->guns[f->slot].id];
	return &WEAPONS[WEAPONS_KNIFE];
}

static void refresh_view(Fighter *f);

void fighter_switch(Fighter *f, int s)
{
	if (s == 5) {
		if (!f->bomb)
			return;
		if (f->slot != 5)
			f->last_slot = f->slot;
		f->slot = 5;
		refresh_view(f);
		return;
	}
	if (s < 1 || s > 4 || !f->has[s] || s == f->slot)
		return;
	f->last_slot = f->slot;
	f->slot = s;
	f->next_fire = fmax(f->next_fire, now() + 0.25);
	f->reload_until = 0;
	f->reload_gun = NULL;
	refresh_view(f);
}

float fighter_max_speed(const Fighter *f)
{
	if (f->slot == 5)
		return 250.0f * U2M;
	return fighter_weapon(f)->move_speed * U2M;
}

bool fighter_buy(Fighter *f, const char *id)
{
	int price = 0;
	const int w = weapon_index(id);
	int e = -1;
	if (w >= 0) {
		const WeaponsDef *d = &WEAPONS[w];
		price = d->price;
		if (strcmp(d->team, "any") && strcmp(d->team, TEAMS[f->team].id))
			return false;
		if (strcmp(d->only_mode, "any") && strcmp(d->only_mode, MODES[G.m.mode].id))
			return false;
		if (f->has[d->slot] && f->guns[d->slot].id == w)
			return false;
	} else {
		for (int i = 0; i < EQUIPMENT_COUNT; i++)
			if (!strcmp(EQUIPMENT[i].id, id))
				e = i;
		if (e < 0)
			return false;
		price = EQUIPMENT[e].price;
		if (strcmp(EQUIPMENT[e].team, "any") && strcmp(EQUIPMENT[e].team, TEAMS[f->team].id))
			return false;
		if (e == EQUIPMENT_DEFUSER && f->kit)
			return false;
		if ((e == EQUIPMENT_KEVLAR || e == EQUIPMENT_KEVLAR_HELMET) && f->armor >= 100 && (f->helmet || e == EQUIPMENT_KEVLAR))
			return false;
	}
	if (f->money < price)
		return false;
	f->money -= price;
	if (w >= 0) {
		fighter_give(f, w);
		fighter_switch(f, WEAPONS[w].slot);
	} else if (e == EQUIPMENT_KEVLAR) {
		f->armor = 100;
	} else if (e == EQUIPMENT_KEVLAR_HELMET) {
		f->armor = 100;
		f->helmet = true;
	} else if (e == EQUIPMENT_DEFUSER) {
		f->kit = true;
	}
	return true;
}

// ---------------------------------------------------------------- tiro

Vec3 fighter_eye(const Fighter *f)
{
	return v3(f->body.position.x, f->body.position.y + (f->body.crouching ? EYE_CROUCH : EYE), f->body.position.z);
}

Vec3 fighter_aim(const Fighter *f)
{
	return look_dir(f->body.yaw, f->body.pitch);
}

static void play_anim(Fighter *f, int kind);

static void trace(Fighter *f, Vec3 from, Vec3 dir, float range, int w, int depth)
{
	const Vec3 to = v3_add(from, v3_scale(dir, range));
	RayHit hit = world_ray(from, to, RAY_WORLD | RAY_PIECES | RAY_FIGHTERS, depth == 0 ? f->index : -1);
	const Vec3 end = hit.point;
	const Vec3 tracer_from = depth == 0 ? v3_add(v3_add(from, v3_scale(dir, 0.6f)), v3(0, -0.08f, 0)) : from;
	// um portal no caminho leva a bala para fora do gemeo
	if (MODES[G.m.mode].portals && depth < 3 && range > 0.5f) {
		Vec3 p;
		Xf through;
		int portal;
		if (portal_on_segment(from, end, &p, &through, &portal)) {
			if (w != WEAPONS_KNIFE)
				fx_tracer(tracer_from, p);
			const Vec3 nd = v3_normalize(xf_dir(&through, dir));
			trace(f, v3_add(xf_point(&through, p), v3_scale(nd, 0.02f)), nd, range - v3_length(v3_sub(p, from)), w,
				depth + 1);
			return;
		}
	}
	if (hit.hit) {
		if (hit.kind == HIT_FIGHTER) {
			Fighter *c = &G.m.f[hit.index];
			const float local_y = hit.point.y - c->body.position.y;
			const float head_y = c->body.crouching ? 0.98f : 1.42f;
			int part = 1; // peito
			if (local_y > head_y)
				part = 0;
			else if (local_y < 0.75f)
				part = 3;
			else if (local_y < 1.05f)
				part = 2;
			fighter_take_hit(c, f, w, part);
			const float blood[4] = { 0.5f, 0.05f, 0.03f, 1 };
			fx_particles(hit.point, blood, 10);
		} else if (hit.kind == HIT_PIECE) {
			build_damage(hit.index, WEAPONS[w].damage);
			fx_impact(hit.point, hit.normal, true);
		} else {
			fx_impact(hit.point, hit.normal, false);
		}
	}
	if (w != WEAPONS_KNIFE)
		fx_tracer(tracer_from, end);
}

static bool can_fire(Fighter *f)
{
	if (!f->alive || f->frozen || f->slot == 5)
		return false;
	Gun *g = fighter_gun(f);
	if (!g || now() < f->next_fire || now() < f->reload_until)
		return false;
	if (WEAPONS[g->id].mag > 0 && g->mag <= 0) {
		fighter_start_reload(f);
		return false;
	}
	return true;
}

static void on_fired(Fighter *f, const WeaponsDef *d);

void fighter_fire(Fighter *f)
{
	if (!can_fire(f))
		return;
	Gun *g = fighter_gun(f);
	const WeaponsDef *d = &WEAPONS[g->id];
	f->next_fire = now() + d->fire_interval;
	if (d->mag > 0)
		g->mag--;
	if (g->id == WEAPONS_PORTALGUN) {
		portal_fire(f);
		sound_at(d->sound_asset, fighter_eye(f), 0);
		on_fired(f, d);
		return;
	}
	float spread = d->spread + f->recoil * 0.004f;
	const float hv = sqrtf(f->body.velocity.x * f->body.velocity.x + f->body.velocity.z * f->body.velocity.z);
	if (hv > 1.5f)
		spread += hv * 0.006f;
	if (!f->body.on_ground)
		spread += 0.06f;
	if (f->body.crouching)
		spread *= 0.75f;
	if (g->id == WEAPONS_AWP && !f->scoped)
		spread += 0.06f;
	Vec3 dir = fighter_aim(f);
	dir = v3_normalize(v3_add(dir, v3(frandn(spread), frandn(spread), frandn(spread))));
	f->recoil = fminf(f->recoil + d->recoil, 14.0f);
	trace(f, fighter_eye(f), dir, d->range_m, g->id, 0);
	sound_at(d->sound_asset, fighter_eye(f), 0);
	play_anim(f, 2);
	on_fired(f, d);
}

void fighter_take_hit(Fighter *f, Fighter *attacker, int w, int part)
{
	if (!f->alive)
		return;
	const WeaponsDef *d = &WEAPONS[w];
	static const float mults[4] = { 0, 1.0f, 1.25f, 0.75f };
	float dmg = d->damage * (part == 0 ? d->head_mult : mults[part]);
	if (w == WEAPONS_KNIFE)
		dmg = d->damage;
	const bool armored = f->armor > 0 && (part != 0 || f->helmet) && part != 3;
	if (armored) {
		const float absorbed = dmg * (1.0f - d->armor_pen);
		f->armor = f->armor - (int)(absorbed * 0.5f);
		if (f->armor < 0)
			f->armor = 0;
		dmg *= d->armor_pen;
	}
	const int hp_loss = (int)lroundf(dmg);
	f->hp -= hp_loss;
	if (f->is_bot)
		bot_hurt(f, attacker);
	else if (f->is_player)
		G.m.hurt = fminf(1.0f, G.m.hurt + hp_loss / 60.0f);
	if (f->hp <= 0) {
		f->hp = 0;
		fighter_die(f, attacker, w, part == 0);
	}
}

void fighter_die(Fighter *f, Fighter *killer, int w, bool headshot)
{
	if (!f->alive)
		return;
	f->alive = false;
	f->deaths++;
	f->body.velocity = v3(0, 0, 0);
	f->anim.paused = true;
	match_died(f, killer, w, headshot);
}

void fighter_start_reload(Fighter *f)
{
	Gun *g = fighter_gun(f);
	if (!g)
		return;
	const WeaponsDef *d = &WEAPONS[g->id];
	if (d->mag == 0 || g->reserve <= 0 || g->mag >= d->mag || now() < f->reload_until)
		return;
	f->reload_until = now() + d->reload_time;
	f->reload_gun = g;
	if (G.m.player == f->index)
		player_viewmodel_play("reload");
}

static void reload_tick(Fighter *f)
{
	if (!f->reload_gun || now() < f->reload_until)
		return;
	Gun *g = f->reload_gun;
	f->reload_gun = NULL;
	if (!f->alive || fighter_gun(f) != g)
		return;
	const int need = WEAPONS[g->id].mag - g->mag;
	const int take = need < g->reserve ? need : g->reserve;
	g->mag += take;
	g->reserve -= take;
}

void fighter_respawn(Fighter *f, Vec3 at, float yaw)
{
	f->net_epoch++; // o cliente da rede pula para o lugar novo
	f->alive = true;
	f->hp = 100;
	memset(&f->body, 0, sizeof f->body);
	f->body.position = at;
	f->body.yaw = yaw;
	f->body.height = 1.83f;
	f->body.eye = EYE;
	f->body.on_ground = true;
	f->recoil = 0;
	f->reload_until = 0;
	f->reload_gun = NULL;
	f->topple = 0;
	f->anim.paused = false;
	f->scoped = false;
	f->build_piece = -1;
	f->editing = -1;
	f->edit_sel_count = 0;
	bool any = false;
	for (int s = 1; s <= 4; s++)
		any |= f->has[s];
	if (!any)
		fighter_reset_loadout(f);
	if (f->slot < 1 || f->slot > 4 || !f->has[f->slot])
		f->slot = 2;
	for (int s = 1; s <= 4; s++)
		if (f->has[s]) {
			f->guns[s].mag = WEAPONS[f->guns[s].id].mag;
			f->guns[s].reserve = WEAPONS[f->guns[s].id].reserve;
		}
	f->anim_kind = -1;
	play_anim(f, 0);
	refresh_view(f);
}

// ---------------------------------------------------------------- modelo em terceira pessoa

static const char *const ANIM_KIND[3] = { "idle", "run", "fire" };

static void play_anim(Fighter *f, int kind)
{
	if (kind == f->anim_kind && kind != 2)
		return;
	char name[64];
	snprintf(name, sizeof name, "agent_%s.%s", f->team ? "t" : "ct", ANIM_KIND[kind]);
	const Anim *a = assets_anim(&G.as, name);
	if (!a)
		return;
	f->anim_kind = kind;
	anim_play(&f->anim, a, 0.15f);
}

void fighter_update_anim(Fighter *f, float dt)
{
	if (f->alive) {
		const float hv = sqrtf(f->body.velocity.x * f->body.velocity.x + f->body.velocity.z * f->body.velocity.z);
		if (f->anim_kind != 2 || anim_done(&f->anim))
			play_anim(f, hv > 1.0f ? 1 : 0);
		anim_update(&f->anim, dt);
	} else if (f->topple < 1.0f) {
		// sem clipe de morte no CS2: o agente tomba, como o fighter.gd
		f->topple = fminf(1.0f, f->topple + dt / 0.45f);
	}
}

static Vec3 grip_offset(int slot)
{
	switch (slot) {
	case 1:
		return v3(-0.1f, 1.29f, 0.02f);
	case 2:
		return v3(-0.05f, 1.33f, 0.38f);
	case 3:
		return v3(-0.14f, 1.22f, 0.32f);
	}
	return v3(-0.1f, 1.29f, 0.1f);
}

void fighter_draw(Fighter *f)
{
	if (!f->used || (f->alive && G.m.player == f->index))
		return;
	char name[32];
	snprintf(name, sizeof name, "agent_%s", f->team ? "t" : "ct");
	const Model *m = assets_model(&G.as, name);
	if (!m)
		return;
	if (f->skin.model != m) {
		skin_free(&f->skin);
		if (!skin_init(&f->skin, m))
			return;
	}
	// o corpo olha para -Z; os modelos do Source para +Z (fighter.gd: model.rotation.y = PI)
	const float k = f->topple * f->topple;
	const Xf body = xf_yaw(f->body.yaw, v3(f->body.position.x, f->body.position.y + 0.15f * k, f->body.position.z));
	const Xf turn = xf_yaw(PI_F, v3(0, 0, 0));
	const Xf tip = xf_rot_x(-PI_F / 2 * k);
	const Xf a = xf_mul(&body, &tip);
	const Xf world = xf_mul(&a, &turn);
	skin_pose(&f->skin, &f->anim);
	const float tint[4] = { 1, 1, 1, 1 };
	skin_draw(&f->skin, &world, tint);
	if (f->alive && f->slot != 5) {
		const Gun *g = f->slot >= 1 && f->slot <= 4 && f->has[f->slot] ? &f->guns[f->slot] : NULL;
		if (g) {
			char wn[32];
			snprintf(wn, sizeof wn, "w_%s", WEAPONS[g->id].id);
			const Xf grip = xf_translate(grip_offset(f->slot));
			Xf held = xf_mul(&world, &grip);
			if (g->id == WEAPONS_PORTALGUN) {
				const Xf r = xf_yaw(PI_F, v3(0, 0, 0));
				held = xf_mul(&held, &r);
			}
			rigid_draw(wn, &held, tint);
		}
	}
}

// ---------------------------------------------------------------- viewmodel (primeira pessoa)

static const char *vm_set(const Fighter *f)
{
	if (f->slot == 5)
		return "c4";
	return fighter_weapon(f)->anim_set;
}

static Skin *vm_skin(const char *set, int team)
{
	char name[48];
	snprintf(name, sizeof name, "vm_%s_%s", set, team ? "t" : "ct");
	const Model *m = assets_model(&G.as, name);
	if (!m)
		return NULL;
	for (int i = 0; i < 16; i++) {
		Skin *s = &G.vm[team][i];
		if (s->model == m)
			return s;
		if (!s->model) {
			return skin_init(s, m) ? s : NULL;
		}
	}
	return NULL;
}

void player_viewmodel_play(const char *clip)
{
	if (G.m.player < 0)
		return;
	const Fighter *f = &G.m.f[G.m.player];
	char name[64];
	snprintf(name, sizeof name, "vm_%s_%s.%s", vm_set(f), f->team ? "t" : "ct", clip);
	const Anim *a = assets_anim(&G.as, name);
	if (!a)
		return;
	anim_play(&G.vm_anim, a, 0.04f);
	if (strcmp(clip, "idle")) {
		snprintf(name, sizeof name, "vm_%s_%s.idle", vm_set(f), f->team ? "t" : "ct");
		anim_queue(&G.vm_anim, assets_anim(&G.as, name));
	}
}

static void refresh_view(Fighter *f)
{
	if (G.m.player != f->index)
		return;
	f->scoped = false;
	player_viewmodel_play("draw");
}

// A camera do jogador no mundo: colunas direita, cima, tras; origem no olho.
static Xf camera_xf(const Fighter *f)
{
	const float cy = cosf(f->body.yaw), sy = sinf(f->body.yaw), cp = cosf(f->body.pitch), sp = sinf(f->body.pitch);
	return xf_basis(v3(cy, 0, -sy), v3(sy * sp, cp, cy * sp), v3(sy * cp, -sp, cy * cp), bs_player_eye(&f->body));
}

void player_draw_viewmodel(Fighter *f)
{
	if (!f->alive || f->scoped || f->build_piece >= 0 || f->editing >= 0)
		return;
	const Xf cam = camera_xf(f);
	const float hv = fminf(sqrtf(f->body.velocity.x * f->body.velocity.x + f->body.velocity.z * f->body.velocity.z), 6.0f);
	const Vec3 bob = v3(sinf(f->bob) * 0.006f * hv / 6.0f, fabsf(cosf(f->bob)) * 0.006f * hv / 6.0f, f->kick);
	const float tint[4] = { 1, 1, 1, 1 };
	if (f->slot == 4 && f->has[4] && f->guns[4].id == WEAPONS_PORTALGUN) {
		const Xf off = xf_translate(v3_add(v3(0.2f, -0.21f, -0.6f), bob));
		Xf w = xf_mul(&cam, &off);
		const Xf rx = xf_rot_x(0.04f), ry = xf_yaw(0.06f, v3(0, 0, 0)), sc = xf_scale(0.85f);
		w = xf_mul(&w, &ry);
		w = xf_mul(&w, &rx);
		w = xf_mul(&w, &sc);
		rigid_draw("w_portalgun", &w, tint);
		return;
	}
	Skin *s = vm_skin(vm_set(f), f->team);
	if (!s)
		return;
	if (!G.vm_anim.cur || strncmp(G.vm_anim.cur->name, s->model->name, strlen(s->model->name)))
		player_viewmodel_play("draw");
	const Xf off = xf_translate(bob);
	const Xf world = xf_mul(&cam, &off);
	skin_pose(s, &G.vm_anim);
	skin_draw(s, &world, tint);
}

static Vec3 vm_point(const Fighter *f, const char *name)
{
	const Xf cam = camera_xf(f);
	Skin *s = vm_skin(vm_set(f), f->team);
	bool found = false;
	Vec3 p = s ? skin_attach(s, name, &cam, &found) : v3(0, 0, 0);
	if (!found)
		p = xf_point(&cam, v3(0.1f, -0.1f, -0.5f));
	return p;
}

static void on_fired(Fighter *f, const WeaponsDef *d)
{
	if (G.m.player != f->index)
		return;
	player_viewmodel_play("shoot");
	const int w = (int)(d - WEAPONS);
	if (w != WEAPONS_KNIFE && w != WEAPONS_PORTALGUN) {
		fx_flash(vm_point(f, "muzzle"), true);
		if (strcmp(d->shell_asset, "none")) {
			const Xf cam = camera_xf(f);
			fx_shell(!strcmp(d->shell_asset, "shell_9mm") ? 0 : (!strcmp(d->shell_asset, "shell_762") ? 1 : 2),
				vm_point(f, "eject"), &cam);
		}
	}
	f->kick = 0.02f + d->recoil * 0.012f;
	f->body.pitch = clampf(f->body.pitch + d->recoil * 0.35f * PI_F / 180.0f, -1.553f, 1.553f);
	f->body.yaw += frand_range(-0.25f, 0.25f) * d->recoil * PI_F / 180.0f;
	if (w == WEAPONS_AWP)
		f->scoped = false;
}

// ---------------------------------------------------------------- o jogador

static int cycle(const Fighter *f, int d)
{
	static const int order[5] = { 1, 2, 3, 4, 5 };
	int i = 0;
	for (int k = 0; k < 5; k++)
		if (order[k] == f->slot)
			i = k;
	for (int k = 1; k < 6; k++) {
		const int s = order[(i + d * k + 10) % 5];
		if ((s <= 4 && f->has[s]) || (s == 5 && f->bomb))
			return s;
	}
	return f->slot;
}

static void cancel_edit(Fighter *f)
{
	f->editing = -1;
	f->edit_sel_count = 0;
}

static void select_slot(Fighter *f, int s)
{
	f->build_piece = -1;
	f->scoped = false;
	cancel_edit(f);
	fighter_switch(f, s);
}

static void toggle_edit(Fighter *f)
{
	if (f->editing >= 0) {
		Piece *p = &G.m.pieces[f->editing];
		if (p->used && !p->dying && p->team == f->team && build_apply_edit(p, f->edit_sel, f->edit_sel_count))
			sound_at("snd_knife", v3_scale(v3_add(p->lo, p->hi), 0.5f), -10);
		cancel_edit(f);
		return;
	}
	const Vec3 from = fighter_eye(f);
	const RayHit h = world_ray(from, v3_add(from, v3_scale(fighter_aim(f), EDIT_RANGE)), RAY_PIECES, -1);
	if (!h.hit || h.kind != HIT_PIECE || h.index < 0 || G.m.pieces[h.index].team != f->team)
		return;
	f->build_piece = -1;
	f->scoped = false;
	f->editing = h.index;
	f->edit_sel_count = 0;
	const Piece *p = &G.m.pieces[h.index];
	const int n = build_edit_grid(p->piece);
	for (int i = 0; i < n * n; i++)
		if (p->edits[i])
			f->edit_sel[f->edit_sel_count++] = i;
}

static bool sel_has(const Fighter *f, int t)
{
	for (int i = 0; i < f->edit_sel_count; i++)
		if (f->edit_sel[i] == t)
			return true;
	return false;
}

static void paint(Fighter *f, int t)
{
	if (t < 0)
		return;
	if (f->edit_paint && !sel_has(f, t) && f->edit_sel_count < 9)
		f->edit_sel[f->edit_sel_count++] = t;
	else if (!f->edit_paint && sel_has(f, t)) {
		for (int i = 0; i < f->edit_sel_count; i++)
			if (f->edit_sel[i] == t) {
				memmove(f->edit_sel + i, f->edit_sel + i + 1, (size_t)(f->edit_sel_count - i - 1) * sizeof(int));
				f->edit_sel_count--;
				break;
			}
	}
}

// Armas, recarga, bomba, construcao e edicao: do jogador local e, no host, dos jogadores remotos (os comandos que
// chegam pela rede viram o mesmo Actions).
static void player_choices(Fighter *f, const Actions *a, bool local)
{
	if (a->slot_pressed)
		select_slot(f, a->slot_pressed == 4 && !f->has[4] ? 5 : a->slot_pressed);
	if (a->next_weapon)
		select_slot(f, cycle(f, 1));
	if (a->prev_weapon && f->build_piece < 0)
		select_slot(f, cycle(f, -1));
	if (a->reload && !(G.m.bomb_planted && f->team == TEAM_CT && v3_length(v3_sub(f->body.position, G.m.bomb_pos)) < 1.8f))
		fighter_start_reload(f);
	if (a->drop_pressed && f->bomb && f->slot == 5)
		match_drop_bomb(f);
	if (build_enabled()) {
		if (a->build_select >= 0) {
			cancel_edit(f);
			f->build_piece = a->build_select;
			f->scoped = false;
		} else if (a->build_cycle) {
			cancel_edit(f);
			f->build_piece = f->build_piece < 0 ? BUILD_PIECES_WALL : (f->build_piece + 1) % BUILD_PIECES_COUNT;
			f->scoped = false;
		}
		if (a->edit_pressed)
			toggle_edit(f);
	}
	if (a->alt_pressed) {
		if (f->editing >= 0) {
			Piece *p = &G.m.pieces[f->editing];
			if (p->used && p->team == f->team)
				build_reset_edit(p);
			cancel_edit(f);
		} else if (f->build_piece >= 0 || a->material_cycle) {
			f->build_mat = (f->build_mat + 1) % BUILD_MATERIALS_COUNT;
			if (local)
				sound_ui("snd_knife", -20);
		} else if (f->slot == 4 && f->has[4] && f->guns[4].id == WEAPONS_PORTALGUN) {
			if (local)
				tacmap_open(); // o remoto abre o mapa na maquina dele
		} else if (fighter_weapon(f) == &WEAPONS[WEAPONS_AWP]) {
			f->scoped = !f->scoped;
		}
	} else if (a->material_cycle && f->build_piece >= 0) {
		f->build_mat = (f->build_mat + 1) % BUILD_MATERIALS_COUNT;
		if (local)
			sound_ui("snd_knife", -20);
	}
}

static void player_fire(Fighter *f, const Actions *a, float dt)
{
	if (f->build_piece >= 0) {
		if (a->fire && now() >= f->next_build) {
			const Placement p = build_placement(f, f->build_piece, NAN, NAN);
			build_try(f, f->build_piece, &p);
			f->next_build = now() + (a->fire_pressed ? TURBO * 2.0f : TURBO);
		}
	} else if (f->editing >= 0) {
		const Piece *p = &G.m.pieces[f->editing];
		if (!p->used || p->dying || v3_length(v3_sub(v3_scale(v3_add(p->lo, p->hi), 0.5f), fighter_eye(f))) > EDIT_RANGE + 4.0f) {
			cancel_edit(f);
		} else if (a->fire) {
			const int t = build_tile_at(p, fighter_eye(f), fighter_aim(f));
			if (a->fire_pressed)
				f->edit_paint = !sel_has(f, t);
			paint(f, t);
		}
	} else if (f->slot == 5) {
		if (a->fire)
			match_plant_tick(f, dt);
		else
			match_plant_cancel(f);
	} else if (a->fire) {
		if (fighter_weapon(f)->auto_ || !f->held_fire)
			fighter_fire(f);
	}
	if (f->slot != 5 && !a->fire)
		match_plant_cancel(f);
	f->held_fire = a->fire;
	if (a->use)
		match_defuse_tick(f, dt);
	else
		match_defuse_cancel(f);
}

// ---------------------------------------------------------------- assistencia de mira (controle, so contra bots)

// O bot mais perto da mira dentro do cone (settings.aim_assist_fov graus), visivel, e o ponto do peito/cabeca dele.
// Devolve o peso (1 no centro da mira, 0 na borda do cone) ou 0 se nao ha alvo.
static float assist_target(const Fighter *f, Vec3 *point)
{
	const float cone = G.settings.aim_assist_fov * PI_F / 180.0f;
	if (cone <= 0.0f)
		return 0.0f;
	const Vec3 eye = fighter_eye(f), aim = fighter_aim(f);
	float best = cone;
	for (int i = 0; i < G.m.count; i++) {
		const Fighter *t = &G.m.f[i];
		if (t == f || !t->used || !t->alive || !t->is_bot || t->team == f->team)
			continue;
		const float h = t->body.crouching ? 0.85f : 1.3f; // peito, um pouco abaixo da cabeca
		const Vec3 p = v3(t->body.position.x, t->body.position.y + h, t->body.position.z);
		const Vec3 d = v3_sub(p, eye);
		const float dist = v3_length(d);
		if (dist < 0.5f || dist > 80.0f)
			continue;
		const float ang = acosf(clampf(v3_dot(aim, v3_scale(d, 1.0f / dist)), -1, 1));
		if (ang >= best)
			continue;
		if (world_ray(eye, p, RAY_WORLD | RAY_PIECES, -1).hit)
			continue;
		best = ang;
		*point = p;
	}
	return best < cone ? 1.0f - best / cone : 0.0f;
}

// Com um bot no cone: a mira desacelera em cima dele (fricção) e é puxada para ele (imã), as duas com a força
// settings.aim_assist. Devolve o fator da friccao para multiplicar o giro do analogico neste quadro.
static float aim_assist(Fighter *f, const Actions *a, float dt, bool stick_active)
{
	const float s = clampf(G.settings.aim_assist, 0.0f, 1.0f);
	if (s <= 0.0f || !a->pad_aim)
		return 1.0f;
	Vec3 p;
	const float w = assist_target(f, &p);
	if (w <= 0.0f)
		return 1.0f;
	// imã: mais forte mirando, atirando ou andando; parado, metade (para nao arrastar a camera sozinho)
	const float drive = (stick_active || a->fire || a->move_x != 0.0f || a->move_y != 0.0f) ? 1.0f : 0.5f;
	const float k = 1.0f - expf(-s * 10.0f * drive * (0.4f + 0.6f * w) * dt);
	const Vec3 d = v3_sub(p, fighter_eye(f));
	f->body.yaw = lerp_angle(f->body.yaw, atan2f(-d.x, -d.z), k);
	const float want_pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
	f->body.pitch = clampf(f->body.pitch + (want_pitch - f->body.pitch) * k, -1.553f, 1.553f);
	return 1.0f - s * w;
}

void player_update(Fighter *f, float dt)
{
	const Actions *a = &G.act;
	f->recoil = fmaxf(0.0f, f->recoil - dt * 18.0f);
	if (G.net_role != NET_ROLE_CLIENT)
		reload_tick(f);
	if (!f->alive)
		return;
	const bool blocked = G.m.buy_open || G.m.tacmap_open || G.paused;
	const bool busy = match_is_busy(f);
	// olhar
	if (!blocked) {
		const bool stick = a->look_x != 0.0f || a->look_y != 0.0f;
		const float s = G.settings.sensitivity / 1.6f * (f->scoped ? 0.3f : 1.0f) * aim_assist(f, a, dt, stick);
		f->body.yaw -= a->look_x * s;
		f->body.pitch = clampf(f->body.pitch + a->look_y * s, -1.553f, 1.553f);
	}
	// no cliente da rede, as escolhas vao para o host (netgame.c) e voltam no estado
	if (!blocked && G.net_role != NET_ROLE_CLIENT)
		player_choices(f, a, true);
	// movimento (o mesmo da etapa 1)
	BsInput in = { 0 };
	if (!blocked && !f->frozen && !busy) {
		in.move_x = a->move_x;
		in.move_y = a->move_y;
		in.jump = a->jump;
	}
	in.crouch = !blocked && a->crouch;
	in.walk = !blocked && a->walk;
	in.max_speed = fighter_max_speed(f);
	if (f->scoped)
		in.max_speed = fminf(in.max_speed, 150.0f * U2M);
	bs_player_update(&f->body, &G.col, &in, dt);
	// balanco e coice da arma
	const float hv = sqrtf(f->body.velocity.x * f->body.velocity.x + f->body.velocity.z * f->body.velocity.z);
	f->bob += dt * hv * 1.6f;
	f->kick += (0.0f - f->kick) * fminf(1.0f, dt * 12.0f);
	f->anim_kind = hv > 1.0f ? 1 : 0;
	if (blocked)
		return;
	if (G.net_role == NET_ROLE_CLIENT) {
		// so o efeito do tiro na hora (arma, brilho, som); o dano e do host
		const Gun *g = fighter_gun(f);
		if (a->fire && f->build_piece < 0 && f->editing < 0 && g && f->slot != 5 && !f->frozen && now() >= f->next_fire &&
			(WEAPONS[g->id].mag == 0 || g->mag > 0) && (WEAPONS[g->id].auto_ || !f->held_fire)) {
			const WeaponsDef *d = &WEAPONS[g->id];
			f->next_fire = now() + d->fire_interval;
			sound_at(d->sound_asset, fighter_eye(f), 0);
			on_fired(f, d);
		}
		f->held_fire = a->fire;
		return;
	}
	player_fire(f, a, dt);
}

// Uma escolha que chegou pela rede (arma, recarga, construcao...), aplicada ao jogador remoto.
void player_remote_choice(Fighter *f, const Actions *a)
{
	if (f->alive)
		player_choices(f, a, false);
}

void player_remote(Fighter *f, const Actions *a, float dt)
{
	f->recoil = fmaxf(0.0f, f->recoil - dt * 18.0f);
	reload_tick(f);
	if (!f->alive)
		return;
	player_choices(f, a, false);
	const float hv = sqrtf(f->body.velocity.x * f->body.velocity.x + f->body.velocity.z * f->body.velocity.z);
	f->anim_kind = hv > 1.0f ? 1 : 0;
	player_fire(f, a, dt);
}

// ---------------------------------------------------------------- bots

void bot_setup(Fighter *f, int skill)
{
	f->is_bot = true;
	f->bot.skill = skill_index(skill);
	f->bot.target = -1;
	f->bot.portal_goal = -1;
	f->bot.target_portal = -1;
}

void bot_round_start(Fighter *f)
{
	Brain *b = &f->bot;
	b->site = f->team == TEAM_T ? G.m.plan_site : 0;
	if (f->team == TEAM_CT) {
		int k = 0;
		for (int i = 0; i < G.m.count; i++) {
			if (&G.m.f[i] == f)
				break;
			if (G.m.f[i].team == TEAM_CT)
				k++;
		}
		b->site = k % 2;
	}
	b->target = -1;
	b->path_count = 0;
	b->goal_kind = GOAL_SITE;
	b->hold_until = 0;
	b->goal = world_random_in_site(b->site);
	b->next_repath = 0;
	b->portal_goal = -1;
	b->target_portal = -1;
	b->portal_delay = frand_range(3.0f, 9.0f);
	b->portal_done = false;
	b->last_pos = f->body.position;
}

void bot_buy(Fighter *f)
{
	const char *rifle = f->team == TEAM_T ? "ak47" : "m4a1s";
	if (f->money >= 4750 + 1000 && frand() < 0.18f)
		fighter_buy(f, "awp");
	else if (f->money >= WEAPONS[weapon_index(rifle)].price + 650)
		fighter_buy(f, rifle);
	if (MODES[G.m.mode].portals && f->money >= WEAPONS[WEAPONS_PORTALGUN].price + 650 && frand() < 0.5f)
		if (fighter_buy(f, "portalgun"))
			plat_log("portal: %s comprou a Portal Gun", f->name);
	if (f->money >= 1000)
		fighter_buy(f, "kevlar_helmet");
	else if (f->money >= 650)
		fighter_buy(f, "kevlar");
	if (f->team == TEAM_CT && f->money >= 400 && frand() < 0.6f)
		fighter_buy(f, "defuser");
	for (int s = 1; s <= 3; s++)
		if (f->has[s]) {
			f->slot = s;
			break;
		}
}

static bool can_see(const Fighter *f, const Fighter *t)
{
	static const float hs[2] = { 1.45f, 1.0f };
	for (int i = 0; i < 2; i++) {
		const float h = t->body.crouching ? hs[i] * 0.7f : hs[i];
		const Vec3 to = v3(t->body.position.x, t->body.position.y + h, t->body.position.z);
		const RayHit r = world_ray(fighter_eye(f), to, RAY_WORLD | RAY_PIECES | RAY_FIGHTERS, f->index);
		if (!r.hit || (r.kind == HIT_FIGHTER && &G.m.f[r.index] == t))
			return true;
	}
	return false;
}

static Vec3 aim_point(const Fighter *f)
{
	const Brain *b = &f->bot;
	const Fighter *t = &G.m.f[b->target];
	Vec3 p = v3(t->body.position.x, t->body.position.y + (t->body.crouching ? b->aim_height * 0.7f : b->aim_height),
		t->body.position.z);
	if (b->target_portal >= 0 && portal_linked(b->target_portal)) {
		const Xf th = portal_through(&G.m.portals[G.m.portals[b->target_portal].twin]);
		p = xf_point(&th, p);
	}
	return p;
}

// Um inimigo visto atraves de um par de portais que o bot enxerga.
static int spot_through_portals(const Fighter *f, float *dist, int *via)
{
	const BotSkillsDef *sk = &BOT_SKILLS[f->bot.skill];
	const Vec3 eye = fighter_eye(f);
	const Vec3 fwd = v3(-sinf(f->body.yaw), 0, -cosf(f->body.yaw));
	int best = -1;
	float best_d = 1e30f;
	for (int i = 0; i < MAX_PORTALS; i++) {
		if (!portal_linked(i))
			continue;
		const Portal *p = &G.m.portals[i];
		const Vec3 pc = xf_origin(&p->xf), pn = portal_normal(p);
		const Vec3 to_p = v3_sub(pc, eye);
		const float dp = v3_length(to_p);
		if (dp > sk->see_range_m || v3_dot(v3_sub(eye, pc), pn) < 0.2f)
			continue;
		const Vec3 flat = v3_normalize(v3(to_p.x, 0, to_p.z));
		if (acosf(clampf(v3_dot(fwd, flat), -1, 1)) * 180.0f / PI_F > sk->fov_deg * 0.5f && dp > 3.0f)
			continue;
		if (world_ray(eye, v3_add(pc, v3_scale(pn, 0.05f)), RAY_WORLD | RAY_PIECES | RAY_FIGHTERS, f->index).hit)
			continue;
		const Portal *q = &G.m.portals[p->twin];
		const Vec3 qc = xf_origin(&q->xf), qn = portal_normal(q);
		const Vec3 from = v3_add(qc, v3_scale(qn, 0.1f));
		for (int k = 0; k < G.m.count; k++) {
			const Fighter *e = &G.m.f[k];
			if (e->team == f->team || !e->alive)
				continue;
			const Vec3 chest = v3(e->body.position.x, e->body.position.y + (e->body.crouching ? 0.85f : 1.2f), e->body.position.z);
			const float d = dp + v3_length(v3_sub(chest, from));
			if (d > sk->see_range_m || v3_dot(v3_sub(chest, qc), qn) < 0.1f || d >= best_d)
				continue;
			const RayHit r = world_ray(from, chest, RAY_WORLD | RAY_PIECES | RAY_FIGHTERS, -1);
			if (!r.hit || (r.kind == HIT_FIGHTER && r.index == k)) {
				best = k;
				best_d = d;
				*via = i;
			}
		}
	}
	*dist = best_d;
	return best;
}

static void think(Fighter *f)
{
	Brain *b = &f->bot;
	const BotSkillsDef *sk = &BOT_SKILLS[b->skill];
	int best = -1;
	float best_d = 1e30f;
	const Vec3 fwd = v3(-sinf(f->body.yaw), 0, -cosf(f->body.yaw));
	for (int i = 0; i < G.m.count; i++) {
		const Fighter *e = &G.m.f[i];
		if (e->team == f->team || !e->alive)
			continue;
		const Vec3 to = v3_sub(e->body.position, f->body.position);
		const float dist = v3_length(to);
		if (dist > sk->see_range_m)
			continue;
		const Vec3 flat = v3_normalize(v3(to.x, 0, to.z));
		const float ang = acosf(clampf(v3_dot(fwd, flat), -1, 1)) * 180.0f / PI_F;
		if (ang > sk->fov_deg * 0.5f && i != b->target && dist > 4.0f)
			continue;
		if (dist < best_d && can_see(f, e)) {
			best = i;
			best_d = dist;
		}
	}
	int via = -1;
	if (best < 0 && MODES[G.m.mode].portals)
		best = spot_through_portals(f, &best_d, &via);
	b->target_portal = via;
	if (best != b->target) {
		if (best >= 0) {
			b->seen_since = now();
			b->aim_height = frand() < sk->build_chance * 0.4f ? 1.55f : frand_range(1.0f, 1.3f);
			const float e = sk->aim_error_deg * PI_F / 180.0f * best_d;
			b->aim_err_x = frand_range(-e, e);
			b->aim_err_y = frand_range(-e * 0.6f, e * 0.6f);
		}
		b->target = best;
	}
	if (b->target >= 0)
		b->last_seen = G.m.f[b->target].body.position;
	// recarrega quando esta seguro
	const Gun *g = fighter_gun(f);
	if (b->target < 0 && g && WEAPONS[g->id].mag > 0 && g->mag < WEAPONS[g->id].mag * 0.4f)
		fighter_start_reload(f);
	match_bot_objective(f);
	if (b->build_cd > 0)
		b->build_cd -= 0.12f;
}

static Vec3 steer(Fighter *f, float dt)
{
	Brain *b = &f->bot;
	const float t = now();
	if (t >= b->next_repath) {
		b->next_repath = t + 2.5;
		b->portal_goal = -1;
		Vec3 approach;
		int entry;
		if (MODES[G.m.mode].portals && portal_shortcut(f, b->goal, &entry, &approach)) {
			b->portal_goal = entry;
			b->path_count = nav_path(f->body.position, approach, b->path, MAX_PATH);
		} else {
			b->path_count = nav_path(f->body.position, b->goal, b->path, MAX_PATH);
		}
		b->path_i = 0;
	}
	if (b->portal_goal >= 0 && !portal_linked(b->portal_goal)) {
		b->portal_goal = -1;
		b->next_repath = 0;
	}
	if (b->path_count == 0)
		return v3(0, 0, 0);
	while (b->path_i < b->path_count) {
		const Vec3 d = v3_sub(b->path[b->path_i], f->body.position);
		if (sqrtf(d.x * d.x + d.z * d.z) >= 0.6f)
			break;
		b->path_i++;
	}
	if (b->path_i >= b->path_count) {
		if (b->portal_goal >= 0) {
			const Portal *p = &G.m.portals[b->portal_goal];
			Vec3 into = v3_sub(xf_origin(&p->xf), f->body.position);
			into.y = 0;
			if (v3_length(into) > 0.05f)
				return v3_normalize(into);
			const Vec3 n = portal_normal(p);
			return v3_normalize(v3(-n.x, 0, -n.z));
		}
		return v3(0, 0, 0);
	}
	Vec3 to = v3_sub(b->path[b->path_i], f->body.position);
	to.y = 0;
	if (t < b->unstick_until)
		return b->unstick;
	// preso: pula e refaz o caminho; se nao resolver, sai de lado
	b->stuck_t += dt;
	if (b->stuck_t > 1.2f) {
		const Vec3 m = v3_sub(f->body.position, b->last_pos);
		if (sqrtf(m.x * m.x + m.z * m.z) < 0.4f) {
			b->stuck_count++;
			if (f->body.on_ground)
				f->body.velocity.y = JUMP_SPEED;
			b->next_repath = 0;
			if (b->stuck_count >= 2) {
				const Vec3 tn = v3_normalize(to);
				const Vec3 side = v3_scale(v3_cross(tn, v3(0, 1, 0)), frand() < 0.5f ? 1.0f : -1.0f);
				b->unstick = v3_normalize(v3_sub(side, v3_scale(tn, 0.5f)));
				b->unstick_until = t + 0.9;
			}
		} else {
			b->stuck_count = 0;
		}
		b->last_pos = f->body.position;
		b->stuck_t = 0;
	}
	return v3_length(to) > 0.05f ? v3_normalize(to) : v3(0, 0, 0);
}

void bot_update(Fighter *f, float dt)
{
	Brain *b = &f->bot;
	f->recoil = fmaxf(0.0f, f->recoil - dt * 18.0f);
	reload_tick(f);
	if (!f->alive)
		return;
	const BotSkillsDef *sk = &BOT_SKILLS[b->skill];
	const float t = now();
	if (t >= b->next_think) {
		b->next_think = t + 0.12;
		think(f);
	}
	if (b->target >= 0 && !G.m.f[b->target].alive)
		b->target = -1;
	Vec3 wish = v3(0, 0, 0);
	if (!f->frozen && !match_is_busy(f))
		wish = steer(f, dt);
	// olhar
	bool has_look = false;
	Vec3 look = v3(0, 0, 0);
	if (b->target >= 0 && t - b->seen_since > 0.0) {
		look = v3_add(aim_point(f), v3(b->aim_err_x, b->aim_err_y, 0));
		has_look = true;
	} else if (v3_length(wish) > 0.1f) {
		look = v3_add(v3(f->body.position.x, f->body.position.y + EYE, f->body.position.z), v3_scale(wish, 5.0f));
		has_look = true;
	}
	if (has_look) {
		const Vec3 d = v3_sub(look, fighter_eye(f));
		const float want_yaw = atan2f(-d.x, -d.z);
		const float want_pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
		const float turn = clampf((b->target >= 0 ? 10.0f : 6.0f) * dt, 0, 1);
		f->body.yaw = lerp_angle(f->body.yaw, want_yaw, turn);
		f->body.pitch += (want_pitch - f->body.pitch) * turn;
	}
	// andar: para para atirar, como um jogador disciplinado
	float speed = fighter_max_speed(f);
	if (b->target >= 0 && fighter_weapon(f) != &WEAPONS[WEAPONS_KNIFE])
		speed = 0;
	const Vec3 want = v3_scale(wish, speed);
	Vec3 hv = v3(f->body.velocity.x, 0, f->body.velocity.z);
	const Vec3 dv = v3_sub(want, hv);
	const float step = (v3_length(want) > 0 ? 40.0f : 30.0f) * dt;
	const float dl = v3_length(dv);
	hv = dl <= step ? want : v3_add(hv, v3_scale(dv, step / dl));
	f->body.velocity.x = hv.x;
	f->body.velocity.z = hv.z;
	const bool was_ground = f->body.on_ground;
	const bool jumped = f->body.velocity.y > 0.5f && was_ground;
	if (!was_ground || jumped)
		f->body.velocity.y -= GRAVITY * dt;
	bs_player_crouch(&f->body, &G.col, false);
	bs_player_move(&f->body, &G.col, dt, was_ground, jumped);
	if (f->body.position.y < G.map.info.min.y - 20.0f)
		fighter_die(f, NULL, WEAPONS_KNIFE, false);
	// atirar quando o tempo de reacao passou e a mira assentou um pouco
	if (b->target >= 0 && t - b->seen_since > sk->reaction_s && !f->frozen) {
		const Vec3 d2 = v3_sub(aim_point(f), f->body.position);
		if (fighter_weapon(f) == &WEAPONS[WEAPONS_KNIFE] && (b->target_portal >= 0 || v3_length(d2) > 2.0f)) {
		} else {
			const float k = clampf(dt / sk->aim_settle_s, 0, 1);
			b->aim_err_x += (0 - b->aim_err_x) * k;
			b->aim_err_y += (0 - b->aim_err_y) * k;
			if (fabsf(angle_diff(f->body.yaw, atan2f(-d2.x, -d2.z))) < 8.0f * PI_F / 180.0f)
				fighter_fire(f);
		}
	}
}

void bot_hurt(Fighter *f, Fighter *attacker)
{
	Brain *b = &f->bot;
	if (!attacker || !attacker->alive)
		return;
	const BotSkillsDef *sk = &BOT_SKILLS[b->skill];
	if (b->target < 0) {
		b->target = attacker->index;
		b->seen_since = now() - sk->reaction_s * 0.5f;
		const float e = sk->aim_error_deg * PI_F / 180.0f * v3_length(v3_sub(f->body.position, attacker->body.position));
		b->aim_err_x = frand_range(-e, e);
		b->aim_err_y = frand_range(-e * 0.6f, e * 0.6f);
		const Vec3 d = v3_sub(attacker->body.position, f->body.position);
		f->body.yaw = lerp_angle(f->body.yaw, atan2f(-d.x, -d.z), 0.5f);
	}
	// reflexo do Build Fight: uma parede na direcao de quem atira
	if (build_enabled() && b->build_cd <= 0 && frand() < sk->build_chance) {
		b->build_cd = 3.5f;
		for (int m = 0; m < BUILD_MATERIALS_COUNT; m++)
			if (f->mats[m] > f->mats[f->build_mat])
				f->build_mat = m;
		const Vec3 d = v3_sub(attacker->body.position, f->body.position);
		const float y = atan2f(-d.x, -d.z);
		Placement p = build_placement(f, BUILD_PIECES_WALL, y, 0.0f);
		build_try(f, BUILD_PIECES_WALL, &p);
		if (frand() < sk->build_chance * 0.5f) {
			p = build_placement(f, BUILD_PIECES_RAMP, y, 0.0f);
			build_try(f, BUILD_PIECES_RAMP, &p);
		}
	}
}
