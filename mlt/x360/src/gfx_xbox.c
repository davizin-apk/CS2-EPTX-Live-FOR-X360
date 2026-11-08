// O renderizador do 360, sobre a camada de GPU do xsdk. O mapa e o mesmo da etapa 1; os modelos com pele recebem os
// vertices da CPU em tres buffers que se revezam, e a CPU so escreve num buffer que a GPU terminou de ler (o de tres
// quadros atras).
//
// Seguranca, como na etapa 1: tudo o que vai para a GPU ja foi conferido pelos leitores (pack.c, assets.c); a
// memoria da GPU tem um teto (GPU_BUDGET_BYTES); quadros presos a 60 Hz; se a GPU parar, xsdk_present_begin desiste
// em BEGIN_TIMEOUT_MS e o jogo volta para a dashboard.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "botstrike_shaders.h"
#include "gfx.h"
#include "plat.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/shader.h"
#include "xsdk/splash.h"
#include "xsdk/text.h"
#include "xsdk/texture.h"

enum {
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	BEGIN_TIMEOUT_MS = 5000,
	ARENA_BLOCK_BYTES = 16 * 1024 * 1024,
	ARENA_MAX_BLOCKS = 64,
	TEXTURE_ALIGN = 4096,
	MAX_TEXTURES = 4096,
	MAX_MESHES = 1024,
	RING_SETS = 3,
	RING_BYTES = 6 * 1024 * 1024,
};

static const uint32_t GPU_BUDGET_BYTES = 320u * 1024 * 1024;
static const float LOD_DISTANCE = 28.0f;

// ---------------------------------------------------------------- memoria da GPU

typedef struct {
	volatile uint32_t *block[ARENA_MAX_BLOCKS];
	uint32_t block_bytes[ARENA_MAX_BLOCKS], used[ARENA_MAX_BLOCKS];
	uint32_t count, total;
} Arena;

static Arena arena;

static volatile uint8_t *arena_alloc(uint32_t bytes, uint32_t align)
{
	if (arena.count) {
		const uint32_t i = arena.count - 1;
		const uint32_t at = (arena.used[i] + align - 1) / align * align;
		if (at + bytes <= arena.block_bytes[i]) {
			arena.used[i] = at + bytes;
			return (volatile uint8_t *)arena.block[i] + at;
		}
	}
	const uint32_t size = bytes > ARENA_BLOCK_BYTES ? (bytes + 0xFFFF) & ~0xFFFFu : ARENA_BLOCK_BYTES;
	if (arena.count == ARENA_MAX_BLOCKS || arena.total + size > GPU_BUDGET_BYTES) {
		xsdk_log_write("gfx: memoria da GPU no teto (%u MB)", (unsigned)(arena.total >> 20));
		return NULL;
	}
	volatile uint32_t *m = xsdk_gpu_alloc(size);
	if (!m)
		return NULL;
	arena.block[arena.count] = m;
	arena.block_bytes[arena.count] = size;
	arena.used[arena.count] = bytes;
	arena.count++;
	arena.total += size;
	return (volatile uint8_t *)m;
}

static uint32_t gpu_addr(volatile const uint8_t *p)
{
	return xsdk_gpu_address((volatile const uint32_t *)p);
}

// Copia para a GPU (o memcpy do xsdk usa so os stores que a memoria write-combined aceita) e invalida o cache.
static void upload(volatile uint8_t *dst, const void *src, uint32_t bytes)
{
	memcpy((void *)dst, src, bytes);
	xsdk_gpu_invalidate(gpu_addr(dst), bytes);
}

// ---------------------------------------------------------------- estado

typedef struct {
	bool dynamic;
	uint32_t vertex_count, index_count;
	volatile uint8_t *v[RING_SETS];
	int latest;
	volatile uint8_t *idx;
	// os mesmos indices um lugar adiante: uma lista de 16 bits comeca em 4 bytes, e uma parte pode comecar num
	// indice impar (xsdk:docs/GUIDE.md, "Memory the GPU reads")
	volatile uint8_t *idx_odd;
} XMesh;

