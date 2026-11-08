#include "pack.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// O pacote e big-endian. No 360 os blocos ficam como estao; no PC (little-endian) o leitor vira cada palavra.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BS_HOST_BIG_ENDIAN 1
#else
#define BS_HOST_BIG_ENDIAN 0
#endif

typedef struct {
	uint32_t tag, offset, bytes, count;
} Section;

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

static void swap32_array(void *data, size_t count)
{
#if !BS_HOST_BIG_ENDIAN
	uint32_t *w = data;
	for (size_t i = 0; i < count; i++)
		w[i] = __builtin_bswap32(w[i]);
#else
	(void)data;
	(void)count;
#endif
}
static void swap16_array(void *data, size_t count)
{
#if !BS_HOST_BIG_ENDIAN
	uint16_t *w = data;
	for (size_t i = 0; i < count; i++)
		w[i] = __builtin_bswap16(w[i]);
#else
	(void)data;
	(void)count;
#endif
}

static bool fail(char *err, size_t err_bytes, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(err, err_bytes, format, args);
	va_end(args);
	return false;
}

static bool read_at(FILE *file, uint32_t offset, void *out, uint32_t bytes)
{
	return fseek(file, (long)offset, SEEK_SET) == 0 && fread(out, 1, bytes, file) == bytes;
}

// Le uma secao inteira para a memoria (heap). Confere que ela existe, cabe no arquivo e tem o tamanho esperado.
static void *read_section(BsPack *pack, const Section *s, uint32_t expect_bytes, char *err, size_t err_bytes)
{
	char tag[5] = { (char)(s->tag >> 24), (char)(s->tag >> 16), (char)(s->tag >> 8), (char)s->tag, 0 };
	if (expect_bytes != s->bytes) {
		fail(err, err_bytes, "secao %s: %u bytes, esperava %u", tag, (unsigned)s->bytes, (unsigned)expect_bytes);
		return NULL;
	}
	void *p = malloc(s->bytes ? s->bytes : 1);
	if (!p) {
		fail(err, err_bytes, "sem memoria para a secao %s (%u bytes)", tag, (unsigned)s->bytes);
		return NULL;
	}
	if (!read_at(pack->file, s->offset, p, s->bytes)) {
		free(p);
		fail(err, err_bytes, "nao consegui ler a secao %s", tag);
		return NULL;
	}
	return p;
}

static const Section *find(const Section *table, uint32_t count, uint32_t tag)
{
	for (uint32_t i = 0; i < count; i++)
		if (table[i].tag == tag)
			return &table[i];
	return NULL;
}

static bool finite3(const float *v)
{
	for (int i = 0; i < 3; i++)
		if (!isfinite(v[i]) || v[i] < -20000.0f || v[i] > 20000.0f)
			return false;
	return true;
}

static uint32_t log2u(uint32_t v)
{
	uint32_t l = 0;
	while (v > 1) {
		v >>= 1;
		l++;
	}
	return l;
}

uint32_t bs_texture_level_bytes(const BsTextureInfo *info, uint32_t level)
{
	uint32_t w = info->width >> level, h = info->height >> level;
	w = w ? w : 1;
	h = h ? h : 1;
	const uint32_t block = info->format == BS_TEXTURE_DXT1 ? 8 : 16;
	return ((w + 3) / 4) * ((h + 3) / 4) * block;
}

