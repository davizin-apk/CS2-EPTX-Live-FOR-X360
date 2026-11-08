// Construcao do Build Fight (build.gd e a parte de construcao do match.gd): grade de 5,12 m x 3,84 m, parede, chao,
// escada e cone, em madeira, tijolo e metal, edicao e queda do que perde o apoio.
#include <stdlib.h>
#include <string.h>

#include "game.h"

static const float TILE = 5.12f;
static const float H = 3.84f;
static const float THICK = 0.22f;
static const float CONE_H = 3.84f / 2;
static const Vec3 CARD[4] = { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 } };
static const float CARD_YAW[4] = { 0.0f, -PI_F / 2, PI_F, PI_F / 2 };

bool build_enabled(void)
{
	return MODES[G.m.mode].building && G.m.phase != PHASE_MATCH_OVER;
}

int build_edit_grid(int piece)
{
	return BUILD_PIECES[piece].edit_grid;
}

static int cardinal(float yaw)
{
	const float fx = -sinf(yaw), fz = -cosf(yaw);
	if (fabsf(fx) > fabsf(fz))
		return fx > 0 ? 1 : 3;
	return fz > 0 ? 2 : 0;
}

static void grid_cell(Vec3 p, int *x, int *z)
{
	*x = (int)floorf(p.x / TILE);
	*z = (int)floorf(p.z / TILE);
}

static int64_t make_key(int kind, int a, int b, int c)
{
	return ((int64_t)kind << 56) ^ ((int64_t)(a & 0xFFFF) << 32) ^ ((int64_t)(b & 0xFFFF) << 16) ^ (int64_t)(c & 0xFFFF);
}

// O nivel 0 perto de alguem: o da estrutura vizinha, se houver (para as pecas se alinharem), ou o chao.
static float grid_origin(Vec3 feet)
{
	float best = 1e30f, oy = feet.y - 0.05f;
	for (int i = 0; i < MAX_PIECES; i++) {
		const Piece *p = &G.m.pieces[i];
		if (!p->used || p->dying)
			continue;
		const Vec3 c = xf_origin(&p->xf);
		const float d = sqrtf((c.x - feet.x) * (c.x - feet.x) + (c.z - feet.z) * (c.z - feet.z));
		const float lv = (feet.y - p->oy) / H;
		if (d < TILE * 1.6f && lv > -1.2f && lv < 2.5f && d < best) {
			best = d;
			oy = p->oy;
		}
	}
	return oy;
}

