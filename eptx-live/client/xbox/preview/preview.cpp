// Preview da dash no PC: o mesmo dash.cpp, com dados de exemplo no lugar do
// servidor e o rasterizador de software (raster.h) no lugar da GPU do console.
// Gera PNGs para conferir o layout sem ligar o Xbox. Nao testa rede, controle nem GPU.
// Tambem grava o corpo do avatar padrao, que o servidor serve para contas novas.
//
//   wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX Live/client/xbox/preview/build.sh"

#include <stdarg.h>
#include <stdlib.h>
#include <map>
#include <vector>

#define main dash_main
#include "../dash.cpp"
#undef main

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

extern "C" uint8_t *image_encode_png(const uint8_t *rgba, int width, int height, int *out_len)
{
	return stbi_write_png_to_mem(rgba, width * 4, width, height, 4, out_len);
}

// --- o que o console daria ----------------------------------------------------------

struct Image {
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
};
static std::map<const void *, Image> g_textures;

extern "C" {
app_state g_app;
void app_lock(void) {}
void app_unlock(void) {}
void app_start(void) {}
void app_sign_in(const char *, const char *) {}
void app_sign_out(void) {}
void app_refresh(void) {}
void app_load_art(int) {}
void app_load_library_art(int) {}
void app_load_friend_avatar(int) {}
void app_install(int64_t) {}
void app_save_avatar(const avatar_look *, uint8_t *png, int) { free(png); }
void image_free(app_image *image)
{
	free(image->rgba);
	free(image->delays);
	image->rgba = nullptr;
	image->delays = nullptr;
}

void xsdk_log_write(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}
int xsdk_launch(const char *, const void *, size_t) { return -1; }
bool xsdk_system_ui_open(void) { return false; }
uint32_t xsdk_xam_keyboard(uint32_t, const uint16_t *, const uint16_t *, const uint16_t *, uint16_t *, uint32_t,
	XAM_OVERLAPPED *)
{
	return ERROR_SUCCESS;
}
uint32_t xsdk_xam_wait(XAM_OVERLAPPED *, uint32_t, uint32_t *) { return ERROR_SUCCESS; }
uint32_t xsdk_xam_signed_in_user(uint32_t *, char *) { return 1; }
uint32_t XamInputGetState(uint32_t, uint32_t, INPUT_STATE *) { return 1; }
NTSTATUS xsdk_splash_show(uint32_t) { return 0; }
NTSTATUS xsdk_gpu_open(void) { return 0; }
NTSTATUS xsdk_gpu_close(void) { return 0; }
NTSTATUS xsdk_gpu_wait(uint32_t, uint32_t) { return 0; }
uint32_t xsdk_gpu_fence(void) { return 0; }
void xsdk_gpu_invalidate(uint32_t, uint32_t) {}
void xsdk_gpu_set_target(uint32_t, uint32_t) {}
void xsdk_gpu_resolve(uint32_t, uint32_t, uint32_t, uint32_t) {}
NTSTATUS xsdk_present_open(XsdkDisplayMode, uint32_t, uint32_t, uint32_t *, uint32_t *) { return 0; }
NTSTATUS xsdk_present_begin(uint32_t *, uint32_t) { return 0; }
void xsdk_present_end(void) {}
void xsdk_present_close(void) {}
void xsdk_texture_swizzle(XsdkTexture *, const uint32_t *) {}
void xsdk_texture_free(XsdkTexture *texture) { g_textures.erase(texture); }
NTSTATUS xsdk_texture_create(XsdkTexture *texture, uint32_t, uint32_t width, uint32_t height, uint32_t, bool)
{
	Image &img = g_textures[texture];
	img.w = (int)width;
	img.h = (int)height;
	img.rgba.assign((size_t)width * height * 4, 0);
	texture->width = width;
	texture->height = height;
	return 0;
}
// dash.cpp sobe bytes A, B, G, R (a palavra 0xAABBGGRR do ImGui); aqui eles voltam a ser RGBA.
void xsdk_texture_upload(XsdkTexture *texture, uint32_t, const void *data, uint32_t)
{
	Image &img = g_textures[texture];
	const uint8_t *src = (const uint8_t *)data;
	for (size_t i = 0; i < img.rgba.size(); i += 4) {
		img.rgba[i + 0] = src[i + 3];
		img.rgba[i + 1] = src[i + 2];
		img.rgba[i + 2] = src[i + 1];
		img.rgba[i + 3] = src[i + 0];
	}
}
}

