// O mundo da partida: raios (tiro, visao) contra o mapa, as pecas e os lutadores; bombsites; e a navegacao dos bots
// no navmesh que o proprio Bot Strike assou (A* nos poligonos e funil, como o NavigationServer do Godot).
#include <stdlib.h>
#include <string.h>

#include "game.h"

// ---------------------------------------------------------------- triangulos das pecas para a colisao

static float dyn_tris[65536 * 9];
static BsDynGroup dyn_groups[MAX_PIECES];

void world_update_dynamic(void)
{
	uint32_t n = 0, groups = 0;
	for (int i = 0; i < MAX_PIECES; i++) {
		const Piece *p = &G.m.pieces[i];
		if (!p->used || p->dying || !p->tris)
			continue;
		if (n + p->tri_count > 65536)
			break;
		memcpy(dyn_tris + (size_t)n * 9, p->tris, p->tri_count * 9 * sizeof(float));
		dyn_groups[groups++] = (BsDynGroup){ p->lo, p->hi, n, p->tri_count, i };
		n += p->tri_count;
	}
	bs_collider_set_dynamic(&G.col, dyn_tris, n, dyn_groups, groups);
}

static int piece_of_tri(uint32_t tri)
{
	const uint32_t k = tri - G.map.triangle_count;
	for (uint32_t g = 0; g < G.col.group_count; g++)
		if (k >= dyn_groups[g].first && k < dyn_groups[g].first + dyn_groups[g].count)
			return dyn_groups[g].owner;
	return -1;
}

// ---------------------------------------------------------------- raios

// Raio contra o corpo de um lutador: um cilindro de raio 0.35 dos pes ate a altura dele.
static bool ray_body(const Fighter *f, Vec3 o, Vec3 d, float max_t, float *t)
{
	const float r = 0.35f;
	const Vec3 base = f->body.position;
	const float ox = o.x - base.x, oz = o.z - base.z;
	const float a = d.x * d.x + d.z * d.z, b = 2 * (ox * d.x + oz * d.z), c = ox * ox + oz * oz - r * r;
	if (a < 1e-9f)
		return false;
	const float disc = b * b - 4 * a * c;
	if (disc < 0)
		return false;
	const float sq = sqrtf(disc);
	float h = (-b - sq) / (2 * a);
	if (h < 0)
		h = (-b + sq) / (2 * a);
	if (h < 0 || h > max_t)
		return false;
	const float y = o.y + d.y * h - base.y;
	if (y < 0.0f || y > f->body.height)
		return false;
	*t = h;
	return true;
}

RayHit world_ray(Vec3 from, Vec3 to, uint32_t mask, int ignore)
{
	RayHit h = { 0 };
	Vec3 d = v3_sub(to, from);
	const float len = v3_length(d);
	if (len < 1e-5f)
		return h;
	d = v3_scale(d, 1.0f / len);
	float best = len;
	if (mask & (RAY_WORLD | RAY_PIECES)) {
		float t;
		const uint32_t tri = bs_collider_ray(&G.col, from, d, len, (mask & RAY_CLIP) ? 0 : BS_COLLISION_CLIP,
			(mask & RAY_PIECES) != 0, &t);
		if (tri != UINT32_MAX && t <= best) {
			const bool dyn = tri >= G.map.triangle_count;
			if (dyn || (mask & RAY_WORLD)) {
				best = t;
				h.hit = true;
				h.t = t;
				h.kind = dyn ? HIT_PIECE : HIT_WORLD;
				h.index = dyn ? piece_of_tri(tri) : (int)tri;
				h.flags = dyn ? 0 : G.map.triangle_flags[tri];
				Vec3 n = bs_collider_normal(&G.col, tri);
				if (v3_dot(n, d) > 0)
					n = v3_scale(n, -1.0f);
				h.normal = n;
			}
		}
	}
	if (mask & RAY_FIGHTERS)
		for (int i = 0; i < G.m.count; i++) {
			const Fighter *f = &G.m.f[i];
			float t;
			if (i == ignore || !f->alive)
				continue;
			if (ray_body(f, from, d, best, &t)) {
				best = t;
				h.hit = true;
				h.t = t;
				h.kind = HIT_FIGHTER;
				h.index = i;
				h.normal = v3_scale(d, -1.0f);
			}
		}
	h.point = v3_add(from, v3_scale(d, h.hit ? h.t : len));
	if (!h.hit)
		h.t = len;
	return h;
}