static bool opened, presenting, text_ok;
static uint32_t width, height, frame_address;
static XsdkTexture textures[MAX_TEXTURES];
static uint32_t texture_count;
static XMesh meshes[MAX_MESHES];
static uint32_t mesh_count;
static XsdkShader sh_map_v, sh_map_p, sh_map_alpha_p, sh_model_v, sh_fx_v, sh_fx_p, sh_fx_alpha_p;
static XsdkSamplerState samp_wrap, samp_clamp;
static XsdkDepthStencilState ds_write, ds_test, ds_none;
static XsdkBlendState bl_none, bl_alpha, bl_add;
static GfxCamera cam;
static Mat4 cam_vp;
static int cam_layer;
static uint32_t frame_n;
static volatile uint8_t *ring[RING_SETS];
static uint32_t ring_used;
static uint32_t clear_argb = 0xFF73A0D9;

// o mapa
typedef struct {
	uint32_t vertex_address, vertex_bytes, index_address;
} MapDraw;
static BsPack *map;
static MapDraw *map_draws;
static int *map_tex;

static const XsdkVertexElement map_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 12 },
};
static const XsdkVertexElement model_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .usage_index = 0, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 24 },
	{ .usage = XSDK_USAGE_TEXCOORD, .usage_index = 1, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 12 },
};
static const XsdkVertexElement fx_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 12 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 28 },
};

static bool load_shader(XsdkShader *s, const XsdkBuiltShader *b)
{
	return NT_SUCCESS(xsdk_shader_load(s, b->container, b->container_bytes));
}

bool gfx_open(uint32_t *w, uint32_t *h, char *err, size_t eb)
{
	NTSTATUS st = xsdk_gpu_open();
	if (!NT_SUCCESS(st)) {
		snprintf(err, eb, "xsdk_gpu_open falhou: 0x%08X", (unsigned)st);
		return false;
	}
	opened = true;
	// o logo fica na tela durante a carga; o xsdk_present_open assume a tela e ele some no primeiro quadro
	if (!NT_SUCCESS(xsdk_splash_begin()))
		xsdk_log_write("gfx: sem logo de abertura");
	text_ok = NT_SUCCESS(xsdk_text_open());
	bool ok = load_shader(&sh_map_v, &map_vert) && load_shader(&sh_map_p, &map_frag) &&
		load_shader(&sh_map_alpha_p, &map_alpha_frag) && load_shader(&sh_model_v, &model_vert) &&
		load_shader(&sh_fx_v, &fx_vert) && load_shader(&sh_fx_p, &fx_frag) && load_shader(&sh_fx_alpha_p, &fx_alpha_frag);
	ok = ok && NT_SUCCESS(xsdk_shader_set_vertex_layout(&sh_map_v, map_layout, 2, (const uint32_t[]){ 24 }));
	ok = ok && NT_SUCCESS(xsdk_shader_set_vertex_layout(&sh_model_v, model_layout, 3, (const uint32_t[]){ 32 }));
	ok = ok && NT_SUCCESS(xsdk_shader_set_vertex_layout(&sh_fx_v, fx_layout, 3, (const uint32_t[]){ 36 }));
	if (!ok) {
		snprintf(err, eb, "os shaders nao carregaram");
		return false;
	}
	const XsdkSampler wrap = { .mag_filter = XSDK_FILTER_LINEAR, .min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_LINEAR, .address_u = XSDK_ADDRESS_WRAP, .address_v = XSDK_ADDRESS_WRAP };
	const XsdkSampler clamp = { .mag_filter = XSDK_FILTER_LINEAR, .min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT, .address_u = XSDK_ADDRESS_CLAMP, .address_v = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&samp_wrap, &wrap);
	xsdk_texture_sampler_state(&samp_clamp, &clamp);
	xsdk_gpu_depth_stencil_state(&ds_write,
		&(XsdkDepthStencil){ .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS_EQUAL });
	xsdk_gpu_depth_stencil_state(&ds_test,
		&(XsdkDepthStencil){ .depth_test = true, .depth_write = false, .depth_compare = XSDK_COMPARE_LESS_EQUAL });
	xsdk_gpu_depth_stencil_state(&ds_none, &(XsdkDepthStencil){ .depth_test = false });
	xsdk_gpu_blend_state(&bl_none, &(XsdkBlend){ XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
		XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL, false });
	xsdk_gpu_blend_state(&bl_alpha, &(XsdkBlend){ XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD,
		XSDK_BLEND_ONE, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL, false });
	xsdk_gpu_blend_state(&bl_add, &(XsdkBlend){ XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE, XSDK_BLEND_OP_ADD, XSDK_BLEND_ZERO,
		XSDK_BLEND_ONE, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL, false });
	// textura 0: branca
	static const uint32_t white[16] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		0xFFFFFFFF };
	XsdkTexture *t = &textures[0];
	volatile uint8_t *mem;
	if (!NT_SUCCESS(xsdk_texture_init(t, XSDK_FORMAT_A8R8G8B8, 4, 4, 1, true)) ||
		!(mem = arena_alloc(t->size_bytes, TEXTURE_ALIGN))) {
		snprintf(err, eb, "sem memoria da GPU");
		return false;
	}
	xsdk_texture_attach(t, (volatile uint32_t *)mem);
	xsdk_texture_upload(t, 0, white, 16);
	xsdk_gpu_invalidate(t->address, t->size_bytes);
	texture_count = 1;
	for (int i = 0; i < RING_SETS; i++)
		if (!(ring[i] = arena_alloc(RING_BYTES, 64))) {
			snprintf(err, eb, "sem memoria da GPU para os efeitos");
			return false;
		}
	*w = GFX_WIDTH;
	*h = GFX_HEIGHT;
	return true;
}

