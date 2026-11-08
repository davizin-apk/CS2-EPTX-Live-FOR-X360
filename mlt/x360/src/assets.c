#include "assets.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pack_format.h"

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}
static float bef(const uint8_t *p)
{
	const uint32_t u = be32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static bool fail(char *err, size_t n, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vsnprintf(err, n, fmt, a);
	va_end(a);
	return false;
}

// Um leitor que nunca passa do fim do item.
typedef struct {
	const uint8_t *p, *end;
	bool bad;
} Rd;

static const uint8_t *take(Rd *r, size_t n)
{
	if (r->bad || (size_t)(r->end - r->p) < n) {
		r->bad = true;
		return NULL;
	}
	const uint8_t *q = r->p;
	r->p += n;
	return q;
}
static uint32_t rd32(Rd *r)
{
	const uint8_t *q = take(r, 4);
	return q ? be32(q) : 0;
}
static float rdf(Rd *r)
{
	const uint8_t *q = take(r, 4);
	return q ? bef(q) : 0.0f;
}
static void rdxf(Rd *r, Xf *x)
{
	for (int i = 0; i < 12; i++)
		x->m[i] = rdf(r);
}
static bool finite_xf(const Xf *x)
{
	for (int i = 0; i < 12; i++)
		if (!isfinite(x->m[i]) || fabsf(x->m[i]) > 100000.0f)
			return false;
	return true;
}

static bool parse_model(Model *m, Rd *r, uint32_t texture_count)
{
	const uint8_t *magic = take(r, 4);
	if (!magic || memcmp(magic, "MDL1", 4) != 0)
		return false;
	m->vertex_count = rd32(r);
	m->index_count = rd32(r);
	m->sub_count = rd32(r);
	m->bind_count = rd32(r);
	m->slot_count = rd32(r);
	m->attach_count = rd32(r);
	if (r->bad || m->vertex_count == 0 || m->vertex_count > 65535 || m->index_count % 3 || m->index_count > 3000000 ||
		m->sub_count == 0 || m->sub_count > 256 || m->bind_count == 0 || m->bind_count > 255 || m->slot_count == 0 ||
		m->slot_count > 512 || m->attach_count > 16)
		return false;
	m->subs = calloc(m->sub_count, sizeof *m->subs);
	m->slot_names = calloc(m->slot_count, 32);
	m->bind_slot = calloc(m->bind_count, 4);
	m->bind_pose = calloc(m->bind_count, sizeof(Xf));
	m->attach_names = calloc(m->attach_count + 1, 32);
	m->attach_slot = calloc(m->attach_count + 1, 4);
	m->pos = malloc((size_t)m->vertex_count * 12);
	m->nrm = malloc((size_t)m->vertex_count * 12);
	m->uv = malloc((size_t)m->vertex_count * 8);
	m->bones = malloc((size_t)m->vertex_count * 4);
	m->weights = malloc((size_t)m->vertex_count * 4);
	m->indices = malloc((size_t)m->index_count * 2 + 2);
	if (!m->subs || !m->slot_names || !m->bind_slot || !m->bind_pose || !m->attach_names || !m->attach_slot ||
		!m->pos || !m->nrm || !m->uv || !m->bones || !m->weights || !m->indices)
		return false;
	for (uint32_t i = 0; i < m->sub_count; i++) {
		MdlSub *s = &m->subs[i];
		s->first_index = rd32(r);
		s->index_count = rd32(r);
		s->texture = (int32_t)rd32(r);
		for (int c = 0; c < 4; c++)
			s->color[c] = clampf(rdf(r), 0.0f, 4.0f);
		s->flags = rd32(r);
		if (s->first_index > m->index_count || s->index_count > m->index_count - s->first_index ||
			s->index_count % 3)
			return false;
		if (s->texture < -1 || s->texture >= (int32_t)texture_count)
			s->texture = -1;
	}
	for (uint32_t i = 0; i < m->slot_count; i++) {
		const uint8_t *n = take(r, 32);
		if (n) {
			memcpy(m->slot_names[i], n, 32);
			m->slot_names[i][31] = 0;
		}
	}
	for (uint32_t i = 0; i < m->bind_count; i++) {
		m->bind_slot[i] = rd32(r);
		rdxf(r, &m->bind_pose[i]);
		if (m->bind_slot[i] >= m->slot_count || !finite_xf(&m->bind_pose[i]))
			return false;
	}
	for (uint32_t i = 0; i < m->attach_count; i++) {
		const uint8_t *n = take(r, 32);
		if (n) {
			memcpy(m->attach_names[i], n, 32);
			m->attach_names[i][31] = 0;
		}
		m->attach_slot[i] = rd32(r);
		if (m->attach_slot[i] >= m->slot_count)
			return false;
	}
	for (uint32_t v = 0; v < m->vertex_count; v++) {
		for (int k = 0; k < 3; k++)
			m->pos[v * 3 + k] = rdf(r);
		for (int k = 0; k < 3; k++)
			m->nrm[v * 3 + k] = rdf(r);
		m->uv[v * 2] = rdf(r);
		m->uv[v * 2 + 1] = rdf(r);
		const uint8_t *b = take(r, 8);
		if (!b)
			return false;
		for (int k = 0; k < 4; k++) {
			m->bones[v * 4 + k] = b[k] < m->bind_count ? b[k] : 0;
			m->weights[v * 4 + k] = b[4 + k];
		}
		if (!isfinite(m->pos[v * 3]) || !isfinite(m->pos[v * 3 + 1]) || !isfinite(m->pos[v * 3 + 2]))
			return false;
	}
	for (uint32_t i = 0; i < m->index_count; i++) {
		const uint8_t *q = take(r, 2);
		if (!q)
			return false;
		m->indices[i] = be16(q);
		if (m->indices[i] >= m->vertex_count)
			return false;
	}
	m->gfx = -1;
	return !r->bad;
}

static bool parse_anim(Anim *a, Rd *r)
{
	const uint8_t *magic = take(r, 4);
	if (!magic || memcmp(magic, "ANM1", 4) != 0)
		return false;
	a->slot_count = rd32(r);
	a->frame_count = rd32(r);
	a->fps = rdf(r);
	a->loop = rd32(r) != 0;
	if (r->bad || a->slot_count == 0 || a->slot_count > 512 || a->frame_count == 0 || a->frame_count > 4096 ||
		!(a->fps > 1.0f && a->fps < 240.0f))
		return false;
	const size_t n = (size_t)a->slot_count * a->frame_count;
	a->frames = malloc(n * sizeof(Xf));
	if (!a->frames)
		return false;
	for (size_t i = 0; i < n; i++) {
		rdxf(r, &a->frames[i]);
		if (!finite_xf(&a->frames[i]))
			return false;
	}
	return !r->bad;
}

static bool parse_texture(TexRes *t, Rd *r)
{
	const uint8_t *magic = take(r, 4);
	if (!magic || memcmp(magic, "TEX1", 4) != 0)
		return false;
	t->format = rd32(r);
	t->width = rd32(r);
	t->height = rd32(r);
	t->levels = rd32(r);
	const bool pot = t->width >= 4 && t->height >= 4 && !(t->width & (t->width - 1)) && !(t->height & (t->height - 1));
	if (r->bad || (t->format != BS_TEXTURE_DXT1 && t->format != BS_TEXTURE_DXT5) || !pot || t->width > 1024 ||
		t->height > 1024 || t->levels == 0 || t->levels > 11)
		return false;
	uint32_t total = 0;
	for (uint32_t l = 0; l < t->levels; l++) {
		const uint32_t w = t->width >> l ? t->width >> l : 1, h = t->height >> l ? t->height >> l : 1;
		total += ((w + 3) / 4) * ((h + 3) / 4) * (t->format == BS_TEXTURE_DXT1 ? 8 : 16);
	}
	const uint8_t *d = take(r, total);
	if (!d)
		return false;
	t->data = (const uint16_t *)d;
	t->bytes = total;
	t->gfx = -1;
	return true;
}

static bool parse_sound(Sound *s, Rd *r)
{
	const uint8_t *magic = take(r, 4);
	if (!magic || memcmp(magic, "SND1", 4) != 0)
		return false;
	s->rate = rd32(r);
	const uint32_t ch = rd32(r);
	s->frames = rd32(r);
	if (r->bad || ch != 1 || s->rate < 4000 || s->rate > 96000 || s->frames == 0 || s->frames > 48000u * 600)
		return false;
	const uint8_t *d = take(r, (size_t)s->frames * 2);
	if (!d)
		return false;
	s->pcm = malloc((size_t)s->frames * 2);
	if (!s->pcm)
		return false;
	for (uint32_t i = 0; i < s->frames; i++)
		s->pcm[i] = (int16_t)be16(d + i * 2);
	return true;
}

static bool parse_font(Font *f, Rd *r)
{
	const uint8_t *magic = take(r, 4);
	if (!magic || memcmp(magic, "FNT1", 4) != 0)
		return false;
	const uint32_t n = rd32(r);
	f->line = rdf(r);
	f->ascent = rdf(r);
	if (r->bad || n > 256)
		return false;
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t c = rd32(r);
		const uint8_t *q = take(r, 8);
		if (!q)
			return false;
		// uma leitura por linha: a ordem dos inicializadores nao e garantida em C
		const float ox = rdf(r);
		const float oy = rdf(r);
		const float adv = rdf(r);
		Glyph g = { be16(q), be16(q + 2), be16(q + 4), be16(q + 6), ox, oy, adv };
		if (c < 256)
			f->glyphs[c] = g;
	}
	return !r->bad;
}