Placement build_placement(const Fighter *f, int id, float yaw, float pitch)
{
	if (isnan(yaw))
		yaw = f->body.yaw;
	if (isnan(pitch))
		pitch = f->body.pitch;
	const Vec3 feet = f->body.position;
	const float origin_y = grid_origin(feet);
	const int c = cardinal(yaw);
	const Vec3 fwd = CARD[c];
	const float rel = (feet.y - origin_y) / H;
	const int lvl_stand = (int)floorf(rel + 0.15f);
	const int lvl_climb = (int)floorf(rel + 0.45f);
	int hx, hz, ax, az;
	grid_cell(feet, &hx, &hz);
	const Vec3 look = v3(-sinf(yaw), 0, -cosf(yaw));
	grid_cell(v3_add(feet, v3_scale(look, TILE * 0.55f)), &ax, &az);
	if (ax == hx && az == hz) {
		ax = hx + (int)lroundf(fwd.x);
		az = hz + (int)lroundf(fwd.z);
	}
	int cx = hx, cz = hz, lvl = lvl_stand, face = c;
	switch (id) {
	case BUILD_PIECES_WALL:
		if (pitch > 0.6f)
			lvl++;
		else if (pitch < -0.9f)
			lvl--;
		break;
	case BUILD_PIECES_FLOOR:
		face = 0;
		if (pitch > 0.55f)
			lvl++;
		else if (pitch > -0.75f) {
			cx = ax;
			cz = az;
		}
		break;
	case BUILD_PIECES_RAMP:
		lvl = lvl_climb;
		if (pitch < -0.75f)
			lvl = lvl_stand;
		else {
			grid_cell(v3_add(feet, v3_scale(look, TILE * 0.55f)), &cx, &cz);
			if (pitch > 0.7f)
				lvl++;
		}
		break;
	case BUILD_PIECES_CONE:
		face = 0;
		if (pitch > -0.3f)
			lvl++;
		else {
			cx = ax;
			cz = az;
		}
		break;
	}
	const float base_y = origin_y + lvl * H;
	const Vec3 center = v3((cx + 0.5f) * TILE, base_y, (cz + 0.5f) * TILE);
	const int hy = (int)lroundf(base_y * 2.0f);
	Placement p = { 0 };
	p.piece = id;
	p.xf = xf_yaw(CARD_YAW[face], center);
	if (id == BUILD_PIECES_WALL) {
		// uma borda e de dois quadrados: o mesmo nome dos dois lados
		switch (c) {
		case 0:
			p.key = make_key(1, cx, hy, cz);
			break;
		case 2:
			p.key = make_key(1, cx, hy, cz + 1);
			break;
		case 1:
			p.key = make_key(2, cx + 1, hy, cz);
			break;
		default:
			p.key = make_key(2, cx, hy, cz);
			break;
		}
	} else {
		p.key = make_key(3 + id, cx, hy, cz);
	}
	p.cell[0] = cx;
	p.cell[1] = lvl;
	p.cell[2] = cz;
	p.card = face;
	p.oy = origin_y;
	p.ok = true;
	return p;
}

// ---------------------------------------------------------------- formas

typedef struct {
	Xf xf;
	Vec3 size;
} Box;

// As caixas (e os triangulos do cone) de uma peca no proprio espaco dela.
static int parts(int id, const bool *removed, int turn, Box *boxes, Vec3 *tris, int *tri_count)
{
	int nb = 0;
	*tri_count = 0;
	const int n = build_edit_grid(id);
	switch (id) {
	case BUILD_PIECES_WALL: {
		const float tw = TILE / n, th = H / n;
		for (int j = 0; j < n; j++)
			for (int i = 0; i < n; i++)
				if (!removed[j * n + i])
					boxes[nb++] = (Box){ xf_translate(v3(-TILE / 2 + (i + 0.5f) * tw, (j + 0.5f) * th, -TILE / 2)), v3(tw, th, THICK) };
		break;
	}
	case BUILD_PIECES_FLOOR: {
		const float tw = TILE / n;
		for (int j = 0; j < n; j++)
			for (int i = 0; i < n; i++)
				if (!removed[j * n + i])
					boxes[nb++] = (Box){ xf_translate(v3(-TILE / 2 + (i + 0.5f) * tw, 0, -TILE / 2 + (j + 0.5f) * tw)), v3(tw, THICK, tw) };
		break;
	}
	case BUILD_PIECES_RAMP: {
		const float ln = sqrtf(TILE * TILE + H * H);
		const Xf y = xf_yaw(CARD_YAW[turn], v3(0, H / 2, 0));
		const Xf x = xf_rot_x(atan2f(H, TILE));
		boxes[nb++] = (Box){ xf_mul(&y, &x), v3(TILE, THICK, ln) };
		break;
	}
	case BUILD_PIECES_CONE:
		for (int j = 0; j < 2; j++)
			for (int i = 0; i < 2; i++) {
				if (removed[j * 2 + i])
					continue;
				const float sx = i == 0 ? -1.0f : 1.0f, sz = j == 0 ? -1.0f : 1.0f;
				const Vec3 corner = v3(sx * TILE / 2, 0, sz * TILE / 2), mx = v3(sx * TILE / 2, 0, 0),
						   mz = v3(0, 0, sz * TILE / 2), apex = v3(0, CONE_H, 0);
				Vec3 *t = tris + *tri_count * 3;
				t[0] = corner;
				t[1] = mx;
				t[2] = apex;
				t[3] = corner;
				t[4] = apex;
				t[5] = mz;
				*tri_count += 2;
			}
		break;
	}
	return nb;
}

