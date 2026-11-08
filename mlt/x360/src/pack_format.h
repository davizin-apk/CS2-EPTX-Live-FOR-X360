// O pacote do mapa (dust2.pak), como o tools/pack.py grava e o pack.c le. Tudo big-endian, que e a ordem do 360:
// no console os blocos de vertices, indices e texturas vao direto para a memoria da GPU, sem conversao.
//
// Cabecalho de 16 bytes, depois uma tabela de secoes de 16 bytes cada. Cada secao comeca alinhada a 16.
#ifndef BS_PACK_FORMAT_H
#define BS_PACK_FORMAT_H

#include <stdint.h>

#define BS_PACK_MAGIC "BS36"
#define BS_PACK_VERSION 1u

// Quatro letras ASCII lidas como um numero big-endian.
#define BS_FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

enum {
	BS_PACK_HEADER_BYTES = 16, // magic, versao, numero de secoes, tamanho do arquivo
	BS_PACK_SECTION_BYTES = 16, // fourcc, offset, tamanho, numero de elementos
	BS_PACK_MAX_SECTIONS = 32,

	BS_INFO_BYTES = 80,
	BS_SPAWN_BYTES = 20, // time (0 CT, 1 T), x, y, z, yaw
	BS_TEXTURE_BYTES = 24, // formato, largura u16, altura u16, niveis u16, pad u16, offset em TEXD, tamanho, pad
	BS_MATERIAL_BYTES = 24, // textura (i32, -1 sem), r, g, b, a, flags
	BS_VERTEX_BYTES = 24, // x, y, z, u, v, luz
	BS_DRAW_BYTES = 48, // material, 1o vertice, vertices, 1o indice, indices, flags, min[3], max[3]
	BS_TRIANGLE_BYTES = 36, // tres pontos
	BS_GRID_HEADER_BYTES = 32, // origem[3], tamanho da celula, nx, ny, nz, referencias

	// Limites que o leitor confere antes de entregar qualquer coisa para a GPU.
	BS_MAX_TEXTURE_SIZE = 1024,
	BS_MAX_TEXTURE_LEVELS = 11,
	BS_MAX_DRAW_INDICES = 65535,
	BS_MAX_DRAW_VERTICES = 65535,
	BS_MAX_GRID_CELLS = 4 * 1024 * 1024,
};

enum {
	BS_SECTION_INFO = BS_FOURCC('I', 'N', 'F', 'O'),
	BS_SECTION_SPAWNS = BS_FOURCC('S', 'P', 'W', 'N'),
	BS_SECTION_TEXTURES = BS_FOURCC('T', 'E', 'X', 'R'),
	BS_SECTION_TEXTURE_DATA = BS_FOURCC('T', 'E', 'X', 'D'),
	BS_SECTION_MATERIALS = BS_FOURCC('M', 'A', 'T', 'L'),
	BS_SECTION_VERTICES = BS_FOURCC('V', 'E', 'R', 'T'),
	BS_SECTION_INDICES = BS_FOURCC('I', 'N', 'D', 'X'),
	BS_SECTION_DRAWS = BS_FOURCC('D', 'R', 'A', 'W'),
	BS_SECTION_COLLISION = BS_FOURCC('C', 'T', 'R', 'I'),
	BS_SECTION_COLLISION_FLAGS = BS_FOURCC('C', 'F', 'L', 'G'),
	BS_SECTION_GRID = BS_FOURCC('C', 'G', 'R', 'D'),
};

enum {
	// Texturas: blocos 4x4 em palavras de 16 bits big-endian (o .dds com cada par de bytes trocado).
	BS_TEXTURE_DXT1 = 1,
	BS_TEXTURE_DXT5 = 2,

	BS_MATERIAL_ALPHA_TEST = 1,

	// Draws: o mesmo do material, e o nivel de detalhe. Sem NEAR nem FAR, o draw vale em qualquer distancia.
	BS_DRAW_ALPHA_TEST = 1,
	BS_DRAW_NEAR = 2,
	BS_DRAW_FAR = 4,

	// Triangulos de colisao: so o jogador bate (as balas passam, quando houver balas), e o teto invisivel do ceu.
	BS_COLLISION_CLIP = 1,
	BS_COLLISION_SKY = 2,
};

#endif