bool ImGui_ImplXsdk_Init() { return true; }
void ImGui_ImplXsdk_Shutdown() {}
void ImGui_ImplXsdk_NewFrame(uint32_t, uint32_t) {}
void ImGui_ImplXsdk_RenderDrawData(ImDrawData *) {}
void ImGui_ImplXsdk_ProcessGamepad(const INPUT_GAMEPAD *) {}

// --- rasterizador ----------------------------------------------------------------------

// Texturas do proprio ImGui (o atlas das fontes), que o backend do console sobe sozinho.
static void update_imgui_textures(ImDrawData *data)
{
	if (!data->Textures)
		return;
	for (ImTextureData *tex : *data->Textures) {
		if (tex->Status == ImTextureStatus_WantDestroy) {
			g_textures.erase(tex);
			tex->SetTexID(ImTextureID_Invalid);
			tex->SetStatus(ImTextureStatus_Destroyed);
			continue;
		}
		if (tex->Status != ImTextureStatus_WantCreate && tex->Status != ImTextureStatus_WantUpdates)
			continue;
		Image &img = g_textures[tex];
		img.w = tex->Width;
		img.h = tex->Height;
		img.rgba.resize((size_t)img.w * img.h * 4);
		for (int y = 0; y < img.h; y++)
			for (int x = 0; x < img.w; x++) {
				uint8_t *dst = &img.rgba[((size_t)y * img.w + x) * 4];
				if (tex->Format == ImTextureFormat_Alpha8) {
					dst[0] = dst[1] = dst[2] = 255;
					dst[3] = *(const uint8_t *)tex->GetPixelsAt(x, y);
				} else {
					memcpy(dst, tex->GetPixelsAt(x, y), 4);
				}
			}
		tex->SetTexID((ImTextureID)(intptr_t)tex);
		tex->SetStatus(ImTextureStatus_OK);
	}
}

static void render(ImDrawData *data, const char *path)
{
	update_imgui_textures(data);
	const int w = (int)g_width, h = (int)g_height;
	std::vector<float> px((size_t)w * h * 4, 0);
	for (size_t i = 3; i < px.size(); i += 4)
		px[i] = 1; // fundo preto opaco
	RasterTarget f = { w, h, px.data() };
	RasterTexture tex;
	for (const ImDrawList *dl : data->CmdLists)
		raster_draw_list(f, dl, [&](const ImDrawCmd &cmd) -> const RasterTexture * {
			auto it = g_textures.find((const void *)(intptr_t)cmd.GetTexID());
			if (it == g_textures.end())
				return nullptr;
			tex = { it->second.w, it->second.h, it->second.rgba.data(), 4 };
			return &tex;
		});
	std::vector<uint8_t> out((size_t)w * h * 4);
	raster_to_rgba8(f, out.data());
	stbi_write_png(path, w, h, 4, out.data(), w * 4);
	printf("gravado %s\n", path);
}

// --- dados de exemplo ------------------------------------------------------------------

// Uma "capa" gerada: degrade com uma faixa, para ver onde as capas caem.
static void fake_cover(app_image *img, int seed)
{
	img->width = img->height = 64;
	img->rgba = (uint8_t *)malloc(64 * 64 * 4);
	static const int colors[][3] = { { 200, 60, 40 }, { 40, 90, 200 }, { 230, 180, 30 }, { 120, 40, 160 },
		{ 30, 150, 110 }, { 60, 60, 70 } };
	const int *c = colors[seed % 6];
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			uint8_t *p = img->rgba + (y * 64 + x) * 4;
			const float k = 0.6f + 0.4f * (float)(64 - y) / 64.0f;
			const bool band = y > 40 && y < 50;
			p[0] = (uint8_t)(band ? 245 : c[0] * k);
			p[1] = (uint8_t)(band ? 245 : c[1] * k);
			p[2] = (uint8_t)(band ? 245 : c[2] * k);
			p[3] = 255;
		}
}

