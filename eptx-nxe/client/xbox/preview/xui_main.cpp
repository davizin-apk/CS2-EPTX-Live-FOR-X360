// Confere o engine XUI sozinho: desenha uma cena de um .xui do FSD, do jeito que esta
// no arquivo, e grava um PNG para comparar com o preview.png da skin.
//
//   xui_main <pasta da skin> <saida.png> [arquivo.xui] [Id da cena] [segundos]

#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "../xui.h"
#include "../raster.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

extern "C" {
extern const unsigned char font_regular[];
extern const unsigned int font_regular_size;
}

struct Image {
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
};
static std::map<const void *, Image> g_textures;
static std::map<std::string, Image *> g_by_path;
static std::string g_skin;
static ImFont *g_font;

static bool skin_texture(const char *path, xui::Texture *out)
{
	auto it = g_by_path.find(path);
	if (it == g_by_path.end()) {
		std::string full = g_skin + "/" + path;
		for (char &c : full)
			if (c == '\\')
				c = '/';
		int w, h, n;
		uint8_t *px = stbi_load(full.c_str(), &w, &h, &n, 4);
		Image *img = nullptr;
		if (px) {
			img = new Image;
			img->w = w, img->h = h;
			img->rgba.assign(px, px + (size_t)w * h * 4);
			stbi_image_free(px);
			g_textures[img] = *img;
		} else {
			fprintf(stderr, "sem imagem: %s\n", full.c_str());
		}
		it = g_by_path.emplace(path, img).first;
	}
	if (!it->second)
		return false;
	out->id = (ImTextureID)(intptr_t)it->second;
	out->w = (float)it->second->w;
	out->h = (float)it->second->h;
	return true;
}

static ImFont *skin_font(const char *) { return g_font; }

static void update_imgui_textures(ImDrawData *data)
{
	if (!data->Textures)
		return;
	for (ImTextureData *tex : *data->Textures) {
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

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "uso: xui_main <skin> <saida.png> [arquivo.xui] [cena] [segundos]\n");
		return 2;
	}
	g_skin = argv[1];
	const char *scene_file = argc > 3 ? argv[3] : "main.xui";
	const char *scene_id = argc > 4 ? argv[4] : nullptr;
	const float seconds = argc > 5 ? strtof(argv[5], nullptr) : 1.0f;

	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(1280, 720);
	io.DeltaTime = 1.0f / 60;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	ImFontConfig config;
	config.FontDataOwnedByAtlas = false;
	g_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(font_regular), (int)font_regular_size, 20, &config);

	xui::g_host.texture = skin_texture;
	xui::g_host.font = skin_font;
	if (const char *v = getenv("XUI_CAMERA"))
		sscanf(v, "%f,%f,%f,%f", &xui::g_host.vanish_x, &xui::g_host.vanish_y, &xui::g_host.focal,
			&xui::g_host.font_scale);

	xui::File skin, scene;
	if (!xui::load(skin, (g_skin + "/skin.xui").c_str()) || !xui::load(scene, (g_skin + "/" + scene_file).c_str())) {
		fprintf(stderr, "nao li a skin ou a cena\n");
		return 1;
	}
	printf("skin: %zu elementos; cena: %zu elementos\n", skin.all.size(), scene.all.size());
	const xui::Def *def = scene_id ? scene.find(scene_id) : scene.root;
	if (!def) {
		fprintf(stderr, "cena %s nao existe\n", scene_id);
		return 1;
	}
	xui::Node *root = xui::instantiate(def, &skin);
	// XUI_STATE=Focus poe todo controle nesse estado (para ver o foco dos botoes de lista).
	if (const char *state = getenv("XUI_STATE")) {
		std::vector<xui::Node *> stack{ root };
		while (!stack.empty()) {
			xui::Node *n = stack.back();
			stack.pop_back();
			if (n->visual)
				xui::play_visual(n, state);
			for (xui::Node *c : n->children)
				stack.push_back(c);
		}
	}
	const int frames = (int)(seconds * 60);
	for (int i = 0; i <= frames; i++) {
		xui::update(root, i ? 1.0f / 60 : 0);
		ImGui::NewFrame();
		xui::draw(ImGui::GetBackgroundDrawList(), root);
		ImGui::Render();
		update_imgui_textures(ImGui::GetDrawData());
	}

	ImDrawData *data = ImGui::GetDrawData();
	std::vector<float> px((size_t)1280 * 720 * 4, 0);
	for (size_t i = 3; i < px.size(); i += 4)
		px[i] = 1;
	RasterTarget f = { 1280, 720, px.data() };
	RasterTexture tex;
	for (const ImDrawList *dl : data->CmdLists)
		raster_draw_list(f, dl, [&](const ImDrawCmd &cmd) -> const RasterTexture * {
			auto it = g_textures.find((const void *)(intptr_t)cmd.GetTexID());
			if (it == g_textures.end())
				return nullptr;
			tex = { it->second.w, it->second.h, it->second.rgba.data(), 4 };
			return &tex;
		});
	std::vector<uint8_t> out((size_t)1280 * 720 * 4);
	raster_to_rgba8(f, out.data());
	stbi_write_png(argv[2], 1280, 720, 4, out.data(), 1280 * 4);
	printf("gravado %s\n", argv[2]);
	delete root;
	return 0;
}