void gfx_close(void)
{
	if (presenting)
		xsdk_present_close();
	presenting = false;
	for (uint32_t i = 0; i < arena.count; i++)
		xsdk_gpu_free(arena.block[i]);
	memset(&arena, 0, sizeof arena);
	if (text_ok)
		xsdk_text_close();
	if (opened)
		xsdk_gpu_close();
	opened = false;
}

// ---------------------------------------------------------------- recursos

static uint32_t xformat(uint32_t f)
{
	return f == BS_TEXTURE_DXT1 ? XSDK_FORMAT_DXT1 : XSDK_FORMAT_DXT5;
}

static int texture_from(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data)
{
	if (texture_count >= MAX_TEXTURES)
		return GFX_WHITE;
	XsdkTexture *t = &textures[texture_count];
	if (!NT_SUCCESS(xsdk_texture_init(t, xformat(format), w, h, levels, true)))
		return GFX_WHITE;
	volatile uint8_t *mem = arena_alloc(t->size_bytes, TEXTURE_ALIGN);
	if (!mem)
		return GFX_WHITE;
	xsdk_texture_attach(t, (volatile uint32_t *)mem);
	uint32_t at = 0;
	for (uint32_t l = 0; l < levels; l++) {
		const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
		const uint32_t row = ((lw + 3) / 4) * (format == BS_TEXTURE_DXT1 ? 8 : 16);
		xsdk_texture_upload(t, l, (const uint8_t *)data + at, row);
		at += row * ((lh + 3) / 4);
	}
	xsdk_gpu_invalidate(t->address, t->size_bytes);
	return (int)texture_count++;
}

int gfx_texture(const TexRes *t)
{
	return texture_from(t->format, t->width, t->height, t->levels, t->data);
}

