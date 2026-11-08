// O assets.pak: modelos (com pele), animacoes, texturas, sons, a atlas da HUD, fontes, navmesh e bombsites, como o
// tools/pack_assets.py grava. Big-endian; no PC o leitor vira as palavras. Tudo e conferido ao ler.
#ifndef BS_ASSETS_H
#define BS_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mathx.h"

enum {
	BS_ASSET_MODEL = 1,
	BS_ASSET_ANIM,
	BS_ASSET_TEXTURE,
	BS_ASSET_SOUND,
	BS_ASSET_IMAGE,
	BS_ASSET_FONT,
	BS_ASSET_NAV,
	BS_ASSET_SITES,
	BS_ASSET_RECTS,
};

// Transformacao afim 3x4, linha a linha: x' = m[0..3] . (x, y, z, 1) e assim por diante.
typedef struct {
	float m[12];
} Xf;

typedef struct {
	uint32_t first_index, index_count;
	int32_t texture; // indice em Assets.textures, ou -1
	float color[4];
	uint32_t flags; // 1: recorte por alfa
} MdlSub;

typedef struct {
	char name[48];
	uint32_t vertex_count, index_count, sub_count, bind_count, slot_count, attach_count;
	MdlSub *subs;
	char (*slot_names)[32];
	uint32_t *bind_slot;
	Xf *bind_pose;
	char (*attach_names)[32];
	uint32_t *attach_slot;
	float *pos, *nrm, *uv; // 3, 3 e 2 por vertice, em espaco de bind
	uint8_t *bones, *weights; // 4 por vertice; pesos somam 255
	uint16_t *indices;
	int gfx; // do renderizador
} Model;

typedef struct {
	char name[48];
	uint32_t slot_count, frame_count;
	float fps;
	bool loop;
	Xf *frames; // frame_count * slot_count
} Anim;

typedef struct {
	uint32_t format; // BS_TEXTURE_DXT1 ou DXT5 (pack_format.h)
	uint32_t width, height, levels;
	const uint16_t *data; // palavras na ordem do 360 (big-endian), como o leitor do mapa
	uint32_t bytes;
	int gfx;
} TexRes;

typedef struct {
	char name[48];
	uint32_t rate, frames;
	int16_t *pcm; // mono
} Sound;

typedef struct {
	char name[32];
	uint16_t x, y, w, h;
} AtlasRect;

typedef struct {
	uint16_t x, y, w, h;
	float ox, oy, advance;
} Glyph;

typedef struct {
	float line, ascent;
	Glyph glyphs[256];
} Font;

typedef struct {
	uint32_t vertex_count, poly_count;
	float *verts;
	uint32_t *poly_start; // poly_count + 1
	uint32_t *poly_index;
} NavData;

typedef struct {
	uint8_t *blob;
	uint32_t blob_bytes;
	Model *models;
	uint32_t model_count;
	Anim *anims;
	uint32_t anim_count;
	TexRes *textures;
	uint32_t texture_count;
	Sound *sounds;
	uint32_t sound_count;
	uint8_t *atlas_rgba;
	uint32_t atlas_w, atlas_h;
	AtlasRect *rects;
	uint32_t rect_count;
	Font font_bold, font_regular;
	bool has_fonts;
	NavData nav;
	bool has_nav;
	float sites[2][6]; // A e B: min xyz, max xyz
	bool has_sites;
	int32_t build_textures[3];
} Assets;

bool assets_load(Assets *a, const char *path, char *err, size_t err_bytes);
void assets_free(Assets *a);
const Model *assets_model(const Assets *a, const char *name);
const Anim *assets_anim(const Assets *a, const char *name);
const Sound *assets_sound(const Assets *a, const char *name);
const AtlasRect *assets_rect(const Assets *a, const char *name);
int assets_attach(const Model *m, const char *name); // slot de um ponto (cano, ejecao), ou -1

