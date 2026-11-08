#include "collide.h"

#include <stdlib.h>
#include <string.h>

bool bs_collider_init(BsCollider *c, const BsPack *pack)
{
	memset(c, 0, sizeof *c);
	c->pack = pack;
	// os dinamicos tambem tem selo: ate 64 mil triangulos de construcao
	c->stamp = calloc(pack->triangle_count + 65536, sizeof *c->stamp);
	return c->stamp != NULL;
}

void bs_collider_free(BsCollider *c)
{
	free(c->stamp);
	c->stamp = NULL;
}

void bs_collider_set_dynamic(BsCollider *c, const float *tris, uint32_t count, const BsDynGroup *groups,
	uint32_t group_count)
{
	c->dyn = tris;
	c->dyn_count = count < 65536 ? count : 65536;
	c->groups = groups;
	c->group_count = group_count;
}

const float *bs_collider_tri(const BsCollider *c, uint32_t i)
{
	const uint32_t n = c->pack->triangle_count;
	return i < n ? c->pack->triangles + (size_t)i * 9 : c->dyn + (size_t)(i - n) * 9;
}

static int32_t cell_of(float v, float origin, float cell, uint32_t n)
{
	int32_t i = (int32_t)floorf((v - origin) / cell);
	if (i < 0)
		i = 0;
	if (i >= (int32_t)n)
		i = (int32_t)n - 1;
	return i;
}

static bool tri_box(const float *v, Vec3 lo, Vec3 hi)
{
	return !(fmaxf(v[0], fmaxf(v[3], v[6])) < lo.x || fminf(v[0], fminf(v[3], v[6])) > hi.x ||
		fmaxf(v[1], fmaxf(v[4], v[7])) < lo.y || fminf(v[1], fminf(v[4], v[7])) > hi.y ||
		fmaxf(v[2], fmaxf(v[5], v[8])) < lo.z || fminf(v[2], fminf(v[5], v[8])) > hi.z);
}

static void next_stamp(BsCollider *c)
{
	if (++c->stamp_id == 0) {
		memset(c->stamp, 0, (c->pack->triangle_count + 65536) * sizeof *c->stamp);
		c->stamp_id = 1;
	}
}

void bs_collider_gather(BsCollider *c, Vec3 lo, Vec3 hi)
{
	const BsGrid *g = &c->pack->grid;
	c->candidate_count = 0;
	c->overflowed = false;
	next_stamp(c);
	// as pecas de construcao primeiro: sao poucas e ficam onde o jogador esta
	for (uint32_t k = 0; k < c->group_count; k++) {
		const BsDynGroup *gr = &c->groups[k];
		if (gr->hi.x < lo.x || gr->lo.x > hi.x || gr->hi.y < lo.y || gr->lo.y > hi.y || gr->hi.z < lo.z ||
			gr->lo.z > hi.z)
			continue;
		for (uint32_t t = gr->first; t < gr->first + gr->count && t < c->dyn_count; t++) {
			if (!tri_box(c->dyn + (size_t)t * 9, lo, hi))
				continue;
			if (c->candidate_count == BS_COLLIDER_MAX_CANDIDATES) {
				c->overflowed = true;
				return;
			}
			c->candidates[c->candidate_count++] = c->pack->triangle_count + t;
		}
	}
	// Fora da grade nao ha triangulo nenhum do mapa.
	const Vec3 gmax = v3(g->origin.x + g->nx * g->cell, g->origin.y + g->ny * g->cell, g->origin.z + g->nz * g->cell);
	if (hi.x < g->origin.x || hi.y < g->origin.y || hi.z < g->origin.z || lo.x > gmax.x || lo.y > gmax.y ||
		lo.z > gmax.z)
		return;
	const int32_t x0 = cell_of(lo.x, g->origin.x, g->cell, g->nx), x1 = cell_of(hi.x, g->origin.x, g->cell, g->nx);
	const int32_t y0 = cell_of(lo.y, g->origin.y, g->cell, g->ny), y1 = cell_of(hi.y, g->origin.y, g->cell, g->ny);
	const int32_t z0 = cell_of(lo.z, g->origin.z, g->cell, g->nz), z1 = cell_of(hi.z, g->origin.z, g->cell, g->nz);
	const float *tri = c->pack->triangles;
	for (int32_t z = z0; z <= z1; z++)
		for (int32_t y = y0; y <= y1; y++)
			for (int32_t x = x0; x <= x1; x++) {
				const uint32_t cell = ((uint32_t)z * g->ny + (uint32_t)y) * g->nx + (uint32_t)x;
				for (uint32_t r = g->cell_start[cell]; r < g->cell_start[cell + 1]; r++) {
					const uint32_t t = g->refs[r];
					if (c->stamp[t] == c->stamp_id)
						continue;
					c->stamp[t] = c->stamp_id;
					if (!tri_box(tri + t * 9, lo, hi))
						continue;
					if (c->candidate_count == BS_COLLIDER_MAX_CANDIDATES) {
						c->overflowed = true;
						return;
					}
					c->candidates[c->candidate_count++] = t;
				}
			}
}