// ---------------------------------------------------------------- bombsites

bool world_site_at(Vec3 p, int *site)
{
	for (int s = 0; s < 2; s++) {
		const float *b = G.as.sites[s];
		const float y = p.y + 0.3f;
		if (p.x >= b[0] && p.x <= b[3] && y >= b[1] && y <= b[4] && p.z >= b[2] && p.z <= b[5]) {
			if (site)
				*site = s;
			return true;
		}
	}
	return false;
}

Vec3 world_random_in_site(int s)
{
	const float *b = G.as.sites[s & 1];
	const Vec3 p = v3(b[0] + frand() * (b[3] - b[0]), b[1] + (b[4] - b[1]) * 0.2f, b[2] + frand() * (b[5] - b[2]));
	return nav_closest(p);
}

// ---------------------------------------------------------------- navmesh

static Vec3 nav_v(uint32_t i)
{
	const float *v = G.as.nav.verts + i * 3;
	return v3(v[0], v[1], v[2]);
}

typedef struct {
	uint32_t a, b, poly;
} Edge;

static int edge_cmp(const void *x, const void *y)
{
	const Edge *e = x, *f = y;
	if (e->a != f->a)
		return e->a < f->a ? -1 : 1;
	if (e->b != f->b)
		return e->b < f->b ? -1 : 1;
	return 0;
}

static bool point_in_poly(uint32_t p, float x, float z)
{
	const NavData *n = G.nav.nav;
	bool in = false;
	const uint32_t s = n->poly_start[p], e = n->poly_start[p + 1];
	for (uint32_t i = s, j = e - 1; i < e; j = i++) {
		const Vec3 a = nav_v(n->poly_index[i]), b = nav_v(n->poly_index[j]);
		if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x)
			in = !in;
	}
	return in;
}

static float poly_height(uint32_t p, float x, float z)
{
	// a altura no ponto, pelo leque de triangulos do poligono
	const NavData *n = G.nav.nav;
	const uint32_t s = n->poly_start[p], e = n->poly_start[p + 1];
	const Vec3 a = nav_v(n->poly_index[s]);
	for (uint32_t i = s + 1; i + 1 < e; i++) {
		const Vec3 b = nav_v(n->poly_index[i]), c = nav_v(n->poly_index[i + 1]);
		const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
		if (fabsf(d) < 1e-9f)
			continue;
		const float l1 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
		const float l2 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
		const float l3 = 1 - l1 - l2;
		if (l1 >= -0.01f && l2 >= -0.01f && l3 >= -0.01f)
			return l1 * a.y + l2 * b.y + l3 * c.y;
	}
	return G.nav.center[p].y;
}

static Vec3 closest_on_seg(Vec3 p, Vec3 a, Vec3 b)
{
	const Vec3 ab = v3_sub(b, a);
	const float l = v3_dot(ab, ab);
	const float t = l > 1e-9f ? clampf(v3_dot(v3_sub(p, a), ab) / l, 0, 1) : 0;
	return v3_add(a, v3_scale(ab, t));
}