static bool parse_nav(NavData *n, Rd *r)
{
	const uint8_t *magic = take(r, 4);
	if (!magic || memcmp(magic, "NAV1", 4) != 0)
		return false;
	n->vertex_count = rd32(r);
	n->poly_count = rd32(r);
	if (r->bad || n->vertex_count == 0 || n->vertex_count > 200000 || n->poly_count == 0 || n->poly_count > 200000)
		return false;
	n->verts = malloc((size_t)n->vertex_count * 12);
	n->poly_start = malloc(((size_t)n->poly_count + 1) * 4);
	n->poly_index = malloc((size_t)n->poly_count * 16 * 4);
	if (!n->verts || !n->poly_start || !n->poly_index)
		return false;
	for (uint32_t i = 0; i < n->vertex_count * 3; i++)
		n->verts[i] = rdf(r);
	uint32_t at = 0;
	for (uint32_t p = 0; p < n->poly_count; p++) {
		const uint32_t k = rd32(r);
		if (k < 3 || k > 16)
			return false;
		n->poly_start[p] = at;
		for (uint32_t j = 0; j < k; j++) {
			const uint32_t v = rd32(r);
			if (v >= n->vertex_count)
				return false;
			n->poly_index[at++] = v;
		}
	}
	n->poly_start[n->poly_count] = at;
	return !r->bad;
}

