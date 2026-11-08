// Portal Strike (portal.gd, a parte de portais do match.gd e o tacmap.gd): portais azul e laranja, quem atravessa,
// atalhos e visao dos bots, e o mapa tatico com as rotas previstas e os lugares recomendados.
#include <stdlib.h>
#include <string.h>

#include "game.h"

static const float RX = 0.62f;
static const float RY = 1.05f;
static const float COLORS[2][3] = { { 0.15f, 0.55f, 1.0f }, { 1.0f, 0.5f, 0.08f } };

Vec3 portal_normal(const Portal *p)
{
	return xf_col(&p->xf, 2);
}

bool portal_linked(int i)
{
	if (i < 0 || i >= MAX_PORTALS)
		return false;
	const Portal *p = &G.m.portals[i];
	return p->used && p->twin >= 0 && G.m.portals[p->twin].used;
}

// Leva o que entra pela frente deste portal para a saida pela frente do gemeo (girado 180 graus em Y).
Xf portal_through(const Portal *p)
{
	const Portal *q = &G.m.portals[p->twin];
	const Xf turn = xf_yaw(PI_F, v3(0, 0, 0));
	const Xf inv = xf_inverse(&p->xf);
	const Xf a = xf_mul(&q->xf, &turn);
	return xf_mul(&a, &inv);
}

static bool inside_oval(const Portal *p, Vec3 w, float slack)
{
	const Xf inv = xf_inverse(&p->xf);
	const Vec3 l = xf_point(&inv, w);
	return (l.x / RX) * (l.x / RX) + (l.y / RY) * (l.y / RY) <= slack;
}

static float segment_t(const Portal *p, Vec3 a, Vec3 b)
{
	const Vec3 n = portal_normal(p);
	const Vec3 d = v3_sub(b, a);
	const float den = v3_dot(d, n);
	if (den >= -0.0001f)
		return -1;
	const float t = v3_dot(v3_sub(xf_origin(&p->xf), a), n) / den;
	if (t < 0 || t > 1)
		return -1;
	return inside_oval(p, v3_add(a, v3_scale(d, t)), 1.0f) ? t : -1;
}

Vec3 portal_front(const Portal *p, float dist)
{
	const Vec3 n = portal_normal(p);
	if (fabsf(n.y) > 0.7f)
		return xf_origin(&p->xf);
	return v3_add(xf_origin(&p->xf), v3_scale(v3_normalize(v3(n.x, 0, n.z)), dist));
}

bool portal_on_segment(Vec3 a, Vec3 b, Vec3 *point, Xf *through, int *portal)
{
	float best_t = 2;
	int best = -1;
	for (int i = 0; i < MAX_PORTALS; i++) {
		if (!portal_linked(i))
			continue;
		const float t = segment_t(&G.m.portals[i], a, b);
		if (t >= 0 && t < best_t) {
			best_t = t;
			best = i;
		}
	}
	if (best < 0)
		return false;
	*point = v3_add(a, v3_scale(v3_sub(b, a), best_t));
	*through = portal_through(&G.m.portals[best]);
	*portal = best;
	return true;
}

void portals_clear(void)
{
	memset(G.m.portals, 0, sizeof G.m.portals);
}

static void place(Fighter *f, int color, Xf xf, bool standing, int surface)
{
	const int base = f->index * 2;
	Portal *p = &G.m.portals[base + color];
	*p = (Portal){ true, f->index, color, xf, standing, surface, -1 };
	Portal *o = &G.m.portals[base + 1 - color];
	if (o->used) {
		p->twin = base + 1 - color;
		o->twin = base + color;
	}
	sound_at("snd_usp", xf_origin(&xf), -4);
	plat_log("portal: %s abriu o %s em %.1f %.1f %.1f", f->name, color ? "laranja" : "azul", xf.m[3], xf.m[7], xf.m[11]);
	for (int i = 0; i < G.m.count; i++)
		if (G.m.f[i].is_bot)
			G.m.f[i].bot.next_repath = 0;
}

