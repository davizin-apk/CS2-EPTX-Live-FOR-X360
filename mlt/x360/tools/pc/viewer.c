// Ve o dust2.pak no PC, com o mesmo leitor, a mesma colisao, o mesmo movimento, a mesma escolha de LOD e o mesmo
// corte por frustum do .xex, desenhado em OpenGL 1.3. Serve para conferir o pacote antes de mandar para o console.
//
//   zig cc -O2 -std=c17 -I src tools/pc/viewer.c src/pack.c src/collide.c src/player.c -lopengl32 -lgdi32 -o out/viewer.exe
//   out/viewer.exe out/dust2.pak                         (WASD, mouse, espaco, ctrl, shift, F voa, N spawn, Esc sai)
//   out/viewer.exe out/dust2.pak --shot a.bmp --spawn 3  (um quadro do spawn 3 salvo em a.bmp, e sai)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collide.h"
#include "pack.h"
#include "player.h"

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif
typedef void(APIENTRY *CompressedTexImage2D)(
	GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);

static const float FOV_Y = 1.2f;
static const float NEAR_Z = 0.05f;
static const float LOD_DISTANCE = 28.0f;

static int g_width = 1280, g_height = 720;
static bool g_keys[256];
static bool g_quit;
static int g_mouse_dx, g_mouse_dy;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	switch (msg) {
	case WM_CLOSE:
		g_quit = true;
		return 0;
	case WM_KEYDOWN:
		g_keys[w & 0xFF] = true;
		return 0;
	case WM_KEYUP:
		g_keys[w & 0xFF] = false;
		return 0;
	case WM_SIZE:
		g_width = LOWORD(l);
		g_height = HIWORD(l) ? HIWORD(l) : 1;
		return 0;
	}
	return DefWindowProcA(h, msg, w, l);
}

typedef struct {
	BsPack pack;
	float *colors; // por vertice: luz do vertice vezes a cor do material
	GLuint *textures;
	GLuint white;
} View;