// Um "avatar-body.png" de exemplo, 150x300 com fundo transparente, no lugar do da conta.
static void fake_avatar(app_image *img, int shirt_r, int shirt_g, int shirt_b)
{
	img->width = 150;
	img->height = 300;
	img->rgba = (uint8_t *)calloc(150 * 300, 4);
	auto put = [&](int x, int y, int r, int g, int b) {
		if (x < 0 || y < 0 || x >= 150 || y >= 300)
			return;
		uint8_t *p = img->rgba + (y * 150 + x) * 4;
		p[0] = (uint8_t)r, p[1] = (uint8_t)g, p[2] = (uint8_t)b, p[3] = 255;
	};
	auto rect = [&](int x0, int y0, int x1, int y1, int r, int g, int b) {
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++) {
				const float k = 1.0f - 0.35f * (float)(y - y0) / (float)(y1 - y0 + 1);
				put(x, y, (int)(r * k), (int)(g * k), (int)(b * k));
			}
	};
	auto disc = [&](int cx, int cy, int rad, int r, int g, int b) {
		for (int y = cy - rad; y <= cy + rad; y++)
			for (int x = cx - rad; x <= cx + rad; x++)
				if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad)
					put(x, y, r, g, b);
	};
	rect(56, 180, 73, 282, 40, 60, 110);
	rect(78, 180, 95, 282, 40, 60, 110);
	rect(50, 280, 75, 294, 230, 230, 235);
	rect(76, 280, 101, 294, 230, 230, 235);
	rect(28, 96, 44, 170, shirt_r, shirt_g, shirt_b);
	rect(106, 96, 122, 170, shirt_r, shirt_g, shirt_b);
	disc(36, 176, 9, 240, 200, 170);
	disc(114, 176, 9, 240, 200, 170);
	rect(44, 88, 106, 184, shirt_r, shirt_g, shirt_b);
	disc(75, 52, 36, 240, 200, 170);
	for (int y = 14; y < 40; y++)
		for (int x = 36; x < 114; x++)
			if ((x - 75) * (x - 75) + (y - 46) * (y - 46) <= 38 * 38)
				put(x, y, 90, 55, 30);
	disc(62, 56, 4, 30, 30, 40);
	disc(88, 56, 4, 30, 30, 40);
	rect(64, 72, 86, 75, 170, 80, 70);
}