int gfx_texture_rgba(uint32_t w, uint32_t h, const uint8_t *rgba)
{
	if (texture_count >= MAX_TEXTURES)
		return GFX_WHITE;
	uint32_t *argb = malloc((size_t)w * h * 4);
	if (!argb)
		return GFX_WHITE;
	for (uint32_t i = 0; i < w * h; i++) {
		const uint8_t *p = rgba + i * 4;
		argb[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
	}
	XsdkTexture *t = &textures[texture_count];
	volatile uint8_t *mem = NULL;
	if (NT_SUCCESS(xsdk_texture_init(t, XSDK_FORMAT_A8R8G8B8, w, h, 1, true)))
		mem = arena_alloc(t->size_bytes, TEXTURE_ALIGN);
	if (!mem) {
		free(argb);
		return GFX_WHITE;
	}
	xsdk_texture_attach(t, (volatile uint32_t *)mem);
	xsdk_texture_upload(t, 0, argb, w * 4);
	xsdk_gpu_invalidate(t->address, t->size_bytes);
	free(argb);
	return (int)texture_count++;
}

static int new_mesh(uint32_t vc, const uint16_t *idx, uint32_t ic, bool dynamic)
{
	if (mesh_count >= MAX_MESHES || vc == 0 || ic == 0)
		return -1;
	XMesh *m = &meshes[mesh_count];
	memset(m, 0, sizeof *m);
	m->dynamic = dynamic;
	m->vertex_count = vc;
	m->index_count = ic;
	for (int i = 0; i < (dynamic ? RING_SETS : 1); i++)
		if (!(m->v[i] = arena_alloc(vc * sizeof(MeshVertex), 16)))
			return -1;
	if (!(m->idx = arena_alloc(ic * 2, 16)) || !(m->idx_odd = arena_alloc(ic * 2 + 2, 16)))
		return -1;
	upload(m->idx, idx, ic * 2);
	uint16_t *shifted = malloc(ic * 2 + 2);
	if (!shifted)
		return -1;
	shifted[0] = 0;
	memcpy(shifted + 1, idx, ic * 2);
	upload(m->idx_odd, shifted, ic * 2 + 2);
	free(shifted);
	return (int)mesh_count++;
}

int gfx_mesh(const MeshVertex *v, uint32_t vc, const uint16_t *idx, uint32_t ic)
{
	const int id = new_mesh(vc, idx, ic, false);
	if (id >= 0)
		upload(meshes[id].v[0], v, vc * sizeof *v);
	return id;
}

int gfx_skinned(uint32_t vc, const uint16_t *idx, uint32_t ic)
{
	return new_mesh(vc, idx, ic, true);
}

void gfx_skinned_vertices(int mesh, const MeshVertex *v, uint32_t count)
{
	if (mesh < 0 || (uint32_t)mesh >= mesh_count || count > meshes[mesh].vertex_count)
		return;
	XMesh *m = &meshes[mesh];
	// o conjunto deste quadro: a GPU terminou com ele ha tres quadros
	const int set = m->dynamic ? (int)(frame_n % RING_SETS) : 0;
	upload(m->v[set], v, count * sizeof *v);
	m->latest = set;
}

bool gfx_map_upload(BsPack *p, char *err, size_t eb)
{
	map = p;
	map_draws = calloc(p->draw_count, sizeof *map_draws);
	map_tex = calloc(p->texture_count + 1, sizeof *map_tex);
	if (!map_draws || !map_tex) {
		snprintf(err, eb, "sem memoria para o mapa");
		return false;
	}
	for (uint32_t i = 0; i < p->draw_count; i++) {
		const BsDraw *d = &p->draws[i];
		const uint32_t vb = d->vertex_count * BS_VERTEX_BYTES, ib = d->index_count * 2;
		volatile uint8_t *v = arena_alloc(vb, 16), *ix = arena_alloc(ib, 16);
		if (!v || !ix) {
			snprintf(err, eb, "a GPU ficou sem memoria para o mapa");
			return false;
		}
		upload(v, p->vertices + (size_t)d->first_vertex * 6, vb);
		upload(ix, p->indices + d->first_index, ib);
		map_draws[i] = (MapDraw){ gpu_addr(v), vb, gpu_addr(ix) };
	}
	bs_pack_release_geometry(p);
	uint32_t biggest = 0;
	for (uint32_t i = 0; i < p->texture_count; i++)
		if (p->textures[i].data_bytes > biggest)
			biggest = p->textures[i].data_bytes;
	uint8_t *staging = malloc(biggest ? biggest : 16);
	if (!staging) {
		snprintf(err, eb, "sem memoria para as texturas do mapa");
		return false;
	}
	for (uint32_t i = 0; i < p->texture_count; i++) {
		const BsTextureInfo *t = &p->textures[i];
		if (!bs_pack_read_texture(p, t, staging)) {
			free(staging);
			snprintf(err, eb, "textura %u do mapa nao le", (unsigned)i);
			return false;
		}
		map_tex[i] = texture_from(t->format, t->width, t->height, t->levels, staging);
	}
	free(staging);
	xsdk_log_write("gfx: mapa na GPU, %.1f MB usados", arena.total / 1048576.0);
	return true;
}

// ---------------------------------------------------------------- quadro

bool gfx_begin(void)
{
	if (!presenting) {
		const NTSTATUS st = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
		if (!NT_SUCCESS(st)) {
			xsdk_log_write("gfx: xsdk_present_open falhou: 0x%08X", (unsigned)st);
			return false;
		}
		presenting = true;
	}
	const NTSTATUS st = xsdk_present_begin(&frame_address, BEGIN_TIMEOUT_MS);
	if (!NT_SUCCESS(st)) {
		xsdk_log_write("gfx: xsdk_present_begin falhou: 0x%08X; saindo", (unsigned)st);
		return false;
	}
	frame_n++;
	ring_used = 0;
	xsdk_gpu_set_target(width, height);
	return true;
}

void gfx_end(void)
{
	xsdk_gpu_resolve(frame_address, width, height, clear_argb);
	xsdk_present_end();
}

void gfx_screenshot(const char *path)
{
	(void)path;
}

void gfx_camera(const GfxCamera *c, int layer)
{
	cam = *c;
	cam_layer = layer;
	cam_vp = mat4_multiply(&c->proj, &c->view);
	const uint32_t r = (uint32_t)(clampf(c->sky_rgb[0], 0, 1) * 255), g = (uint32_t)(clampf(c->sky_rgb[1], 0, 1) * 255),
				   b = (uint32_t)(clampf(c->sky_rgb[2], 0, 1) * 255);
	clear_argb = 0xFF000000u | r << 16 | g << 8 | b;
	// a arma em primeira pessoa fica na frente de tudo: a profundidade dela vai de 0 a 0.02, o mundo de 0.02 a 1
	xsdk_gpu_set_viewport(0, 0, (float)width, (float)height, layer ? 0.0f : 0.02f, layer ? 0.02f : 1.0f);
}

static void set_program(const XsdkShader *v, const XsdkBuiltShader *bv, const XsdkShader *p, const XsdkBuiltShader *bp)
{
	xsdk_gpu_set_shaders(v, p);
	xsdk_built_shader_set_literals(bv);
	xsdk_built_shader_set_literals(bp);
}

static bool box_visible(const float planes[6][4], Vec3 lo, Vec3 hi)
{
	for (int i = 0; i < 6; i++) {
		const float *p = planes[i];
		const float x = p[0] >= 0 ? hi.x : lo.x, y = p[1] >= 0 ? hi.y : lo.y, z = p[2] >= 0 ? hi.z : lo.z;
		if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f)
			return false;
	}
	return true;
}