// O ponto do navmesh mais perto de p, e o poligono dele.
static Vec3 closest(Vec3 p, uint32_t *poly)
{
	const Nav *nv = &G.nav;
	const NavData *n = nv->nav;
	if (!n) {
		if (poly)
			*poly = UINT32_MAX;
		return p;
	}
	uint32_t best = UINT32_MAX;
	float best_d = 1e30f;
	Vec3 best_p = p;
	for (uint32_t i = 0; i < n->poly_count; i++) {
		const float *b = nv->box + i * 6;
		if (p.x < b[0] - 0.01f || p.x > b[3] + 0.01f || p.z < b[2] - 0.01f || p.z > b[5] + 0.01f)
			continue;
		if (!point_in_poly(i, p.x, p.z))
			continue;
		const float y = poly_height(i, p.x, p.z);
		const float d = fabsf(y - p.y) + (y > p.y + 0.5f ? 1.0f : 0.0f); // prefere o chao embaixo
		if (d < best_d) {
			best_d = d;
			best = i;
			best_p = v3(p.x, y, p.z);
		}
	}
	if (best == UINT32_MAX || best_d > 3.0f) {
		// fora de qualquer poligono: o ponto mais perto nas bordas
		best_d = 1e30f;
		for (uint32_t i = 0; i < n->poly_count; i++) {
			const float *b = nv->box + i * 6;
			const float dx = fmaxf(fmaxf(b[0] - p.x, 0), p.x - b[3]), dz = fmaxf(fmaxf(b[2] - p.z, 0), p.z - b[5]);
			const float dy = fmaxf(fmaxf(b[1] - p.y, 0), p.y - b[4]);
			if (dx * dx + dz * dz + dy * dy * 4 > best_d)
				continue;
			const uint32_t s = n->poly_start[i], e = n->poly_start[i + 1];
			for (uint32_t k = s; k < e; k++) {
				const Vec3 a = nav_v(n->poly_index[k]), c = nav_v(n->poly_index[k + 1 < e ? k + 1 : s]);
				const Vec3 q = closest_on_seg(p, a, c);
				const Vec3 dd = v3_sub(q, p);
				const float d = dd.x * dd.x + dd.z * dd.z + dd.y * dd.y * 4;
				if (d < best_d) {
					best_d = d;
					best = i;
					// um pouco para dentro do poligono, para nao ficar na borda
					best_p = v3_add(q, v3_scale(v3_sub(nv->center[i], q), 0.05f));
				}
			}
		}
	}
	if (poly)
		*poly = best;
	return best_p;
}

Vec3 nav_closest(Vec3 p)
{
	return closest(p, NULL);
}

static float triarea2(Vec3 a, Vec3 b, Vec3 c)
{
	const float ax = b.x - a.x, ay = b.z - a.z, bx = c.x - a.x, by = c.z - a.z;
	return bx * ay - ax * by;
}

static bool vequal(Vec3 a, Vec3 b)
{
	const Vec3 d = v3_sub(a, b);
	return v3_dot(d, d) < 1e-6f;
}