Vec3 bs_collider_normal(const BsCollider *c, uint32_t i)
{
	const float *v = bs_collider_tri(c, i);
	const Vec3 a = v3(v[0], v[1], v[2]), b = v3(v[3], v[4], v[5]), d = v3(v[6], v[7], v[8]);
	return v3_normalize(v3_cross(v3_sub(b, a), v3_sub(d, a)));
}

// Ericson, Real-Time Collision Detection, 5.1.5.
Vec3 bs_collider_closest(const BsCollider *c, uint32_t i, Vec3 p)
{
	const float *v = bs_collider_tri(c, i);
	const Vec3 a = v3(v[0], v[1], v[2]), b = v3(v[3], v[4], v[5]), cc = v3(v[6], v[7], v[8]);
	const Vec3 ab = v3_sub(b, a), ac = v3_sub(cc, a), ap = v3_sub(p, a);
	const float d1 = v3_dot(ab, ap), d2 = v3_dot(ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f)
		return a;
	const Vec3 bp = v3_sub(p, b);
	const float d3 = v3_dot(ab, bp), d4 = v3_dot(ac, bp);
	if (d3 >= 0.0f && d4 <= d3)
		return b;
	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
		return v3_add(a, v3_scale(ab, d1 / (d1 - d3)));
	const Vec3 cp = v3_sub(p, cc);
	const float d5 = v3_dot(ab, cp), d6 = v3_dot(ac, cp);
	if (d6 >= 0.0f && d5 <= d6)
		return cc;
	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
		return v3_add(a, v3_scale(ac, d2 / (d2 - d6)));
	const float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
		return v3_add(b, v3_scale(v3_sub(cc, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
	const float denom = 1.0f / (va + vb + vc);
	return v3_add(a, v3_add(v3_scale(ab, vb * denom), v3_scale(ac, vc * denom)));
}

// Moller-Trumbore, nos dois lados.
static bool ray_tri(const float *v, Vec3 o, Vec3 d, float *t)
{
	const Vec3 a = v3(v[0], v[1], v[2]);
	const Vec3 e1 = v3(v[3] - v[0], v[4] - v[1], v[5] - v[2]), e2 = v3(v[6] - v[0], v[7] - v[1], v[8] - v[2]);
	const Vec3 p = v3_cross(d, e2);
	const float det = v3_dot(e1, p);
	if (fabsf(det) < 1e-9f)
		return false;
	const float inv = 1.0f / det;
	const Vec3 s = v3_sub(o, a);
	const float u = v3_dot(s, p) * inv;
	if (u < 0.0f || u > 1.0f)
		return false;
	const Vec3 q = v3_cross(s, e1);
	const float w = v3_dot(d, q) * inv;
	if (w < 0.0f || u + w > 1.0f)
		return false;
	*t = v3_dot(e2, q) * inv;
	return *t >= 0.0f;
}

uint32_t bs_collider_ray(BsCollider *c, Vec3 o, Vec3 d, float max_t, uint8_t skip, bool dynamic, float *out_t)
{
	uint32_t best = UINT32_MAX;
	float best_t = max_t;
	if (dynamic)
		for (uint32_t k = 0; k < c->group_count; k++) {
			const BsDynGroup *gr = &c->groups[k];
			// raio contra a caixa do grupo
			float t0 = 0.0f, t1 = best_t;
			const float ol[3] = { o.x, o.y, o.z }, dl[3] = { d.x, d.y, d.z };
			const float lo[3] = { gr->lo.x, gr->lo.y, gr->lo.z }, hi[3] = { gr->hi.x, gr->hi.y, gr->hi.z };
			bool miss = false;
			for (int ax = 0; ax < 3 && !miss; ax++) {
				if (fabsf(dl[ax]) < 1e-9f) {
					miss = ol[ax] < lo[ax] || ol[ax] > hi[ax];
					continue;
				}
				float ta = (lo[ax] - ol[ax]) / dl[ax], tb = (hi[ax] - ol[ax]) / dl[ax];
				if (ta > tb) {
					const float s = ta;
					ta = tb;
					tb = s;
				}
				t0 = fmaxf(t0, ta);
				t1 = fminf(t1, tb);
				miss = t0 > t1;
			}
			if (miss)
				continue;
			for (uint32_t t = gr->first; t < gr->first + gr->count && t < c->dyn_count; t++) {
				float h;
				if (ray_tri(c->dyn + (size_t)t * 9, o, d, &h) && h < best_t) {
					best_t = h;
					best = c->pack->triangle_count + t;
				}
			}
		}
	// DDA pela grade do mapa (Amanatides e Woo)
	const BsGrid *g = &c->pack->grid;
	const float ol[3] = { o.x - g->origin.x, o.y - g->origin.y, o.z - g->origin.z };
	const float dl[3] = { d.x, d.y, d.z };
	const int32_t n[3] = { (int32_t)g->nx, (int32_t)g->ny, (int32_t)g->nz };
	// entra na grade
	float tin = 0.0f, tout = best_t;
	for (int ax = 0; ax < 3; ax++) {
		const float lo = 0.0f, hi = n[ax] * g->cell;
		if (fabsf(dl[ax]) < 1e-9f) {
			if (ol[ax] < lo || ol[ax] > hi)
				goto done;
			continue;
		}
		float ta = (lo - ol[ax]) / dl[ax], tb = (hi - ol[ax]) / dl[ax];
		if (ta > tb) {
			const float s = ta;
			ta = tb;
			tb = s;
		}
		tin = fmaxf(tin, ta);
		tout = fminf(tout, tb);
	}
	if (tin > tout)
		goto done;
	{
		int32_t cell[3], step[3];
		float next[3], delta[3];
		for (int ax = 0; ax < 3; ax++) {
			const float p = ol[ax] + dl[ax] * (tin + 1e-4f);
			cell[ax] = (int32_t)floorf(p / g->cell);
			if (cell[ax] < 0)
				cell[ax] = 0;
			if (cell[ax] >= n[ax])
				cell[ax] = n[ax] - 1;
			if (dl[ax] > 1e-9f) {
				step[ax] = 1;
				next[ax] = ((cell[ax] + 1) * g->cell - ol[ax]) / dl[ax];
				delta[ax] = g->cell / dl[ax];
			} else if (dl[ax] < -1e-9f) {
				step[ax] = -1;
				next[ax] = (cell[ax] * g->cell - ol[ax]) / dl[ax];
				delta[ax] = -g->cell / dl[ax];
			} else {
				step[ax] = 0;
				next[ax] = 1e30f;
				delta[ax] = 1e30f;
			}
		}
		next_stamp(c);
		float t_cell = tin;
		for (int guard = 0; guard < 4096; guard++) {
			if (t_cell > best_t)
				break;
			const uint32_t ci = ((uint32_t)cell[2] * g->ny + (uint32_t)cell[1]) * g->nx + (uint32_t)cell[0];
			for (uint32_t r = g->cell_start[ci]; r < g->cell_start[ci + 1]; r++) {
				const uint32_t t = g->refs[r];
				if (c->stamp[t] == c->stamp_id)
					continue;
				c->stamp[t] = c->stamp_id;
				if (c->pack->triangle_flags[t] & skip)
					continue;
				float h;
				if (ray_tri(c->pack->triangles + (size_t)t * 9, o, d, &h) && h < best_t) {
					best_t = h;
					best = t;
				}
			}
			int ax = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : (next[1] < next[2] ? 1 : 2);
			t_cell = next[ax];
			cell[ax] += step[ax];
			if (step[ax] == 0 || cell[ax] < 0 || cell[ax] >= n[ax])
				break;
			next[ax] += delta[ax];
		}
	}
done:
	if (out_t)
		*out_t = best_t;
	return best;
}