static float box_distance(Vec3 p, Vec3 lo, Vec3 hi)
{
	const float dx = fmaxf(fmaxf(lo.x - p.x, 0.0f), p.x - hi.x);
	const float dy = fmaxf(fmaxf(lo.y - p.y, 0.0f), p.y - hi.y);
	const float dz = fmaxf(fmaxf(lo.z - p.z, 0.0f), p.z - hi.z);
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

GfxStats gfx_draw_map(void)
{
	GfxStats st = { 0, 0 };
	if (!map)
		return st;
	float r[4][4], planes[6][4];
	for (int i = 0; i < 4; i++)
		for (int c = 0; c < 4; c++)
			r[i][c] = cam_vp.m[c * 4 + i];
	for (int c = 0; c < 4; c++) {
		planes[0][c] = r[3][c] + r[0][c];
		planes[1][c] = r[3][c] - r[0][c];
		planes[2][c] = r[3][c] + r[1][c];
		planes[3][c] = r[3][c] - r[1][c];
		planes[4][c] = r[2][c];
		planes[5][c] = r[3][c] - r[2][c];
	}
	float vc[24];
	memcpy(vc, cam_vp.m, sizeof cam_vp.m);
	vc[16] = cam.eye.x;
	vc[17] = cam.eye.y;
	vc[18] = cam.eye.z;
	vc[19] = 1;
	vc[20] = cam.fog_start;
	vc[21] = 1.0f / (cam.fog_end - cam.fog_start);
	vc[22] = vc[23] = 0;
	xsdk_gpu_set_depth_stencil(&ds_write);
	xsdk_gpu_set_blend(&bl_none);
	bool alpha = false;
	set_program(&sh_map_v, &map_vert, &sh_map_p, &map_frag);
	xsdk_gpu_set_vertex_constants(MAP_VERT_PUSH, vc, MAP_VERT_PUSH_REGISTERS);
	uint32_t bound = UINT32_MAX;
	for (uint32_t i = 0; i < map->draw_count; i++) {
		const BsDraw *d = &map->draws[i];
		if (!box_visible(planes, d->min, d->max))
			continue;
		const bool is_near = box_distance(cam.eye, d->min, d->max) < LOD_DISTANCE;
		if ((d->flags & BS_DRAW_NEAR && !is_near) || (d->flags & BS_DRAW_FAR && is_near))
			continue;
		const bool want_alpha = (d->flags & BS_DRAW_ALPHA_TEST) != 0;
		if (want_alpha != alpha) {
			alpha = want_alpha;
			set_program(&sh_map_v, &map_vert, alpha ? &sh_map_alpha_p : &sh_map_p, alpha ? &map_alpha_frag : &map_frag);
			xsdk_gpu_set_vertex_constants(MAP_VERT_PUSH, vc, MAP_VERT_PUSH_REGISTERS);
			bound = UINT32_MAX;
		}
		if (d->material != bound) {
			bound = d->material;
			const BsMaterial *m = &map->materials[d->material];
			const int t = m->texture >= 0 ? map_tex[m->texture] : GFX_WHITE;
			xsdk_gpu_set_texture(alpha ? MAP_ALPHA_FRAG_SAMPLER_0_0 : MAP_FRAG_SAMPLER_0_0, &textures[t], &samp_wrap);
			const float pc[8] = { m->color[0], m->color[1], m->color[2], m->color[3], cam.fog_rgb[0], cam.fog_rgb[1],
				cam.fog_rgb[2], 1 };
			xsdk_gpu_set_pixel_constants(alpha ? MAP_ALPHA_FRAG_PUSH : MAP_FRAG_PUSH, pc, 2);
		}
		const MapDraw *g = &map_draws[i];
		xsdk_gpu_set_vertices(0, g->vertex_address, g->vertex_bytes);
		xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, g->index_address, d->index_count, false);
		st.draws++;
		st.triangles += d->index_count / 3;
	}
	return st;
}

