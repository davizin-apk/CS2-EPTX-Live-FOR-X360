// O renderizador do PC (OpenGL 1.3 com arrays do cliente), para testar o jogo sem o console. Desenha o mesmo que
// o gfx_xbox.c, com as mesmas regras de LOD e corte.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
typedef void(APIENTRY *CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);

HDC plat_pc_dc(void);
void plat_pc_size(uint32_t *w, uint32_t *h);

enum {
	MAX_TEXTURES = 8192,
	MAX_MESHES = 1024,
};

static const float LOD_DISTANCE = 28.0f;

typedef struct {
	MeshVertex *v;
	uint32_t vertex_count;
	uint16_t *idx;
	uint32_t index_count;
} GlMesh;

static HGLRC ctx;
static CompressedTexImage2D compressed;
static GLuint textures[MAX_TEXTURES];
static uint32_t texture_count;
static GlMesh meshes[MAX_MESHES];
static uint32_t mesh_count;
static BsPack *map;
static GLuint *map_tex;
static float *map_colors;
static GfxCamera cam;
static int layer;
static uint32_t win_w, win_h;

bool gfx_open(uint32_t *width, uint32_t *height, char *err, size_t eb)
{
	HDC dc = plat_pc_dc();
	PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
		PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
	if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd) || !(ctx = wglCreateContext(dc)) ||
		!wglMakeCurrent(dc, ctx)) {
		snprintf(err, eb, "OpenGL nao abriu");
		return false;
	}
	compressed = (CompressedTexImage2D)wglGetProcAddress("glCompressedTexImage2D");
	if (!compressed) {
		snprintf(err, eb, "a placa nao tem texturas DXT");
		return false;
	}
	// textura 0: branca
	glGenTextures(1, &textures[0]);
	glBindTexture(GL_TEXTURE_2D, textures[0]);
	const uint32_t white[16] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	texture_count = 1;
	*width = GFX_WIDTH;
	*height = GFX_HEIGHT;
	return true;
}

void gfx_close(void)
{
	wglMakeCurrent(0, 0);
	if (ctx)
		wglDeleteContext(ctx);
	ctx = 0;
}

int gfx_texture(const TexRes *t)
{
	if (texture_count >= MAX_TEXTURES)
		return GFX_WHITE;
	uint16_t *data = malloc(t->bytes);
	if (!data)
		return GFX_WHITE;
	for (uint32_t k = 0; k < t->bytes / 2; k++) {
		const uint8_t *b = (const uint8_t *)t->data + k * 2;
		data[k] = (uint16_t)(b[0] << 8 | b[1]); // as palavras vem big-endian
	}
	GLuint id;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
	uint32_t at = 0;
	for (uint32_t l = 0; l < t->levels; l++) {
		const uint32_t w = t->width >> l ? t->width >> l : 1, h = t->height >> l ? t->height >> l : 1;
		const uint32_t n = ((w + 3) / 4) * ((h + 3) / 4) * (t->format == BS_TEXTURE_DXT1 ? 8 : 16);
		compressed(GL_TEXTURE_2D, (GLint)l,
			t->format == BS_TEXTURE_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
			(GLsizei)w, (GLsizei)h, 0, (GLsizei)n, (const uint8_t *)data + at);
		at += n;
	}
	free(data);
	textures[texture_count] = id;
	return (int)texture_count++;
}

int gfx_texture_rgba(uint32_t w, uint32_t h, const uint8_t *rgba)
{
	if (texture_count >= MAX_TEXTURES)
		return GFX_WHITE;
	GLuint id;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	textures[texture_count] = id;
	return (int)texture_count++;
}

int gfx_mesh(const MeshVertex *v, uint32_t vc, const uint16_t *idx, uint32_t ic)
{
	if (mesh_count >= MAX_MESHES)
		return -1;
	GlMesh *m = &meshes[mesh_count];
	m->v = malloc(vc * sizeof *v);
	m->idx = malloc(ic * 2 + 2);
	if (!m->v || !m->idx)
		return -1;
	if (v)
		memcpy(m->v, v, vc * sizeof *v);
	memcpy(m->idx, idx, ic * 2);
	m->vertex_count = vc;
	m->index_count = ic;
	return (int)mesh_count++;
}

int gfx_skinned(uint32_t vc, const uint16_t *idx, uint32_t ic)
{
	return gfx_mesh(NULL, vc, idx, ic);
}

void gfx_skinned_vertices(int mesh, const MeshVertex *v, uint32_t count)
{
	if (mesh < 0 || (uint32_t)mesh >= mesh_count || count > meshes[mesh].vertex_count)
		return;
	memcpy(meshes[mesh].v, v, count * sizeof *v);
}