int nav_path(Vec3 from, Vec3 to, Vec3 *out, int max)
{
	Nav *nv = &G.nav;
	if (!nv->nav || max < 2) {
		out[0] = to;
		return 1;
	}
	uint32_t sp, gp;
	const Vec3 s = closest(from, &sp);
	const Vec3 g = closest(to, &gp);
	if (sp == UINT32_MAX || gp == UINT32_MAX) {
		out[0] = to;
		return 1;
	}
	if (sp == gp) {
		out[0] = g;
		return 1;
	}
	// A* nos poligonos, custo pela distancia entre centros
	const uint32_t pc = nv->nav->poly_count;
	if (++nv->search == 0) {
		memset(nv->stamp, 0, pc * 4);
		nv->search = 1;
	}
	uint32_t open_n = 0;
	nv->g[sp] = 0;
	nv->came[sp] = UINT32_MAX;
	nv->stamp[sp] = nv->search;
	nv->open[open_n++] = sp;
	bool found = false;
	int guard = 0;
	while (open_n && guard++ < 20000) {
		// o menor f (lista simples: o navmesh tem poucos milhares de poligonos)
		uint32_t bi = 0;
		float bf = 1e30f;
		for (uint32_t k = 0; k < open_n; k++) {
			const uint32_t q = nv->open[k];
			const float f = nv->g[q] + v3_length(v3_sub(nv->center[q], g));
			if (f < bf) {
				bf = f;
				bi = k;
			}
		}
		const uint32_t cur = nv->open[bi];
		nv->open[bi] = nv->open[--open_n];
		if (cur == gp) {
			found = true;
			break;
		}
		for (uint32_t k = nv->adj_start[cur]; k < nv->adj_start[cur + 1]; k++) {
			const uint32_t nb = nv->adj[k];
			const float ng = nv->g[cur] + v3_length(v3_sub(nv->center[nb], nv->center[cur]));
			if (nv->stamp[nb] == nv->search && ng >= nv->g[nb])
				continue;
			const bool fresh = nv->stamp[nb] != nv->search || nv->g[nb] >= 1e29f;
			nv->stamp[nb] = nv->search;
			nv->g[nb] = ng;
			nv->came[nb] = cur;
			if (fresh || true) {
				bool in_open = false;
				for (uint32_t o = 0; o < open_n; o++)
					if (nv->open[o] == nb) {
						in_open = true;
						break;
					}
				if (!in_open && open_n < pc)
					nv->open[open_n++] = nb;
			}
		}
	}
	if (!found) {
		out[0] = g;
		return 1;
	}
	// corredor de poligonos (do fim para o comeco)
	static uint32_t corridor[4096];
	int cn = 0;
	for (uint32_t q = gp; q != UINT32_MAX && cn < 4096; q = nv->came[q])
		corridor[cn++] = q;
	// portais: a aresta comum de cada par, direita e esquerda no sentido do caminho
	static Vec3 left[4098], right[4098];
	int np = 0;
	left[np] = right[np] = s;
	np++;
	for (int i = cn - 1; i > 0; i--) {
		const uint32_t a = corridor[i], b = corridor[i - 1];
		Vec3 pa = s, pb = s;
		for (uint32_t k = nv->adj_start[a]; k < nv->adj_start[a + 1]; k++)
			if (nv->adj[k] == b) {
				pa = nav_v(nv->adj_edge[k * 2]);
				pb = nav_v(nv->adj_edge[k * 2 + 1]);
				break;
			}
		if (triarea2(nv->center[a], pa, pb) < 0) {
			right[np] = pa;
			left[np] = pb;
		} else {
			right[np] = pb;
			left[np] = pa;
		}
		np++;
	}
	left[np] = right[np] = g;
	np++;
	// funil (Mononen, "simple stupid funnel")
	int n = 0;
	Vec3 apex = s, pl = left[0], pr = right[0];
	int ai = 0, li = 0, ri = 0;
	for (int i = 1; i < np && n < max - 1; i++) {
		const Vec3 l = left[i], r = right[i];
		if (triarea2(apex, pr, r) <= 0.0f) {
			if (vequal(apex, pr) || triarea2(apex, pl, r) > 0.0f) {
				pr = r;
				ri = i;
			} else {
				apex = pl;
				ai = li;
				out[n++] = apex;
				pl = pr = apex;
				li = ri = ai;
				i = ai;
				continue;
			}
		}
		if (triarea2(apex, pl, l) >= 0.0f) {
			if (vequal(apex, pl) || triarea2(apex, pr, l) < 0.0f) {
				pl = l;
				li = i;
			} else {
				apex = pr;
				ai = ri;
				out[n++] = apex;
				pl = pr = apex;
				li = ri = ai;
				i = ai;
				continue;
			}
		}
	}
	out[n++] = g;
	return n;
}

// ---------------------------------------------------------------- carga