static void apply_flags(uint32_t flags)
{
	xsdk_gpu_set_blend(flags & GFX_ADD ? &bl_add : (flags & GFX_BLEND ? &bl_alpha : &bl_none));
	if (flags & GFX_NO_DEPTH)
		xsdk_gpu_set_depth_stencil(&ds_none);
	else if (flags & (GFX_NO_DEPTH_WRITE | GFX_BLEND | GFX_ADD))
		xsdk_gpu_set_depth_stencil(&ds_test);
	else
		xsdk_gpu_set_depth_stencil(&ds_write);
}

void gfx_draw_mesh(int mesh, const Mat4 *model, uint32_t first, uint32_t count, int texture, const float color[4],
	uint32_t flags)
{
	if (mesh < 0 || (uint32_t)mesh >= mesh_count || count == 0)
		return;
	const XMesh *m = &meshes[mesh];
	if (first > m->index_count || count > m->index_count - first || count > 65535)
		return;
	const Mat4 id = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
	const Mat4 *mm = model ? model : &id;
	const Mat4 mvp = mat4_multiply(&cam_vp, mm);
	float vc[40];
	memcpy(vc, mvp.m, 64);
	for (int row = 0; row < 3; row++)
		for (int c = 0; c < 4; c++)
			vc[16 + row * 4 + c] = mm->m[c * 4 + row];
	vc[28] = cam.eye.x;
	vc[29] = cam.eye.y;
	vc[30] = cam.eye.z;
	vc[31] = 1;
	vc[32] = cam.fog_start;
	vc[33] = 1.0f / (cam.fog_end - cam.fog_start);
	vc[34] = vc[35] = 0;
	vc[36] = cam.sun_dir.x;
	vc[37] = cam.sun_dir.y;
	vc[38] = cam.sun_dir.z;
	vc[39] = cam.ambient;
	const float l2[4] = { cam.sun, (flags & GFX_UNLIT) ? 1.0f : 0.0f, 0, 0 };
	const bool alpha = (flags & GFX_ALPHA_TEST) != 0;
	apply_flags(flags);
	set_program(&sh_model_v, &model_vert, alpha ? &sh_map_alpha_p : &sh_map_p, alpha ? &map_alpha_frag : &map_frag);
	_Static_assert(MODEL_VERT_PUSH_REGISTERS == 11, "o model.vert tem 11 registradores de push");
	xsdk_gpu_set_vertex_constants(MODEL_VERT_PUSH, vc, 10);
	xsdk_gpu_set_vertex_constants(MODEL_VERT_PUSH + 10, l2, 1);
	const float pc[8] = { color[0], color[1], color[2], color[3], cam.fog_rgb[0], cam.fog_rgb[1], cam.fog_rgb[2], 1 };
	xsdk_gpu_set_pixel_constants(alpha ? MAP_ALPHA_FRAG_PUSH : MAP_FRAG_PUSH, pc, 2);
	const int t = texture >= 0 && (uint32_t)texture < texture_count ? texture : GFX_WHITE;
	xsdk_gpu_set_texture(alpha ? MAP_ALPHA_FRAG_SAMPLER_0_0 : MAP_FRAG_SAMPLER_0_0, &textures[t], &samp_wrap);
	xsdk_gpu_set_vertices(0, gpu_addr(m->v[m->latest]), m->vertex_count * sizeof(MeshVertex));
	const uint32_t ia = first % 2 ? gpu_addr(m->idx_odd) + (first + 1) * 2 : gpu_addr(m->idx) + first * 2;
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, ia, count, false);
}