bool gfx_map_upload(BsPack *p, char *err, size_t eb)
{
	map = p;
	map_tex = calloc(p->texture_count + 1, sizeof *map_tex);
	map_colors = malloc((size_t)p->vertex_count * 4 * sizeof(float));
	if (!map_tex || !map_colors) {
		snprintf(err, eb, "sem memoria para o mapa");
		return false;
	}
	for (uint32_t i = 0; i < p->texture_count; i++) {
		const BsTextureInfo *t = &p->textures[i];
		uint16_t *buf = malloc(t->data_bytes);
		if (!buf || !bs_pack_read_texture(p, t, buf)) {
			free(buf);
			snprintf(err, eb, "textura %u do mapa nao le", (unsigned)i);
			return false;
		}
		const TexRes tr = { t->format, t->width, t->height, t->levels, buf, t->data_bytes, -1 };
		map_tex[i] = textures[gfx_texture(&tr)];
		free(buf);
	}
	for (uint32_t d = 0; d < p->draw_count; d++) {
		const BsDraw *dr = &p->draws[d];
		const float *c = p->materials[dr->material].color;
		for (uint32_t k = 0; k < dr->vertex_count; k++) {
			const uint32_t vi = dr->first_vertex + k;
			const float s = p->vertices[vi * 6 + 5];
			float *o = map_colors + (size_t)vi * 4;
			o[0] = c[0] * s;
			o[1] = c[1] * s;
			o[2] = c[2] * s;
			o[3] = c[3];
		}
	}
	return true;
}

bool gfx_begin(void)
{
	plat_pc_size(&win_w, &win_h);
	glViewport(0, 0, (GLsizei)win_w, (GLsizei)win_h);
	glDepthMask(GL_TRUE);
	glClearColor(cam.sky_rgb[0], cam.sky_rgb[1], cam.sky_rgb[2], 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	return true;
}

void gfx_end(void)
{
	SwapBuffers(plat_pc_dc());
}

void gfx_camera(const GfxCamera *c, int l)
{
	cam = *c;
	layer = l;
	glMatrixMode(GL_PROJECTION);
	// de profundidade 0..1 (Xenos) para -1..1 (OpenGL): z' = 2z - w
	Mat4 p = c->proj;
	for (int col = 0; col < 4; col++)
		p.m[col * 4 + 2] = 2.0f * p.m[col * 4 + 2] - p.m[col * 4 + 3];
	glLoadMatrixf(p.m);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(c->view.m);
	glDepthRange(l ? 0.0 : 0.02, l ? 0.02 : 1.0);
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, c->fog_start);
	glFogf(GL_FOG_END, c->fog_end);
	const float fog[4] = { c->fog_rgb[0], c->fog_rgb[1], c->fog_rgb[2], 1 };
	glFogfv(GL_FOG_COLOR, fog);
	// a luz do sol no espaco da camera (a matriz carregada agora e a view)
	const float sun[4] = { -c->sun_dir.x, -c->sun_dir.y, -c->sun_dir.z, 0 };
	glLightfv(GL_LIGHT0, GL_POSITION, sun);
	const float amb[4] = { c->ambient, c->ambient, c->ambient, 1 }, dif[4] = { c->sun, c->sun, c->sun, 1 };
	glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
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
	const Mat4 vp = mat4_multiply(&cam.proj, &cam.view);
	float r[4][4], planes[6][4];
	for (int i = 0; i < 4; i++)
		for (int c = 0; c < 4; c++)
			r[i][c] = vp.m[c * 4 + i];
	for (int c = 0; c < 4; c++) {
		planes[0][c] = r[3][c] + r[0][c];
		planes[1][c] = r[3][c] - r[0][c];
		planes[2][c] = r[3][c] + r[1][c];
		planes[3][c] = r[3][c] - r[1][c];
		planes[4][c] = r[2][c];
		planes[5][c] = r[3][c] - r[2][c];
	}
	glDisable(GL_LIGHTING);
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glAlphaFunc(GL_GEQUAL, 0.5f);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	for (uint32_t i = 0; i < map->draw_count; i++) {
		const BsDraw *d = &map->draws[i];
		if (!box_visible(planes, d->min, d->max))
			continue;
		const bool is_near = box_distance(cam.eye, d->min, d->max) < LOD_DISTANCE;
		if ((d->flags & BS_DRAW_NEAR && !is_near) || (d->flags & BS_DRAW_FAR && is_near))
			continue;
		const BsMaterial *m = &map->materials[d->material];
		glBindTexture(GL_TEXTURE_2D, m->texture >= 0 ? map_tex[m->texture] : textures[0]);
		if (d->flags & BS_DRAW_ALPHA_TEST)
			glEnable(GL_ALPHA_TEST);
		else
			glDisable(GL_ALPHA_TEST);
		const float *vb = map->vertices + (size_t)d->first_vertex * 6;
		glVertexPointer(3, GL_FLOAT, 24, vb);
		glTexCoordPointer(2, GL_FLOAT, 24, vb + 3);
		glColorPointer(4, GL_FLOAT, 0, map_colors + (size_t)d->first_vertex * 4);
		glDrawElements(GL_TRIANGLES, (GLsizei)d->index_count, GL_UNSIGNED_SHORT, map->indices + d->first_index);
		st.draws++;
		st.triangles += d->index_count / 3;
	}
	glDisableClientState(GL_COLOR_ARRAY);
	glDisable(GL_ALPHA_TEST);
	return st;
}

