// Animacao e pele: os clipes que o x360_bake.gd tirou do proprio Godot (a pose de cada slot a cada 1/30 s), tocados
// com mistura entre clipes, e os vertices calculados na CPU a cada quadro (o 360 nao tem como fazer pele no shader
// com o xost de hoje).
#include <stdlib.h>
#include <string.h>

#include "game.h"

static float anim_length(const Anim *a)
{
	return a->frame_count / a->fps;
}

void anim_play(AnimPlayer *p, const Anim *a, float blend)
{
	if (!a)
		return;
	if (p->cur && blend > 0.0f) {
		p->prev = p->cur;
		p->prev_t = p->t;
		p->blend = 0.0f;
		p->blend_len = blend;
	} else {
		p->prev = NULL;
		p->blend = 1.0f;
	}
	p->cur = a;
	p->t = 0.0f;
	p->queued = NULL;
	p->paused = false;
}

void anim_queue(AnimPlayer *p, const Anim *a)
{
	p->queued = a;
}

bool anim_done(const AnimPlayer *p)
{
	return !p->cur || (!p->cur->loop && p->t >= anim_length(p->cur));
}

void anim_update(AnimPlayer *p, float dt)
{
	if (!p->cur || p->paused)
		return;
	p->t += dt;
	if (p->prev) {
		p->prev_t += dt;
		p->blend += p->blend_len > 0.0f ? dt / p->blend_len : 1.0f;
		if (p->blend >= 1.0f) {
			p->blend = 1.0f;
			p->prev = NULL;
		}
	}
	const float len = anim_length(p->cur);
	if (p->t >= len) {
		if (p->cur->loop)
			p->t = fmodf(p->t, len);
		else if (p->queued)
			anim_play(p, p->queued, 0.08f);
		else
			p->t = len;
	}
}

static void sample(const Anim *a, float t, uint32_t slot_count, Xf *out)
{
	const uint32_t n = a->frame_count;
	float f = t * a->fps;
	if (a->loop) {
		f = fmodf(f, (float)n);
		if (f < 0)
			f += n;
	} else if (f > n - 1) {
		f = (float)(n - 1);
	}
	uint32_t i0 = (uint32_t)f;
	if (i0 >= n)
		i0 = n - 1;
	const uint32_t i1 = a->loop ? (i0 + 1) % n : (i0 + 1 < n ? i0 + 1 : i0);
	const float k = f - (float)i0;
	const uint32_t slots = slot_count < a->slot_count ? slot_count : a->slot_count;
	for (uint32_t s = 0; s < slots; s++)
		out[s] = xf_lerp(&a->frames[i0 * a->slot_count + s], &a->frames[i1 * a->slot_count + s], k);
}

static bool skin_alloc(Skin *s, const Model *m)
{
	memset(s, 0, sizeof *s);
	s->mesh = -1;
	if (!m)
		return false;
	s->model = m;
	s->out = malloc(m->vertex_count * sizeof *s->out);
	s->pose = calloc(m->slot_count, sizeof *s->pose);
	s->palette = calloc(m->bind_count, sizeof *s->palette);
	if (!s->out || !s->pose || !s->palette)
		return false;
	for (uint32_t i = 0; i < m->slot_count; i++)
		s->pose[i] = xf_identity();
	// a pose de descanso, se nao houver clipe
	char name[64];
	snprintf(name, sizeof name, "%s.rest", m->name);
	const Anim *rest = assets_anim(&G.as, name);
	if (rest)
		sample(rest, 0.0f, m->slot_count, s->pose);
	return true;
}

bool skin_init(Skin *s, const Model *m)
{
	if (!skin_alloc(s, m))
		return false;
	s->mesh = gfx_skinned(m->vertex_count, m->indices, m->index_count);
	return s->mesh >= 0;
}

void skin_free(Skin *s)
{
	free(s->out);
	free(s->pose);
	free(s->palette);
	memset(s, 0, sizeof *s);
	s->mesh = -1;
}

void skin_pose(Skin *s, const AnimPlayer *p)
{
	if (!s->model || !p->cur)
		return;
	const uint32_t n = s->model->slot_count;
	sample(p->cur, p->t, n, s->pose);
	if (p->prev && p->blend < 1.0f) {
		static Xf old[512];
		sample(p->prev, p->prev_t, n, old);
		for (uint32_t i = 0; i < n; i++)
			s->pose[i] = xf_lerp(&old[i], &s->pose[i], p->blend);
	}
}

