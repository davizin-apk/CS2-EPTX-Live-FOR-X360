// O que o jogo pede para desenhar. Duas implementacoes: gfx_xbox.c (xsdk, no 360) e gfx_gl.c (OpenGL 1.3, no PC,
// para testar). O jogo nao sabe qual esta rodando.
#ifndef BS_GFX_H
#define BS_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "assets.h"
#include "mathx.h"
#include "pack.h"

enum {
	GFX_WIDTH = 1280, // a HUD e desenhada nessa grade, o que for a tela
	GFX_HEIGHT = 720,
	GFX_WHITE = 0, // textura branca
};

enum {
	GFX_ALPHA_TEST = 1,
	GFX_BLEND = 2, // mistura por alfa
	GFX_ADD = 4, // soma (brilho, tracos)
	GFX_NO_DEPTH = 8, // por cima de tudo
	GFX_UNLIT = 16,
	GFX_NO_DEPTH_WRITE = 32,
};

typedef struct {
	float x, y, z;
	float r, g, b, a;
	float u, v;
} FxVertex;

typedef struct {
	float x, y, z;
	float nx, ny, nz;
	float u, v;
} MeshVertex;

typedef struct {
	Mat4 view, proj;
	Vec3 eye;
	float fog_start, fog_end;
	Vec3 sun_dir; // para onde a luz vai
	float ambient, sun;
	float sky_rgb[3], fog_rgb[3];
} GfxCamera;

typedef struct {
	uint32_t draws, triangles;
} GfxStats;

// Abre a tela; width/height sao os do quadro (1280x720 no 360).
bool gfx_open(uint32_t *width, uint32_t *height, char *err, size_t err_bytes);
void gfx_close(void);

// Recursos (na carga, antes do primeiro quadro).
bool gfx_map_upload(BsPack *map, char *err, size_t err_bytes);
int gfx_texture(const TexRes *t);
int gfx_texture_rgba(uint32_t w, uint32_t h, const uint8_t *rgba);
// Um buffer estatico de vertices MeshVertex e indices de 16 bits; devolve o numero, ou -1.
int gfx_mesh(const MeshVertex *v, uint32_t vertex_count, const uint16_t *indices, uint32_t index_count);
// Os indices de uma malha cujos vertices mudam todo quadro (pele na CPU).
int gfx_skinned(uint32_t vertex_count, const uint16_t *indices, uint32_t index_count);

// Quadro.
bool gfx_begin(void);
// layer 0: mundo; 1: arma em primeira pessoa (nunca entra na parede).
void gfx_camera(const GfxCamera *c, int layer);
GfxStats gfx_draw_map(void);
// Desenha indices [first, first+count) de uma malha com a transformacao model.
void gfx_draw_mesh(int mesh, const Mat4 *model, uint32_t first, uint32_t count, int texture, const float color[4],
	uint32_t flags);
// Para malha com pele: os vertices deste quadro, ja no espaco do mundo (ou da camera, na layer 1).
void gfx_skinned_vertices(int mesh, const MeshVertex *v, uint32_t count);
// Triangulos soltos (efeitos), no espaco do mundo.
void gfx_draw_fx(const FxVertex *v, uint32_t count, int texture, uint32_t flags);
// 2D: triangulos em pixels da grade 1280x720, por cima de tudo.
void gfx_draw_2d(const FxVertex *v, uint32_t count, int texture);
void gfx_end(void);
// So no PC: salva o quadro atual (para os testes automaticos). No 360 nao faz nada.
void gfx_screenshot(const char *path);

#endif
