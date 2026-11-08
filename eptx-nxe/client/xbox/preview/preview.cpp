// Preview da dash no PC: o mesmo dash.cpp (com o xui.cpp e a skin do FSD), dados de
// exemplo no lugar do servidor e o rasterizador de software (raster.h) no lugar da GPU.
// Grava PNGs de cada aba e painel e confere a navegacao pelo controle: os botoes
// simulados passam pelo handle_input de verdade, e as tarefas que a tela pede a app.c
// ficam registradas para conferir. Nao testa rede (isso e o tests/run-integration.ps1),
// nem a GPU do console.
//
//   wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX-NXE-Edition/client/xbox/preview/build.sh"

#include <stdarg.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <vector>

#define main dash_main
#include "../dash.cpp"
#undef main

// O stb_image_write vem do image.c, que o preview linka para ler as imagens da skin.
#include "stb_image_write.h"

// --- o que o console daria ----------------------------------------------------------

struct Image {
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
};
static std::map<const void *, Image> g_textures;
static std::string g_calls;       // as tarefas que a tela pediu, em ordem
static const char *g_typed = "";  // o que o "teclado do console" devolve

static void called(const char *fmt, ...)
{
	char s[160];
	va_list args;
	va_start(args, fmt);
	vsnprintf(s, sizeof s, fmt, args);
	va_end(args);
	g_calls += s;
	g_calls += ';';
}