static bool load(Assets *a, const char *path, char *err, size_t eb)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return fail(err, eb, "nao achei %s", path);
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 16 || size > 512L * 1024 * 1024) {
		fclose(f);
		return fail(err, eb, "%s tem tamanho invalido", path);
	}
	a->blob = malloc((size_t)size);
	if (!a->blob) {
		fclose(f);
		return fail(err, eb, "sem memoria para %s (%ld MB)", path, size >> 20);
	}
	const size_t got = fread(a->blob, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size)
		return fail(err, eb, "nao consegui ler %s", path);
	a->blob_bytes = (uint32_t)size;
	if (memcmp(a->blob, "BSA1", 4) != 0 || be32(a->blob + 4) != 1 || be32(a->blob + 12) != (uint32_t)size)
		return fail(err, eb, "%s nao e um assets.pak desta versao (gere de novo)", path);
	const uint32_t count = be32(a->blob + 8);
	if (count == 0 || count > 8192 || 16 + (uint64_t)count * 60 > (uint64_t)size)
		return fail(err, eb, "tabela do assets.pak invalida");
	uint32_t nm = 0, na = 0, nt = 0, ns = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t t = be32(a->blob + 16 + i * 60 + 48);
		nm += t == BS_ASSET_MODEL;
		na += t == BS_ASSET_ANIM;
		nt += t == BS_ASSET_TEXTURE;
		ns += t == BS_ASSET_SOUND;
	}
	a->models = calloc(nm + 1, sizeof *a->models);
	a->anims = calloc(na + 1, sizeof *a->anims);
	a->textures = calloc(nt + 1, sizeof *a->textures);
	a->sounds = calloc(ns + 1, sizeof *a->sounds);
	if (!a->models || !a->anims || !a->textures || !a->sounds)
		return fail(err, eb, "sem memoria");
	a->build_textures[0] = a->build_textures[1] = a->build_textures[2] = -1;
	// texturas primeiro: os modelos apontam para elas pelo numero ("tex/N")
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *e = a->blob + 16 + i * 60;
			char name[49];
			memcpy(name, e, 48);
			name[48] = 0;
			const uint32_t type = be32(e + 48), off = be32(e + 52), bytes = be32(e + 56);
			if (off > (uint32_t)size || bytes > (uint32_t)size - off)
				return fail(err, eb, "item %s fora do arquivo", name);
			if ((type == BS_ASSET_TEXTURE) != (pass == 0))
				continue;
			Rd r = { a->blob + off, a->blob + off + bytes, false };
			bool ok = true;
			switch (type) {
			case BS_ASSET_TEXTURE: {
				const uint32_t id = (uint32_t)atoi(name + 4);
				if (id >= nt)
					return fail(err, eb, "textura %s fora da ordem", name);
				ok = parse_texture(&a->textures[id], &r);
				if (id + 1 > a->texture_count)
					a->texture_count = id + 1;
				break;
			}
			case BS_ASSET_MODEL: {
				Model *m = &a->models[a->model_count++];
				snprintf(m->name, sizeof m->name, "%s", name + 4);
				ok = parse_model(m, &r, nt);
				break;
			}
			case BS_ASSET_ANIM: {
				Anim *an = &a->anims[a->anim_count++];
				snprintf(an->name, sizeof an->name, "%s", name + 4);
				ok = parse_anim(an, &r);
				break;
			}
			case BS_ASSET_SOUND: {
				Sound *s = &a->sounds[a->sound_count++];
				snprintf(s->name, sizeof s->name, "%s", name + 4);
				ok = parse_sound(s, &r);
				break;
			}
			case BS_ASSET_IMAGE: {
				const uint8_t *magic = take(&r, 4);
				a->atlas_w = rd32(&r);
				a->atlas_h = rd32(&r);
				ok = magic && !memcmp(magic, "IMG1", 4) && a->atlas_w && a->atlas_w <= 2048 && a->atlas_h &&
					a->atlas_h <= 2048;
				const uint8_t *px = ok ? take(&r, (size_t)a->atlas_w * a->atlas_h * 4) : NULL;
				ok = ok && px;
				a->atlas_rgba = (uint8_t *)px;
				break;
			}
			case BS_ASSET_RECTS:
				if (!strcmp(name, "map/build_textures")) {
					const uint8_t *magic = take(&r, 4);
					ok = magic && !memcmp(magic, "BTX1", 4);
					for (int k = 0; ok && k < 3; k++) {
						const int32_t t = (int32_t)rd32(&r);
						a->build_textures[k] = t >= 0 && t < (int32_t)nt ? t : -1;
					}
				} else {
					const uint8_t *magic = take(&r, 4);
					a->rect_count = rd32(&r);
					ok = magic && !memcmp(magic, "RCT1", 4) && a->rect_count < 4096;
					a->rects = ok ? calloc(a->rect_count + 1, sizeof *a->rects) : NULL;
					for (uint32_t k = 0; ok && k < a->rect_count; k++) {
						const uint8_t *q = take(&r, 40);
						if (!q || !a->rects) {
							ok = false;
							break;
						}
						memcpy(a->rects[k].name, q, 31);
						a->rects[k].x = be16(q + 32);
						a->rects[k].y = be16(q + 34);
						a->rects[k].w = be16(q + 36);
						a->rects[k].h = be16(q + 38);
					}
				}
				break;
			case BS_ASSET_FONT:
				ok = parse_font(!strcmp(name, "font/bold") ? &a->font_bold : &a->font_regular, &r);
				a->has_fonts = ok;
				break;
			case BS_ASSET_NAV:
				ok = parse_nav(&a->nav, &r);
				a->has_nav = ok;
				break;
			case BS_ASSET_SITES: {
				const uint8_t *magic = take(&r, 4);
				ok = magic && !memcmp(magic, "SIT1", 4);
				for (int k = 0; ok && k < 12; k++)
					a->sites[k / 6][k % 6] = rdf(&r);
				a->has_sites = ok;
				break;
			}
			default:
				break;
			}
			if (!ok)
				return fail(err, eb, "item %s do assets.pak invalido", name);
		}
	// o bloco das texturas fica: os TexRes apontam para ele; o resto ja foi copiado
	if (!a->atlas_rgba || !a->rects || !a->has_fonts)
		return fail(err, eb, "assets.pak sem a HUD (atlas, fontes)");
	return true;
}