static bool build_nav(char *err, size_t eb)
{
	Nav *nv = &G.nav;
	memset(nv, 0, sizeof *nv);
	if (!G.as.has_nav) {
		snprintf(err, eb, "o assets.pak nao tem o navmesh (abra uma partida no Bot Strike do PC e gere de novo)");
		return false;
	}
	const NavData *n = &G.as.nav;
	nv->nav = n;
	const uint32_t pc = n->poly_count;
	nv->box = malloc(pc * 6 * sizeof(float));
	nv->center = malloc(pc * sizeof(Vec3));
	nv->g = malloc(pc * sizeof(float));
	nv->came = malloc(pc * 4);
	nv->open = malloc(pc * 4);
	nv->stamp = calloc(pc, 4);
	nv->reachable = calloc(pc, 1);
	const uint32_t edges = n->poly_start[pc];
	Edge *e = malloc(edges * sizeof *e);
	if (!nv->box || !nv->center || !nv->g || !nv->came || !nv->open || !nv->stamp || !nv->reachable || !e) {
		free(e);
		snprintf(err, eb, "sem memoria para o navmesh");
		return false;
	}
	uint32_t en = 0;
	for (uint32_t p = 0; p < pc; p++) {
		float *b = nv->box + p * 6;
		b[0] = b[1] = b[2] = 1e30f;
		b[3] = b[4] = b[5] = -1e30f;
		Vec3 c = v3(0, 0, 0);
		const uint32_t s = n->poly_start[p], t = n->poly_start[p + 1];
		for (uint32_t k = s; k < t; k++) {
			const Vec3 v = nav_v(n->poly_index[k]);
			c = v3_add(c, v);
			b[0] = fminf(b[0], v.x);
			b[1] = fminf(b[1], v.y);
			b[2] = fminf(b[2], v.z);
			b[3] = fmaxf(b[3], v.x);
			b[4] = fmaxf(b[4], v.y);
			b[5] = fmaxf(b[5], v.z);
			const uint32_t a = n->poly_index[k], bb = n->poly_index[k + 1 < t ? k + 1 : s];
			e[en++] = (Edge){ a < bb ? a : bb, a < bb ? bb : a, p };
		}
		nv->center[p] = v3_scale(c, 1.0f / (float)(t - s));
	}
	qsort(e, en, sizeof *e, edge_cmp);
	// vizinhos: arestas iguais em poligonos diferentes
	uint32_t *count = calloc(pc + 1, 4);
	for (uint32_t i = 0; i + 1 < en; i++)
		if (e[i].a == e[i + 1].a && e[i].b == e[i + 1].b && e[i].poly != e[i + 1].poly) {
			count[e[i].poly]++;
			count[e[i + 1].poly]++;
		}
	nv->adj_start = malloc((pc + 1) * 4);
	uint32_t total = 0;
	for (uint32_t p = 0; p < pc; p++) {
		nv->adj_start[p] = total;
		total += count[p];
	}
	nv->adj_start[pc] = total;
	nv->adj = malloc((total + 1) * 4);
	nv->adj_edge = malloc((total + 1) * 8);
	memset(count, 0, (pc + 1) * 4);
	for (uint32_t i = 0; i + 1 < en; i++)
		if (e[i].a == e[i + 1].a && e[i].b == e[i + 1].b && e[i].poly != e[i + 1].poly) {
			const uint32_t p = e[i].poly, q = e[i + 1].poly;
			uint32_t k = nv->adj_start[p] + count[p]++;
			nv->adj[k] = q;
			nv->adj_edge[k * 2] = e[i].a;
			nv->adj_edge[k * 2 + 1] = e[i].b;
			k = nv->adj_start[q] + count[q]++;
			nv->adj[k] = p;
			nv->adj_edge[k * 2] = e[i].a;
			nv->adj_edge[k * 2 + 1] = e[i].b;
		}
	free(count);
	free(e);
	// o que se alcanca dos spawns (o radar do mapa tatico mostra so isso: os telhados ficam de fora)
	uint32_t *todo = malloc(pc * 4);
	uint32_t tn = 0;
	for (uint32_t s = 0; s < G.map.spawn_count; s++) {
		uint32_t p;
		closest(G.map.spawns[s].position, &p);
		if (p != UINT32_MAX && !nv->reachable[p]) {
			nv->reachable[p] = true;
			todo[tn++] = p;
		}
	}
	while (tn) {
		const uint32_t p = todo[--tn];
		for (uint32_t k = nv->adj_start[p]; k < nv->adj_start[p + 1]; k++)
			if (!nv->reachable[nv->adj[k]]) {
				nv->reachable[nv->adj[k]] = true;
				todo[tn++] = nv->adj[k];
			}
	}
	free(todo);
	plat_log("navmesh: %u poligonos, %u ligacoes", (unsigned)pc, (unsigned)total / 2);
	return true;
}

bool world_init(char *err, size_t eb)
{
	if (!G.as.has_sites) {
		snprintf(err, eb, "o assets.pak nao tem os bombsites");
		return false;
	}
	return build_nav(err, eb);
}