static void blend_mode(uint32_t flags)
{
	if (flags & GFX_ADD) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	} else if (flags & GFX_BLEND) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}
	if (flags & GFX_ALPHA_TEST)
		glEnable(GL_ALPHA_TEST);
	else
		glDisable(GL_ALPHA_TEST);
	if (flags & GFX_NO_DEPTH)
		glDisable(GL_DEPTH_TEST);
	else
		glEnable(GL_DEPTH_TEST);
	glDepthMask((flags & (GFX_NO_DEPTH_WRITE | GFX_BLEND | GFX_ADD)) ? GL_FALSE : GL_TRUE);
}

void gfx_draw_mesh(int mesh, const Mat4 *model, uint32_t first, uint32_t count, int texture, const float color[4],
	uint32_t flags)
{
	if (mesh < 0 || (uint32_t)mesh >= mesh_count)
		return;
	const GlMesh *m = &meshes[mesh];
	if (first > m->index_count || count > m->index_count - first)
		return;
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	if (model)
		glMultMatrixf(model->m);
	blend_mode(flags);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, textures[texture >= 0 && (uint32_t)texture < texture_count ? texture : 0]);
	if (flags & GFX_UNLIT) {
		glDisable(GL_LIGHTING);
	} else {
		glEnable(GL_LIGHTING);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
		glEnable(GL_NORMALIZE);
	}
	glColor4fv(color);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_NORMAL_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(MeshVertex), &m->v[0].x);
	glNormalPointer(GL_FLOAT, sizeof(MeshVertex), &m->v[0].nx);
	glTexCoordPointer(2, GL_FLOAT, sizeof(MeshVertex), &m->v[0].u);
	glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_SHORT, m->idx + first);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisable(GL_LIGHTING);
	glPopMatrix();
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_DEPTH_TEST);
}

static void draw_fx(const FxVertex *v, uint32_t count, int texture, uint32_t flags)
{
	blend_mode(flags);
	glDisable(GL_LIGHTING);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, textures[texture >= 0 && (uint32_t)texture < texture_count ? texture : 0]);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(FxVertex), &v[0].x);
	glColorPointer(4, GL_FLOAT, sizeof(FxVertex), &v[0].r);
	glTexCoordPointer(2, GL_FLOAT, sizeof(FxVertex), &v[0].u);
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
	glDisableClientState(GL_COLOR_ARRAY);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_DEPTH_TEST);
}

void gfx_draw_fx(const FxVertex *v, uint32_t count, int texture, uint32_t flags)
{
	if (count)
		draw_fx(v, count, texture, flags);
}

void gfx_draw_2d(const FxVertex *v, uint32_t count, int texture)
{
	if (!count)
		return;
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, GFX_WIDTH, GFX_HEIGHT, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	glDisable(GL_FOG);
	glDepthRange(0, 1);
	draw_fx(v, count, texture, GFX_BLEND | GFX_NO_DEPTH);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glEnable(GL_FOG);
}

void gfx_screenshot(const char *path)
{
	const int w = (int)win_w, h = (int)win_h, row = (w * 3 + 3) & ~3;
	uint8_t *px = malloc((size_t)row * h);
	if (!px)
		return;
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadBuffer(GL_BACK);
	glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
	FILE *f = fopen(path, "wb");
	if (f) {
		BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(54 + row * h), 0, 0, 54 };
		BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 24, BI_RGB, (DWORD)(row * h), 2835, 2835, 0, 0 };
		fwrite(&fh, sizeof fh, 1, f);
		fwrite(&ih, sizeof ih, 1, f);
		fwrite(px, (size_t)row * h, 1, f);
		fclose(f);
	}
	free(px);
}