// Os vertices no espaco do mundo: world * pose do slot * bind, misturados pelos pesos.
static void skin_compute(Skin *s, const Xf *world)
{
	const Model *m = s->model;
	for (uint32_t b = 0; b < m->bind_count; b++) {
		const Xf a = xf_mul(&s->pose[m->bind_slot[b]], &m->bind_pose[b]);
		s->palette[b] = xf_mul(world, &a);
	}
	for (uint32_t v = 0; v < m->vertex_count; v++) {
		const float *p = m->pos + v * 3, *n = m->nrm + v * 3;
		const uint8_t *bi = m->bones + v * 4, *bw = m->weights + v * 4;
		float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
		for (int k = 0; k < 4; k++) {
			if (!bw[k])
				continue;
			const float w = bw[k] * (1.0f / 255.0f);
			const float *q = s->palette[bi[k]].m;
			x += w * (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3]);
			y += w * (q[4] * p[0] + q[5] * p[1] + q[6] * p[2] + q[7]);
			z += w * (q[8] * p[0] + q[9] * p[1] + q[10] * p[2] + q[11]);
			nx += w * (q[0] * n[0] + q[1] * n[1] + q[2] * n[2]);
			ny += w * (q[4] * n[0] + q[5] * n[1] + q[6] * n[2]);
			nz += w * (q[8] * n[0] + q[9] * n[1] + q[10] * n[2]);
		}
		MeshVertex *o = &s->out[v];
		o->x = x;
		o->y = y;
		o->z = z;
		const float l = sqrtf(nx * nx + ny * ny + nz * nz);
		const float il = l > 1e-8f ? 1.0f / l : 0.0f;
		o->nx = nx * il;
		o->ny = ny * il;
		o->nz = nz * il;
		o->u = m->uv[v * 2];
		o->v = m->uv[v * 2 + 1];
	}
}

static void draw_subs(const Model *m, int mesh, const Mat4 *model, const float tint[4], uint32_t extra)
{
	for (uint32_t i = 0; i < m->sub_count; i++) {
		const MdlSub *sb = &m->subs[i];
		const float c[4] = { sb->color[0] * tint[0], sb->color[1] * tint[1], sb->color[2] * tint[2],
			sb->color[3] * tint[3] };
		const int tex = sb->texture >= 0 ? G.model_tex[sb->texture] : GFX_WHITE;
		gfx_draw_mesh(mesh, model, sb->first_index, sb->index_count, tex, c,
			(sb->flags & 1 ? GFX_ALPHA_TEST : 0) | extra);
	}
}

void skin_draw(Skin *s, const Xf *world, const float tint[4])
{
	if (!s->model || s->mesh < 0)
		return;
	skin_compute(s, world);
	gfx_skinned_vertices(s->mesh, s->out, s->model->vertex_count);
	draw_subs(s->model, s->mesh, NULL, tint, tint[3] < 0.99f ? GFX_BLEND : 0);
}

Vec3 skin_attach(const Skin *s, const char *name, const Xf *world, bool *found)
{
	const int slot = s->model ? assets_attach(s->model, name) : -1;
	if (found)
		*found = slot >= 0;
	if (slot < 0)
		return xf_origin(world);
	return xf_point(world, xf_origin(&s->pose[slot]));
}

// ---------------------------------------------------------------- modelos fixos (pele aplicada uma vez)

int rigid_mesh_of(const char *name)
{
	for (int i = 0; i < G.rigid_count; i++)
		if (!strcmp(G.rigid_model[i]->name, name))
			return i;
	if (G.rigid_count >= (int)(sizeof G.rigid_mesh / sizeof G.rigid_mesh[0]))
		return -1;
	const Model *m = assets_model(&G.as, name);
	if (!m)
		return -1;
	Skin s;
	if (!skin_alloc(&s, m)) {
		skin_free(&s);
		return -1;
	}
	const Xf id = xf_identity();
	skin_compute(&s, &id);
	const int mesh = gfx_mesh(s.out, m->vertex_count, m->indices, m->index_count);
	skin_free(&s);
	if (mesh < 0)
		return -1;
	G.rigid_model[G.rigid_count] = m;
	G.rigid_mesh[G.rigid_count] = mesh;
	return G.rigid_count++;
}

void rigid_draw(const char *name, const Xf *world, const float tint[4])
{
	const int r = rigid_mesh_of(name);
	if (r < 0)
		return;
	const Mat4 mm = xf_to_mat4(world);
	draw_subs(G.rigid_model[r], G.rigid_mesh[r], &mm, tint, 0);
}