static inline Xf xf_identity(void)
{
	return (Xf){ { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 } };
}
static inline Xf xf_mul(const Xf *a, const Xf *b)
{
	Xf r;
	for (int i = 0; i < 3; i++) {
		const float *ra = a->m + i * 4;
		for (int j = 0; j < 3; j++)
			r.m[i * 4 + j] = ra[0] * b->m[j] + ra[1] * b->m[4 + j] + ra[2] * b->m[8 + j];
		r.m[i * 4 + 3] = ra[0] * b->m[3] + ra[1] * b->m[7] + ra[2] * b->m[11] + ra[3];
	}
	return r;
}
static inline Vec3 xf_point(const Xf *a, Vec3 p)
{
	return v3(a->m[0] * p.x + a->m[1] * p.y + a->m[2] * p.z + a->m[3],
		a->m[4] * p.x + a->m[5] * p.y + a->m[6] * p.z + a->m[7],
		a->m[8] * p.x + a->m[9] * p.y + a->m[10] * p.z + a->m[11]);
}
static inline Vec3 xf_dir(const Xf *a, Vec3 p)
{
	return v3(a->m[0] * p.x + a->m[1] * p.y + a->m[2] * p.z, a->m[4] * p.x + a->m[5] * p.y + a->m[6] * p.z,
		a->m[8] * p.x + a->m[9] * p.y + a->m[10] * p.z);
}
static inline Vec3 xf_origin(const Xf *a)
{
	return v3(a->m[3], a->m[7], a->m[11]);
}
// Inversa de uma afim de base ortogonal (com escala uniforme).
static inline Xf xf_inverse(const Xf *a)
{
	const float sx = a->m[0] * a->m[0] + a->m[4] * a->m[4] + a->m[8] * a->m[8];
	const float k = sx > 1e-12f ? 1.0f / sx : 1.0f;
	Xf r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i * 4 + j] = a->m[j * 4 + i] * k;
	for (int i = 0; i < 3; i++)
		r.m[i * 4 + 3] = -(r.m[i * 4] * a->m[3] + r.m[i * 4 + 1] * a->m[7] + r.m[i * 4 + 2] * a->m[11]);
	return r;
}
// Rotacao em Y por yaw e translacao (o "corpo" de um lutador no Godot).
static inline Xf xf_yaw(float yaw, Vec3 at)
{
	const float c = cosf(yaw), s = sinf(yaw);
	return (Xf){ { c, 0, s, at.x, 0, 1, 0, at.y, -s, 0, c, at.z } };
}
static inline Xf xf_rot_x(float a)
{
	const float c = cosf(a), s = sinf(a);
	return (Xf){ { 1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0 } };
}
static inline Xf xf_rot_z(float a)
{
	const float c = cosf(a), s = sinf(a);
	return (Xf){ { c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0 } };
}
static inline Xf xf_translate(Vec3 t)
{
	return (Xf){ { 1, 0, 0, t.x, 0, 1, 0, t.y, 0, 0, 1, t.z } };
}
static inline Xf xf_scale(float s)
{
	return (Xf){ { s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0 } };
}
// Base com colunas x, y, z e origem o.
static inline Xf xf_basis(Vec3 x, Vec3 y, Vec3 z, Vec3 o)
{
	return (Xf){ { x.x, y.x, z.x, o.x, x.y, y.y, z.y, o.y, x.z, y.z, z.z, o.z } };
}
static inline Vec3 xf_col(const Xf *a, int c)
{
	return v3(a->m[c], a->m[4 + c], a->m[8 + c]);
}
static inline Mat4 xf_to_mat4(const Xf *a)
{
	return (Mat4){ { a->m[0], a->m[4], a->m[8], 0, a->m[1], a->m[5], a->m[9], 0, a->m[2], a->m[6], a->m[10], 0,
		a->m[3], a->m[7], a->m[11], 1 } };
}
static inline Xf xf_lerp(const Xf *a, const Xf *b, float t)
{
	Xf r;
	for (int i = 0; i < 12; i++)
		r.m[i] = a->m[i] + (b->m[i] - a->m[i]) * t;
	return r;
}

#endif
