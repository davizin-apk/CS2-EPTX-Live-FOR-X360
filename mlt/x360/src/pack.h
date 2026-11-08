// Le e confere o dust2.pak. C portavel: o mesmo codigo roda no 360 e no PC (tools/pc), que testa o pacote antes de
// ele ir para o console. Nada do que sai daqui chega na GPU sem ter passado pelas conferencias de bs_pack_load.
#ifndef BS_PACK_H
#define BS_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "mathx.h"
#include "pack_format.h"

typedef struct {
	uint32_t team;
	Vec3 position;
	float yaw;
} BsSpawn;

typedef struct {
	uint32_t format;
	uint32_t width, height, levels;
	uint32_t data_offset, data_bytes;
} BsTextureInfo;

typedef struct {
	int32_t texture;
	float color[4];
	uint32_t flags;
} BsMaterial;

typedef struct {
	uint32_t material;
	uint32_t first_vertex, vertex_count;
	uint32_t first_index, index_count;
	uint32_t flags;
	Vec3 min, max;
} BsDraw;

typedef struct {
	Vec3 sun_dir;
	float ambient, sun;
	float fog_start, fog_end;
	float sky_rgb[3], fog_rgb[3];
	Vec3 min, max;
} BsInfo;

typedef struct {
	Vec3 origin;
	float cell;
	uint32_t nx, ny, nz;
	uint32_t *cell_start; // nx*ny*nz + 1
	uint32_t *refs;
	uint32_t ref_count;
} BsGrid;

typedef struct {
	FILE *file;
	uint32_t file_bytes;
	BsInfo info;
	BsSpawn *spawns;
	uint32_t spawn_count;
	BsTextureInfo *textures;
	uint32_t texture_count;
	uint32_t texture_data_offset, texture_data_bytes; // TEXD, lido textura a textura
	BsMaterial *materials;
	uint32_t material_count;
	BsDraw *draws;
	uint32_t draw_count;
	// Vertices (BS_VERTEX_BYTES cada) e indices de 16 bits, na ordem do 360; no PC o leitor ja os virou.
	float *vertices;
	uint32_t vertex_count;
	uint16_t *indices;
	uint32_t index_count;
	// Colisao: triangulos (9 floats cada), flags e a grade que diz quais triangulos tocam cada celula.
	float *triangles;
	uint8_t *triangle_flags;
	uint32_t triangle_count;
	BsGrid grid;
} BsPack;

// Abre, le tudo menos as texturas e confere cada indice, faixa e tamanho. Em erro, err diz o que e o pack fica vazio.
bool bs_pack_load(BsPack *pack, const char *path, char *err, size_t err_bytes);
// Le os dados de uma textura (os niveis em sequencia, palavras de 16 bits na ordem do 360) para out, que tem
// info->data_bytes. Os bytes no PC continuam big-endian: quem usa no PC vira.
bool bs_pack_read_texture(BsPack *pack, const BsTextureInfo *info, void *out);
// Bytes de cada nivel de uma textura DXT.
uint32_t bs_texture_level_bytes(const BsTextureInfo *info, uint32_t level);
// Libera vertices e indices depois de copiados para a GPU; o resto fica para a colisao e o desenho.
void bs_pack_release_geometry(BsPack *pack);
void bs_pack_free(BsPack *pack);

#endif