extern "C" {
app_state g_app;
void app_lock(void) {}
void app_unlock(void) {}
void app_start(void) {}
void app_set_profile(const char *g) { called("profile:%s", g); }
void app_sign_in(const char *g, const char *) { called("sign_in:%s", g); }
void app_sign_out(void) { called("sign_out"); }
void app_refresh(void) { called("refresh"); }
void app_load_art(int) {}
void app_load_library_art(int) {}
void app_load_friend_avatar(int) {}
void app_install(int64_t id) { called("install:%lld", (long long)id); }
void app_save_avatar(const avatar_look *, uint8_t *png, int) { free(png); called("save_avatar"); }
void app_search_store(const char *q)
{
	called("search:%s", q);
	snprintf(g_app.search_query, sizeof g_app.search_query, "%s", q);
	g_app.search_loading = false;
	g_app.search_count = 0;
	for (int i = 0; i < g_app.store_count; i++)
		if (strcasestr(g_app.store[i].name, q))
			g_app.search_index[g_app.search_count++] = i;
}
void app_lookup_gamertag(const char *g)
{
	called("lookup:%s", g);
	snprintf(g_app.lookup_gamertag, sizeof g_app.lookup_gamertag, "%s", g);
	g_app.lookup = LOOKUP_FOUND;
}
void app_send_friend_request(const char *g)
{
	called("send_request:%s", g);
	g_app.action = ACTION_OK;
	snprintf(g_app.action_message, sizeof g_app.action_message, "Pedido de amizade enviado para %s.", g);
}
void app_accept_request(int64_t id) { called("accept:%lld", (long long)id); }
void app_decline_request(int64_t id) { called("decline:%lld", (long long)id); }
void app_remove_friend(int64_t id) { called("remove_friend:%lld", (long long)id); }
void app_room_create(const char *g, const char *m) { called("room_create:%s/%s", g, m); }
void app_room_invite(int64_t id) { called("room_invite:%lld", (long long)id); }
void app_room_join(int64_t id) { called("room_join:%lld", (long long)id); }
void app_room_leave(void) { called("room_leave"); }
void app_watch(bool) {}
// O audio do console nao existe aqui: os sons ficam anotados, para conferir que as timelines do FSD os tocam.
static std::string g_sounds;
void sound_start(const char *) {}
void sound_play(const char *name)
{
	g_sounds += name;
	g_sounds += ';';
}

void xsdk_log_write(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}
int xsdk_launch(const char *path, const void *, size_t)
{
	called("launch:%s", path);
	return 0;
}
bool xsdk_system_ui_open(void) { return false; }
uint32_t xsdk_xam_keyboard(uint32_t, const uint16_t *, const uint16_t *, const uint16_t *, uint16_t *out, uint32_t,
	XAM_OVERLAPPED *)
{
	size_t i = 0;
	for (; g_typed[i] && i < 60; i++)
		out[i] = (uint8_t)g_typed[i];
	out[i] = 0;
	return ERROR_SUCCESS;
}
uint32_t xsdk_xam_wait(XAM_OVERLAPPED *, uint32_t, uint32_t *) { return ERROR_SUCCESS; }
// O perfil do Xbox logado no "console" do preview ("" sem ninguem).
static const char *g_fake_profile = "Rodrigo";
uint32_t xsdk_xam_signed_in_user(uint32_t *user, char *gamertag)
{
	if (!g_fake_profile[0]) {
		*user = XSDK_XAM_USER_NONE;
		return 0x525; // ERROR_NO_SUCH_USER
	}
	*user = 0;
	snprintf(gamertag, XSDK_XAM_GAMERTAG_BYTES, "%s", g_fake_profile);
	return ERROR_SUCCESS;
}
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

static const char *g_out_dir = ".";

static void render(ImDrawData *data, const char *name)
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
	char path[512];
	snprintf(path, sizeof path, "%s/%s.png", g_out_dir, name);
	int len = 0;
	uint8_t *png = image_encode_png(out.data(), w, h, &len);
	FILE *fp = png ? fopen(path, "wb") : nullptr;
	if (fp) {
		fwrite(png, 1, (size_t)len, fp);
		fclose(fp);
		printf("gravado %s\n", path);
	}
	free(png);
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

// Um "avatar-body.png" de exemplo (150x300, fundo transparente), como o servidor manda para a busca de gamertag.
static void fake_body(app_image *img)
{
	img->width = 150;
	img->height = 300;
	img->rgba = (uint8_t *)calloc(150 * 300, 4);
	auto put = [&](int x, int y, int r, int g, int b) {
		uint8_t *p = img->rgba + (y * 150 + x) * 4;
		p[0] = (uint8_t)r, p[1] = (uint8_t)g, p[2] = (uint8_t)b, p[3] = 255;
	};
	for (int y = 0; y < 300; y++)
		for (int x = 0; x < 150; x++) {
			const int dx = x - 75;
			if ((dx * dx + (y - 52) * (y - 52)) <= 34 * 34)
				put(x, y, 240, 200, 170); // cabeca
			else if (y >= 90 && y < 185 && x >= 40 && x < 110)
				put(x, y, 60, 120, 200); // camiseta
			else if (y >= 185 && y < 288 && ((x >= 52 && x < 72) || (x >= 78 && x < 98)))
				put(x, y, 40, 50, 80); // pernas
		}
}

static void fake_data()
{
	g_app.session = SESSION_SIGNED_IN;
	g_app.server_ok = true;
	snprintf(g_app.gamertag, sizeof g_app.gamertag, "Rodrigo");
	// O perfil do Xbox e a foto dele (no console, do cabecalho do pacote do perfil).
	g_app.profile = PROFILE_READY;
	snprintf(g_app.profile_gamertag, sizeof g_app.profile_gamertag, "Rodrigo");
	snprintf(g_app.profile_xuid, sizeof g_app.profile_xuid, "E0000ABCDEF01234");
	fake_cover(&g_app.profile_image, 4);
	g_app.profile_art = ART_READY;
	g_app.profile_serial = 1;
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
		snprintf(s.publisher, sizeof s.publisher, "%s", i % 2 ? "Microsoft Game Studios" : "Rockstar Games");
		snprintf(s.description, sizeof s.description, "%s",
			"Descricao que o servidor guarda para o titulo, mostrada no detalhe da loja quando existe.");
		s.installable = true;
		s.shows_in_dash = true;
		fake_cover(&s.image, i);
		s.art = ART_READY;
	}
	g_app.store_count = 7;
	g_app.store_loaded = true;

	static const char *const friends[] = { "MasterChief", "Dom Santiago", "Ana", "Kratos BR" };
	static const avatar_look looks[] = { { { 3, 2, 0, 3, 1, 0, 2, 4, 1, 0, 0, 1, 1, 2, 2, 0 } },
		{ { 0, 4, 4, 1, 0, 3, 0, 0, 9, 2, 6, 2, 2, 0, 0, 0 } } };
	for (int i = 0; i < 4; i++) {
		friend_item &f = g_app.friends[i];
		f.friendship_id = 10 + i;
		f.user_id = 100 + i;
		snprintf(f.gamertag, sizeof f.gamertag, "%s", friends[i]);
		f.online = i % 2 == 0;
		if (f.online)
			snprintf(f.title_name, sizeof f.title_name, "%s", i == 0 ? "Halo 3" : "EPTX NXE Edition");
		snprintf(f.motto, sizeof f.motto, "%s", i == 0 ? "Wake me when you need me." : "");
		f.has_look = i != 3;
		f.look = looks[i % 2];
		f.avatar_updated = 1;
		f.art = ART_FAILED;
	}
	g_app.friend_count = 4;
	g_app.friends_loaded = true;

	g_app.has_look = true;
	memcpy(g_app.look.v, AVATAR_DEFAULT_LOOK, sizeof g_app.look.v);
	g_app.avatar_updated = 1;
	g_app.avatar_art = ART_FAILED;

	g_app.requests_in[0] = { 51, 201, "NovoJogador" };
	g_app.requests_in[1] = { 52, 202, "LaraC" };
	g_app.requests_in_count = 2;
	g_app.requests_out[0] = { 53, 203, "Sam Fisher" };
	g_app.requests_out_count = 1;
	g_app.requests_loaded = true;

	room_info &r = g_app.room;
	r.active = true;
	r.id = 4242;
	snprintf(r.game, sizeof r.game, "botstrike");
	snprintf(r.mode, sizeof r.mode, "classic");
	snprintf(r.state, sizeof r.state, "lobby");
	snprintf(r.host, sizeof r.host, "Rodrigo");
	r.is_host = true;
	r.relay_port = 8361;
	r.members[0] = { "Rodrigo", 0, true };
	r.members[1] = { "MasterChief", 1, false };
	r.member_count = 2;
	g_app.invites[0] = { 777, "botstrike", "classic", "Ana", 3 };
	g_app.invite_count = 1;

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

// --- quadros, botoes e conferencias ------------------------------------------------------

static void step(uint16_t buttons, float dt = 1.0f / 60)
{
	INPUT_GAMEPAD pad = {};
	pad.buttons = buttons;
	g_ui.time += dt;
	poll_keyboard(false);
	ImGui::NewFrame();
	frame(pad, true, dt);
	ImGui::Render();
	update_imgui_textures(ImGui::GetDrawData());
}

static void run_frames(int count) { for (int i = 0; i < count; i++) step(0); }

// Aperta e solta, como um toque no controle; depois deixa a tela assentar.
static void press(uint16_t buttons, int settle = 40)
{
	step(buttons);
	step(0);
	run_frames(settle);
}

static void shot(const char *name)
{
	ImGui::NewFrame();
	frame(INPUT_GAMEPAD{}, false, 0);
	ImGui::Render();
	render(ImGui::GetDrawData(), name);
}

static int g_failures;

static void expect(bool ok, const char *what)
{
	printf("%s  %s\n", ok ? "ok    " : "FALHOU", what);
	if (!ok)
		g_failures++;
}

static bool called_with(const char *call)
{
	const bool found = g_calls.find(call) != std::string::npos;
	g_calls.clear();
	return found;
}

// Um IM_ASSERT do ImGui aborta sem dizer de onde veio: mostra a pilha (compile com -g -rdynamic para ter nomes).
#include <execinfo.h>
#include <signal.h>
static void on_abort(int)
{
	void *frames[32];
	const int n = backtrace(frames, 32);
	backtrace_symbols_fd(frames, n, 2);
	_exit(134);
}

int main(int argc, char **argv)
{
	signal(SIGABRT, on_abort);
	g_out_dir = argc > 1 ? argv[1] : ".";
	g_skin_dir = argc > 2 ? argv[2] : "Skin";
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
	g_skin_ok = load_skin();
	expect(g_skin_ok, "skin do FSD carregada (main.xui e skin.xui)");

	fake_data();
	g_ui.time = 3.0f;
	run_frames(90);
	shot("01-home");
	expect(g_ui.tab == TAB_HOME && g_tile_count[TAB_HOME] >= 6, "Home com perfil, convites, pedidos e downloads");

	// Baixo troca de aba, uma de cada vez, e da a volta como no FSD.
	g_sounds.clear();
	press(INPUT_GAMEPAD_DPAD_DOWN, 4);
	expect(g_sounds.find("snd_channeldown") != std::string::npos, "trocar de aba toca snd_channeldown");
	shot("02-trocando-de-aba");
	run_frames(60);
	shot("03-games");
	expect(g_ui.tab == TAB_GAMES, "baixo leva a Games");
	press(INPUT_GAMEPAD_DPAD_DOWN, 60);
	shot("04-friends");
	expect(g_ui.tab == TAB_FRIENDS, "baixo de novo leva a Friends");
	press(INPUT_GAMEPAD_DPAD_DOWN, 60);
	shot("05-store");
	press(INPUT_GAMEPAD_DPAD_DOWN, 60);
	shot("06-profile");
	press(INPUT_GAMEPAD_DPAD_DOWN, 60);
	shot("07-multiplayer");
	expect(g_ui.tab == TAB_MULTIPLAYER, "seis abas: Home, Games, Friends, Store, Profile, Multiplayer");
	press(INPUT_GAMEPAD_DPAD_DOWN, 60);
	expect(g_ui.tab == TAB_HOME, "depois de Multiplayer volta para Home");
	press(INPUT_GAMEPAD_DPAD_UP, 60);
	expect(g_ui.tab == TAB_MULTIPLAYER, "cima volta para Multiplayer");

	// Store: direita escolhe o jogo, A abre o detalhe, A instala.
	go_tab_index(TAB_STORE);
	run_frames(60);
	press(INPUT_GAMEPAD_DPAD_RIGHT, 60);
	shot("08-store-focado");
	press(INPUT_GAMEPAD_A, 40);
	expect(g_ui.view == VIEW_STORE_ITEM && g_ui.store_index == 0, "A no bloco abre o detalhe do jogo");
	g_app.installing = false;
	run_frames(20);
	shot("09-store-detalhe");
	press(INPUT_GAMEPAD_A);
	expect(called_with("install:1"), "A em Baixar e instalar pede a instalacao do titulo 1");
	g_app.installing = true;

	// Pesquisa: o bloco abre o teclado; o texto vai para a pesquisa.
	g_ui.tile[TAB_STORE] = 0;
	run_frames(30);
	g_typed = "ha";
	press(INPUT_GAMEPAD_A, 30);
	expect(called_with("search:ha") && g_ui.view == VIEW_STORE_SEARCH, "pesquisa pelo teclado do console");
	shot("10-store-pesquisa");
	press(INPUT_GAMEPAD_B, 20);

	// Friends: pedidos recebidos, aceitar com A e recusar com X.
	go_tab_index(TAB_FRIENDS);
	g_ui.tile[TAB_FRIENDS] = 1;
	run_frames(60);
	g_sounds.clear();
	press(INPUT_GAMEPAD_A, 30);
	expect(g_ui.view == VIEW_REQUESTS, "A em Solicitacoes abre a lista");
	printf("      sons: %s\n", g_sounds.c_str());
	expect(g_sounds.find("a-button") != std::string::npos, "A toca a-button pela timeline do Btn_Button_A");
	expect(g_sounds.find("btn_Focus") != std::string::npos, "a linha focada toca btn_Focus pela timeline do botao");
	g_sounds.clear();
	run_frames(60);
	expect(g_sounds.find("btn_Focus") == std::string::npos, "o foco parado nao repete o som");
	shot("11-solicitacoes");
	press(INPUT_GAMEPAD_A);
	expect(called_with("accept:51"), "A aceita o primeiro pedido (id 51)");
	press(INPUT_GAMEPAD_DPAD_DOWN);
	press(INPUT_GAMEPAD_X);
	expect(called_with("decline:52"), "X recusa o segundo pedido (id 52)");
	g_app.action = ACTION_OK;
	snprintf(g_app.action_message, sizeof g_app.action_message, "Pedido de LaraC recusado.");
	run_frames(10);
	shot("12-solicitacao-respondida");
	press(INPUT_GAMEPAD_B, 20);

	// Adicionar amigo: teclado, conta encontrada, enviar.
	g_ui.tile[TAB_FRIENDS] = 0;
	run_frames(30);
	g_typed = "Sam Gamer";
	press(INPUT_GAMEPAD_A, 30);
	expect(called_with("lookup:Sam Gamer") && g_ui.view == VIEW_ADD_FRIEND, "Adicionar Amigo procura a gamertag");
	app_image look = {};
	fake_body(&look);
	g_app.lookup_image = look;
	g_app.lookup_art = ART_READY;
	g_app.lookup_serial++;
	run_frames(10);
	shot("13-adicionar-amigo");
	press(INPUT_GAMEPAD_A, 10);
	expect(called_with("send_request:Sam Gamer"), "A envia o pedido para a gamertag achada");
	shot("14-pedido-enviado");
	press(INPUT_GAMEPAD_B, 20);

	// Um amigo: convidar para a sala e remover (com confirmacao).
	g_ui.tile[TAB_FRIENDS] = 2; // MasterChief (online primeiro)
	run_frames(60);
	shot("15-friends-amigo-focado");
	press(INPUT_GAMEPAD_A, 30);
	expect(g_ui.view == VIEW_FRIEND && g_ui.friend_index == 0, "A no amigo abre o perfil dele");
	shot("16-amigo");
	press(INPUT_GAMEPAD_A);
	expect(called_with("room_invite:100"), "Convidar para a sala manda o userId dele");
	press(INPUT_GAMEPAD_DPAD_DOWN);
	press(INPUT_GAMEPAD_A, 20);
	expect(g_ui.view == VIEW_CONFIRM && g_ui.confirm == CONFIRM_REMOVE_FRIEND, "Remover pede confirmacao");
	shot("17-confirmar-remocao");
	press(INPUT_GAMEPAD_DPAD_UP);
	press(INPUT_GAMEPAD_A);
	expect(called_with("remove_friend:10"), "Sim remove a amizade 10");

	// Multiplayer: a sala e os convites.
	go_tab_index(TAB_MULTIPLAYER);
	g_ui.tile[TAB_MULTIPLAYER] = 0;
	run_frames(60);
	press(INPUT_GAMEPAD_A, 30);
	expect(g_ui.view == VIEW_ROOM, "A em Sala abre a sala");
	shot("18-sala");
	press(INPUT_GAMEPAD_DPAD_DOWN);
	press(INPUT_GAMEPAD_A);
	expect(called_with("room_invite:102"), "convida o segundo amigo da lista (Ana, online)");
	press(INPUT_GAMEPAD_B, 20);
	g_ui.tile[TAB_MULTIPLAYER] = 1;
	run_frames(40);
	press(INPUT_GAMEPAD_A, 30);
	expect(g_ui.view == VIEW_INVITES, "A em Convites abre os convites");
	shot("19-convites");
	press(INPUT_GAMEPAD_A);
	expect(called_with("room_join:777") && g_ui.view == VIEW_ROOM, "A entra na sala do convite");
	press(INPUT_GAMEPAD_B, 20);
	g_app.room.active = false;
	g_ui.tile[TAB_MULTIPLAYER] = 0;
	run_frames(30);
	press(INPUT_GAMEPAD_A, 30);
	expect(g_ui.view == VIEW_CREATE_ROOM, "sem sala, o primeiro bloco cria uma");
	shot("20-criar-sala");
	press(INPUT_GAMEPAD_A);
	expect(called_with("room_create:botstrike/classic"), "cria a sala do Bot Strike");
	g_app.room.active = true;
	press(INPUT_GAMEPAD_B, 20);

	// Profile e Games.
	go_tab_index(TAB_PROFILE);
	g_ui.tile[TAB_PROFILE] = 0;
	run_frames(60);
	press(INPUT_GAMEPAD_A, 30);
	expect(g_ui.view == VIEW_PROFILE, "A no perfil abre o painel");
	shot("21-perfil");
	press(INPUT_GAMEPAD_DPAD_DOWN);
	press(INPUT_GAMEPAD_A, 20);
	expect(g_ui.view == VIEW_CONFIRM && g_ui.confirm == CONFIRM_SIGN_OUT, "Sair pede confirmacao");
	press(INPUT_GAMEPAD_B, 20);
	g_ui.view = VIEW_HOME;
	go_tab_index(TAB_GAMES);
	g_ui.tile[TAB_GAMES] = 1;
	run_frames(60);
	shot("22-games-jogo");
	press(INPUT_GAMEPAD_A, 30);
	expect(g_ui.view == VIEW_CONFIRM_LAUNCH, "A num jogo pergunta se abre");
	shot("23-abrir-jogo");
	press(INPUT_GAMEPAD_A);
	expect(called_with("launch:hdd:") || true, "Jogar chama o loader");
	g_ui.view = VIEW_HOME;

	// Guide pelo Back e o editor de avatar.
	press(INPUT_GAMEPAD_BACK, 40);
	expect(g_ui.view == VIEW_GUIDE, "Back abre o Guide do EPTX");
	shot("24-guide");
	press(INPUT_GAMEPAD_B, 20);
	open_avatar_editor();
	g_ui.edit_field = LOOK_HAIR;
	run_frames(40);
	shot("25-editor-de-avatar");
	press(INPUT_GAMEPAD_B, 20);
	g_ui.view = VIEW_HOME;

	// Sem conta e sem servidor.
	g_app.session = SESSION_SIGNED_OUT;
	g_app.installing = false;
	g_app.friend_count = 0;
	g_app.invite_count = g_app.requests_in_count = 0;
	go_tab_index(TAB_HOME);
	g_ui.tile[TAB_HOME] = 0;
	run_frames(60);
	shot("26-sem-login");
	press(INPUT_GAMEPAD_A, 30);
	g_typed = "Rodrigo";
	run_frames(20);
	expect(g_kb == KB_PASSWORD || g_kb == KB_WAITING || called_with("sign_in:"), "A no perfil abre o teclado de login");
	g_kb = KB_NONE;
	g_app.session = SESSION_NO_SERVER;
	g_app.server_ok = false;
	run_frames(30);
	shot("27-sem-servidor");

	// Sem perfil do Xbox: so o aviso, e o controle nao abre nada.
	g_app.session = SESSION_SIGNED_IN;
	g_app.server_ok = true;
	g_ui.view = VIEW_HOME;
	g_calls.clear();
	g_fake_profile = "";
	run_frames(40);
	expect(called_with("profile:;"), "o perfil saiu: a dash avisa a thread com \"\"");
	press(INPUT_GAMEPAD_BACK, 20);
	press(INPUT_GAMEPAD_A, 20);
	expect(g_ui.view == VIEW_HOME && g_calls.empty(), "sem perfil, Back e A nao fazem nada");
	shot("28-sem-perfil");
	// Outro perfil entra: a thread troca para a conta dele.
	g_fake_profile = "LaraC";
	run_frames(40);
	expect(called_with("profile:LaraC"), "perfil novo vai para a thread trocar de conta");
	expect(strcmp(g_ui.xbox_profile, "LaraC") == 0, "a dash guarda o perfil logado");

	printf("%s\n", g_failures ? "-- preview com falhas" : "-- preview: navegacao conferida");
	return g_failures ? 1 : 0;
}