static void fake_data()
{
	g_app.session = SESSION_SIGNED_IN;
	snprintf(g_app.gamertag, sizeof g_app.gamertag, "Rodrigo");
	snprintf(g_app.server, sizeof g_app.server, "192.168.0.10:8360");
	static const char *const games[] = { "Halo 3", "Gears of War 2", "Forza Motorsport 3", "Minecraft: Xbox 360 Edition",
		"Red Dead Redemption", "Castle Crashers", "Fable II" };
	static const char *const kinds[] = { "god", "god", "god", "stfs", "god", "stfs", "god" };
	for (int i = 0; i < 7; i++) {
		store_item &s = g_app.store[i];
		s.id = i + 1;
		s.size = (int64_t)(i * 1.3 + 0.4) * 1024LL * 1024 * 1024 + 300LL * 1024 * 1024;
		snprintf(s.name, sizeof s.name, "%s", games[i]);
		snprintf(s.kind, sizeof s.kind, "%s", kinds[i]);
		snprintf(s.title_id, sizeof s.title_id, "4D5307%02X", i);
		s.installable = true;
		s.shows_in_dash = true;
		fake_cover(&s.image, i);
		s.art = ART_READY;
	}
	g_app.store_count = 7;
	g_app.store_loaded = true;
	static const char *const friends[] = { "MasterChief", "Dom Santiago", "Ana", "Kratos BR" };
	for (int i = 0; i < 4; i++) {
		friend_item &f = g_app.friends[i];
		snprintf(f.gamertag, sizeof f.gamertag, "%s", friends[i]);
		f.online = i % 2 == 0;
		if (f.online)
			snprintf(f.title_name, sizeof f.title_name, "%s", i == 0 ? "Halo 3" : "EPTX Live");
		// Os tres jeitos: avatar do editor, PNG mandado de fora e nenhum (gamerpic).
		static const avatar_look looks[] = { { { 3, 2, 0, 3, 1, 0, 2, 4, 1, 0, 0, 1, 1, 2, 2, 0 } },
			{ { 0, 4, 4, 1, 0, 3, 0, 0, 9, 2, 6, 2, 2, 0, 0, 0 } } };
		if (i == 0 || i == 2) {
			f.has_look = true;
			f.look = looks[i / 2];
			f.avatar_updated = 1;
			f.art = ART_FAILED;
		} else if (i == 1) {
			fake_avatar(&f.image, 200, 60, 50);
			f.avatar_updated = 1;
			f.art = ART_READY;
		} else {
			// Um "GIF do 360sona" de mentira: quatro quadros de capa, em sequencia.
			app_image frame[4];
			for (int k = 0; k < 4; k++)
				fake_cover(&frame[k], k + 1);
			f.image.width = f.image.height = 64;
			f.image.frames = 4;
			f.image.rgba = (uint8_t *)malloc(64 * 64 * 4 * 4);
			f.image.delays = (uint16_t *)malloc(4 * sizeof(uint16_t));
			for (int k = 0; k < 4; k++) {
				memcpy(f.image.rgba + k * 64 * 64 * 4, frame[k].rgba, 64 * 64 * 4);
				free(frame[k].rgba);
				f.image.delays[k] = 100;
			}
			f.avatar_gif = true;
			f.avatar_updated = 1;
			f.art = ART_READY;
		}
	}
	g_app.has_look = true;
	memcpy(g_app.look.v, AVATAR_DEFAULT_LOOK, sizeof g_app.look.v);
	g_app.avatar_updated = 1;
	g_app.avatar_art = ART_FAILED;
	g_app.friend_count = 4;
	g_app.friends_loaded = true;
	for (int i = 0; i < 3; i++) {
		library_item &l = g_app.library[i];
		snprintf(l.name, sizeof l.name, "%s", games[i]);
		snprintf(l.title_id, sizeof l.title_id, "4D5307%02X", i);
		l.content_type = 0x7000;
		fake_cover(&l.image, i + 3);
		l.art = ART_READY;
	}
	g_app.library_count = 3;
	g_app.library_loaded = true;
	g_app.installing = true;
	snprintf(g_app.install_name, sizeof g_app.install_name, "Red Dead Redemption");
	g_app.install_total = 7LL * 1024 * 1024 * 1024;
	g_app.install_done = g_app.install_total * 45 / 100;
	g_app.install_file = 12;
	g_app.install_files = 46;
}

static void run_frames(int count, float dt)
{
	INPUT_GAMEPAD pad = {};
	for (int i = 0; i < count; i++) {
		g_ui.time += dt;
		ImGui::NewFrame();
		frame(pad, false, dt);
		ImGui::Render();
		update_imgui_textures(ImGui::GetDrawData());
	}
}