// Triangulos soltos: copiados para o anel deste quadro e desenhados sem indices.
static void draw_fx(const FxVertex *v, uint32_t count, int texture, uint32_t flags, const Mat4 *mvp, bool clamp)
{
	while (count >= 3) {
		uint32_t n = count > 30000 ? 30000 : count;
		n -= n % 3;
		const uint32_t bytes = n * sizeof *v;
		if (ring_used + bytes > RING_BYTES)
			return; // o anel do quadro encheu: o resto fica para o proximo
		volatile uint8_t *dst = ring[frame_n % RING_SETS] + ring_used;
		ring_used += (bytes + 63) & ~63u;
		upload(dst, v, bytes);
		const bool alpha = (flags & GFX_ALPHA_TEST) != 0;
		apply_flags(flags);
		set_program(&sh_fx_v, &fx_vert, alpha ? &sh_fx_alpha_p : &sh_fx_p, alpha ? &fx_alpha_frag : &fx_frag);
		xsdk_gpu_set_vertex_constants(FX_VERT_PUSH, mvp->m, FX_VERT_PUSH_REGISTERS);
		const int t = texture >= 0 && (uint32_t)texture < texture_count ? texture : GFX_WHITE;
		xsdk_gpu_set_texture(alpha ? FX_ALPHA_FRAG_SAMPLER_0_0 : FX_FRAG_SAMPLER_0_0, &textures[t], clamp ? &samp_clamp : &samp_wrap);
		xsdk_gpu_set_vertices(0, gpu_addr(dst), bytes);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, n);
		v += n;
		count -= n;
	}
}

void gfx_draw_fx(const FxVertex *v, uint32_t count, int texture, uint32_t flags)
{
	draw_fx(v, count, texture, flags, &cam_vp, true);
}

void gfx_draw_2d(const FxVertex *v, uint32_t count, int texture)
{
	const Mat4 ortho = { { 2.0f / GFX_WIDTH, 0, 0, 0, 0, -2.0f / GFX_HEIGHT, 0, 0, 0, 0, 0, 0, -1, 1, 0.5f, 1 } };
	xsdk_gpu_set_viewport(0, 0, (float)width, (float)height, 0.0f, 1.0f);
	draw_fx(v, count, texture, GFX_BLEND | GFX_NO_DEPTH, &ortho, true);
}

// ---------------------------------------------------------------- tela de erro (antes ou sem as fontes do jogo)

void gfx_xbox_fatal(const char *msg)
{
	if (!opened && !NT_SUCCESS(xsdk_gpu_open()))
		return;
	opened = true;
	if (!text_ok)
		text_ok = NT_SUCCESS(xsdk_text_open());
	if (!presenting) {
		if (!NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height)))
			return;
		presenting = true;
	}
	for (;;) {
		INPUT_STATE input = { 0 };
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			return;
		uint32_t address;
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS)))
			return;
		xsdk_gpu_set_target(width, height);
		char screen[640];
		snprintf(screen, sizeof screen,
			"Bot Strike 360 nao conseguiu abrir.\n\n%s\n\n"
			"O dust2.pak e o assets.pak vao ao lado do .xex ou em Hdd1:\\BotStrike\\.\n\nBack volta para a dashboard.",
			msg);
		if (text_ok)
			xsdk_text_draw(80, 120, 1, 0xFFFFFFFF, screen);
		xsdk_gpu_resolve(address, width, height, 0xFF1A1A1E);
		xsdk_present_end();
	}
}