// Um portal deitado numa superficie: o de parede assenta no chao se mirado baixo; o de chao e teto aponta o topo
// para longe de quem atirou.
static Xf surface_xf(Vec3 point, Vec3 n, Vec3 view)
{
	Vec3 up = v3(0, 1, 0);
	if (fabsf(n.y) > 0.7f) {
		up = v3(view.x, 0, view.z);
		up = v3_length(up) > 0.05f ? v3_normalize(up) : v3(0, 0, -1);
	} else {
		const Vec3 probe = v3_add(point, v3_scale(n, 0.35f));
		const RayHit h = world_ray(probe, v3_add(probe, v3(0, -3, 0)), RAY_WORLD | RAY_PIECES, -1);
		if (h.hit && point.y - RY < h.point.y + 0.05f)
			point.y = h.point.y + RY + 0.05f;
	}
	const Vec3 x = v3_normalize(v3_cross(up, n));
	const Vec3 y = v3_normalize(v3_cross(n, x));
	return xf_basis(x, y, n, v3_add(point, v3_scale(n, 0.03f)));
}

static Xf standing_xf(Vec3 feet, float yaw)
{
	return xf_yaw(yaw + PI_F, v3_add(feet, v3(0, RY + 0.08f, 0)));
}

void portal_fire(Fighter *f)
{
	const Vec3 from = fighter_eye(f), dir = fighter_aim(f);
	const RayHit h = world_ray(from, v3_add(from, v3_scale(dir, WEAPONS[WEAPONS_PORTALGUN].range_m)),
		RAY_WORLD | RAY_PIECES | RAY_FIGHTERS, f->index);
	if (!h.hit || h.kind == HIT_FIGHTER) {
		if (f->is_player)
			match_notice("No surface for a portal there", 1.0f);
		return;
	}
	fx_tracer(v3_add(v3_add(from, v3_scale(dir, 0.6f)), v3(0, -0.08f, 0)), h.point);
	place(f, 0, surface_xf(h.point, h.normal, dir), false, h.kind == HIT_PIECE ? h.index : -1);
}

void portal_place_orange(Fighter *f, Vec3 feet, float yaw)
{
	if (G.net_role == NET_ROLE_CLIENT) {
		netcl_orange(feet, yaw);
		G.m.tacmap_open = false;
		plat_capture_mouse(true);
		return;
	}
	place(f, 1, standing_xf(feet, yaw), true, -1);
	G.m.tacmap_open = false;
	plat_capture_mouse(true);
}

// ---------------------------------------------------------------- atravessar

static bool enters(const Fighter *f, const Portal *p)
{
	const Vec3 n = portal_normal(p);
	if (v3_dot(f->body.velocity, n) > 0.2f)
		return false; // indo embora (ou pelas costas de um portal de pe)
	const Xf inv = xf_inverse(&p->xf);
	// ate 0.6 m da frente: encostado na parede, o corpo fica a 0.45 m do plano de um portal de parede (no Godot a
	// margem da capsula deixava mais perto)
	static const float hs[3] = { 0.15f, 0.9f, 1.55f };
	for (int k = 0; k < 3; k++) {
		const Vec3 l = xf_point(&inv, v3_add(f->body.position, v3(0, hs[k], 0)));
		if (l.z > -0.05f && l.z < 0.6f && (l.x / RX) * (l.x / RX) + (l.y / RY) * (l.y / RY) <= 1.0f)
			return true;
	}
	return false;
}

static void send_through(Fighter *f, const Portal *p)
{
	const Portal *q = &G.m.portals[p->twin];
	const Xf m = portal_through(p);
	Vec3 center = xf_point(&m, v3_add(f->body.position, v3(0, 0.9f, 0)));
	const Vec3 nq = portal_normal(q);
	const Xf qi = xf_inverse(&q->xf);
	Vec3 l = xf_point(&qi, center);
	l.x = clampf(l.x, -0.3f, 0.3f);
	l.y = clampf(l.y, -0.6f, 0.6f);
	l.z = fmaxf(l.z, fabsf(nq.y) > 0.5f ? 1.05f : 0.75f);
	center = xf_point(&q->xf, l);
	if (fabsf(nq.y) <= 0.5f) {
		// nao sai com os pes no chao
		const RayHit h = world_ray(center, v3_add(center, v3(0, -0.95f, 0)), RAY_WORLD | RAY_PIECES, -1);
		if (h.hit)
			center.y = h.point.y + 0.92f;
	}
	Vec3 v = xf_dir(&m, f->body.velocity);
	const float out = v3_dot(v, nq);
	if (out < 3.0f)
		v = v3_add(v, v3_scale(nq, 3.0f - out)); // coisa rapida entra, coisa rapida sai
	const Vec3 look = xf_dir(&m, fighter_aim(f));
	f->body.position = v3_sub(center, v3(0, 0.9f, 0));
	f->body.velocity = v;
	f->body.on_ground = false;
	if (sqrtf(look.x * look.x + look.z * look.z) > 0.2f)
		f->body.yaw = atan2f(-look.x, -look.z);
	f->body.pitch = clampf(asinf(clampf(look.y, -1, 1)), -1.553f, 1.553f);
	f->portal_cd = G.time + 0.4;
	f->net_epoch++;
	if (f->is_bot) {
		f->bot.portal_goal = -1;
		f->bot.path_count = 0;
		f->bot.next_repath = 0;
		f->bot.stuck_t = 0;
		f->bot.last_pos = f->body.position;
	}
	sound_at("snd_usp", center, -8);
	plat_log("portal: %s atravessou %s -> %s", f->name, p->color ? "laranja" : "azul", q->color ? "laranja" : "azul");
}