static bool load(BsPack *p, const char *path, char *err, size_t eb)
{
	p->file = fopen(path, "rb");
	if (!p->file)
		return fail(err, eb, "nao achei %s", path);
	if (fseek(p->file, 0, SEEK_END) != 0)
		return fail(err, eb, "nao consegui medir %s", path);
	const long size = ftell(p->file);
	if (size < BS_PACK_HEADER_BYTES || size > 0x7FFFFFFF)
		return fail(err, eb, "%s tem tamanho invalido", path);
	p->file_bytes = (uint32_t)size;

	uint8_t head[BS_PACK_HEADER_BYTES];
	if (!read_at(p->file, 0, head, sizeof head))
		return fail(err, eb, "nao consegui ler o cabecalho");
	if (memcmp(head, BS_PACK_MAGIC, 4) != 0)
		return fail(err, eb, "%s nao e um pacote do Bot Strike 360", path);
	if (be32(head + 4) != BS_PACK_VERSION)
		return fail(err, eb, "versao %u do pacote; este jogo le a %u. Gere o pacote de novo", (unsigned)be32(head + 4),
			(unsigned)BS_PACK_VERSION);
	const uint32_t count = be32(head + 8);
	if (count == 0 || count > BS_PACK_MAX_SECTIONS || be32(head + 12) != p->file_bytes)
		return fail(err, eb, "cabecalho corrompido (arquivo cortado no envio?)");

	uint8_t raw[BS_PACK_MAX_SECTIONS * BS_PACK_SECTION_BYTES];
	Section table[BS_PACK_MAX_SECTIONS];
	if (!read_at(p->file, BS_PACK_HEADER_BYTES, raw, count * BS_PACK_SECTION_BYTES))
		return fail(err, eb, "nao consegui ler a tabela de secoes");
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *r = raw + i * BS_PACK_SECTION_BYTES;
		table[i] = (Section){ be32(r), be32(r + 4), be32(r + 8), be32(r + 12) };
		if (table[i].offset % 16 || table[i].offset > p->file_bytes || table[i].bytes > p->file_bytes - table[i].offset)
			return fail(err, eb, "secao %u fora do arquivo", (unsigned)i);
	}
	static const uint32_t needed[] = { BS_SECTION_INFO, BS_SECTION_SPAWNS, BS_SECTION_TEXTURES, BS_SECTION_TEXTURE_DATA,
		BS_SECTION_MATERIALS, BS_SECTION_VERTICES, BS_SECTION_INDICES, BS_SECTION_DRAWS, BS_SECTION_COLLISION,
		BS_SECTION_COLLISION_FLAGS, BS_SECTION_GRID };
	for (size_t i = 0; i < sizeof needed / sizeof needed[0]; i++)
		if (!find(table, count, needed[i]))
			return fail(err, eb, "falta uma secao no pacote");
	const Section *s;

	// INFO
	s = find(table, count, BS_SECTION_INFO);
	uint8_t *info = read_section(p, s, BS_INFO_BYTES, err, eb);
	if (!info)
		return false;
	BsInfo *in = &p->info;
	in->sun_dir = v3(bef(info), bef(info + 4), bef(info + 8));
	in->ambient = bef(info + 12);
	in->sun = bef(info + 16);
	in->fog_start = bef(info + 20);
	in->fog_end = bef(info + 24);
	for (int i = 0; i < 3; i++) {
		in->sky_rgb[i] = bef(info + 28 + i * 4);
		in->fog_rgb[i] = bef(info + 40 + i * 4);
	}
	in->min = v3(bef(info + 52), bef(info + 56), bef(info + 60));
	in->max = v3(bef(info + 64), bef(info + 68), bef(info + 72));
	free(info);
	if (!finite3(&in->min.x) || !finite3(&in->max.x) || !(in->fog_end > in->fog_start))
		return fail(err, eb, "INFO invalido");

	// SPWN
	s = find(table, count, BS_SECTION_SPAWNS);
	if (s->count == 0 || s->count > 256)
		return fail(err, eb, "numero de spawns invalido");
	uint8_t *sp = read_section(p, s, s->count * BS_SPAWN_BYTES, err, eb);
	if (!sp)
		return false;
	p->spawns = calloc(s->count, sizeof *p->spawns);
	if (!p->spawns) {
		free(sp);
		return fail(err, eb, "sem memoria");
	}
	p->spawn_count = s->count;
	for (uint32_t i = 0; i < s->count; i++) {
		const uint8_t *r = sp + i * BS_SPAWN_BYTES;
		p->spawns[i] = (BsSpawn){ be32(r), v3(bef(r + 4), bef(r + 8), bef(r + 12)), bef(r + 16) };
		if (!finite3(&p->spawns[i].position.x) || !isfinite(p->spawns[i].yaw)) {
			free(sp);
			return fail(err, eb, "spawn %u invalido", (unsigned)i);
		}
	}
	free(sp);

	// TEXR e TEXD (os dados ficam no arquivo e sao lidos textura a textura)
	const Section *td = find(table, count, BS_SECTION_TEXTURE_DATA);
	p->texture_data_offset = td->offset;
	p->texture_data_bytes = td->bytes;
	s = find(table, count, BS_SECTION_TEXTURES);
	if (s->count > 4096)
		return fail(err, eb, "texturas demais");
	uint8_t *tr = read_section(p, s, s->count * BS_TEXTURE_BYTES, err, eb);
	if (!tr)
		return false;
	p->textures = calloc(s->count ? s->count : 1, sizeof *p->textures);
	if (!p->textures) {
		free(tr);
		return fail(err, eb, "sem memoria");
	}
	p->texture_count = s->count;
	for (uint32_t i = 0; i < s->count; i++) {
		const uint8_t *r = tr + i * BS_TEXTURE_BYTES;
		BsTextureInfo *t = &p->textures[i];
		*t = (BsTextureInfo){ be32(r), be16(r + 4), be16(r + 6), be16(r + 8), be32(r + 12), be32(r + 16) };
		const bool pot = t->width && t->height && !(t->width & (t->width - 1)) && !(t->height & (t->height - 1));
		const uint32_t big = t->width > t->height ? t->width : t->height;
		bool ok = (t->format == BS_TEXTURE_DXT1 || t->format == BS_TEXTURE_DXT5) && pot && t->width >= 4 &&
			t->height >= 4 && big <= BS_MAX_TEXTURE_SIZE && t->levels >= 1 && t->levels <= BS_MAX_TEXTURE_LEVELS &&
			t->levels <= log2u(big) + 1;
		uint32_t total = 0;
		for (uint32_t l = 0; ok && l < t->levels; l++)
			total += bs_texture_level_bytes(t, l);
		ok = ok && total == t->data_bytes && t->data_offset <= td->bytes && t->data_bytes <= td->bytes - t->data_offset;
		if (!ok) {
			free(tr);
			return fail(err, eb, "textura %u invalida", (unsigned)i);
		}
	}
	free(tr);

	// MATL
	s = find(table, count, BS_SECTION_MATERIALS);
	if (s->count == 0 || s->count > 65536)
		return fail(err, eb, "numero de materiais invalido");
	uint8_t *ml = read_section(p, s, s->count * BS_MATERIAL_BYTES, err, eb);
	if (!ml)
		return false;
	p->materials = calloc(s->count, sizeof *p->materials);
	if (!p->materials) {
		free(ml);
		return fail(err, eb, "sem memoria");
	}
	p->material_count = s->count;
	for (uint32_t i = 0; i < s->count; i++) {
		const uint8_t *r = ml + i * BS_MATERIAL_BYTES;
		BsMaterial *m = &p->materials[i];
		m->texture = (int32_t)be32(r);
		for (int c = 0; c < 4; c++)
			m->color[c] = clampf(bef(r + 4 + c * 4), 0.0f, 4.0f);
		m->flags = be32(r + 20);
		if (m->texture < -1 || m->texture >= (int32_t)p->texture_count)
			m->texture = -1;
	}
	free(ml);

	// VERT
	s = find(table, count, BS_SECTION_VERTICES);
	if (s->count == 0 || s->count > 16u * 1024 * 1024)
		return fail(err, eb, "numero de vertices invalido");
	p->vertices = read_section(p, s, s->count * BS_VERTEX_BYTES, err, eb);
	if (!p->vertices)
		return false;
	p->vertex_count = s->count;
	swap32_array(p->vertices, (size_t)s->count * 6);
	for (uint32_t i = 0; i < s->count; i++) {
		const float *v = p->vertices + i * 6;
		if (!finite3(v) || !isfinite(v[3]) || !isfinite(v[4]) || !isfinite(v[5]))
			return fail(err, eb, "vertice %u invalido", (unsigned)i);
	}

	// INDX
	s = find(table, count, BS_SECTION_INDICES);
	if (s->count == 0 || s->count > 64u * 1024 * 1024)
		return fail(err, eb, "numero de indices invalido");
	p->indices = read_section(p, s, s->count * 2, err, eb);
	if (!p->indices)
		return false;
	p->index_count = s->count;
	swap16_array(p->indices, s->count);

	// DRAW: cada faixa dentro dos blocos, alinhada, e cada indice abaixo do numero de vertices do draw.
	s = find(table, count, BS_SECTION_DRAWS);
	if (s->count == 0 || s->count > 65536)
		return fail(err, eb, "numero de draws invalido");
	uint8_t *dr = read_section(p, s, s->count * BS_DRAW_BYTES, err, eb);
	if (!dr)
		return false;
	p->draws = calloc(s->count, sizeof *p->draws);
	if (!p->draws) {
		free(dr);
		return fail(err, eb, "sem memoria");
	}
	p->draw_count = s->count;
	for (uint32_t i = 0; i < s->count; i++) {
		const uint8_t *r = dr + i * BS_DRAW_BYTES;
		BsDraw *d = &p->draws[i];
		*d = (BsDraw){ be32(r), be32(r + 4), be32(r + 8), be32(r + 12), be32(r + 16), be32(r + 20),
			v3(bef(r + 24), bef(r + 28), bef(r + 32)), v3(bef(r + 36), bef(r + 40), bef(r + 44)) };
		bool ok = d->material < p->material_count && d->vertex_count >= 3 &&
			d->vertex_count <= BS_MAX_DRAW_VERTICES && d->first_vertex <= p->vertex_count &&
			d->vertex_count <= p->vertex_count - d->first_vertex && d->index_count >= 3 && d->index_count % 3 == 0 &&
			d->index_count <= BS_MAX_DRAW_INDICES && d->first_index % 2 == 0 && d->first_index <= p->index_count &&
			d->index_count <= p->index_count - d->first_index && finite3(&d->min.x) && finite3(&d->max.x);
		for (uint32_t k = 0; ok && k < d->index_count; k++)
			ok = p->indices[d->first_index + k] < d->vertex_count;
		if (!ok) {
			free(dr);
			return fail(err, eb, "draw %u invalido", (unsigned)i);
		}
	}
	free(dr);

	// CTRI, CFLG
	s = find(table, count, BS_SECTION_COLLISION);
	if (s->count == 0 || s->count > 8u * 1024 * 1024)
		return fail(err, eb, "numero de triangulos de colisao invalido");
	p->triangles = read_section(p, s, s->count * BS_TRIANGLE_BYTES, err, eb);
	if (!p->triangles)
		return false;
	p->triangle_count = s->count;
	swap32_array(p->triangles, (size_t)s->count * 9);
	for (uint32_t i = 0; i < s->count * 3; i++)
		if (!finite3(p->triangles + i * 3))
			return fail(err, eb, "triangulo de colisao %u invalido", (unsigned)(i / 3));
	s = find(table, count, BS_SECTION_COLLISION_FLAGS);
	if (s->count != p->triangle_count)
		return fail(err, eb, "flags de colisao nao batem");
	p->triangle_flags = read_section(p, s, s->count, err, eb);
	if (!p->triangle_flags)
		return false;

	// CGRD
	s = find(table, count, BS_SECTION_GRID);
	if (s->bytes < BS_GRID_HEADER_BYTES)
		return fail(err, eb, "grade curta demais");
	uint8_t gh[BS_GRID_HEADER_BYTES];
	if (!read_at(p->file, s->offset, gh, sizeof gh))
		return fail(err, eb, "nao consegui ler a grade");
	BsGrid *g = &p->grid;
	g->origin = v3(bef(gh), bef(gh + 4), bef(gh + 8));
	g->cell = bef(gh + 12);
	g->nx = be32(gh + 16);
	g->ny = be32(gh + 20);
	g->nz = be32(gh + 24);
	g->ref_count = be32(gh + 28);
	const uint64_t cells = (uint64_t)g->nx * g->ny * g->nz;
	if (!finite3(&g->origin.x) || !(g->cell >= 0.25f && g->cell <= 16.0f) || cells == 0 || cells > BS_MAX_GRID_CELLS ||
		g->ref_count > 64u * 1024 * 1024 || BS_GRID_HEADER_BYTES + (cells + 1 + g->ref_count) * 4 != s->bytes)
		return fail(err, eb, "grade invalida");
	g->cell_start = malloc((size_t)(cells + 1) * 4);
	g->refs = malloc((size_t)(g->ref_count ? g->ref_count : 1) * 4);
	if (!g->cell_start || !g->refs)
		return fail(err, eb, "sem memoria para a grade");
	if (!read_at(p->file, s->offset + BS_GRID_HEADER_BYTES, g->cell_start, (uint32_t)(cells + 1) * 4) ||
		!read_at(p->file, s->offset + BS_GRID_HEADER_BYTES + (uint32_t)(cells + 1) * 4, g->refs, g->ref_count * 4))
		return fail(err, eb, "nao consegui ler a grade");
	swap32_array(g->cell_start, (size_t)cells + 1);
	swap32_array(g->refs, g->ref_count);
	if (g->cell_start[0] != 0 || g->cell_start[cells] != g->ref_count)
		return fail(err, eb, "grade incoerente");
	for (uint64_t c = 0; c < cells; c++)
		if (g->cell_start[c] > g->cell_start[c + 1])
			return fail(err, eb, "grade incoerente");
	for (uint32_t r = 0; r < g->ref_count; r++)
		if (g->refs[r] >= p->triangle_count)
			return fail(err, eb, "grade aponta para triangulo inexistente");
	return true;
}

bool bs_pack_load(BsPack *pack, const char *path, char *err, size_t err_bytes)
{
	memset(pack, 0, sizeof *pack);
	if (load(pack, path, err, err_bytes))
		return true;
	bs_pack_free(pack);
	return false;
}

bool bs_pack_read_texture(BsPack *pack, const BsTextureInfo *info, void *out)
{
	return pack->file && read_at(pack->file, pack->texture_data_offset + info->data_offset, out, info->data_bytes);
}

void bs_pack_release_geometry(BsPack *pack)
{
	free(pack->vertices);
	free(pack->indices);
	pack->vertices = NULL;
	pack->indices = NULL;
}

void bs_pack_free(BsPack *pack)
{
	if (pack->file)
		fclose(pack->file);
	bs_pack_release_geometry(pack);
	free(pack->spawns);
	free(pack->textures);
	free(pack->materials);
	free(pack->draws);
	free(pack->triangles);
	free(pack->triangle_flags);
	free(pack->grid.cell_start);
	free(pack->grid.refs);
	memset(pack, 0, sizeof *pack);
}