static void upload(View *v)
{
	BsPack *p = &v->pack;
	CompressedTexImage2D compressed = (CompressedTexImage2D)wglGetProcAddress("glCompressedTexImage2D");
	v->textures = calloc(p->texture_count + 1, sizeof *v->textures);
	glGenTextures((GLsizei)p->texture_count, v->textures);
	for (uint32_t i = 0; i < p->texture_count; i++) {
		const BsTextureInfo *t = &p->textures[i];
		uint16_t *data = malloc(t->data_bytes);
		bs_pack_read_texture(p, t, data);
		for (uint32_t k = 0; k < t->data_bytes / 2; k++) // o pacote guarda palavras big-endian
			data[k] = (uint16_t)(data[k] >> 8 | data[k] << 8);
		glBindTexture(GL_TEXTURE_2D, v->textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, (GLint)t->levels - 1);
		uint32_t at = 0;
		for (uint32_t l = 0; l < t->levels; l++) {
			const uint32_t n = bs_texture_level_bytes(t, l);
			uint32_t w = t->width >> l, h = t->height >> l;
			compressed(GL_TEXTURE_2D, (GLint)l,
				t->format == BS_TEXTURE_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
				w ? (GLsizei)w : 1, h ? (GLsizei)h : 1, 0, (GLsizei)n, (const uint8_t *)data + at);
			at += n;
		}
		free(data);
	}
	glGenTextures(1, &v->white);
	glBindTexture(GL_TEXTURE_2D, v->white);
	const uint32_t white[16] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

	v->colors = malloc((size_t)p->vertex_count * 4 * sizeof(float));
	for (uint32_t d = 0; d < p->draw_count; d++) {
		const BsDraw *dr = &p->draws[d];
		const float *c = p->materials[dr->material].color;
		for (uint32_t k = 0; k < dr->vertex_count; k++) {
			const uint32_t vi = dr->first_vertex + k;
			const float s = p->vertices[vi * 6 + 5];
			float *o = v->colors + (size_t)vi * 4;
			o[0] = c[0] * s;
			o[1] = c[1] * s;
			o[2] = c[2] * s;
			o[3] = c[3];
		}
	}
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

static void draw(View *v, const BsPlayer *pl, uint32_t *draws, uint32_t *tris)
{
	const BsInfo *in = &v->pack.info;
	const Vec3 eye = bs_player_eye(pl);
	const float far_z = in->fog_end + 30.0f;
	const float aspect = (float)g_width / (float)g_height;
	glViewport(0, 0, g_width, g_height);
	glClearColor(in->sky_rgb[0], in->sky_rgb[1], in->sky_rgb[2], 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	const float t = NEAR_Z * tanf(FOV_Y * 0.5f);
	glFrustum(-t * aspect, t * aspect, -t, t, NEAR_Z, far_z);
	glMatrixMode(GL_MODELVIEW);
	const Mat4 view = mat4_view(eye, pl->yaw, pl->pitch);
	glLoadMatrixf(view.m);
	// o mesmo corte do console: planos da projecao do .xex
	const Mat4 proj = mat4_perspective(FOV_Y, aspect, NEAR_Z, far_z);
	const Mat4 vp = mat4_multiply(&proj, &view);
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
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, in->fog_start);
	glFogf(GL_FOG_END, in->fog_end);
	const float fog[4] = { in->fog_rgb[0], in->fog_rgb[1], in->fog_rgb[2], 1 };
	glFogfv(GL_FOG_COLOR, fog);
	glAlphaFunc(GL_GEQUAL, 0.5f);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	*draws = *tris = 0;
	for (uint32_t i = 0; i < v->pack.draw_count; i++) {
		const BsDraw *d = &v->pack.draws[i];
		if (!box_visible(planes, d->min, d->max))
			continue;
		const bool is_near = box_distance(eye, d->min, d->max) < LOD_DISTANCE;
		if ((d->flags & BS_DRAW_NEAR && !is_near) || (d->flags & BS_DRAW_FAR && is_near))
			continue;
		const BsMaterial *m = &v->pack.materials[d->material];
		glBindTexture(GL_TEXTURE_2D, m->texture >= 0 ? v->textures[m->texture] : v->white);
		if (d->flags & BS_DRAW_ALPHA_TEST)
			glEnable(GL_ALPHA_TEST);
		else
			glDisable(GL_ALPHA_TEST);
		const float *vb = v->pack.vertices + (size_t)d->first_vertex * 6;
		glVertexPointer(3, GL_FLOAT, 24, vb);
		glTexCoordPointer(2, GL_FLOAT, 24, vb + 3);
		glColorPointer(4, GL_FLOAT, 0, v->colors + (size_t)d->first_vertex * 4);
		glDrawElements(GL_TRIANGLES, (GLsizei)d->index_count, GL_UNSIGNED_SHORT, v->pack.indices + d->first_index);
		(*draws)++;
		*tris += d->index_count / 3;
	}
}

static void save_bmp(const char *path)
{
	const int w = g_width, h = g_height, row = (w * 3 + 3) & ~3;
	uint8_t *px = malloc((size_t)row * h);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(54 + row * h), 0, 0, 54 };
	BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 24, BI_RGB, (DWORD)(row * h), 2835, 2835, 0, 0 };
	fwrite(&fh, sizeof fh, 1, f);
	fwrite(&ih, sizeof ih, 1, f);
	fwrite(px, (size_t)row * h, 1, f);
	fclose(f);
	free(px);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "uso: viewer <dust2.pak> [--shot arquivo.bmp] [--spawn N] [--yaw graus] [--pitch graus]\n");
		return 2;
	}
	const char *shot = NULL;
	int spawn = 0;
	float yaw_add = 0, pitch = 0, fly_up = 0;
	for (int i = 2; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--shot"))
			shot = argv[i + 1];
		else if (!strcmp(argv[i], "--spawn"))
			spawn = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--yaw"))
			yaw_add = (float)atof(argv[i + 1]) * 3.14159265f / 180.0f;
		else if (!strcmp(argv[i], "--pitch"))
			pitch = (float)atof(argv[i + 1]) * 3.14159265f / 180.0f;
		else if (!strcmp(argv[i], "--up"))
			fly_up = (float)atof(argv[i + 1]);
	}
	static View v;
	char err[256];
	if (!bs_pack_load(&v.pack, argv[1], err, sizeof err)) {
		fprintf(stderr, "FALHOU: %s\n", err);
		return 1;
	}
	static BsCollider col;
	bs_collider_init(&col, &v.pack);

	WNDCLASSA wc = { CS_OWNDC, wndproc, 0, 0, GetModuleHandleA(NULL), 0, LoadCursor(NULL, IDC_ARROW), 0, 0, "bs360" };
	RegisterClassA(&wc);
	RECT rc = { 0, 0, g_width, g_height };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowA("bs360", "Bot Strike 360 - visualizador do pacote", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, 0, 0, wc.hInstance, 0);
	HDC dc = GetDC(hwnd);
	PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
		PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
	SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
	HGLRC gl = wglCreateContext(dc);
	wglMakeCurrent(dc, gl);
	upload(&v);

	BsPlayer pl;
	if (spawn < 0 || spawn >= (int)v.pack.spawn_count)
		spawn = 0;
	bs_player_spawn(&pl, &v.pack.spawns[spawn]);
	pl.yaw += yaw_add;
	pl.pitch = pitch;
	if (fly_up > 0) {
		pl.fly = true;
		pl.position.y += fly_up;
	}
	LARGE_INTEGER freq, last;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&last);
	bool prev_f = false, prev_n = false;
	int frames = 0;
	double title_t = 0;
	if (!shot)
		ShowCursor(FALSE);
	while (!g_quit) {
		MSG msg;
		while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const float dt = (float)((now.QuadPart - last.QuadPart) / (double)freq.QuadPart);
		last = now;
		BsInput in = { 0 };
		if (!shot && GetForegroundWindow() == hwnd) {
			POINT c = { g_width / 2, g_height / 2 }, m;
			ClientToScreen(hwnd, &c);
			GetCursorPos(&m);
			g_mouse_dx = m.x - c.x;
			g_mouse_dy = m.y - c.y;
			SetCursorPos(c.x, c.y);
			in.look_x = g_mouse_dx * 0.0025f;
			in.look_y = -g_mouse_dy * 0.0025f;
			in.move_y = (float)g_keys['W'] - (float)g_keys['S'];
			in.move_x = (float)g_keys['D'] - (float)g_keys['A'];
			in.jump = g_keys[VK_SPACE];
			in.crouch = g_keys[VK_CONTROL];
			in.walk = g_keys[VK_SHIFT];
			if (g_keys['F'] && !prev_f)
				pl.fly = !pl.fly;
			if (g_keys['N'] && !prev_n) {
				spawn = (spawn + 1) % (int)v.pack.spawn_count;
				bs_player_spawn(&pl, &v.pack.spawns[spawn]);
			}
			prev_f = g_keys['F'];
			prev_n = g_keys['N'];
			if (g_keys[VK_ESCAPE])
				g_quit = true;
		}
		// o quadro do --shot espera o jogador assentar no chao
		bs_player_update(&pl, &col, &in, shot ? 1.0f / 60.0f : dt);
		uint32_t draws, tris;
		draw(&v, &pl, &draws, &tris);
		frames++;
		if (shot && frames == 90) {
			save_bmp(shot);
			printf("salvo %s: spawn %d em (%.1f %.1f %.1f), %u draws, %u triangulos\n", shot, spawn, pl.position.x,
				pl.position.y, pl.position.z, draws, tris);
			break;
		}
		SwapBuffers(dc);
		title_t += dt;
		if (title_t > 0.5) {
			title_t = 0;
			char title[256];
			snprintf(title, sizeof title, "Bot Strike 360 - %u draws, %uk tri, pos %.1f %.1f %.1f %s, spawn %d", draws,
				tris / 1000, pl.position.x, pl.position.y, pl.position.z, pl.fly ? "voando" : (pl.on_ground ? "chao" : "ar"),
				spawn);
			SetWindowTextA(hwnd, title);
		}
	}
	wglMakeCurrent(0, 0);
	wglDeleteContext(gl);
	DestroyWindow(hwnd);
	return 0;
}