void portals_update(void)
{
	for (int i = 0; i < MAX_PORTALS; i++) {
		Portal *p = &G.m.portals[i];
		// a peca em que estava quebrou: o portal some
		if (p->used && p->surface_piece >= 0 && (!G.m.pieces[p->surface_piece].used || G.m.pieces[p->surface_piece].dying)) {
			if (p->twin >= 0)
				G.m.portals[p->twin].twin = -1;
			p->used = false;
		}
	}
	for (int k = 0; k < G.m.count; k++) {
		Fighter *f = &G.m.f[k];
		if (!f->alive || G.time < f->portal_cd)
			continue;
		for (int i = 0; i < MAX_PORTALS; i++)
			if (portal_linked(i) && enters(f, &G.m.portals[i])) {
				send_through(f, &G.m.portals[i]);
				break;
			}
	}
}

void portals_draw(void)
{
	static FxVertex v[MAX_PORTALS * 6];
	uint32_t n = 0;
	const AtlasRect *r = assets_rect(&G.as, "swirl");
	if (!r)
		return;
	const float u0 = (float)r->x / G.as.atlas_w, v0 = (float)r->y / G.as.atlas_h;
	const float u1 = (float)(r->x + r->w) / G.as.atlas_w, v1 = (float)(r->y + r->h) / G.as.atlas_h;
	// o redemoinho gira
	const float a = (float)G.time * 1.3f;
	for (int i = 0; i < MAX_PORTALS; i++) {
		const Portal *p = &G.m.portals[i];
		if (!p->used)
			continue;
		const float *c = COLORS[p->color];
		const float lit = portal_linked(i) ? 1.0f : 0.6f;
		Vec3 q[4];
		const float ca = cosf(a), sa = sinf(a);
		static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		for (int k = 0; k < 4; k++) {
			// gira os cantos da textura, nao o oval
			(void)ca;
			(void)sa;
			q[k] = xf_point(&p->xf, v3(corner[k][0] * RX, corner[k][1] * RY, 0.0f));
		}
		const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v1, v1, v0, v0 };
		static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
		for (int k = 0; k < 6; k++) {
			const int j = idx[k];
			v[n++] = (FxVertex){ q[j].x, q[j].y, q[j].z, c[0] * 1.6f * lit, c[1] * 1.6f * lit, c[2] * 1.6f * lit, 1,
				us[j], vs[j] };
		}
	}
	gfx_draw_fx(v, n, G.atlas_tex, GFX_ALPHA_TEST | GFX_UNLIT);
}

// ---------------------------------------------------------------- bots e portais

static float path_len(Vec3 a, Vec3 c)
{
	static Vec3 pth[MAX_PATH];
	const int n = nav_path(a, c, pth, MAX_PATH);
	if (n < 2)
		return v3_length(v3_sub(a, c)) * 1.4f;
	float l = v3_length(v3_sub(a, pth[0]));
	for (int i = 1; i < n; i++)
		l += v3_length(v3_sub(pth[i], pth[i - 1]));
	return l + v3_length(v3_sub(pth[n - 1], c)) * 3.0f;
}