static void shot(const char *path)
{
	ImGui::NewFrame();
	frame(INPUT_GAMEPAD{}, false, 0);
	ImGui::Render();
	render(ImGui::GetDrawData(), path);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char path[512];
	g_width = 1280;
	g_height = 720;
	g_sx = g_sy = 1;

	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(g_width, g_height);
	io.DeltaTime = 1.0f / 60;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	g_regular = add_font(font_regular, font_regular_size);
	g_semibold = add_font(font_semibold, font_semibold_size);
	g_light = add_font(font_light, font_light_size);

	fake_data();
	g_ui.time = 3.0f;
	run_frames(90, 1.0f / 60);
	snprintf(path, sizeof path, "%s/1-meu-eptx.png", dir);
	shot(path);

	g_ui.tile[CHANNEL_MY] = 1;
	run_frames(90, 1.0f / 60);
	snprintf(path, sizeof path, "%s/2-biblioteca-focada.png", dir);
	shot(path);

	g_ui.channel = CHANNEL_STORE;
	run_frames(9, 1.0f / 60);
	snprintf(path, sizeof path, "%s/3-trocando-canal.png", dir);
	shot(path);
	run_frames(90, 1.0f / 60);
	snprintf(path, sizeof path, "%s/4-loja.png", dir);
	shot(path);

	g_ui.view = VIEW_STORE_ITEM;
	g_ui.store_index = 0;
	g_ui.menu = 0;
	run_frames(60, 1.0f / 60);
	snprintf(path, sizeof path, "%s/5-detalhe-do-jogo.png", dir);
	shot(path);

	g_ui.view = VIEW_GUIDE;
	g_ui.menu = 4;
	run_frames(60, 1.0f / 60);
	snprintf(path, sizeof path, "%s/8-guide.png", dir);
	shot(path);

	g_ui.view = VIEW_PROFILE;
	g_ui.menu = 0;
	run_frames(60, 1.0f / 60);
	snprintf(path, sizeof path, "%s/9-perfil.png", dir);
	shot(path);

	open_avatar_editor();
	g_ui.edit_field = LOOK_HAIR;
	run_frames(60, 1.0f / 60);
	snprintf(path, sizeof path, "%s/10-editor-de-avatar.png", dir);
	shot(path);

	// Uma volta por varias opcoes, para conferir cada parte desenhada.
	static const avatar_look variants[] = { { { 5, 5, 0, 3, 1, 0, 3, 2, 5, 1, 2, 1, 1, 2, 2, 1 } },
		{ { 2, 3, 3, 2, 3, 2, 0, 3, 6, 2, 7, 2, 6, 0, 0, 3 } },
		{ { 4, 6, 7, 0, 2, 3, 1, 4, 1, 0, 3, 0, 4, 1, 1, 2 } },
		{ { 0, 0, 6, 1, 0, 0, 3, 1, 2, 1, 5, 1, 1, 2, 0, 0 } } };
	for (int v = 0; v < 4; v++) {
		g_ui.edit_look = variants[v];
		g_ui.edit_field = v * 4;
		run_frames(2, 1.0f / 60);
		snprintf(path, sizeof path, "%s/11-editor-variacao-%d.png", dir, v + 1);
		shot(path);
	}

	// O corpo do avatar padrao, igual ao que o editor manda para o servidor.
	if (argc > 2) {
		int len = 0;
		avatar_look standard;
		memcpy(standard.v, AVATAR_DEFAULT_LOOK, sizeof standard.v);
		uint8_t *png = look_png(standard, &len);
		FILE *out = png ? fopen(argv[2], "wb") : nullptr;
		if (!out || fwrite(png, 1, (size_t)len, out) != (size_t)len) {
			fprintf(stderr, "nao gravei %s\n", argv[2]);
			return 1;
		}
		fclose(out);
		free(png);
		printf("gravado %s (%d bytes)\n", argv[2], len);
	}

	g_ui.view = VIEW_HOME;
	g_ui.channel = CHANNEL_FRIENDS;
	run_frames(90, 1.0f / 60);
	snprintf(path, sizeof path, "%s/6-amigos.png", dir);
	shot(path);

	// O amigo do GIF escolhido: a animacao anda (Kratos BR, o ultimo, quadro 1 depois de 0,15 s).
	g_ui.tile[CHANNEL_FRIENDS] = 3;
	run_frames(70, 1.0f / 60);
	snprintf(path, sizeof path, "%s/12-amigo-gif-rodando.png", dir);
	shot(path);
	printf("quadro do gif do amigo: %d de %d\n", g_friend_art[3].frame, g_friend_art[3].frame_count);
	g_ui.tile[CHANNEL_FRIENDS] = 0;
	run_frames(2, 1.0f / 60);
	printf("quadro do gif sem foco: %d\n", g_friend_art[3].frame);

	g_app.session = SESSION_SIGNED_OUT;
	g_app.installing = false;
	g_ui.channel = CHANNEL_MY;
	g_ui.tile[CHANNEL_MY] = 0;
	run_frames(90, 1.0f / 60);
	snprintf(path, sizeof path, "%s/7-sem-login.png", dir);
	shot(path);
	return 0;
}