// Triangulos de uma caixa (12) no espaco do mundo.
static void box_tris(const Xf *world, const Box *b, float *out)
{
	const Xf x = xf_mul(world, &b->xf);
	const Vec3 h = v3_scale(b->size, 0.5f);
	Vec3 c[8];
	for (int i = 0; i < 8; i++)
		c[i] = xf_point(&x, v3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z));
	static const int f[12][3] = { { 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 }, { 0, 1, 4 }, { 1, 5, 4 },
		{ 2, 6, 3 }, { 3, 6, 7 }, { 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
	for (int t = 0; t < 12; t++)
		for (int k = 0; k < 3; k++) {
			const Vec3 v = c[f[t][k]];
			out[t * 9 + k * 3] = v.x;
			out[t * 9 + k * 3 + 1] = v.y;
			out[t * 9 + k * 3 + 2] = v.z;
		}
}

// Gera os triangulos e a malha de uma peca a partir do estado (edicoes, giro da escada).
static void rebuild(Piece *p)
{
	Box boxes[9];
	Vec3 tris[24];
	int tc = 0;
	const int nb = parts(p->piece, p->edits, p->ramp_card, boxes, tris, &tc);
	free(p->tris);
	p->tri_count = (uint32_t)(nb * 12 + tc);
	p->tris = malloc(p->tri_count * 9 * sizeof(float) + 4);
	for (int i = 0; i < nb; i++)
		box_tris(&p->xf, &boxes[i], p->tris + i * 12 * 9);
	for (int t = 0; t < tc * 3; t++) {
		const Vec3 v = xf_point(&p->xf, tris[t]);
		float *o = p->tris + nb * 12 * 9 + t * 3;
		o[0] = v.x;
		o[1] = v.y;
		o[2] = v.z;
	}
	p->lo = v3(1e30f, 1e30f, 1e30f);
	p->hi = v3(-1e30f, -1e30f, -1e30f);
	for (uint32_t k = 0; k < p->tri_count * 3; k++) {
		const float *v = p->tris + k * 3;
		p->lo = v3(fminf(p->lo.x, v[0]), fminf(p->lo.y, v[1]), fminf(p->lo.z, v[2]));
		p->hi = v3(fmaxf(p->hi.x, v[0]), fmaxf(p->hi.y, v[1]), fmaxf(p->hi.z, v[2]));
	}
	// malha: os mesmos triangulos, com normal plana e UV triplanar no mundo (como o build.gd)
	static MeshVertex mv[256 * 3];
	static uint16_t mi[256 * 3];
	const uint32_t n = p->tri_count < 256 ? p->tri_count : 256;
	for (uint32_t t = 0; t < n; t++) {
		const float *v = p->tris + t * 9;
		Vec3 nn = v3_normalize(v3_cross(v3(v[3] - v[0], v[4] - v[1], v[5] - v[2]), v3(v[6] - v[0], v[7] - v[1], v[8] - v[2])));
		for (int k = 0; k < 3; k++) {
			MeshVertex *o = &mv[t * 3 + k];
			o->x = v[k * 3];
			o->y = v[k * 3 + 1];
			o->z = v[k * 3 + 2];
			o->nx = nn.x;
			o->ny = nn.y;
			o->nz = nn.z;
			const Vec3 a = v3(fabsf(nn.x), fabsf(nn.y), fabsf(nn.z));
			if (a.x > a.y && a.x > a.z) {
				o->u = o->z * 0.4f;
				o->v = o->y * 0.4f;
			} else if (a.y > a.z) {
				o->u = o->x * 0.4f;
				o->v = o->z * 0.4f;
			} else {
				o->u = o->x * 0.4f;
				o->v = o->y * 0.4f;
			}
			mi[t * 3 + k] = (uint16_t)(t * 3 + k);
		}
	}
	if (p->mesh < 0)
		p->mesh = gfx_skinned(256 * 3, mi, 256 * 3);
	gfx_skinned_vertices(p->mesh, mv, n * 3);
	p->tri_count = p->tri_count; // a colisao usa todos
	world_update_dynamic();
}

static bool box_hits_world(Vec3 lo, Vec3 hi)
{
	// a fatia de baixo da peca, um pouco abaixo dela, contra o mapa (os clips tambem)
	const Vec3 c = v3_scale(v3_add(lo, hi), 0.5f);
	const Vec3 h = v3(fmaxf((hi.x - lo.x) - 0.3f, 0.1f) / 2, 0.25f, fmaxf((hi.z - lo.z) - 0.3f, 0.1f) / 2);
	const Vec3 bl = v3(c.x - h.x, lo.y + 0.1f - h.y, c.z - h.z), bh = v3(c.x + h.x, lo.y + 0.1f + h.y, c.z + h.z);
	BsCollider *col = &G.col;
	const uint32_t keep = col->group_count;
	col->group_count = 0; // so o mapa
	bs_collider_gather(col, bl, bh);
	col->group_count = keep;
	return col->candidate_count > 0;
}

static bool supported(const Placement *pl)
{
	Piece tmp;
	memset(&tmp, 0, sizeof tmp);
	tmp.piece = pl->piece;
	tmp.xf = pl->xf;
	Box boxes[9];
	Vec3 tris[24];
	int tc;
	const int nb = parts(pl->piece, tmp.edits, 0, boxes, tris, &tc);
	Vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
	for (int i = 0; i < nb; i++) {
		float t[12 * 9];
		box_tris(&pl->xf, &boxes[i], t);
		for (int k = 0; k < 36; k++) {
			lo = v3(fminf(lo.x, t[k * 3]), fminf(lo.y, t[k * 3 + 1]), fminf(lo.z, t[k * 3 + 2]));
			hi = v3(fmaxf(hi.x, t[k * 3]), fmaxf(hi.y, t[k * 3 + 1]), fmaxf(hi.z, t[k * 3 + 2]));
		}
	}
	for (int k = 0; k < tc * 3; k++) {
		const Vec3 v = xf_point(&pl->xf, tris[k]);
		lo = v3(fminf(lo.x, v.x), fminf(lo.y, v.y), fminf(lo.z, v.z));
		hi = v3(fmaxf(hi.x, v.x), fmaxf(hi.y, v.y), fmaxf(hi.z, v.z));
	}
	for (int i = 0; i < MAX_PIECES; i++) {
		const Piece *q = &G.m.pieces[i];
		if (q->used && !q->dying && lo.x - 0.15f <= q->hi.x && hi.x + 0.15f >= q->lo.x && lo.y - 0.15f <= q->hi.y &&
			hi.y + 0.15f >= q->lo.y && lo.z - 0.15f <= q->hi.z && hi.z + 0.15f >= q->lo.z)
			return true;
	}
	return box_hits_world(lo, hi);
}

static int piece_with_key(int64_t k)
{
	for (int i = 0; i < MAX_PIECES; i++)
		if (G.m.pieces[i].used && !G.m.pieces[i].dying && G.m.pieces[i].key == k)
			return i;
	return -1;
}

bool build_can(const Fighter *f, int id, const Placement *p)
{
	if (!p->ok || !build_enabled() || !f->alive || G.m.phase == PHASE_FREEZE)
		return false;
	if (f->mats[f->build_mat] < BUILD_PIECES[id].cost)
		return false;
	return piece_with_key(p->key) < 0 && supported(p);
}

bool build_try(Fighter *f, int id, const Placement *p)
{
	if (!build_can(f, id, p))
		return false;
	int slot = -1;
	for (int i = 0; i < MAX_PIECES; i++)
		if (!G.m.pieces[i].used) {
			slot = i;
			break;
		}
	if (slot < 0)
		return false;
	f->mats[f->build_mat] -= BUILD_PIECES[id].cost;
	Piece *pc = &G.m.pieces[slot];
	const int keep_mesh = pc->mesh;
	memset(pc, 0, sizeof *pc);
	pc->mesh = keep_mesh;
	pc->used = true;
	pc->piece = id;
	pc->mat = f->build_mat;
	pc->team = f->team;
	pc->key = p->key;
	memcpy(pc->cell, p->cell, sizeof pc->cell);
	pc->card = p->card;
	pc->oy = p->oy;
	pc->xf = p->xf;
	const BuildPiecesDef *row = &BUILD_PIECES[id];
	const BuildMaterialsDef *mr = &BUILD_MATERIALS[pc->mat];
	pc->max_hp = row->max_hp * mr->hp_mult;
	pc->build_time = row->build_time * mr->time_mult;
	pc->hp = pc->max_hp * mr->start_hp;
	rebuild(pc);
	pc->grounded = box_hits_world(pc->lo, pc->hi);
	sound_at("snd_knife", xf_origin(&p->xf), -6);
	return true;
}

static void check_support(void);

void build_damage(int i, float amount)
{
	if (i < 0 || i >= MAX_PIECES)
		return;
	Piece *p = &G.m.pieces[i];
	if (!p->used || p->dying)
		return;
	p->hp -= amount;
	if (p->hp <= 0) {
		p->dying = true;
		p->die_at = (float)G.time;
		const float c[4] = { 0.6f, 0.42f, 0.22f, 1 };
		fx_particles(v3_scale(v3_add(p->lo, p->hi), 0.5f), c, 24);
		world_update_dynamic();
		check_support();
	}
}

static bool touches(const Piece *a, const Piece *b)
{
	return a->lo.x - 0.15f <= b->hi.x && a->hi.x + 0.15f >= b->lo.x && a->lo.y - 0.15f <= b->hi.y &&
		a->hi.y + 0.15f >= b->lo.y && a->lo.z - 0.15f <= b->hi.z && a->hi.z + 0.15f >= b->lo.z;
}

// Depois que uma peca quebra, cai tudo o que nao se liga mais ao chao por outras pecas.
static void check_support(void)
{
	bool held[MAX_PIECES] = { 0 };
	int todo[MAX_PIECES], tn = 0;
	for (int i = 0; i < MAX_PIECES; i++)
		if (G.m.pieces[i].used && !G.m.pieces[i].dying && G.m.pieces[i].grounded) {
			held[i] = true;
			todo[tn++] = i;
		}
	while (tn) {
		const int a = todo[--tn];
		for (int b = 0; b < MAX_PIECES; b++)
			if (!held[b] && G.m.pieces[b].used && !G.m.pieces[b].dying && touches(&G.m.pieces[a], &G.m.pieces[b])) {
				held[b] = true;
				todo[tn++] = b;
			}
	}
	int k = 0;
	for (int i = 0; i < MAX_PIECES; i++) {
		Piece *p = &G.m.pieces[i];
		if (p->used && !p->dying && !held[i]) {
			p->dying = true;
			p->die_at = (float)G.time + 0.04f * ++k;
		}
	}
	world_update_dynamic();
}

// ---------------------------------------------------------------- edicao

int build_tile_at(const Piece *p, Vec3 from, Vec3 dir)
{
	const Xf inv = xf_inverse(&p->xf);
	const Vec3 o = xf_point(&inv, from);
	const Vec3 d = v3_normalize(xf_dir(&inv, dir));
	const int n = build_edit_grid(p->piece);
	Vec3 pn = v3(0, 1, 0), pp = v3(0, 0, 0);
	if (p->piece == BUILD_PIECES_WALL) {
		pn = v3(0, 0, 1);
		pp = v3(0, 0, -TILE / 2);
	} else if (p->piece == BUILD_PIECES_RAMP) {
		const Xf y = xf_yaw(CARD_YAW[p->ramp_card], v3(0, 0, 0));
		const Xf x = xf_rot_x(atan2f(H, TILE));
		const Xf b = xf_mul(&y, &x);
		pn = xf_dir(&b, v3(0, 1, 0));
		pp = v3(0, H / 2, 0);
	} else if (p->piece == BUILD_PIECES_CONE) {
		pp = v3(0, CONE_H / 2, 0);
	}
	const float den = v3_dot(d, pn);
	if (fabsf(den) < 0.0001f)
		return -1;
	const float t = v3_dot(v3_sub(pp, o), pn) / den;
	if (t < 0 || t > 9.0f)
		return -1;
	const Vec3 hit = v3_add(o, v3_scale(d, t));
	const int i = (int)floorf((hit.x + TILE / 2) / (TILE / n));
	const int j = (int)floorf(p->piece == BUILD_PIECES_WALL ? hit.y / (H / n) : (hit.z + TILE / 2) / (TILE / n));
	if (i < 0 || j < 0 || i >= n || j >= n)
		return -1;
	return j * n + i;
}

Xf build_tile_xf(const Piece *p, int idx, Vec3 viewer, float *w, float *h)
{
	const int n = build_edit_grid(p->piece);
	const int i = idx % n, j = idx / n;
	const Xf inv = xf_inverse(&p->xf);
	const Vec3 lv = xf_point(&inv, viewer);
	const float x = -TILE / 2 + (i + 0.5f) * TILE / n;
	Xf t = xf_identity();
	*w = TILE / n * 0.94f;
	*h = TILE / n * 0.94f;
	switch (p->piece) {
	case BUILD_PIECES_WALL: {
		const float side = lv.z > -TILE / 2 ? 1.0f : -1.0f;
		const Xf r = xf_rot_x(PI_F / 2);
		const Xf tr = xf_translate(v3(x, (j + 0.5f) * H / n, -TILE / 2 + side * (THICK / 2 + 0.03f)));
		t = xf_mul(&tr, &r);
		*h = H / n * 0.94f;
		break;
	}
	case BUILD_PIECES_FLOOR: {
		const float up = lv.y > 0 ? 1.0f : -1.0f;
		t = xf_translate(v3(x, up * (THICK / 2 + 0.03f), -TILE / 2 + (j + 0.5f) * TILE / n));
		break;
	}
	case BUILD_PIECES_CONE: {
		const bool above = lv.y > CONE_H / 2;
		t = xf_translate(v3(x, above ? CONE_H + 0.05f : -0.05f, -TILE / 2 + (j + 0.5f) * TILE / n));
		break;
	}
	default: {
		const float z = -TILE / 2 + (j + 0.5f) * TILE / n;
		const Xf rb = xf_yaw(CARD_YAW[p->ramp_card], v3(0, 0, 0));
		const Xf rbi = xf_inverse(&rb);
		const Vec3 lp = xf_point(&rbi, v3(x, 0, z));
		const float y = H / 2 - lp.z * H / TILE + THICK / 2 + 0.04f;
		const Xf sl = xf_rot_x(atan2f(H, TILE));
		const Xf b = xf_mul(&rb, &sl);
		const Xf tr = xf_translate(v3(x, y, z));
		t = xf_mul(&tr, &b);
		*h = sqrtf(TILE * TILE + H * H) / n * 0.94f;
		break;
	}
	}
	return xf_mul(&p->xf, &t);
}

bool build_apply_edit(Piece *p, const int *sel, int count)
{
	const int n = build_edit_grid(p->piece);
	if (p->piece == BUILD_PIECES_RAMP) {
		if (count < 2)
			return false;
		const int a = sel[0], b = sel[count - 1];
		const Vec3 d = v3((float)(b % n - a % n), 0, (float)(b / n - a / n));
		if (v3_length(d) < 0.5f)
			return false;
		int best = 0;
		for (int k = 0; k < 4; k++)
			if (v3_dot(CARD[k], d) > v3_dot(CARD[best], d))
				best = k;
		p->ramp_card = best;
		rebuild(p);
		return true;
	}
	bool out[9] = { 0 };
	for (int i = 0; i < count; i++)
		if (sel[i] >= 0 && sel[i] < n * n)
			out[sel[i]] = true;
	bool any_left = false;
	for (int i = 0; i < n * n; i++)
		any_left |= !out[i];
	if (!any_left)
		return false; // tirar tudo nao vale, como no Fortnite
	memcpy(p->edits, out, sizeof out);
	rebuild(p);
	return true;
}

void build_reset_edit(Piece *p)
{
	memset(p->edits, 0, sizeof p->edits);
	p->ramp_card = 0;
	rebuild(p);
}

// ---------------------------------------------------------------- desenho

static void quad(FxVertex *v, const Xf *x, float w, float h, const float c[4])
{
	const Vec3 a = xf_point(x, v3(-w / 2, 0, -h / 2)), b = xf_point(x, v3(w / 2, 0, -h / 2)),
			   cc = xf_point(x, v3(w / 2, 0, h / 2)), d = xf_point(x, v3(-w / 2, 0, h / 2));
	const Vec3 q[6] = { a, b, cc, a, cc, d };
	static const float uv[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
	for (int i = 0; i < 6; i++)
		v[i] = (FxVertex){ q[i].x, q[i].y, q[i].z, c[0], c[1], c[2], c[3], uv[i][0], uv[i][1] };
}

static uint32_t hex_color(const char *hex, float *rgb)
{
	const unsigned v = (unsigned)strtoul(hex, NULL, 16);
	rgb[0] = ((v >> 16) & 0xFF) / 255.0f;
	rgb[1] = ((v >> 8) & 0xFF) / 255.0f;
	rgb[2] = (v & 0xFF) / 255.0f;
	return v;
}

void build_update_and_draw(float dt)
{
	for (int i = 0; i < MAX_PIECES; i++) {
		Piece *p = &G.m.pieces[i];
		if (!p->used)
			continue;
		if (p->dying) {
			if (G.time >= p->die_at) {
				const float c[4] = { 0.6f, 0.42f, 0.22f, 1 };
				fx_particles(v3_scale(v3_add(p->lo, p->hi), 0.5f), c, 24);
				free(p->tris);
				p->tris = NULL;
				p->used = false;
				world_update_dynamic();
			}
			continue;
		}
		if (p->built < p->build_time) {
			p->built += dt;
			p->hp = fminf(p->max_hp, p->hp + p->max_hp * 0.9f * dt / p->build_time);
		}
		float rgb[3];
		hex_color(BUILD_MATERIALS[p->mat].color, rgb);
		const float k = clampf(p->built / p->build_time, 0, 1);
		const int tex = G.build_tex[p->mat];
		const float tint = tex != GFX_WHITE ? 0.25f : 1.0f;
		const float c[4] = { 1 + (rgb[0] - 1) * tint, 1 + (rgb[1] - 1) * tint, 1 + (rgb[2] - 1) * tint, 0.4f + 0.6f * k };
		const uint32_t n = (p->tri_count < 256 ? p->tri_count : 256) * 3;
		gfx_draw_mesh(p->mesh, NULL, 0, n, tex, c, c[3] < 0.99f ? GFX_BLEND : 0);
	}
}

// O holograma azul onde a peca vai (vermelho onde nao da), e os quadrados da edicao.
void build_draw_ghost(const Fighter *f)
{
	static FxVertex v[256 * 3 + 9 * 6];
	uint32_t n = 0;
	if (f->build_piece >= 0) {
		const Placement p = build_placement(f, f->build_piece, NAN, NAN);
		const bool ok = build_can(f, f->build_piece, &p);
		Box boxes[9];
		Vec3 tris[24];
		int tc;
		bool none[9] = { 0 };
		const int nb = parts(f->build_piece, none, 0, boxes, tris, &tc);
		const float c[4] = { ok ? 0.3f : 1.0f, ok ? 0.6f : 0.3f, ok ? 1.0f : 0.25f, 0.3f };
		for (int i = 0; i < nb; i++) {
			float t[12 * 9];
			box_tris(&p.xf, &boxes[i], t);
			for (int k = 0; k < 36; k++)
				v[n++] = (FxVertex){ t[k * 3], t[k * 3 + 1], t[k * 3 + 2], c[0], c[1], c[2], c[3], 0, 0 };
		}
		for (int k = 0; k < tc * 3; k++) {
			const Vec3 q = xf_point(&p.xf, tris[k]);
			v[n++] = (FxVertex){ q.x, q.y, q.z, c[0], c[1], c[2], c[3], 0, 0 };
		}
	}
	if (f->editing >= 0 && G.m.pieces[f->editing].used) {
		const Piece *p = &G.m.pieces[f->editing];
		const int g = build_edit_grid(p->piece);
		const int hover = build_tile_at(p, fighter_eye(f), fighter_aim(f));
		for (int i = 0; i < g * g; i++) {
			bool sel = false;
			for (int k = 0; k < f->edit_sel_count; k++)
				sel |= f->edit_sel[k] == i;
			float c[4] = { 0.3f, 0.6f, 1.0f, 0.3f };
			if (sel) {
				c[0] = 1.0f;
				c[1] = 0.6f;
				c[2] = 0.25f;
				c[3] = 0.1f;
			}
			if (i == hover) {
				c[0] = sel ? 1.0f : 0.55f;
				c[1] = sel ? 0.7f : 0.85f;
				c[2] = sel ? 0.4f : 1.0f;
				c[3] = sel ? 0.35f : 0.55f;
			}
			float w, h;
			const Xf x = build_tile_xf(p, i, fighter_eye(f), &w, &h);
			quad(v + n, &x, w, h, c);
			n += 6;
		}
	}
	gfx_draw_fx(v, n, GFX_WHITE, GFX_BLEND);
}

void build_clear(void)
{
	for (int i = 0; i < MAX_PIECES; i++) {
		Piece *p = &G.m.pieces[i];
		free(p->tris);
		p->tris = NULL;
		p->used = false;
		p->dying = false;
	}
	world_update_dynamic();
}


// No cliente da rede: a peca idx como o host diz. So reconstroi a malha (e a colisao) quando a forma muda.
void piece_set_remote(int idx, bool live, int piece, int mat, int team, int card, int ramp, uint32_t edits, Vec3 c, float oy,
	float built, float hp)
{
	Piece *p = &G.m.pieces[idx];
	if (!live) {
		if (p->used) {
			const float col[4] = { 0.6f, 0.42f, 0.22f, 1 };
			fx_particles(v3_scale(v3_add(p->lo, p->hi), 0.5f), col, 24);
			free(p->tris);
			p->tris = NULL;
			p->used = false;
			world_update_dynamic();
		}
		return;
	}
	bool ed[9];
	for (int e = 0; e < 9; e++)
		ed[e] = (edits >> e & 1) != 0;
	const Xf xf = xf_yaw(CARD_YAW[card & 3], c);
	const bool same = p->used && !p->dying && p->piece == piece && p->card == card && p->ramp_card == ramp &&
		!memcmp(p->edits, ed, sizeof ed) && fabsf(p->xf.m[3] - c.x) < 0.01f && fabsf(p->xf.m[7] - c.y) < 0.01f &&
		fabsf(p->xf.m[11] - c.z) < 0.01f;
	const BuildPiecesDef *row = &BUILD_PIECES[piece];
	const BuildMaterialsDef *mr = &BUILD_MATERIALS[mat];
	p->mat = mat;
	p->team = team;
	p->max_hp = row->max_hp * mr->hp_mult;
	p->build_time = row->build_time * mr->time_mult;
	p->built = built * p->build_time;
	p->hp = hp * p->max_hp;
	if (same)
		return;
	p->used = true;
	p->dying = false;
	p->piece = piece;
	p->card = card;
	p->ramp_card = ramp;
	p->oy = oy;
	p->xf = xf;
	memcpy(p->edits, ed, sizeof ed);
	rebuild(p);
}