bool portal_shortcut(Fighter *b, Vec3 goal, int *entry, Vec3 *approach)
{
	bool any = false;
	for (int i = 0; i < MAX_PORTALS; i++)
		any |= portal_linked(i);
	if (!any)
		return false;
	const float direct = path_len(b->body.position, goal);
	float best_len = direct * 0.75f - 5.0f;
	bool found = false;
	for (int i = 0; i < MAX_PORTALS; i++) {
		if (!portal_linked(i))
			continue;
		const Portal *p = &G.m.portals[i];
		const Vec3 front = portal_front(p, 0.7f);
		const Vec3 nav = nav_closest(front);
		if (sqrtf((nav.x - front.x) * (nav.x - front.x) + (nav.z - front.z) * (nav.z - front.z)) > 1.2f)
			continue;
		const float rise = p->xf.m[7] - nav.y;
		const Vec3 n = portal_normal(p);
		if (fabsf(n.y) > 0.7f) {
			if (fabsf(rise) > 0.4f || n.y < 0)
				continue; // so portal de chao, e que da para pisar
		} else if (rise > 1.8f || rise < 0.4f) {
			continue; // alto (ou baixo) demais na parede
		}
		const Vec3 out = nav_closest(portal_front(&G.m.portals[p->twin], 1.0f));
		const float l = path_len(b->body.position, nav) + path_len(out, goal) + 2.0f;
		if (l < best_len) {
			best_len = l;
			*entry = i;
			*approach = nav;
			found = true;
		}
	}
	return found;
}

void portal_bot_open(Fighter *b)
{
	const Vec3 eye = v3_add(b->body.position, v3(0, 1.15f, 0));
	RayHit best = { 0 };
	float best_d = 1e30f;
	for (int i = 0; i < 16; i++) {
		const float a = 2 * PI_F * i / 16.0f;
		const Vec3 d = v3(-sinf(a), 0, -cosf(a));
		const RayHit h = world_ray(eye, v3_add(eye, v3_scale(d, 12.0f)), RAY_WORLD, b->index);
		const float dist = v3_length(v3_sub(h.point, eye));
		if (h.hit && fabsf(h.normal.y) < 0.3f && dist > 1.2f && dist < best_d) {
			best_d = dist;
			best = h;
		}
	}
	if (!best.hit) {
		b->bot.portal_delay += 2.0f; // nenhuma parede aqui: tenta um pouco adiante
		return;
	}
	Vec3 toward = b->bot.goal;
	if (b->team == TEAM_T)
		toward = world_random_in_site(G.m.plan_site);
	Vec3 recs[3];
	float yaws[3];
	const char *labels[3];
	const int nr = tacmap_recommend(b->team, recs, yaws, labels, 3);
	Vec3 spot = v3(0, 0, 0);
	float yaw = 0;
	bool good = false;
	for (int i = 0; i < nr; i++)
		if (v3_length(v3_sub(recs[i], toward)) < 25.0f) {
			spot = recs[i];
			yaw = yaws[i];
			good = true;
			break;
		}
	if (!good) {
		spot = nav_closest(v3_add(toward, v3(frand_range(-4, 4), 0, frand_range(-4, 4))));
		const Vec3 d = v3_sub(toward, spot);
		yaw = v3_length(d) > 0.5f ? atan2f(-d.x, -d.z) : frand() * 2 * PI_F;
	}
	b->bot.portal_done = true;
	if (v3_length(v3_sub(spot, b->body.position)) < 15.0f)
		return; // ja esta la: nao vale um portal
	const Vec3 dir = v3_normalize(v3_sub(best.point, eye));
	b->body.yaw = atan2f(-dir.x, -dir.z);
	fighter_switch(b, 4);
	sound_at(WEAPONS[WEAPONS_PORTALGUN].sound_asset, eye, 0);
	fx_tracer(eye, best.point);
	place(b, 0, surface_xf(best.point, best.normal, dir), false, -1);
	place(b, 1, standing_xf(spot, yaw), true, -1);
	b->next_fire = G.time + 0.6;
	fighter_switch(b, b->has[1] ? 1 : 2);
}

// ---------------------------------------------------------------- mapa tatico

void tacmap_routes(int team, Vec3 routes[MAX_FIGHTERS][MAX_PATH + 2], int *lens, int *count)
{
	*count = 0;
	for (int i = 0; i < G.m.count; i++) {
		const Fighter *f = &G.m.f[i];
		if (f->team == team || !f->alive)
			continue;
		Vec3 *r = routes[*count];
		int n = 0;
		r[n++] = f->body.position;
		if (f->is_bot) {
			for (int k = f->bot.path_i; k < f->bot.path_count && n < MAX_PATH + 1; k++)
				r[n++] = f->bot.path[k];
			if (n == 1)
				r[n++] = f->bot.goal;
		} else {
			r[n++] = v3_add(f->body.position, v3(f->body.velocity.x * 4, 0, f->body.velocity.z * 4));
		}
		lens[(*count)++] = n;
	}
}