bool assets_load(Assets *a, const char *path, char *err, size_t err_bytes)
{
	memset(a, 0, sizeof *a);
	if (load(a, path, err, err_bytes))
		return true;
	assets_free(a);
	return false;
}

void assets_free(Assets *a)
{
	for (uint32_t i = 0; i < a->model_count; i++) {
		Model *m = &a->models[i];
		free(m->subs);
		free(m->slot_names);
		free(m->bind_slot);
		free(m->bind_pose);
		free(m->attach_names);
		free(m->attach_slot);
		free(m->pos);
		free(m->nrm);
		free(m->uv);
		free(m->bones);
		free(m->weights);
		free(m->indices);
	}
	for (uint32_t i = 0; i < a->anim_count; i++)
		free(a->anims[i].frames);
	for (uint32_t i = 0; i < a->sound_count; i++)
		free(a->sounds[i].pcm);
	free(a->models);
	free(a->anims);
	free(a->textures);
	free(a->sounds);
	free(a->rects);
	free(a->nav.verts);
	free(a->nav.poly_start);
	free(a->nav.poly_index);
	free(a->blob);
	memset(a, 0, sizeof *a);
}

const Model *assets_model(const Assets *a, const char *name)
{
	for (uint32_t i = 0; i < a->model_count; i++)
		if (!strcmp(a->models[i].name, name))
			return &a->models[i];
	return NULL;
}

const Anim *assets_anim(const Assets *a, const char *name)
{
	for (uint32_t i = 0; i < a->anim_count; i++)
		if (!strcmp(a->anims[i].name, name))
			return &a->anims[i];
	return NULL;
}

const Sound *assets_sound(const Assets *a, const char *name)
{
	for (uint32_t i = 0; i < a->sound_count; i++)
		if (!strcmp(a->sounds[i].name, name))
			return &a->sounds[i];
	return NULL;
}

const AtlasRect *assets_rect(const Assets *a, const char *name)
{
	for (uint32_t i = 0; i < a->rect_count; i++)
		if (!strcmp(a->rects[i].name, name))
			return &a->rects[i];
	return NULL;
}

int assets_attach(const Model *m, const char *name)
{
	for (uint32_t i = 0; i < m->attach_count; i++)
		if (!strcmp(m->attach_names[i], name))
			return (int)m->attach_slot[i];
	return -1;
}