static float route_len(const Vec3 *r, int n)
{
	float l = 0;
	for (int i = 1; i < n; i++)
		l += v3_length(v3_sub(r[i], r[i - 1]));
	return l;
}

static void along(const Vec3 *r, int n, float dist, Vec3 *p, Vec3 *dir)
{
	for (int i = 1; i < n; i++) {
		const float seg = v3_length(v3_sub(r[i], r[i - 1]));
		if (dist <= seg && seg > 0.01f) {
			*dir = v3_normalize(v3_sub(r[i], r[i - 1]));
			*p = v3_add(r[i - 1], v3_scale(*dir, dist));
			return;
		}
		dist -= seg;
	}
	*p = r[n - 1];
	*dir = n > 1 ? v3_normalize(v3_sub(r[n - 1], r[n - 2])) : v3(0, 0, -1);
}

int tacmap_recommend(int team, Vec3 *pos, float *yaw, const char **label, int max)
{
	static Vec3 routes[MAX_FIGHTERS][MAX_PATH + 2];
	int lens[MAX_FIGHTERS], rc;
	tacmap_routes(team, routes, lens, &rc);
	typedef struct {
		Vec3 pos, look;
		const char *label;
		float score, yaw;
	} Cand;
	static Cand cands[MAX_FIGHTERS * 8];
	int cn = 0;
	static const float fracs[4] = { 0.45f, 0.65f, 0.85f, 1.0f };
	for (int r = 0; r < rc; r++) {
		if (lens[r] < 2)
			continue;
		const float total = route_len(routes[r], lens[r]);
		if (total < 3.0f)
			continue;
		for (int fi = 0; fi < 4; fi++) {
			Vec3 p, dir;
			along(routes[r], lens[r], total * fracs[fi], &p, &dir);
			const Vec3 side = v3_normalize(v3_cross(dir, v3(0, 1, 0)));
			for (int s = -1; s <= 1; s += 2) {
				const Vec3 c = nav_closest(v3_add(v3_add(p, v3_scale(side, s * 5.0f)), v3_scale(dir, 2.0f)));
				if (v3_length(v3_sub(c, p)) < 2.5f || fabsf(c.y - p.y) > 2.5f)
					continue;
				const char *lb = "AMBUSH";
				int st;
				if (fracs[fi] >= 0.99f)
					lb = world_site_at(p, &st) ? (st ? "SITE B" : "SITE A") : "WHERE THEY GO";
				else if (fracs[fi] < 0.5f)
					lb = "CUT OFF";
				if (cn < MAX_FIGHTERS * 8)
					cands[cn++] = (Cand){ c, p, lb, 0, 0 };
			}
		}
	}
	for (int i = 0; i < cn; i++) {
		Cand *c = &cands[i];
		float score = 0;
		for (int r = 0; r < rc; r++) {
			float dmin = 1e30f;
			for (int k = 0; k < lens[r]; k++)
				dmin = fminf(dmin, v3_length(v3_sub(routes[r][k], c->pos)));
			score += expf(-(dmin * dmin) / 144.0f);
			if (v3_length(v3_sub(routes[r][0], c->pos)) < 10.0f)
				score -= 1.5f; // nao cai do lado deles
		}
		const RayHit h = world_ray(v3_add(c->pos, v3(0, 1.4f, 0)), v3_add(c->look, v3(0, 1.2f, 0)), RAY_WORLD | RAY_PIECES, -1);
		score += h.hit ? -0.4f : 0.8f;
		c->score = score;
		const Vec3 d = v3_sub(c->look, c->pos);
		c->yaw = atan2f(-d.x, -d.z);
	}
	// os melhores, a pelo menos 10 m um do outro
	int out = 0;
	for (int pass = 0; pass < cn && out < max; pass++) {
		int bi = -1;
		for (int i = 0; i < cn; i++)
			if (cands[i].label && (bi < 0 || cands[i].score > cands[bi].score))
				bi = i;
		if (bi < 0)
			break;
		bool ok = true;
		for (int o = 0; o < out; o++)
			if (v3_length(v3_sub(pos[o], cands[bi].pos)) < 10.0f)
				ok = false;
		if (ok) {
			pos[out] = cands[bi].pos;
			yaw[out] = cands[bi].yaw;
			label[out] = cands[bi].label;
			out++;
		}
		cands[bi].label = NULL;
	}
	return out;
}
