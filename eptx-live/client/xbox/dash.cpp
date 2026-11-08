// A dash do EPTX, no jeito da NXE de 2008: canais empilhados a esquerda (cima e
// baixo troca de canal, os proximos aparecem menores acima do atual), e em cada
// canal uma fileira de blocos que somem em profundidade para a direita (esquerda
// e direita escolhem o bloco).
//
// Toda a arte e desenhada aqui, com formas e a fonte Selawik (OFL): nada vem da
// dash da Microsoft. O layout segue as medidas de uma captura da NXE em 1280x720.
//
// A tela nunca fala com o servidor; ela pede tarefas a app.c e desenha g_app.

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "imgui.h"
#include "imgui_impl_xsdk.h"

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/net.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"
#include "xsdk/xam.h"

#include "app.h"
#include "image.h"
#include "raster.h"

extern "C" {
extern const unsigned char font_regular[], font_semibold[], font_light[];
extern const unsigned int font_regular_size, font_semibold_size, font_light_size;
}

// 63 KB, o maximo do XAM, em vez de 16: com 16 um download fica em ~0,6 MB/s (xsdk:docs/GUIDE.md, "Sockets").
extern "C" const XsdkNetworkLimits xsdk_network_limits = { 0, 0, 63, 63 };

namespace {

constexpr int SPLASH_MS = 1200;
constexpr int BEGIN_TIMEOUT_MS = 5000;
constexpr float PI = 3.14159265f;
constexpr float TOAST_SECONDS = 6.0f;
constexpr int MAX_UPLOADS_PER_FRAME = 4;

// --- coordenadas: tudo e escrito em 1280x720 e escalado para a tela real ---------

float g_sx = 1, g_sy = 1, g_width = 1280, g_height = 720;
ImVec2 P(float x, float y) { return ImVec2(x * g_sx, y * g_sy); }
float S(float v) { return v * g_sy; }

ImU32 rgba(int r, int g, int b, float a = 1) { return IM_COL32(r, g, b, (int)(a * 255.0f + 0.5f)); }
float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
// Aproxima sem passar do alvo, igual em qualquer taxa de quadros.
float approach(float value, float target, float dt, float speed = 12.0f)
{
	return lerp(value, target, 1.0f - expf(-speed * dt));
}

ImFont *g_regular, *g_semibold, *g_light;

// Texto com uma sombra verde-escura discreta, para ler em cima do fundo claro.
void text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *s, bool shadow = true)
{
	const float alpha = (float)((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
	if (shadow)
		dl->AddText(font, S(size), ImVec2(pos.x + S(1.5f), pos.y + S(1.5f)), rgba(30, 60, 10, 0.35f * alpha), s);
	dl->AddText(font, S(size), pos, col, s);
}

float text_width(ImFont *font, float size, const char *s) { return font->CalcTextSizeA(S(size), FLT_MAX, 0, s).x; }

// Corta com reticencias para caber em max_width pixels.
const char *fit(ImFont *font, float size, const char *s, float max_width)
{
	static char out[160];
	if (text_width(font, size, s) <= max_width)
		return s;
	size_t len = strlen(s);
	if (len >= sizeof out - 4)
		len = sizeof out - 4;
	while (len > 0) {
		do
			len--;
		while (len > 0 && (s[len] & 0xC0) == 0x80); // nao corta no meio de um caractere UTF-8
		memcpy(out, s, len);
		memcpy(out + len, "...", 4);
		if (text_width(font, size, out) <= max_width)
			return out;
	}
	return "...";
}

// --- volume: gradientes, sombras e brilho ---------------------------------------------
//
// O ImGui so pinta cor solida em formas arredondadas. Para ter o degrade e o brilho
// da NXE, a forma e desenhada branca e depois os vertices que ela gerou recebem a
// cor pela posicao (a franja de antisserrilhado mantem o alfa dela).

int mark(ImDrawList *dl) { return dl->VtxBuffer.Size; }

ImU32 mix(ImU32 a, ImU32 b, float t)
{
	ImU32 out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
		out |= (ImU32)(int)(lerp(ca, cb, t) + 0.5f) << shift;
	}
	return out;
}

// Cor bilinear pelos cantos do retangulo a..b, multiplicada pelo alfa que cada vertice ja tinha.
void shade(ImDrawList *dl, int start, ImVec2 a, ImVec2 b, ImU32 tl, ImU32 tr, ImU32 br, ImU32 bl)
{
	const float w = fmaxf(b.x - a.x, 1e-3f), h = fmaxf(b.y - a.y, 1e-3f);
	for (int i = start; i < dl->VtxBuffer.Size; i++) {
		ImDrawVert &v = dl->VtxBuffer[i];
		const float u = clamp01((v.pos.x - a.x) / w), t = clamp01((v.pos.y - a.y) / h);
		const ImU32 c = mix(mix(tl, tr, u), mix(bl, br, u), t);
		const float keep = (float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
		const int alpha = (int)((float)((c >> IM_COL32_A_SHIFT) & 0xFF) * keep + 0.5f);
		v.col = (c & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
	}
}

void shade_vertical(ImDrawList *dl, int start, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom)
{
	shade(dl, start, a, b, top, top, bottom, bottom);
}

void round_gradient(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, ImU32 top, ImU32 bottom,
	ImDrawFlags flags = 0)
{
	const int start = mark(dl);
	dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
	shade_vertical(dl, start, a, b, top, bottom);
}

// Sombra macia: camadas que crescem e somem.
void soft_shadow(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, ImVec2 offset, float spread, float alpha)
{
	constexpr int LAYERS = 6;
	for (int i = LAYERS - 1; i >= 0; i--) {
		const float grow = spread * (float)(i + 1) / LAYERS;
		dl->AddRectFilled(ImVec2(a.x - grow + offset.x, a.y - grow + offset.y),
			ImVec2(b.x + grow + offset.x, b.y + grow + offset.y), rgba(0, 0, 0, alpha / LAYERS), rounding + grow);
	}
}

// Gira em torno de pivot tudo que foi desenhado desde start.
void rotate_since(ImDrawList *dl, int start, ImVec2 pivot, float radians)
{
	const float c = cosf(radians), s = sinf(radians);
	for (int i = start; i < dl->VtxBuffer.Size; i++) {
		ImVec2 &p = dl->VtxBuffer[i].pos;
		const float x = p.x - pivot.x, y = p.y - pivot.y;
		p = ImVec2(pivot.x + x * c - y * s, pivot.y + x * s + y * c);
	}
}

// A barra de selecao verde e brilhante, com sombra embaixo e reflexo na metade de cima.
void aero_bar(ImDrawList *dl, ImVec2 a, ImVec2 b, float alpha)
{
	const float r = S(5);
	soft_shadow(dl, a, b, r, ImVec2(0, S(3)), S(5), 0.55f * alpha);
	dl->AddRectFilled(ImVec2(a.x - S(1), a.y - S(1)), ImVec2(b.x + S(1), b.y + S(1)), rgba(36, 80, 4, alpha), r + S(1));
	round_gradient(dl, a, b, r, rgba(170, 220, 50, alpha), rgba(70, 140, 8, alpha));
	const float mid = a.y + (b.y - a.y) * 0.5f;
	round_gradient(dl, ImVec2(a.x + S(1), a.y + S(1)), ImVec2(b.x - S(1), mid), r, rgba(255, 255, 255, 0.55f * alpha),
		rgba(255, 255, 255, 0.12f * alpha), ImDrawFlags_RoundCornersTop);
	dl->AddLine(ImVec2(a.x + r, a.y + S(1.5f)), ImVec2(b.x - r, a.y + S(1.5f)), rgba(255, 255, 255, 0.75f * alpha), S(1));
	dl->AddLine(ImVec2(a.x + r, b.y - S(2)), ImVec2(b.x - r, b.y - S(2)), rgba(200, 255, 120, 0.35f * alpha), S(1));
}

// Botao do controle em esfera brilhante, como os da NXE.
void orb(ImDrawList *dl, ImVec2 c, float r, int red, int green, int blue, const char *letter, float alpha = 1)
{
	dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.18f), r * 1.05f, rgba(0, 0, 0, 0.25f * alpha), 32);
	dl->AddCircleFilled(c, r, rgba(red / 3, green / 3, blue / 3, alpha), 32);
	int start = mark(dl);
	dl->AddCircleFilled(c, r * 0.88f, IM_COL32_WHITE, 32);
	shade_vertical(dl, start, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
		rgba(red + 70 > 255 ? 255 : red + 70, green + 70 > 255 ? 255 : green + 70, blue + 70 > 255 ? 255 : blue + 70,
			alpha),
		rgba(red * 2 / 3, green * 2 / 3, blue * 2 / 3, alpha));
	start = mark(dl);
	dl->AddEllipseFilled(ImVec2(c.x, c.y - r * 0.4f), ImVec2(r * 0.62f, r * 0.38f), IM_COL32_WHITE);
	shade_vertical(dl, start, ImVec2(c.x, c.y - r * 0.8f), ImVec2(c.x, c.y), rgba(255, 255, 255, 0.75f * alpha),
		rgba(255, 255, 255, 0.08f * alpha));
	if (letter && letter[0]) {
		const float size = r / g_sy * 1.25f;
		const float w = text_width(g_semibold, size, letter);
		const ImVec2 p(c.x - w / 2, c.y - S(size) * 0.6f);
		dl->AddText(g_semibold, S(size), ImVec2(p.x + S(1), p.y + S(1)), rgba(0, 0, 0, 0.45f * alpha), letter);
		dl->AddText(g_semibold, S(size), p, rgba(255, 255, 255, alpha), letter);
	}
}

void human_size(int64_t bytes, char *out, size_t cap)
{
	if (bytes >= 1024LL * 1024 * 1024)
		snprintf(out, cap, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
	else
		snprintf(out, cap, "%.0f MB", (double)bytes / (1024.0 * 1024.0));
}

// --- texturas das capas ------------------------------------------------------------

struct Art {
	XsdkTexture texture;
	bool valid;
	// GIF animado: os quadros ficam na memoria, ja no formato da textura, e o quadro
	// da vez e subido de novo quando a animacao anda.
	uint8_t *frames;
	uint16_t *delays;
	int frame_count, frame, w, h;
	float clock;
};
Art g_store_art[APP_MAX_STORE];
Art g_library_art[APP_MAX_LIBRARY];
Art g_friend_art[APP_MAX_FRIENDS];
Art g_avatar_art; // o avatar da propria conta
uint32_t g_store_serial = ~0u, g_library_serial = ~0u, g_friends_serial = ~0u, g_avatar_serial = ~0u;
int g_uploads_this_frame;

void free_frames(Art &art)
{
	free(art.frames);
	free(art.delays);
	art.frames = nullptr;
	art.delays = nullptr;
	art.frame_count = art.frame = 0;
	art.clock = 0;
}

void free_arts(Art *arts, int count)
{
	bool any = false;
	for (int i = 0; i < count; i++) {
		any |= arts[i].valid;
		free_frames(arts[i]);
	}
	if (!any)
		return;
	// A GPU pode estar desenhando o quadro anterior com elas.
	xsdk_gpu_wait(xsdk_gpu_fence(), 1000);
	for (int i = 0; i < count; i++) {
		if (arts[i].valid)
			xsdk_texture_free(&arts[i].texture);
		arts[i].valid = false;
	}
}

// Mesmo caminho das texturas do proprio ImGui (imgui_impl_xsdk.cpp): palavras 0xAABBGGRR com vermelho e azul
// trocados no swizzle. O stb entrega bytes R, G, B, A, que invertidos viram exatamente essa palavra.
bool upload(Art *art, app_image *image)
{
	const int w = image->width, h = image->height;
	if (!NT_SUCCESS(xsdk_texture_create(&art->texture, XSDK_FORMAT_A8R8G8B8, (uint32_t)w, (uint32_t)h, 1, true)))
		return false;
	// Num GIF, todos os quadros de uma vez: os proximos sobem depois sem conversao.
	const int frames = image->frames > 1 ? image->frames : 1;
	uint32_t *words = (uint32_t *)image->rgba;
	for (int i = 0; i < w * h * frames; i++)
		words[i] = __builtin_bswap32(words[i]);
	static const uint32_t channels[4] = { XSDK_CHANNEL_BLUE, XSDK_CHANNEL_GREEN, XSDK_CHANNEL_RED, XSDK_CHANNEL_ALPHA };
	xsdk_texture_swizzle(&art->texture, channels);
	xsdk_texture_upload(&art->texture, 0, image->rgba, (uint32_t)w * 4);
	xsdk_gpu_invalidate(art->texture.address, art->texture.size_bytes);
	art->valid = true;
	return true;
}

// Chamada com g_app travado: sobe o que a thread de rede ja decodificou.
void take_image(art_state &state, app_image &image, Art &art)
{
	if (state != ART_READY || g_uploads_this_frame >= MAX_UPLOADS_PER_FRAME)
		return;
	g_uploads_this_frame++;
	// Uma imagem nova no lugar de outra (um amigo trocou de avatar): a antiga sai depois da GPU largar.
	if (art.valid) {
		xsdk_gpu_wait(xsdk_gpu_fence(), 1000);
		xsdk_texture_free(&art.texture);
		art.valid = false;
	}
	free_frames(art);
	if (!upload(&art, &image))
		xsdk_log_write("eptx: imagem nao subiu para a GPU");
	if (art.valid && image.frames > 1 && image.delays) {
		// Fica com os quadros do GIF; a textura mostra o primeiro ate o avatar ser escolhido.
		art.frames = image.rgba;
		art.delays = image.delays;
		art.frame_count = image.frames;
		art.w = image.width;
		art.h = image.height;
		image.rgba = nullptr;
		image.delays = nullptr;
	}
	image_free(&image);
	state = art.valid ? ART_SHOWN : ART_FAILED;
}

template <typename Item> void take_art(Item &item, Art &art) { take_image(item.art, item.image, art); }

// O avatar da conta, se ja estiver na GPU.
const Art *my_avatar()
{
	return g_app.avatar_art == ART_SHOWN && g_avatar_art.valid ? &g_avatar_art : nullptr;
}

const Art *friend_avatar(int index)
{
	return g_app.friends[index].art == ART_SHOWN && g_friend_art[index].valid ? &g_friend_art[index] : nullptr;
}

ImTextureID tex_id(const Art &art) { return (ImTextureID)(intptr_t)&art.texture; }

// Roda o GIF enquanto playing; parado, volta para o primeiro quadro.
void animate(Art &art, bool playing, float dt)
{
	if (!art.valid || art.frame_count < 2)
		return;
	int next = art.frame;
	if (!playing) {
		next = 0;
		art.clock = 0;
	} else {
		art.clock += dt * 1000.0f;
		// Um quadro por vez no maximo: se a dash engasgar, a animacao so fica mais lenta.
		if (art.clock >= (float)art.delays[art.frame]) {
			art.clock = fminf(art.clock - (float)art.delays[art.frame], 100.0f);
			next = (art.frame + 1) % art.frame_count;
		}
	}
	if (next == art.frame)
		return;
	art.frame = next;
	const size_t bytes = (size_t)art.w * (size_t)art.h * 4;
	xsdk_texture_upload(&art.texture, 0, art.frames + bytes * (size_t)next, (uint32_t)art.w * 4);
	xsdk_gpu_invalidate(art.texture.address, art.texture.size_bytes);
}

// O avatar do 360sona num quadrinho: a imagem cortada no meio para ficar quadrada.
void avatar_card(ImDrawList *dl, ImVec2 a, float side, const Art &art, float alpha, bool chosen = false)
{
	const ImVec2 b(a.x + side, a.y + side);
	const float w = (float)(art.w ? art.w : (int)art.texture.width), h = (float)(art.h ? art.h : (int)art.texture.height);
	ImVec2 uv0(0, 0), uv1(1, 1);
	if (w > h)
		uv0.x = (1 - h / w) / 2, uv1.x = 1 - uv0.x;
	else if (h > w)
		uv0.y = 0, uv1.y = w / h; // retrato: fica o alto, onde esta o rosto
	dl->AddImage(tex_id(art), a, b, uv0, uv1, rgba(255, 255, 255, alpha));
	dl->AddRect(a, b, chosen ? rgba(190, 240, 80, alpha) : rgba(255, 255, 255, 0.8f * alpha), S(2), 0,
		chosen ? S(3) : S(2));
}

// --- icones, desenhados com formas ----------------------------------------------------

enum Icon { ICON_NONE, ICON_CONTROLLER, ICON_BAG, ICON_FRIENDS, ICON_DOWNLOAD, ICON_HOME, ICON_SERVER, ICON_REFRESH,
	ICON_INFO, ICON_PERSON };

void draw_icon(ImDrawList *dl, Icon icon, ImVec2 c, float u, float alpha)
{
	const ImU32 white = rgba(255, 255, 255, 0.95f * alpha);
	const ImU32 green = rgba(120, 180, 20, alpha);
	switch (icon) {
	case ICON_NONE:
		break;
	case ICON_CONTROLLER:
		dl->AddCircleFilled(ImVec2(c.x - u * 0.72f, c.y + u * 0.25f), u * 0.5f, white, 32);
		dl->AddCircleFilled(ImVec2(c.x + u * 0.72f, c.y + u * 0.25f), u * 0.5f, white, 32);
		dl->AddRectFilled(ImVec2(c.x - u * 1.0f, c.y - u * 0.5f), ImVec2(c.x + u * 1.0f, c.y + u * 0.35f), white, u * 0.4f);
		dl->AddCircleFilled(ImVec2(c.x - u * 0.55f, c.y - u * 0.05f), u * 0.17f, green, 20);
		for (int i = 0; i < 4; i++) {
			const float a = (float)i * PI / 2;
			dl->AddCircleFilled(ImVec2(c.x + u * 0.55f + cosf(a) * u * 0.17f, c.y - u * 0.05f + sinf(a) * u * 0.17f),
				u * 0.07f, green, 12);
		}
		break;
	case ICON_BAG:
		dl->PathArcTo(ImVec2(c.x, c.y - u * 0.35f), u * 0.42f, PI, 2 * PI, 20);
		dl->PathStroke(white, 0, u * 0.16f);
		dl->AddRectFilled(ImVec2(c.x - u * 0.8f, c.y - u * 0.35f), ImVec2(c.x + u * 0.8f, c.y + u * 0.85f), white, u * 0.12f);
		dl->AddRectFilled(ImVec2(c.x - u * 0.8f, c.y - u * 0.1f), ImVec2(c.x + u * 0.8f, c.y), green);
		break;
	case ICON_PERSON:
	case ICON_FRIENDS: {
		const int people = icon == ICON_FRIENDS ? 2 : 1;
		for (int p = 0; p < people; p++) {
			const float x = c.x + (people == 2 ? (p ? u * 0.45f : -u * 0.45f) : 0);
			const float k = people == 2 && p == 0 ? 0.85f : 1.0f;
			dl->AddCircleFilled(ImVec2(x, c.y - u * 0.35f * k), u * 0.34f * k, white, 24);
			dl->PathArcTo(ImVec2(x, c.y + u * 0.75f), u * 0.68f * k, PI, 2 * PI, 24);
			dl->PathFillConvex(white);
		}
		break;
	}
	case ICON_DOWNLOAD:
		dl->AddRectFilled(ImVec2(c.x - u * 0.18f, c.y - u * 0.85f), ImVec2(c.x + u * 0.18f, c.y + u * 0.1f), white);
		dl->AddTriangleFilled(ImVec2(c.x - u * 0.55f, c.y), ImVec2(c.x + u * 0.55f, c.y), ImVec2(c.x, c.y + u * 0.55f),
			white);
		dl->AddRectFilled(ImVec2(c.x - u * 0.8f, c.y + u * 0.7f), ImVec2(c.x + u * 0.8f, c.y + u * 0.88f), white, u * 0.06f);
		break;
	case ICON_HOME:
		dl->AddTriangleFilled(ImVec2(c.x - u, c.y - u * 0.05f), ImVec2(c.x + u, c.y - u * 0.05f), ImVec2(c.x, c.y - u * 0.9f),
			white);
		dl->AddRectFilled(ImVec2(c.x - u * 0.7f, c.y - u * 0.1f), ImVec2(c.x + u * 0.7f, c.y + u * 0.8f), white);
		dl->AddRectFilled(ImVec2(c.x - u * 0.18f, c.y + u * 0.3f), ImVec2(c.x + u * 0.18f, c.y + u * 0.8f), green);
		break;
	case ICON_SERVER:
		for (int i = 0; i < 3; i++) {
			const float y = c.y - u * 0.8f + (float)i * u * 0.58f;
			dl->AddRectFilled(ImVec2(c.x - u * 0.85f, y), ImVec2(c.x + u * 0.85f, y + u * 0.45f), white, u * 0.08f);
			dl->AddCircleFilled(ImVec2(c.x + u * 0.55f, y + u * 0.225f), u * 0.08f, green, 12);
		}
		break;
	case ICON_REFRESH:
		dl->PathArcTo(c, u * 0.7f, -PI * 0.35f, PI * 1.25f, 32);
		dl->PathStroke(white, 0, u * 0.2f);
		dl->AddTriangleFilled(ImVec2(c.x + u * 0.25f, c.y - u * 0.95f), ImVec2(c.x + u * 0.75f, c.y - u * 0.35f),
			ImVec2(c.x + u * 0.95f, c.y - u * 1.0f), white);
		break;
	case ICON_INFO:
		dl->AddCircle(c, u * 0.85f, white, 32, u * 0.14f);
		dl->AddCircleFilled(ImVec2(c.x, c.y - u * 0.42f), u * 0.11f, white, 12);
		dl->AddRectFilled(ImVec2(c.x - u * 0.09f, c.y - u * 0.18f), ImVec2(c.x + u * 0.09f, c.y + u * 0.5f), white);
		break;
	}
}

// Quadrado de "gamerpic": cor tirada do gamertag e a inicial dele.
void gamerpic(ImDrawList *dl, ImVec2 a, float side, const char *gamertag, float alpha)
{
	uint32_t hash = 2166136261u;
	for (const char *p = gamertag; *p; p++)
		hash = (hash ^ (uint8_t)*p) * 16777619u;
	static const int palette[][3] = { { 40, 130, 200 }, { 220, 120, 30 }, { 150, 70, 180 }, { 40, 160, 120 },
		{ 200, 60, 70 }, { 90, 110, 130 } };
	const int *c = palette[hash % (sizeof palette / sizeof palette[0])];
	const ImVec2 b(a.x + side, a.y + side);
	dl->AddRectFilledMultiColor(a, b, rgba(c[0] + 30, c[1] + 30, c[2] + 30, alpha), rgba(c[0], c[1], c[2], alpha),
		rgba(c[0] - 30, c[1] - 30, c[2] - 30, alpha), rgba(c[0] - 10, c[1] - 10, c[2] - 10, alpha));
	dl->AddRect(a, b, rgba(255, 255, 255, 0.8f * alpha), 0, 0, S(2));
	char initial[2] = { gamertag[0] ? (char)(gamertag[0] >= 'a' && gamertag[0] <= 'z' ? gamertag[0] - 32 : gamertag[0])
						: '?',
		0 };
	const float size = side / g_sy * 0.6f;
	const float w = text_width(g_semibold, size, initial);
	dl->AddText(g_semibold, S(size), ImVec2(a.x + (side - w) / 2, a.y + side * 0.15f), rgba(255, 255, 255, alpha),
		initial);
}

// --- o modelo da tela -----------------------------------------------------------------

enum TileKind { TILE_PROFILE, TILE_LIBRARY, TILE_DOWNLOADS, TILE_DASHBOARD, TILE_STORE_ITEM, TILE_STORE_EMPTY,
	TILE_FRIEND, TILE_FRIENDS_EMPTY, TILE_SERVER, TILE_REFRESH, TILE_ABOUT };

struct Tile {
	TileKind kind;
	int index; // na loja, nos amigos
};

enum Channel { CHANNEL_MY, CHANNEL_STORE, CHANNEL_FRIENDS, CHANNEL_SYSTEM, CHANNEL_COUNT };
const char *const CHANNEL_NAMES[CHANNEL_COUNT] = { "Meu EPTX", "Loja de Jogos", "Amigos", "Sistema" };

constexpr int MAX_TILES = APP_MAX_STORE + 1;

int build_tiles(int channel, Tile *tiles)
{
	int n = 0;
	switch (channel) {
	case CHANNEL_MY:
		tiles[n++] = { TILE_PROFILE, 0 };
		tiles[n++] = { TILE_LIBRARY, 0 };
		tiles[n++] = { TILE_DOWNLOADS, 0 };
		tiles[n++] = { TILE_DASHBOARD, 0 };
		break;
	case CHANNEL_STORE:
		for (int i = 0; i < g_app.store_count && n < MAX_TILES; i++)
			tiles[n++] = { TILE_STORE_ITEM, i };
		if (!n)
			tiles[n++] = { TILE_STORE_EMPTY, 0 };
		break;
	case CHANNEL_FRIENDS:
		// Quem esta online primeiro, como na NXE.
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < g_app.friend_count; i++)
				if (g_app.friends[i].online == (pass == 0))
					tiles[n++] = { TILE_FRIEND, i };
		if (!n)
			tiles[n++] = { TILE_FRIENDS_EMPTY, 0 };
		break;
	case CHANNEL_SYSTEM:
		tiles[n++] = { TILE_SERVER, 0 };
		tiles[n++] = { TILE_REFRESH, 0 };
		tiles[n++] = { TILE_ABOUT, 0 };
		break;
	}
	return n;
}

enum View { VIEW_HOME, VIEW_STORE_ITEM, VIEW_LIBRARY, VIEW_PROFILE, VIEW_ABOUT, VIEW_CONFIRM_LAUNCH, VIEW_GUIDE,
	VIEW_AVATAR_EDITOR };

struct UiState {
	int channel = CHANNEL_MY;
	float channel_pos = CHANNEL_MY;
	int tile[CHANNEL_COUNT] = {};
	float tile_pos[CHANNEL_COUNT] = {};
	View view = VIEW_HOME;
	float view_fade = 0;
	int menu = 0;        // item escolhido no painel aberto
	int store_index = 0; // jogo aberto no painel da loja
	int library_index = 0;
	float library_scroll = 0;
	uint32_t seen_message = 0;
	float toast_time = 0;
	char toast[200] = "";
	float time = 0;
	avatar_look edit_look = {}; // o avatar no editor, antes de salvar
	int edit_field = 0;
};
UiState g_ui;

// --- fundo ----------------------------------------------------------------------------

// A borda do chao: sobe da esquerda para a direita com uma barriga no meio, como o palco curvo da NXE.
float horizon(float x) { return 430.0f - 150.0f * (x / 1280.0f) - 30.0f * sinf(PI * x / 1280.0f); }

void draw_background(ImDrawList *dl)
{
	const float W = g_width, H = g_height;
	// Ceu: verde-oliva no alto a esquerda, lima mais viva no centro, quase branco perto do horizonte.
	dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), rgba(98, 150, 20), rgba(150, 198, 36), rgba(222, 234, 196),
		rgba(186, 212, 116));
	// Uma luz difusa atras dos blocos.
	for (int i = 0; i < 8; i++) {
		const float k = (float)(i + 1) / 8.0f;
		dl->AddEllipseFilled(P(780, 300), ImVec2(S(560) * k, S(210) * k), rgba(255, 255, 230, 0.035f), 0, 64);
	}

	// Bolhas desfocadas que andam devagar, so no ceu.
	for (int i = 0; i < 26; i++) {
		const float seed = (float)i * 12.9898f;
		const float fx = fmodf(fabsf(sinf(seed) * 43758.5453f), 1.0f);
		const float fy = fmodf(fabsf(sinf(seed * 1.7f) * 24634.6345f), 1.0f);
		const float r = 14.0f + 70.0f * fmodf(fabsf(sinf(seed * 2.3f) * 9631.17f), 1.0f);
		const float x = fmodf(fx * 1400.0f + g_ui.time * (4.0f + (float)(i % 5)), 1400.0f) - 60.0f;
		const float y = 20.0f + fy * 300.0f + sinf(g_ui.time * 0.3f + seed) * 8.0f;
		if (y > horizon(x) - 20)
			continue;
		dl->AddCircleFilled(P(x, y), S(r), rgba(255, 255, 255, 0.05f + 0.06f * fy), 40);
		if (i % 4 == 0)
			dl->AddCircle(P(x, y), S(r * 1.2f), rgba(255, 255, 255, 0.12f), 48, S(2));
	}

	// O chao: cinza claro na borda, escurecendo para baixo, medido a partir do horizonte de cada coluna.
	constexpr int STEPS = 48;
	for (int i = 0; i <= STEPS; i++) {
		const float x = 1280.0f * (float)i / STEPS;
		dl->PathLineTo(P(x, horizon(x)));
	}
	dl->PathLineTo(P(1280, 720));
	dl->PathLineTo(P(0, 720));
	const int start = mark(dl);
	dl->PathFillConvex(IM_COL32_WHITE);
	for (int i = start; i < dl->VtxBuffer.Size; i++) {
		ImDrawVert &v = dl->VtxBuffer[i];
		const float top = S(horizon(v.pos.x / g_sx));
		const float t = clamp01((v.pos.y - top) / fmaxf(H - top, 1));
		const ImU32 c = mix(rgba(232, 236, 238), rgba(162, 168, 174), powf(t, 0.7f));
		const float keep = (float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
		v.col = (c & ~IM_COL32_A_MASK) | ((ImU32)(int)(255 * keep) << IM_COL32_A_SHIFT);
	}
	// Brilho na borda do palco.
	for (int i = 0; i <= STEPS; i++) {
		const float x = 1280.0f * (float)i / STEPS;
		dl->PathLineTo(P(x, horizon(x)));
	}
	dl->PathStroke(rgba(255, 255, 255, 0.85f), 0, S(2));
	for (int i = 0; i <= STEPS; i++) {
		const float x = 1280.0f * (float)i / STEPS;
		dl->PathLineTo(P(x, horizon(x) - 5));
	}
	dl->PathStroke(rgba(255, 255, 255, 0.18f), 0, S(10));
	// Aneis no canto de baixo, so decoracao.
	for (int i = 0; i < 6; i++)
		dl->AddCircle(P(1210, 670), S(30.0f + (float)i * 26.0f), rgba(120, 128, 136, 0.18f - 0.025f * (float)i), 64,
			S(1.5f));
}

// --- canais -----------------------------------------------------------------------------

// Tamanho, alfa e altura de cada degrau acima do canal atual (0 e o atual), tirados da captura.
const float CHANNEL_SIZE[] = { 46, 31, 23, 19, 16 };
const float CHANNEL_ALPHA[] = { 1, 0.78f, 0.62f, 0.45f, 0.28f };
const float CHANNEL_Y[] = { 160, 120, 86, 58, 34 };
constexpr int CHANNEL_STEPS = sizeof CHANNEL_SIZE / sizeof CHANNEL_SIZE[0];

void draw_channels(ImDrawList *dl)
{
	for (int c = CHANNEL_COUNT - 1; c >= 0; c--) {
		const float d = (float)c - g_ui.channel_pos;
		float size, alpha, y;
		if (d < 0) {
			// O canal que ficou para tras desce e some.
			const float t = clamp01(-d);
			size = CHANNEL_SIZE[0];
			alpha = 1 - t;
			y = CHANNEL_Y[0] + 50 * t;
		} else if (d >= CHANNEL_STEPS - 1) {
			size = CHANNEL_SIZE[CHANNEL_STEPS - 1];
			alpha = CHANNEL_ALPHA[CHANNEL_STEPS - 1] * clamp01((float)CHANNEL_STEPS - d);
			y = CHANNEL_Y[CHANNEL_STEPS - 1] - 22 * (d - (float)(CHANNEL_STEPS - 1));
		} else {
			const int i = (int)d;
			const float t = d - (float)i;
			size = lerp(CHANNEL_SIZE[i], CHANNEL_SIZE[i + 1], t);
			alpha = lerp(CHANNEL_ALPHA[i], CHANNEL_ALPHA[i + 1], t);
			y = lerp(CHANNEL_Y[i], CHANNEL_Y[i + 1], t);
		}
		if (alpha <= 0.01f)
			continue;
		ImFont *font = d < 0.5f ? g_semibold : g_regular;
		text(dl, font, size, P(36, y), rgba(255, 255, 255, alpha), CHANNEL_NAMES[c]);
		if (fabsf(d) < 0.5f) {
			const float k = 1 - fabsf(d) * 2;
			dl->AddRectFilled(P(22, y + size * 0.42f), P(30, y + size * 0.42f + 8), rgba(255, 255, 255, k));
		}
	}
}

// --- blocos ----------------------------------------------------------------------------

// Posicao dos blocos a partir do focado (0), medidas da captura: esquerda, topo, largura, altura.
const float TILE_X[] = { 36, 500, 845, 1048, 1185, 1280 };
const float TILE_TOP[] = { 236, 273, 296, 311, 321, 330 };
const float TILE_W[] = { 464, 345, 203, 137, 95, 60 };
const float TILE_H[] = { 350, 262, 208, 172, 145, 120 };
constexpr int TILE_STEPS = sizeof TILE_X / sizeof TILE_X[0];

struct TileBox {
	ImVec2 a, b;
	float alpha;
	float depth; // 0 no focado, cresce para o fundo
	float scale; // altura relativa ao focado
};

TileBox tile_box(float d)
{
	TileBox box;
	float x, top, w, h;
	if (d < 0) {
		// Saindo pela esquerda.
		x = TILE_X[0] + d * 480;
		top = TILE_TOP[0];
		w = TILE_W[0];
		h = TILE_H[0];
		box.alpha = clamp01(1 + d * 1.4f);
	} else if (d >= TILE_STEPS - 1) {
		x = TILE_X[TILE_STEPS - 1];
		top = TILE_TOP[TILE_STEPS - 1];
		w = TILE_W[TILE_STEPS - 1];
		h = TILE_H[TILE_STEPS - 1];
		box.alpha = 0;
	} else {
		const int i = (int)d;
		const float t = d - (float)i;
		x = lerp(TILE_X[i], TILE_X[i + 1], t);
		top = lerp(TILE_TOP[i], TILE_TOP[i + 1], t);
		w = lerp(TILE_W[i], TILE_W[i + 1], t);
		h = lerp(TILE_H[i], TILE_H[i + 1], t);
		box.alpha = d > TILE_STEPS - 2 ? clamp01((float)(TILE_STEPS - 1) - d) : 1;
	}
	box.a = P(x, top);
	box.b = P(x + w, top + h);
	box.depth = d > 0 ? d : 0;
	box.scale = h / TILE_H[0];
	return box;
}

void tile_panel(ImDrawList *dl, const TileBox &box)
{
	const ImVec2 a = box.a, b = box.b;
	const float w = b.x - a.x, h = b.y - a.y, al = box.alpha;
	const float r = S(8) * box.scale + S(2);
	// Sombra no chao, puxada para a direita e para baixo, e o reflexo do bloco no palco.
	soft_shadow(dl, ImVec2(a.x + S(4), b.y - h * 0.08f), ImVec2(b.x + S(6), b.y + S(6)), r, ImVec2(S(6), S(6)),
		S(16) * box.scale, 0.42f * al);
	round_gradient(dl, ImVec2(a.x, b.y + S(4)), ImVec2(b.x, b.y + h * 0.22f), r, rgba(150, 196, 50, 0.32f * al),
		rgba(150, 196, 50, 0), ImDrawFlags_RoundCornersBottom);
	soft_shadow(dl, a, b, r, ImVec2(S(3), S(5)), S(8) * box.scale, 0.30f * al);

	// O corpo: lima clara no alto a esquerda, verde fechado embaixo.
	int start = mark(dl);
	dl->AddRectFilled(a, b, IM_COL32_WHITE, r);
	shade(dl, start, a, b, rgba(210, 236, 62, al), rgba(186, 222, 36, al), rgba(70, 132, 10, al), rgba(92, 154, 18, al));

	// Circulos do papel de parede da NXE, discretos.
	dl->PushClipRect(ImVec2(a.x + r, a.y + r * 0.5f), ImVec2(b.x - r * 0.5f, b.y - r * 0.5f), true);
	dl->AddCircle(ImVec2(a.x + w * 0.22f, a.y + h * 0.62f), h * 0.34f, rgba(255, 255, 255, 0.13f * al), 64, S(3));
	dl->AddCircle(ImVec2(a.x + w * 0.22f, a.y + h * 0.62f), h * 0.22f, rgba(255, 255, 255, 0.09f * al), 64, S(2));
	dl->AddCircle(ImVec2(b.x - w * 0.12f, a.y + h * 0.25f), h * 0.16f, rgba(255, 255, 255, 0.11f * al), 48, S(2));
	dl->AddCircleFilled(ImVec2(b.x - w * 0.3f, b.y - h * 0.2f), h * 0.09f, rgba(255, 255, 255, 0.07f * al), 32);
	dl->AddCircleFilled(ImVec2(a.x + w * 0.62f, a.y + h * 0.18f), h * 0.05f, rgba(255, 255, 255, 0.08f * al), 24);
	dl->PopClipRect();

	// Brilho de vidro na metade de cima e um friso claro no alto, que dao o volume.
	round_gradient(dl, a, ImVec2(b.x, a.y + h * 0.48f), r, rgba(255, 255, 255, 0.34f * al),
		rgba(255, 255, 255, 0.02f * al), ImDrawFlags_RoundCornersTop);
	dl->AddLine(ImVec2(a.x + r, a.y + S(1.5f)), ImVec2(b.x - r, a.y + S(1.5f)), rgba(255, 255, 210, 0.85f * al), S(1.5f));
	dl->AddLine(ImVec2(a.x + S(1.5f), a.y + r), ImVec2(a.x + S(1.5f), b.y - r), rgba(255, 255, 255, 0.25f * al), S(1));
	dl->AddLine(ImVec2(a.x + r, b.y - S(1.5f)), ImVec2(b.x - r, b.y - S(1.5f)), rgba(30, 70, 0, 0.55f * al), S(2));
	dl->AddRect(a, b, rgba(60, 110, 10, 0.45f * al), r, 0, S(1));
	if (box.depth > 0)
		dl->AddRectFilled(a, b, rgba(20, 50, 0, clamp01(box.depth * 0.06f) * al), r);
}

// Os blocos de tras viram de leve para dentro da tela: a borda direita fica mais baixa que a esquerda.
void perspective_since(ImDrawList *dl, int start, const TileBox &box)
{
	const float k = 0.09f * clamp01(box.depth);
	if (k <= 0)
		return;
	const float w = fmaxf(box.b.x - box.a.x, 1), cy = (box.a.y + box.b.y) * 0.5f;
	for (int i = start; i < dl->VtxBuffer.Size; i++) {
		ImVec2 &p = dl->VtxBuffer[i].pos;
		const float t = clamp01((p.x - box.a.x) / w);
		p.y = cy + (p.y - cy) * (1 - k * t);
	}
}

// Rotulo no canto de baixo, como "Game Library" na captura.
void tile_label(ImDrawList *dl, const TileBox &box, const char *label, const char *sub)
{
	const float w = box.b.x - box.a.x, h = box.b.y - box.a.y;
	const float size = 22 * box.scale + 2;
	const float x = box.a.x + S(14) * box.scale + S(4);
	const float y = box.b.y - h * 0.16f - (sub ? S(size * 0.5f) : 0);
	text(dl, g_regular, size, ImVec2(x, y), rgba(255, 255, 255, box.alpha), fit(g_regular, size, label, w - (x - box.a.x) * 2));
	if (sub)
		text(dl, g_regular, size * 0.7f, ImVec2(x, y + S(size * 1.05f)), rgba(240, 255, 220, 0.85f * box.alpha),
			fit(g_regular, size * 0.7f, sub, w - (x - box.a.x) * 2));
}

void tile_icon(ImDrawList *dl, const TileBox &box, Icon icon)
{
	const float w = box.b.x - box.a.x, h = box.b.y - box.a.y;
	draw_icon(dl, icon, ImVec2(box.a.x + w * 0.5f, box.a.y + h * 0.4f), h * 0.2f, box.alpha);
}

void tile_cover(ImDrawList *dl, const TileBox &box, const Art &art, Icon fallback)
{
	const float w = box.b.x - box.a.x, h = box.b.y - box.a.y;
	if (!art.valid) {
		tile_icon(dl, box, fallback);
		return;
	}
	const float side = fminf(w * 0.6f, h * 0.56f);
	const ImVec2 a(box.a.x + (w - side) / 2, box.a.y + h * 0.1f);
	const ImVec2 b(a.x + side, a.y + side);
	dl->AddRectFilled(ImVec2(a.x + S(4), a.y + S(5)), ImVec2(b.x + S(4), b.y + S(5)), rgba(20, 60, 0, 0.3f * box.alpha));
	dl->AddImage(tex_id(art), a, b, ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, box.alpha));
	dl->AddRect(a, b, rgba(255, 255, 255, 0.7f * box.alpha), 0, 0, S(2));
}

// --- avatar -------------------------------------------------------------------------------
//
// Um boneco proprio, desenhado com formas: os avatares do 360 sao da Microsoft e
// vem do sistema. Com gamertag ele ganha cor (a camiseta sai do gamertag); sem,
// e a silhueta escura que a NXE mostra quando ninguem entrou.

struct AvatarColours {
	ImU32 skin_top, skin_bottom, hair_top, hair_bottom, shirt_top, shirt_bottom, jeans_top, jeans_bottom, shoe;
	bool face;
};

AvatarColours avatar_colours(const char *gamertag, float al)
{
	if (!gamertag) {
		const ImU32 top = rgba(40, 60, 82, al), bottom = rgba(16, 28, 42, al);
		return { top, bottom, top, bottom, top, bottom, top, bottom, bottom, false };
	}
	uint32_t hash = 2166136261u;
	for (const char *p = gamertag; *p; p++)
		hash = (hash ^ (uint8_t)*p) * 16777619u;
	static const int shirts[][3] = { { 30, 120, 220 }, { 220, 60, 50 }, { 250, 170, 30 }, { 40, 170, 90 },
		{ 140, 60, 200 }, { 240, 240, 240 } };
	static const int hairs[][3] = { { 70, 44, 24 }, { 30, 24, 20 }, { 150, 96, 40 }, { 200, 160, 80 } };
	static const int skins[][3] = { { 248, 212, 178 }, { 214, 166, 120 }, { 160, 108, 70 }, { 236, 190, 150 } };
	const int *s = shirts[hash % 6], *h = hairs[(hash >> 8) % 4], *k = skins[(hash >> 16) % 4];
	return { rgba(k[0], k[1], k[2], al), rgba(k[0] * 4 / 5, k[1] * 4 / 5, k[2] * 4 / 5, al),
		rgba(h[0] + 40, h[1] + 30, h[2] + 20, al), rgba(h[0], h[1], h[2], al),
		rgba(s[0] + (255 - s[0]) / 4, s[1] + (255 - s[1]) / 4, s[2] + (255 - s[2]) / 4, al),
		rgba(s[0] * 3 / 5, s[1] * 3 / 5, s[2] * 3 / 5, al), rgba(70, 104, 170, al), rgba(30, 46, 90, al),
		rgba(54, 54, 60, al), true };
}

void avatar_part(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, ImU32 top, ImU32 bottom)
{
	const int start = mark(dl);
	dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding);
	// Luz vinda do alto a esquerda: o lado direito um pouco mais escuro.
	shade(dl, start, a, b, top, mix(top, bottom, 0.3f), bottom, mix(bottom, top, 0.2f));
}

// Braco: retangulo em pe no ombro, girado para fora.
void avatar_arm(ImDrawList *dl, ImVec2 shoulder, float u, float angle, const AvatarColours &c)
{
	const int start = mark(dl);
	avatar_part(dl, ImVec2(shoulder.x - 4 * u, shoulder.y), ImVec2(shoulder.x + 4 * u, shoulder.y + 14 * u), 4 * u,
		c.shirt_top, c.shirt_bottom);
	avatar_part(dl, ImVec2(shoulder.x - 3.3f * u, shoulder.y + 12 * u), ImVec2(shoulder.x + 3.3f * u, shoulder.y + 29 * u),
		3.3f * u, c.skin_top, c.skin_bottom);
	const int hand = mark(dl);
	dl->AddCircleFilled(ImVec2(shoulder.x, shoulder.y + 30 * u), 3.9f * u, IM_COL32_WHITE, 20);
	shade_vertical(dl, hand, ImVec2(0, shoulder.y + 26 * u), ImVec2(0, shoulder.y + 34 * u), c.skin_top, c.skin_bottom);
	rotate_since(dl, start, shoulder, angle);
}

// O avatar de verdade, da imagem da conta (corpo inteiro, 150x300 no formato do Xbox LIVE).
void draw_avatar_image(ImDrawList *dl, ImVec2 feet, float height, const Art &art, float al)
{
	const float u = height / 100.0f;
	dl->AddEllipseFilled(ImVec2(feet.x + 2 * u, feet.y), ImVec2(16 * u, 3.4f * u), rgba(0, 0, 0, 0.3f * al), 0, 40);
	const float h = height * 1.04f;
	// 1:2 e a proporcao do corpo no Xbox LIVE; so vale se a textura nao disser a dela.
	const float aspect = art.texture.height ? (float)art.texture.width / (float)art.texture.height : 0.5f;
	const float w = h * aspect;
	const ImVec2 a(feet.x - w / 2, feet.y + height * 0.015f - h);
	dl->AddImage(tex_id(art), a, ImVec2(a.x + w, a.y + h), ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, al));
}

// O rosto: o alto da imagem do corpo, recortado em quadrado, como o avatarpic do Xbox LIVE.
void avatar_headshot(ImDrawList *dl, ImVec2 a, float side, const Art &art, float alpha)
{
	const ImVec2 b(a.x + side, a.y + side);
	int start = mark(dl);
	dl->AddRectFilled(a, b, IM_COL32_WHITE);
	shade_vertical(dl, start, a, b, rgba(214, 228, 240, alpha), rgba(140, 164, 186, alpha));
	// 0,6 da largura por 0,3 da altura e um quadrado na imagem de 1:2.
	dl->AddImage(tex_id(art), a, b, ImVec2(0.2f, 0.0f), ImVec2(0.8f, 0.3f), rgba(255, 255, 255, alpha));
	dl->AddRect(a, b, rgba(255, 255, 255, 0.8f * alpha), 0, 0, S(2));
}

void draw_avatar(ImDrawList *dl, ImVec2 feet, float height, const char *gamertag, float al, const Art *image = nullptr)
{
	if (image) {
		draw_avatar_image(dl, feet, height, *image, al);
		return;
	}
	const float u = height / 100.0f;
	const float x = feet.x, y = feet.y;
	const AvatarColours c = avatar_colours(gamertag, al);

	dl->AddEllipseFilled(ImVec2(x + 2 * u, y), ImVec2(16 * u, 3.4f * u), rgba(0, 0, 0, 0.3f * al), 0, 40);

	// Proporcao de boneco, como os avatares da NXE: cabeca grande, corpo curto e arredondado.
	// Pernas, levemente abertas, e tenis.
	for (int side = -1; side <= 1; side += 2) {
		const float sx = x + (float)side * 5.6f * u;
		const int leg = mark(dl);
		avatar_part(dl, ImVec2(sx - 4.6f * u, y - 40 * u), ImVec2(sx + 4.6f * u, y - 5 * u), 4 * u, c.jeans_top,
			c.jeans_bottom);
		rotate_since(dl, leg, ImVec2(sx, y - 40 * u), -(float)side * 0.04f);
		avatar_part(dl, ImVec2(sx - 6 * u + (float)side * u, y - 7.5f * u), ImVec2(sx + 6 * u + (float)side * 2 * u, y),
			3.6f * u, mix(c.shoe, rgba(200, 200, 200, al), c.face ? 0.25f : 0), c.shoe);
		if (c.face)
			dl->AddLine(ImVec2(sx - 5.5f * u + (float)side * u, y - 1.3f * u),
				ImVec2(sx + 5.5f * u + (float)side * 2 * u, y - 1.3f * u), rgba(235, 235, 235, al), 1.4f * u);
	}

	// Bracos atras do tronco, quase colados ao corpo.
	avatar_arm(dl, ImVec2(x - 12.5f * u, y - 66 * u), u * 0.88f, 0.10f, c);
	avatar_arm(dl, ImVec2(x + 12.5f * u, y - 66 * u), u * 0.88f, -0.10f, c);

	// Tronco: mais largo nos ombros, com a gola e a barra da camiseta.
	int start = mark(dl);
	dl->PathLineTo(ImVec2(x - 14 * u, y - 64 * u));
	dl->PathArcTo(ImVec2(x - 9 * u, y - 64 * u), 5 * u, PI, PI * 1.5f, 8);
	dl->PathArcTo(ImVec2(x + 9 * u, y - 64 * u), 5 * u, PI * 1.5f, PI * 2, 8);
	dl->PathLineTo(ImVec2(x + 12.5f * u, y - 40 * u));
	dl->PathLineTo(ImVec2(x - 12.5f * u, y - 40 * u));
	dl->PathFillConvex(IM_COL32_WHITE);
	shade(dl, start, ImVec2(x - 14 * u, y - 69 * u), ImVec2(x + 14 * u, y - 40 * u), c.shirt_top,
		mix(c.shirt_top, c.shirt_bottom, 0.4f), c.shirt_bottom, mix(c.shirt_bottom, c.shirt_top, 0.25f));
	dl->AddLine(ImVec2(x - 12.5f * u, y - 41 * u), ImVec2(x + 12.5f * u, y - 41 * u),
		mix(c.shirt_bottom, rgba(0, 0, 0, al), 0.25f), 1.5f * u);
	avatar_part(dl, ImVec2(x - 3 * u, y - 72 * u), ImVec2(x + 3 * u, y - 67 * u), 2 * u, c.skin_bottom, c.skin_bottom);
	if (c.face) {
		dl->PathArcTo(ImVec2(x, y - 69 * u), 4.2f * u, 0.1f * PI, 0.9f * PI, 16);
		dl->PathStroke(mix(c.shirt_bottom, rgba(0, 0, 0, al), 0.35f), 0, 1.4f * u);
	}

	// Cabeca grande, com orelhas.
	const ImVec2 head(x, y - 84 * u);
	const ImVec2 radius(13 * u, 14 * u);
	dl->AddCircleFilled(ImVec2(head.x - 12.8f * u, head.y + 2 * u), 2.8f * u, c.skin_bottom, 12);
	dl->AddCircleFilled(ImVec2(head.x + 12.8f * u, head.y + 2 * u), 2.8f * u, c.skin_bottom, 12);
	start = mark(dl);
	dl->AddEllipseFilled(head, radius, IM_COL32_WHITE, 0, 48);
	shade(dl, start, ImVec2(head.x - radius.x, head.y - radius.y), ImVec2(head.x + radius.x, head.y + radius.y),
		c.skin_top, mix(c.skin_top, c.skin_bottom, 0.3f), c.skin_bottom, mix(c.skin_top, c.skin_bottom, 0.5f));

	// Cabelo: uma calota que cobre o alto da cabeca, tufos redondos por cima e a franja de lado.
	start = mark(dl);
	dl->PathEllipticalArcTo(ImVec2(head.x, head.y - 2 * u), ImVec2(14 * u, 13.5f * u), 0, PI * 1.0f, PI * 2.0f, 32);
	dl->PathFillConvex(IM_COL32_WHITE);
	for (int i = 0; i < 5; i++) {
		const float a = PI * (1.12f + 0.19f * (float)i);
		dl->AddCircleFilled(ImVec2(head.x + cosf(a) * 11.5f * u, head.y - 3 * u + sinf(a) * 11.5f * u), 5 * u,
			IM_COL32_WHITE, 20);
	}
	dl->AddTriangleFilled(ImVec2(head.x - 12 * u, head.y - 4 * u), ImVec2(head.x + 4 * u, head.y - 4 * u),
		ImVec2(head.x - 9 * u, head.y + 1.5f * u), IM_COL32_WHITE);
	shade_vertical(dl, start, ImVec2(0, head.y - 20 * u), ImVec2(0, head.y + 1 * u), c.hair_top, c.hair_bottom);

	if (c.face) {
		for (int side = -1; side <= 1; side += 2) {
			const ImVec2 eye(head.x + (float)side * 4.6f * u, head.y + 2.5f * u);
			dl->AddEllipseFilled(eye, ImVec2(1.9f * u, 2.6f * u), rgba(40, 36, 44, al), 0, 16);
			dl->AddCircleFilled(ImVec2(eye.x + 0.6f * u, eye.y - 0.9f * u), 0.7f * u, rgba(255, 255, 255, al), 8);
			dl->PathArcTo(ImVec2(eye.x, eye.y - 1.5f * u), 2.6f * u, PI * 1.25f, PI * 1.75f, 8);
			dl->PathStroke(c.hair_bottom, 0, 0.9f * u);
		}
		dl->AddEllipseFilled(ImVec2(head.x, head.y + 6.5f * u), ImVec2(1.2f * u, 0.8f * u), c.skin_bottom, 0, 10);
		dl->PathArcTo(ImVec2(head.x, head.y + 7 * u), 3.8f * u, 0.22f * PI, 0.78f * PI, 16);
		dl->PathStroke(rgba(150, 70, 60, al), 0, 1.2f * u);
	}
}

// --- o avatar da conta, montado pelas escolhas do editor ------------------------------------
//
// Um boneco proprio do EPTX, de cabeca grande e corpo curto. Cada numero de
// avatar_look escolhe uma parte; as tabelas abaixo dao os nomes que o editor
// mostra. Tudo e medido em u = altura / 100, com os pes em feet.

struct Rgb {
	int r, g, b;
};
const Rgb LOOK_SKINS[] = { { 255, 226, 200 }, { 242, 202, 168 }, { 224, 172, 128 }, { 184, 128, 88 }, { 142, 94, 62 },
	{ 98, 64, 44 } };
const Rgb LOOK_HAIR_COLOURS[] = { { 34, 30, 30 }, { 74, 46, 28 }, { 124, 80, 42 }, { 156, 68, 32 }, { 222, 182, 104 },
	{ 236, 226, 200 }, { 150, 150, 154 }, { 228, 110, 160 } };
const Rgb LOOK_TOP_COLOURS[] = { { 236, 236, 236 }, { 44, 44, 50 }, { 200, 52, 46 }, { 58, 92, 140 }, { 92, 162, 222 },
	{ 62, 150, 72 }, { 236, 190, 42 }, { 236, 122, 32 }, { 122, 72, 172 }, { 230, 122, 172 } };
const Rgb LOOK_BOTTOM_COLOURS[] = { { 62, 92, 152 }, { 198, 180, 140 }, { 44, 44, 50 }, { 122, 122, 128 },
	{ 112, 76, 46 }, { 102, 112, 62 }, { 232, 232, 232 }, { 170, 42, 42 } };
const Rgb LOOK_SHOE_COLOURS[] = { { 240, 240, 240 }, { 40, 40, 44 }, { 200, 46, 40 }, { 238, 124, 34 }, { 56, 100, 200 },
	{ 60, 150, 70 }, { 112, 72, 40 }, { 130, 132, 138 } };

const char *const LOOK_FIELD_NAMES[LOOK_FIELDS] = { "Pele", "Cabelo", "Cor do cabelo", "Olhos", "Boca", "Oculos",
	"Barba", "Roupa de cima", "Cor de cima", "Roupa de baixo", "Cor de baixo", "Calcado", "Cor do calcado", "Corpo",
	"Altura", "Na cabeca" };
const char *const LOOK_SKIN_NAMES[] = { "Muito clara", "Clara", "Morena clara", "Morena", "Parda", "Negra" };
const char *const LOOK_HAIR_NAMES[] = { "Careca", "Curto de lado", "Espetado", "Comprido", "Rabo de cavalo", "Cacheado",
	"Moicano" };
const char *const LOOK_HAIR_COLOUR_NAMES[] = { "Preto", "Castanho-escuro", "Castanho", "Ruivo", "Loiro", "Platinado",
	"Grisalho", "Rosa" };
const char *const LOOK_EYES_NAMES[] = { "Redondos", "Alegres", "Sonolentos", "Grandes" };
const char *const LOOK_MOUTH_NAMES[] = { "Sorriso", "Sorrisao", "Serio", "Surpreso" };
const char *const LOOK_GLASSES_NAMES[] = { "Nenhum", "Redondos", "Quadrados", "Escuros" };
const char *const LOOK_BEARD_NAMES[] = { "Nenhuma", "Bigode", "Cavanhaque", "Barba cheia" };
const char *const LOOK_TOP_NAMES[] = { "Camiseta", "Camisa de botao", "Moletom", "Regata", "Jaqueta" };
const char *const LOOK_COLOUR_NAMES[] = { "Branco", "Preto", "Vermelho", "Azul-jeans", "Azul-claro", "Verde", "Amarelo",
	"Laranja", "Roxo", "Rosa" };
const char *const LOOK_BOTTOM_NAMES[] = { "Calca", "Bermuda", "Saia" };
const char *const LOOK_BOTTOM_COLOUR_NAMES[] = { "Jeans", "Caqui", "Preto", "Cinza", "Marrom", "Verde-oliva", "Branco",
	"Vinho" };
const char *const LOOK_SHOES_NAMES[] = { "Tenis", "Botas", "Sapatos" };
const char *const LOOK_SHOE_COLOUR_NAMES[] = { "Branco", "Preto", "Vermelho", "Laranja", "Azul", "Verde", "Marrom",
	"Cinza" };
const char *const LOOK_BUILD_NAMES[] = { "Magro", "Medio", "Forte" };
const char *const LOOK_HEIGHT_NAMES[] = { "Baixo", "Medio", "Alto" };
const char *const LOOK_HAT_NAMES[] = { "Nada", "Bone", "Gorro", "Fones" };

const char *look_option_name(int field, int value)
{
	static const char *const *const names[LOOK_FIELDS] = { LOOK_SKIN_NAMES, LOOK_HAIR_NAMES, LOOK_HAIR_COLOUR_NAMES,
		LOOK_EYES_NAMES, LOOK_MOUTH_NAMES, LOOK_GLASSES_NAMES, LOOK_BEARD_NAMES, LOOK_TOP_NAMES, LOOK_COLOUR_NAMES,
		LOOK_BOTTOM_NAMES, LOOK_BOTTOM_COLOUR_NAMES, LOOK_SHOES_NAMES, LOOK_SHOE_COLOUR_NAMES, LOOK_BUILD_NAMES,
		LOOK_HEIGHT_NAMES, LOOK_HAT_NAMES };
	return names[field][value];
}

// A cor de uma opcao, para a amostra do editor; null se a parte nao for de cor.
const Rgb *look_option_colour(int field, int value)
{
	switch (field) {
	case LOOK_SKIN:
		return &LOOK_SKINS[value];
	case LOOK_HAIR_COLOUR:
		return &LOOK_HAIR_COLOURS[value];
	case LOOK_TOP_COLOUR:
		return &LOOK_TOP_COLOURS[value];
	case LOOK_BOTTOM_COLOUR:
		return &LOOK_BOTTOM_COLOURS[value];
	case LOOK_SHOE_COLOUR:
		return &LOOK_SHOE_COLOURS[value];
	default:
		return nullptr;
	}
}

ImU32 tone(Rgb c, float k, float al)
{
	auto ch = [k](int v) { return (int)fminf((float)v * k, 255.0f); };
	return rgba(ch(c.r), ch(c.g), ch(c.b), al);
}

// Claro em cima, escuro embaixo: o volume de todas as partes.
struct Paint {
	ImU32 top, bottom;
};
Paint paint(Rgb c, float al) { return { tone(c, 1.12f, al), tone(c, 0.70f, al) }; }

void look_part(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, Paint p, ImDrawFlags flags = 0)
{
	const int start = mark(dl);
	dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
	shade(dl, start, a, b, p.top, mix(p.top, p.bottom, 0.3f), p.bottom, mix(p.bottom, p.top, 0.2f));
}

void look_ellipse(ImDrawList *dl, ImVec2 c, ImVec2 r, Paint p)
{
	const int start = mark(dl);
	dl->AddEllipseFilled(c, r, IM_COL32_WHITE, 0, 32);
	shade(dl, start, ImVec2(c.x - r.x, c.y - r.y), ImVec2(c.x + r.x, c.y + r.y), p.top, mix(p.top, p.bottom, 0.3f),
		p.bottom, mix(p.top, p.bottom, 0.5f));
}

// Braco do ombro ate a mao, girado para fora; a manga cobre sleeve unidades.
void look_arm(ImDrawList *dl, ImVec2 shoulder, float u, float bw, float angle, float sleeve, Paint cloth, Paint skin)
{
	const int start = mark(dl);
	const float half = 3.9f * bw * u;
	look_part(dl, ImVec2(shoulder.x - half * 0.85f, shoulder.y + 4 * u),
		ImVec2(shoulder.x + half * 0.85f, shoulder.y + 29 * u), half * 0.85f, skin);
	if (sleeve > 0) {
		look_part(dl, ImVec2(shoulder.x - half, shoulder.y), ImVec2(shoulder.x + half, shoulder.y + sleeve * u), half,
			cloth);
		dl->AddLine(ImVec2(shoulder.x - half + 0.5f * u, shoulder.y + (sleeve - 1) * u),
			ImVec2(shoulder.x + half - 0.5f * u, shoulder.y + (sleeve - 1) * u), mix(cloth.bottom, IM_COL32_BLACK, 0.2f),
			1.1f * u);
	}
	look_ellipse(dl, ImVec2(shoulder.x, shoulder.y + 30 * u), ImVec2(3.8f * u, 4.2f * u), skin);
	rotate_since(dl, start, shoulder, angle);
}

void look_shoe(ImDrawList *dl, float sx, float y, float u, int side, int kind, Paint p, float al)
{
	const float out = (float)side;
	if (kind == 1) // bota: cano ate a canela
		look_part(dl, ImVec2(sx - 4.8f * u, y - 13 * u), ImVec2(sx + 4.8f * u, y - 3 * u), 2 * u, p);
	const float h = kind == 2 ? 6 : 8;
	look_part(dl, ImVec2(sx - 6 * u + out * u, y - h * u), ImVec2(sx + 6 * u + out * 2 * u, y), 3.6f * u, p);
	if (kind == 0) { // tenis: sola branca e cadarco
		dl->AddRectFilled(ImVec2(sx - 6 * u + out * u, y - 2.2f * u), ImVec2(sx + 6 * u + out * 2 * u, y),
			rgba(245, 245, 245, al), 1.2f * u);
		dl->AddLine(ImVec2(sx - 2 * u, y - 6.5f * u), ImVec2(sx + 2.4f * u, y - 6.5f * u), rgba(255, 255, 255, 0.8f * al),
			0.9f * u);
	} else if (kind == 2) { // sapato: brilho
		dl->AddEllipseFilled(ImVec2(sx + out * 1.5f * u, y - 4.2f * u), ImVec2(2.4f * u, 0.8f * u),
			rgba(255, 255, 255, 0.45f * al), 0, 12);
	}
}

// Calota de cabelo sobre o alto da cabeca; base para quase todos os penteados.
void hair_cap(ImDrawList *dl, ImVec2 head, float u, float grow)
{
	dl->PathEllipticalArcTo(ImVec2(head.x, head.y - 2 * u), ImVec2((14 + grow) * u, (13.5f + grow) * u), 0, PI, PI * 2,
		32);
	dl->PathFillConvex(IM_COL32_WHITE);
}

// O que fica atras da cabeca (cabelo comprido, rabo de cavalo, capuz).
void look_hair_back(ImDrawList *dl, ImVec2 head, float u, int style, Paint hair)
{
	if (style == 3) {
		look_part(dl, ImVec2(head.x - 15.5f * u, head.y - 8 * u), ImVec2(head.x + 15.5f * u, head.y + 19 * u), 7 * u, hair,
			ImDrawFlags_RoundCornersBottom);
	} else if (style == 4) {
		const int start = mark(dl);
		dl->AddEllipseFilled(ImVec2(head.x + 15 * u, head.y + 4 * u), ImVec2(4.5f * u, 10 * u), IM_COL32_WHITE, 0, 24);
		rotate_since(dl, start, ImVec2(head.x + 13 * u, head.y - 6 * u), -0.35f);
		shade_vertical(dl, start, ImVec2(0, head.y - 8 * u), ImVec2(0, head.y + 14 * u), hair.top, hair.bottom);
	} else if (style == 5) {
		const int start = mark(dl);
		for (int i = 0; i < 9; i++) {
			const float a = PI * (0.85f + 0.163f * (float)i);
			dl->AddCircleFilled(ImVec2(head.x + cosf(a) * 14.5f * u, head.y - 2 * u + sinf(a) * 13.5f * u), 5.6f * u,
				IM_COL32_WHITE, 20);
		}
		shade_vertical(dl, start, ImVec2(0, head.y - 22 * u), ImVec2(0, head.y + 6 * u), hair.top, hair.bottom);
	}
}

void look_hair_front(ImDrawList *dl, ImVec2 head, float u, int style, Paint hair, bool hat)
{
	const int start = mark(dl);
	switch (style) {
	case 0: // careca
		break;
	case 1: // curto, repartido de lado: liso, com volume em cima e a franja penteada para um lado
		hair_cap(dl, head, u, 0.6f);
		dl->AddEllipseFilled(ImVec2(head.x + 2 * u, head.y - 12.5f * u), ImVec2(12 * u, 4.5f * u), IM_COL32_WHITE, -0.12f,
			32);
		dl->AddTriangleFilled(ImVec2(head.x - 13.5f * u, head.y - 5 * u), ImVec2(head.x + 6 * u, head.y - 5 * u),
			ImVec2(head.x - 11.5f * u, head.y + 1 * u), IM_COL32_WHITE);
		break;
	case 2: // espetado
		hair_cap(dl, head, u, -0.5f);
		if (!hat)
			for (int i = 0; i < 6; i++) {
				const float a = PI * (1.1f + 0.16f * (float)i);
				const ImVec2 base(head.x + cosf(a) * 11 * u, head.y - 3 * u + sinf(a) * 11 * u);
				const ImVec2 tip(head.x + cosf(a) * 19 * u, head.y - 4 * u + sinf(a) * 19 * u);
				const ImVec2 side(-sinf(a) * 4 * u, cosf(a) * 4 * u);
				dl->AddTriangleFilled(ImVec2(base.x - side.x, base.y - side.y), ImVec2(base.x + side.x, base.y + side.y),
					tip, IM_COL32_WHITE);
			}
		break;
	case 3: // comprido: franja e mechas dos lados
		hair_cap(dl, head, u, 0.5f);
		dl->AddRectFilled(ImVec2(head.x - 15 * u, head.y - 4 * u), ImVec2(head.x - 10.5f * u, head.y + 16 * u),
			IM_COL32_WHITE, 2.5f * u);
		dl->AddRectFilled(ImVec2(head.x + 10.5f * u, head.y - 4 * u), ImVec2(head.x + 15 * u, head.y + 16 * u),
			IM_COL32_WHITE, 2.5f * u);
		dl->AddTriangleFilled(ImVec2(head.x - 13 * u, head.y - 5 * u), ImVec2(head.x + 8 * u, head.y - 5 * u),
			ImVec2(head.x - 11 * u, head.y + 0.5f * u), IM_COL32_WHITE);
		break;
	case 4: // rabo de cavalo: puxado para tras, lisinho
		hair_cap(dl, head, u, 0.2f);
		break;
	case 5: // cacheado
		hair_cap(dl, head, u, 0.5f);
		for (int i = 0; i < 6; i++)
			dl->AddCircleFilled(ImVec2(head.x - 10 * u + (float)i * 4 * u, head.y - 9 * u + (i % 2 ? 1.5f : 0) * u),
				3.6f * u, IM_COL32_WHITE, 16);
		break;
	case 6: // moicano: lados raspados e a crista
		if (!hat)
			dl->AddRectFilled(ImVec2(head.x - 3.6f * u, head.y - 22 * u), ImVec2(head.x + 3.6f * u, head.y - 6 * u),
				IM_COL32_WHITE, 3.6f * u, ImDrawFlags_RoundCornersTop);
		break;
	}
	shade_vertical(dl, start, ImVec2(0, head.y - 22 * u), ImVec2(0, head.y + 16 * u), hair.top, hair.bottom);
	if (style == 6) { // a sombra do cabelo raspado
		dl->PathEllipticalArcTo(ImVec2(head.x, head.y - 2 * u), ImVec2(13.2f * u, 12.6f * u), 0, PI * 1.08f, PI * 1.92f, 24);
		dl->PathFillConvex(mix(hair.bottom, IM_COL32(0, 0, 0, 0), 0.65f));
	}
	if (style == 4) { // o elastico
		dl->AddCircleFilled(ImVec2(head.x + 12.5f * u, head.y - 6.5f * u), 1.8f * u, hair.bottom, 12);
	}
}

void look_hat(ImDrawList *dl, ImVec2 head, float u, int hat, Paint cloth, float al)
{
	if (hat == 1) { // bone, aba para a frente e um pouco de lado
		look_ellipse(dl, ImVec2(head.x - 6 * u, head.y - 7.5f * u), ImVec2(12 * u, 3 * u), cloth);
		const int start = mark(dl);
		dl->PathEllipticalArcTo(ImVec2(head.x, head.y - 6 * u), ImVec2(14.4f * u, 12 * u), 0, PI, PI * 2, 32);
		dl->PathFillConvex(IM_COL32_WHITE);
		shade_vertical(dl, start, ImVec2(0, head.y - 18 * u), ImVec2(0, head.y - 6 * u), cloth.top, cloth.bottom);
		dl->AddCircleFilled(ImVec2(head.x, head.y - 17.6f * u), 1.4f * u, cloth.bottom, 12);
	} else if (hat == 2) { // gorro com barra dobrada
		const int start = mark(dl);
		dl->PathEllipticalArcTo(ImVec2(head.x, head.y - 4 * u), ImVec2(14.6f * u, 15 * u), 0, PI, PI * 2, 32);
		dl->PathFillConvex(IM_COL32_WHITE);
		shade_vertical(dl, start, ImVec2(0, head.y - 19 * u), ImVec2(0, head.y - 4 * u), cloth.top, cloth.bottom);
		look_part(dl, ImVec2(head.x - 15 * u, head.y - 7 * u), ImVec2(head.x + 15 * u, head.y - 1.5f * u), 2.5f * u,
			{ mix(cloth.top, IM_COL32(255, 255, 255, 255), 0.15f), cloth.bottom });
		for (int i = -3; i <= 3; i++)
			dl->AddLine(ImVec2(head.x + (float)i * 4 * u, head.y - 6.4f * u), ImVec2(head.x + (float)i * 4 * u, head.y - 2 * u),
				mix(cloth.bottom, IM_COL32(0, 0, 0, 255), 0.2f), 0.8f * u);
	} else if (hat == 3) { // fones de ouvido
		dl->PathArcTo(ImVec2(head.x, head.y - 1 * u), 15.5f * u, PI * 1.05f, PI * 1.95f, 32);
		dl->PathStroke(rgba(50, 52, 58, al), 0, 2.4f * u);
		for (int side = -1; side <= 1; side += 2)
			look_part(dl, ImVec2(head.x + (float)side * 15.5f * u - 3 * u, head.y - 3 * u),
				ImVec2(head.x + (float)side * 15.5f * u + 3 * u, head.y + 7 * u), 2.8f * u,
				{ rgba(80, 84, 92, al), rgba(30, 32, 36, al) });
	}
}

void look_face(ImDrawList *dl, ImVec2 head, float u, const avatar_look &look, Paint skin, Paint hair, float al)
{
	const ImU32 ink = rgba(40, 36, 44, al);
	for (int side = -1; side <= 1; side += 2) {
		const ImVec2 eye(head.x + (float)side * 4.6f * u, head.y + 2.5f * u);
		switch (look.v[LOOK_EYES]) {
		case 0: // redondos
			dl->AddEllipseFilled(eye, ImVec2(1.9f * u, 2.6f * u), ink, 0, 16);
			dl->AddCircleFilled(ImVec2(eye.x + 0.6f * u, eye.y - 0.9f * u), 0.7f * u, rgba(255, 255, 255, al), 8);
			break;
		case 1: // alegres: arcos para cima
			dl->PathArcTo(ImVec2(eye.x, eye.y + 1 * u), 2.2f * u, PI * 1.1f, PI * 1.9f, 12);
			dl->PathStroke(ink, 0, 1.2f * u);
			break;
		case 2: // sonolentos: meio fechados
			dl->AddEllipseFilled(ImVec2(eye.x, eye.y + 0.6f * u), ImVec2(1.9f * u, 1.5f * u), ink, 0, 16);
			dl->AddLine(ImVec2(eye.x - 2.4f * u, eye.y - 0.3f * u), ImVec2(eye.x + 2.4f * u, eye.y - 0.3f * u),
				skin.bottom, 1.2f * u);
			break;
		case 3: // grandes, com o branco
			dl->AddEllipseFilled(eye, ImVec2(2.6f * u, 3.2f * u), rgba(255, 255, 255, al), 0, 20);
			dl->AddEllipseFilled(ImVec2(eye.x, eye.y + 0.4f * u), ImVec2(1.7f * u, 2.2f * u), rgba(70, 110, 60, al), 0, 16);
			dl->AddCircleFilled(ImVec2(eye.x, eye.y + 0.4f * u), 1 * u, ink, 12);
			dl->AddCircleFilled(ImVec2(eye.x + 0.6f * u, eye.y - 0.5f * u), 0.6f * u, rgba(255, 255, 255, al), 8);
			break;
		}
		// Sobrancelha da cor do cabelo (grisalho para quem e careca, para nao sumir).
		dl->PathArcTo(ImVec2(eye.x, eye.y - 1.2f * u), 3 * u, PI * 1.25f, PI * 1.75f, 8);
		dl->PathStroke(look.v[LOOK_HAIR] == 0 ? skin.bottom : hair.bottom, 0, 1 * u);
	}
	dl->AddEllipseFilled(ImVec2(head.x, head.y + 6.2f * u), ImVec2(1.2f * u, 0.8f * u), skin.bottom, 0, 10);

	const int beard = look.v[LOOK_BEARD];
	if (beard == 3) { // barba cheia, ao redor do queixo
		dl->PathEllipticalArcTo(ImVec2(head.x, head.y + 3 * u), ImVec2(12.6f * u, 11 * u), 0, 0, PI, 24);
		dl->PathFillConvex(hair.bottom);
	} else if (beard == 2) { // cavanhaque
		dl->AddEllipseFilled(ImVec2(head.x, head.y + 11.5f * u), ImVec2(2.6f * u, 2.2f * u), hair.bottom, 0, 16);
	}

	const ImU32 lips = rgba(150, 70, 60, al);
	const ImVec2 mouth(head.x, head.y + 8.6f * u);
	switch (look.v[LOOK_MOUTH]) {
	case 0: // sorriso
		dl->PathArcTo(ImVec2(mouth.x, mouth.y - 1.6f * u), 3.8f * u, 0.22f * PI, 0.78f * PI, 16);
		dl->PathStroke(lips, 0, 1.2f * u);
		break;
	case 1: // sorrisao, de boca aberta
		dl->PathEllipticalArcTo(ImVec2(mouth.x, mouth.y - 0.8f * u), ImVec2(3.6f * u, 3 * u), 0, 0, PI, 16);
		dl->PathFillConvex(rgba(110, 40, 40, al));
		dl->AddRectFilled(ImVec2(mouth.x - 3 * u, mouth.y - 0.8f * u), ImVec2(mouth.x + 3 * u, mouth.y + 0.3f * u),
			rgba(255, 255, 255, al));
		break;
	case 2: // serio
		dl->AddLine(ImVec2(mouth.x - 2.6f * u, mouth.y), ImVec2(mouth.x + 2.6f * u, mouth.y), lips, 1.2f * u);
		break;
	case 3: // surpreso
		dl->AddEllipseFilled(ImVec2(mouth.x, mouth.y + 0.2f * u), ImVec2(1.6f * u, 2 * u), rgba(110, 40, 40, al), 0, 16);
		break;
	}
	if (beard == 1 || beard == 2) { // bigode
		for (int side = -1; side <= 1; side += 2)
			dl->AddEllipseFilled(ImVec2(head.x + (float)side * 2 * u, head.y + 7.2f * u), ImVec2(2.4f * u, 1.1f * u),
				hair.bottom, (float)side * -0.25f, 12);
	}

	const ImVec2 left(head.x - 4.6f * u, head.y + 2.5f * u), right(head.x + 4.6f * u, head.y + 2.5f * u);
	const ImU32 frame = rgba(40, 40, 46, al);
	switch (look.v[LOOK_GLASSES]) {
	case 1: // redondos
		dl->AddCircle(left, 3.6f * u, frame, 24, 0.9f * u);
		dl->AddCircle(right, 3.6f * u, frame, 24, 0.9f * u);
		dl->AddLine(ImVec2(left.x + 3.6f * u, left.y), ImVec2(right.x - 3.6f * u, right.y), frame, 0.9f * u);
		break;
	case 2: // quadrados
		dl->AddRect(ImVec2(left.x - 3.8f * u, left.y - 3 * u), ImVec2(left.x + 3.4f * u, left.y + 3 * u), frame, 1 * u, 0,
			1.1f * u);
		dl->AddRect(ImVec2(right.x - 3.4f * u, right.y - 3 * u), ImVec2(right.x + 3.8f * u, right.y + 3 * u), frame, 1 * u,
			0, 1.1f * u);
		dl->AddLine(ImVec2(left.x + 3.4f * u, left.y - 1 * u), ImVec2(right.x - 3.4f * u, right.y - 1 * u), frame, 1.1f * u);
		break;
	case 3: // escuros
		const ImVec2 lenses[2] = { left, right };
		for (const ImVec2 &c : lenses) {
			look_part(dl, ImVec2(c.x - 3.8f * u, c.y - 2.8f * u), ImVec2(c.x + 3.8f * u, c.y + 2.6f * u), 2 * u,
				{ rgba(70, 76, 90, al), rgba(12, 12, 16, al) });
			dl->AddLine(ImVec2(c.x - 2.4f * u, c.y - 1.6f * u), ImVec2(c.x - 0.6f * u, c.y - 1.6f * u),
				rgba(255, 255, 255, 0.5f * al), 0.7f * u);
		}
		dl->AddLine(ImVec2(left.x + 3.8f * u, left.y - 1 * u), ImVec2(right.x - 3.8f * u, right.y - 1 * u), frame, 1.1f * u);
		break;
	}
}

// Detalhes da roupa de cima, por cima do tronco.
void look_top_details(ImDrawList *dl, float x, float y, float u, float bw, int top, Paint cloth, Paint skin, float al)
{
	const ImU32 seam = mix(cloth.bottom, IM_COL32(0, 0, 0, 255), 0.3f);
	switch (top) {
	case 0: // camiseta: gola redonda
		dl->PathArcTo(ImVec2(x, y - 69 * u), 4.2f * u, 0.1f * PI, 0.9f * PI, 16);
		dl->PathStroke(seam, 0, 1.4f * u);
		break;
	case 1: // camisa de botao: colarinho, vista e botoes
		dl->AddLine(ImVec2(x, y - 66 * u), ImVec2(x, y - 41 * u), seam, 0.9f * u);
		for (int i = 0; i < 4; i++)
			dl->AddCircleFilled(ImVec2(x + 1.3f * u, y - 62 * u + (float)i * 5.5f * u), 0.7f * u,
				rgba(235, 235, 235, 0.9f * al), 8);
		for (int side = -1; side <= 1; side += 2)
			dl->AddTriangleFilled(ImVec2(x, y - 65 * u), ImVec2(x + (float)side * 5.5f * u, y - 70 * u),
				ImVec2(x + (float)side * 4.5f * u, y - 63 * u), mix(cloth.top, IM_COL32(255, 255, 255, 255), 0.2f));
		dl->AddLine(ImVec2(x - 9 * bw * u, y - 56 * u), ImVec2(x - 4 * bw * u, y - 56 * u), seam, 0.8f * u); // bolso
		break;
	case 2: // moletom: bolso canguru e cordoes
		look_part(dl, ImVec2(x - 7 * bw * u, y - 52 * u), ImVec2(x + 7 * bw * u, y - 43 * u), 2.5f * u,
			{ mix(cloth.top, cloth.bottom, 0.25f), cloth.bottom });
		for (int side = -1; side <= 1; side += 2)
			dl->AddLine(ImVec2(x + (float)side * 2 * u, y - 67 * u), ImVec2(x + (float)side * 2.4f * u, y - 58 * u),
				rgba(240, 240, 240, al), 0.8f * u);
		break;
	case 3: // regata: decote e alcas
		dl->PathArcTo(ImVec2(x, y - 69 * u), 5.5f * u, 0.05f * PI, 0.95f * PI, 16);
		dl->PathFillConvex(skin.top);
		break;
	case 4: // jaqueta aberta com camiseta branca por baixo
		look_part(dl, ImVec2(x - 4 * u, y - 67 * u), ImVec2(x + 4 * u, y - 41 * u), 1 * u,
			{ rgba(245, 245, 245, al), rgba(200, 200, 205, al) });
		for (int side = -1; side <= 1; side += 2) {
			dl->AddLine(ImVec2(x + (float)side * 4 * u, y - 67 * u), ImVec2(x + (float)side * 4 * u, y - 41 * u), seam,
				1 * u);
			dl->AddTriangleFilled(ImVec2(x + (float)side * 4 * u, y - 67 * u), ImVec2(x + (float)side * 8 * u, y - 67 * u),
				ImVec2(x + (float)side * 4 * u, y - 58 * u), mix(cloth.top, IM_COL32(255, 255, 255, 255), 0.12f));
		}
		break;
	}
}

void draw_look(ImDrawList *dl, ImVec2 feet, float height, const avatar_look &look, float al, bool shadow = true)
{
	static const float HEIGHTS[] = { 0.92f, 1.0f, 1.07f }, BUILDS[] = { 0.86f, 1.0f, 1.2f };
	const float u = height / 100.0f * HEIGHTS[look.v[LOOK_HEIGHT]];
	const float bw = BUILDS[look.v[LOOK_BUILD]];
	const float x = feet.x, y = feet.y;
	const Paint skin = paint(LOOK_SKINS[look.v[LOOK_SKIN]], al);
	const Paint hair = paint(LOOK_HAIR_COLOURS[look.v[LOOK_HAIR_COLOUR]], al);
	const Paint cloth = paint(LOOK_TOP_COLOURS[look.v[LOOK_TOP_COLOUR]], al);
	const Paint bottom = paint(LOOK_BOTTOM_COLOURS[look.v[LOOK_BOTTOM_COLOUR]], al);
	const Paint shoe = paint(LOOK_SHOE_COLOURS[look.v[LOOK_SHOES] == 2 && look.v[LOOK_SHOE_COLOUR] == 0
			? 1
			: look.v[LOOK_SHOE_COLOUR]],
		al);
	const int top = look.v[LOOK_TOP], legs = look.v[LOOK_BOTTOM];

	if (shadow)
		dl->AddEllipseFilled(ImVec2(x + 2 * u, y), ImVec2(16 * bw * u, 3.4f * u), rgba(0, 0, 0, 0.3f * al), 0, 40);

	// Pernas, levemente abertas: calca inteira, ou pele com a bermuda/saia por cima.
	for (int side = -1; side <= 1; side += 2) {
		const float sx = x + (float)side * 5.6f * bw * u;
		const int leg = mark(dl);
		const float half = 4.6f * bw * u;
		if (legs == 0) {
			look_part(dl, ImVec2(sx - half, y - 40 * u), ImVec2(sx + half, y - 5 * u), 4 * u, bottom);
		} else {
			look_part(dl, ImVec2(sx - half * 0.82f, y - 34 * u), ImVec2(sx + half * 0.82f, y - 5 * u), 3.4f * u, skin);
			if (legs == 1)
				look_part(dl, ImVec2(sx - half * 1.05f, y - 40 * u), ImVec2(sx + half * 1.05f, y - 23 * u), 3 * u, bottom);
		}
		rotate_since(dl, leg, ImVec2(sx, y - 40 * u), -(float)side * 0.04f);
		look_shoe(dl, sx, y, u, side, look.v[LOOK_SHOES], shoe, al);
	}
	if (legs == 2) { // saia: trapezio rodado
		const int start = mark(dl);
		dl->PathLineTo(ImVec2(x - 12.5f * bw * u, y - 42 * u));
		dl->PathLineTo(ImVec2(x + 12.5f * bw * u, y - 42 * u));
		dl->PathLineTo(ImVec2(x + 17 * bw * u, y - 24 * u));
		dl->PathLineTo(ImVec2(x - 17 * bw * u, y - 24 * u));
		dl->PathFillConvex(IM_COL32_WHITE);
		shade_vertical(dl, start, ImVec2(0, y - 42 * u), ImVec2(0, y - 24 * u), bottom.top, bottom.bottom);
	}

	// Bracos atras do tronco; a manga depende da roupa.
	static const float SLEEVES[] = { 13, 26, 27, 0, 27 };
	const float sleeve = SLEEVES[top];
	look_arm(dl, ImVec2(x - 12.5f * bw * u, y - 66 * u), u * 0.88f, bw, 0.10f, sleeve, cloth, skin);
	look_arm(dl, ImVec2(x + 12.5f * bw * u, y - 66 * u), u * 0.88f, bw, -0.10f, sleeve, cloth, skin);

	const ImVec2 head(x, y - 84 * u);
	if (top == 2) // capuz caido atras do pescoco
		look_ellipse(dl, ImVec2(x, y - 69 * u), ImVec2(10 * u, 4 * u), { cloth.bottom, cloth.bottom });

	// Tronco: mais largo nos ombros (na regata, os ombros sao pele).
	if (top == 3)
		look_part(dl, ImVec2(x - 14 * bw * u, y - 69 * u), ImVec2(x + 14 * bw * u, y - 58 * u), 5 * u, skin);
	const float sh = top == 3 ? 10.5f : 14;
	int start = mark(dl);
	dl->PathLineTo(ImVec2(x - sh * bw * u, y - 64 * u));
	dl->PathArcTo(ImVec2(x - (sh - 5) * bw * u, y - 64 * u), 5 * u, PI, PI * 1.5f, 8);
	dl->PathArcTo(ImVec2(x + (sh - 5) * bw * u, y - 64 * u), 5 * u, PI * 1.5f, PI * 2, 8);
	dl->PathLineTo(ImVec2(x + 12.5f * bw * u, y - 40 * u));
	dl->PathLineTo(ImVec2(x - 12.5f * bw * u, y - 40 * u));
	dl->PathFillConvex(IM_COL32_WHITE);
	shade(dl, start, ImVec2(x - 14 * bw * u, y - 69 * u), ImVec2(x + 14 * bw * u, y - 40 * u), cloth.top,
		mix(cloth.top, cloth.bottom, 0.4f), cloth.bottom, mix(cloth.bottom, cloth.top, 0.25f));
	if (legs != 2) // cos da calca
		dl->AddRectFilled(ImVec2(x - 12.5f * bw * u, y - 42 * u), ImVec2(x + 12.5f * bw * u, y - 39 * u), bottom.bottom,
			1 * u);
	look_part(dl, ImVec2(x - 3 * u, y - 72 * u), ImVec2(x + 3 * u, y - 67 * u), 2 * u, { skin.bottom, skin.bottom });
	look_top_details(dl, x, y, u, bw, top, cloth, skin, al);

	// Cabeca: cabelo de tras, orelhas, rosto, cabelo da frente e o que vai na cabeca.
	const int hat = look.v[LOOK_HAT];
	look_hair_back(dl, head, u, look.v[LOOK_HAIR], hair);
	dl->AddCircleFilled(ImVec2(head.x - 12.8f * u, head.y + 2 * u), 2.8f * u, skin.bottom, 12);
	dl->AddCircleFilled(ImVec2(head.x + 12.8f * u, head.y + 2 * u), 2.8f * u, skin.bottom, 12);
	start = mark(dl);
	dl->AddEllipseFilled(head, ImVec2(13 * u, 14 * u), IM_COL32_WHITE, 0, 48);
	shade(dl, start, ImVec2(head.x - 13 * u, head.y - 14 * u), ImVec2(head.x + 13 * u, head.y + 14 * u), skin.top,
		mix(skin.top, skin.bottom, 0.3f), skin.bottom, mix(skin.top, skin.bottom, 0.5f));
	// Bochechas coradas.
	for (int side = -1; side <= 1; side += 2)
		dl->AddEllipseFilled(ImVec2(head.x + (float)side * 7.5f * u, head.y + 6.5f * u), ImVec2(2.2f * u, 1.3f * u),
			rgba(240, 120, 110, 0.22f * al), 0, 12);
	look_face(dl, head, u, look, skin, hair, al);
	look_hair_front(dl, head, u, look.v[LOOK_HAIR], hair, hat == 1 || hat == 2);
	if (hat)
		look_hat(dl, head, u, hat, cloth, al);
}

// So o rosto, recortado num quadrado: o lugar do gamerpic no canto e no Guide.
void look_headshot(ImDrawList *dl, ImVec2 a, float side, const avatar_look &look, float alpha)
{
	const ImVec2 b(a.x + side, a.y + side);
	int start = mark(dl);
	dl->AddRectFilled(a, b, IM_COL32_WHITE);
	shade_vertical(dl, start, a, b, rgba(214, 228, 240, alpha), rgba(140, 164, 186, alpha));
	dl->PushClipRect(a, b, true);
	// A cabeca tem uns 34 u de altura com o cabelo; ela ocupa uns 85% do quadrado.
	const float height = side * 0.85f / 34.0f * 100.0f;
	avatar_look standard = look;
	standard.v[LOOK_HEIGHT] = 1;
	draw_look(dl, ImVec2(a.x + side / 2, a.y + side * 0.56f + 84.0f * height / 100.0f), height, standard, alpha, false);
	dl->PopClipRect();
	dl->AddRect(a, b, rgba(255, 255, 255, 0.8f * alpha), 0, 0, S(2));
}

// O corpo inteiro em PNG de 150x300 com fundo transparente, no formato do avatar-body.png.
uint8_t *look_png(const avatar_look &look, int *len)
{
	constexpr int W = 150, H = 300;
	*len = 0;
	ImDrawList list(ImGui::GetDrawListSharedData());
	list._ResetForNewFrame();
	// Sem isto a lista nova tem densidade 0 e o ImGui descarta todas as linhas (boca, oculos...).
	list._SetPixelDensity(1.0f);
	list.PushClipRect(ImVec2(0, 0), ImVec2((float)W, (float)H));
	draw_look(&list, ImVec2(W / 2.0f, H - 6.0f), 252.0f, look, 1, false);

	// O antisserrilhado do ImGui le o atlas das fontes; a cor lisa tambem sai de um pixel branco dele.
	ImTextureData *atlas = ImGui::GetIO().Fonts->TexData;
	const RasterTexture tex = { atlas->Width, atlas->Height, (const uint8_t *)atlas->GetPixels(),
		atlas->Format == ImTextureFormat_Alpha8 ? 1 : 4 };
	RasterTarget target = { W, H, (float *)calloc((size_t)W * H * 4, sizeof(float)) };
	uint8_t *rgba8 = (uint8_t *)malloc((size_t)W * H * 4);
	uint8_t *png = nullptr;
	if (target.px && rgba8 && tex.px) {
		raster_draw_list(target, &list, [&](const ImDrawCmd &) { return &tex; });
		raster_to_rgba8(target, rgba8);
		png = image_encode_png(rgba8, W, H, len);
	}
	free(target.px);
	free(rgba8);
	return png;
}

void tile_profile(ImDrawList *dl, const TileBox &box)
{
	const float w = box.b.x - box.a.x, h = box.b.y - box.a.y, k = box.scale, al = box.alpha;
	const float x = box.a.x + S(18) * k;
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;
	const char *name = signed_in ? g_app.gamertag : "Entrar no EPTX";
	text(dl, g_regular, 34 * k, ImVec2(x, box.a.y + S(30) * k), rgba(255, 255, 255, al), name);
	const char *status = signed_in                               ? "Online"
		: g_app.session == SESSION_SIGNING_IN                     ? "Entrando..."
		: g_app.session == SESSION_STARTING                       ? "Conectando..."
		: g_app.session == SESSION_NO_SERVER                      ? "Servidor fora do ar"
																  : "Aperte A para entrar";
	text(dl, g_regular, 22 * k, ImVec2(x, box.a.y + S(74) * k), rgba(240, 255, 220, 0.9f * al), status);

	if (g_app.library_count > 0) {
		text(dl, g_regular, 20 * k, ImVec2(x, box.a.y + S(156) * k), rgba(255, 255, 255, 0.9f * al), "Jogos instalados");
		const float side = S(58) * k;
		for (int i = 0; i < 4 && i < g_app.library_count; i++) {
			const ImVec2 a(x + (float)i * (side + S(8) * k), box.a.y + S(184) * k);
			const ImVec2 b(a.x + side, a.y + side);
			if (g_library_art[i].valid)
				dl->AddImage(tex_id(g_library_art[i]), a, b, ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, al));
			else
				dl->AddRectFilled(a, b, rgba(255, 255, 255, 0.25f * al));
			dl->AddRect(a, b, rgba(255, 255, 255, 0.6f * al), 0, 0, S(1));
		}
	}
	// O avatar fica de pe na frente do bloco, passando da borda de baixo, como na NXE.
	const ImVec2 feet(box.a.x + w * 0.76f, box.b.y + h * 0.11f);
	if (signed_in && g_app.has_look) {
		draw_look(dl, feet, h * 1.14f, g_app.look, al);
	} else if (signed_in && g_app.avatar_gif && my_avatar()) {
		// O GIF do 360sona num quadrinho a direita; roda quando este bloco esta escolhido.
		const float side = fminf(w * 0.42f, h * 0.72f);
		const ImVec2 a(box.b.x - side - S(18) * k, box.a.y + (h - side) / 2);
		soft_shadow(dl, a, ImVec2(a.x + side, a.y + side), S(4), ImVec2(S(3), S(5)), S(8), 0.45f * al);
		avatar_card(dl, a, side, *my_avatar(), al, box.depth < 0.5f);
	} else
		draw_avatar(dl, feet, h * 1.14f, signed_in ? g_app.gamertag : nullptr, al, signed_in ? my_avatar() : nullptr);
}

void draw_tile(ImDrawList *dl, const Tile &tile, const TileBox &box)
{
	char line[192], size[24];
	tile_panel(dl, box);
	switch (tile.kind) {
	case TILE_PROFILE:
		if (box.depth < 0.5f) {
			tile_profile(dl, box);
		} else {
			tile_icon(dl, box, ICON_PERSON);
			tile_label(dl, box, g_app.session == SESSION_SIGNED_IN ? g_app.gamertag : "Perfil", nullptr);
		}
		break;
	case TILE_LIBRARY:
		snprintf(line, sizeof line, "%d instalado(s)", g_app.library_count);
		tile_icon(dl, box, ICON_CONTROLLER);
		tile_label(dl, box, "Biblioteca de Jogos", line);
		break;
	case TILE_DOWNLOADS:
		tile_icon(dl, box, ICON_DOWNLOAD);
		if (g_app.installing && g_app.install_total > 0) {
			snprintf(line, sizeof line, "%s  %d%%", g_app.install_name,
				(int)(g_app.install_done * 100 / g_app.install_total));
			tile_label(dl, box, "Downloads", line);
			const float w = box.b.x - box.a.x, h = box.b.y - box.a.y;
			const ImVec2 a(box.a.x + w * 0.12f, box.a.y + h * 0.66f), b(box.b.x - w * 0.12f, a.y + S(8) * box.scale);
			dl->AddRectFilled(a, b, rgba(30, 70, 0, 0.5f * box.alpha));
			dl->AddRectFilled(a,
				ImVec2(a.x + (b.x - a.x) * (float)((double)g_app.install_done / (double)g_app.install_total), b.y),
				rgba(255, 255, 255, box.alpha));
		} else {
			tile_label(dl, box, "Downloads", "Nada baixando");
		}
		break;
	case TILE_DASHBOARD:
		tile_icon(dl, box, ICON_HOME);
		tile_label(dl, box, "Dashboard Original", "Volta para a dash do console");
		break;
	case TILE_STORE_ITEM: {
		const store_item &s = g_app.store[tile.index];
		human_size(s.size, size, sizeof size);
		snprintf(line, sizeof line, "%s  %s", s.kind, size);
		tile_cover(dl, box, g_store_art[tile.index], ICON_CONTROLLER);
		tile_label(dl, box, s.name, line);
		break;
	}
	case TILE_STORE_EMPTY:
		tile_icon(dl, box, ICON_BAG);
		tile_label(dl, box,
			g_app.session == SESSION_SIGNED_IN ? (g_app.store_loaded ? "Loja vazia" : "Carregando a loja...")
											   : "Entre no EPTX para ver a loja",
			nullptr);
		break;
	case TILE_FRIEND: {
		const friend_item &f = g_app.friends[tile.index];
		const float w = box.b.x - box.a.x, h = box.b.y - box.a.y;
		// Com avatar, o amigo aparece de corpo inteiro em cima do rotulo; sem, o gamerpic com a inicial.
		if (f.has_look) {
			draw_look(dl, ImVec2(box.a.x + w * 0.5f, box.b.y - h * 0.27f), h * 0.62f, f.look, box.alpha);
		} else if (f.avatar_gif && friend_avatar(tile.index)) {
			const float side = fminf(w * 0.7f, h * 0.56f);
			avatar_card(dl, ImVec2(box.a.x + (w - side) / 2, box.a.y + h * 0.1f), side, *friend_avatar(tile.index),
				box.alpha, box.depth < 0.5f);
		} else if (const Art *avatar = friend_avatar(tile.index)) {
			draw_avatar_image(dl, ImVec2(box.a.x + w * 0.5f, box.b.y - h * 0.27f), h * 0.64f, *avatar, box.alpha);
		} else {
			const float side = h * 0.38f;
			gamerpic(dl, ImVec2(box.a.x + (w - side) / 2, box.a.y + h * 0.16f), side, f.gamertag, box.alpha);
		}
		if (f.online)
			snprintf(line, sizeof line, "Online%s%s", f.title_name[0] ? " - " : "", f.title_name);
		tile_label(dl, box, f.gamertag, f.online ? line : "Offline");
		break;
	}
	case TILE_FRIENDS_EMPTY:
		tile_icon(dl, box, ICON_FRIENDS);
		tile_label(dl, box, g_app.session == SESSION_SIGNED_IN ? "Nenhum amigo ainda" : "Entre para ver seus amigos",
			nullptr);
		break;
	case TILE_SERVER:
		tile_icon(dl, box, ICON_SERVER);
		tile_label(dl, box, "Servidor EPTX", g_app.server[0] ? g_app.server : "nao configurado");
		break;
	case TILE_REFRESH:
		tile_icon(dl, box, ICON_REFRESH);
		tile_label(dl, box, "Atualizar", "Loja, amigos e biblioteca");
		break;
	case TILE_ABOUT:
		tile_icon(dl, box, ICON_INFO);
		tile_label(dl, box, "Sobre", "EPTX Live e licencas");
		break;
	}
}

void draw_tiles(ImDrawList *dl, const Tile *tiles, int count, int channel)
{
	const float pos = g_ui.tile_pos[channel];
	// Do fundo para a frente, para o focado ficar por cima.
	for (int i = count - 1; i >= 0; i--) {
		const float d = (float)i - pos;
		if (d < -1.2f || d > (float)(TILE_STEPS - 1))
			continue;
		const TileBox box = tile_box(d);
		if (box.alpha <= 0.01f)
			continue;
		const int start = mark(dl);
		draw_tile(dl, tiles[i], box);
		perspective_since(dl, start, box);
	}
	char where[32];
	snprintf(where, sizeof where, "%d de %d", g_ui.tile[channel] + 1, count);
	text(dl, g_regular, 19, P(36, 598), rgba(100, 106, 112), where, false);
}

// --- topo, rodape e avisos -----------------------------------------------------------------

void draw_profile_corner(ImDrawList *dl)
{
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;
	const char *name = signed_in ? g_app.gamertag : "Offline";
	const float side = 62;
	const float w = text_width(g_regular, 28, name);
	text(dl, g_regular, 28, ImVec2(P(1158, 30).x - w, P(0, 30).y), rgba(255, 255, 255), name);
	const char *sub = signed_in ? "EPTX Live" : g_app.session == SESSION_NO_SERVER ? "sem servidor" : "nao conectado";
	const float sw = text_width(g_regular, 20, sub);
	text(dl, g_regular, 20, ImVec2(P(1158, 0).x - sw, P(0, 66).y), rgba(255, 255, 255, 0.9f), sub);
	dl->AddCircleFilled(ImVec2(P(1158, 0).x - sw - S(12), P(0, 79).y), S(5),
		signed_in ? rgba(90, 200, 40) : rgba(150, 150, 150));
	soft_shadow(dl, P(1172, 30), P(1172 + side, 30 + side), S(3), ImVec2(S(2), S(4)), S(6), 0.4f);
	if (signed_in && g_app.has_look)
		look_headshot(dl, P(1172, 30), S(side), g_app.look, 1);
	else if (signed_in && g_app.avatar_gif && my_avatar())
		avatar_card(dl, P(1172, 30), S(side), *my_avatar(), 1);
	else if (signed_in && my_avatar())
		avatar_headshot(dl, P(1172, 30), S(side), *my_avatar(), 1);
	else if (signed_in)
		gamerpic(dl, P(1172, 30), S(side), g_app.gamertag, 1);
	else {
		dl->AddRectFilled(P(1172, 30), P(1172 + side, 30 + side), rgba(255, 255, 255, 0.35f));
		draw_icon(dl, ICON_PERSON, P(1172 + side / 2, 30 + side / 2), S(side * 0.4f), 1);
	}
}

struct ButtonColour {
	int r, g, b;
};
constexpr ButtonColour BUTTON_A = { 80, 170, 30 }, BUTTON_B = { 210, 40, 40 }, BUTTON_X = { 30, 100, 210 },
					   BUTTON_Y = { 230, 180, 20 };

// Um botao do controle e o que ele faz, em linha; x anda para o proximo.
void button_hint(ImDrawList *dl, float &x, float y, const char *letter, ButtonColour colour, const char *label,
	ImU32 label_colour, float alpha = 1)
{
	orb(dl, P(x + 13, y), S(13), colour.r, colour.g, colour.b, letter, alpha);
	text(dl, g_regular, 21, P(x + 33, y - 13), label_colour, label, false);
	x += 33 + text_width(g_regular, 21, label) / g_sx + 26;
}

// O orbe de vidro com a marca do EPTX, no canto onde a NXE tem o botao do Guide.
void emblem(ImDrawList *dl, ImVec2 c, float r, float alpha = 1)
{
	dl->AddEllipseFilled(ImVec2(c.x, c.y + r * 1.05f), ImVec2(r * 0.9f, r * 0.22f), rgba(0, 0, 0, 0.25f * alpha), 0, 32);
	int start = mark(dl);
	dl->AddCircleFilled(c, r, IM_COL32_WHITE, 48);
	shade_vertical(dl, start, ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), rgba(250, 252, 250, alpha),
		rgba(170, 176, 180, alpha));
	start = mark(dl);
	dl->AddCircle(c, r * 0.72f, IM_COL32_WHITE, 48, r * 0.2f);
	shade_vertical(dl, start, ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), rgba(170, 230, 60, alpha),
		rgba(60, 130, 10, alpha));
	const float size = r / g_sy * 0.85f;
	const float w = text_width(g_semibold, size, "E");
	dl->AddText(g_semibold, S(size), ImVec2(c.x - w / 2, c.y - S(size) * 0.62f), rgba(80, 140, 20, alpha), "E");
	start = mark(dl);
	dl->AddEllipseFilled(ImVec2(c.x, c.y - r * 0.45f), ImVec2(r * 0.7f, r * 0.42f), IM_COL32_WHITE, 0, 32);
	shade_vertical(dl, start, ImVec2(c.x, c.y - r * 0.9f), ImVec2(c.x, c.y), rgba(255, 255, 255, 0.85f * alpha),
		rgba(255, 255, 255, 0.05f * alpha));
	dl->AddCircle(c, r, rgba(120, 126, 130, 0.8f * alpha), 48, S(1));
}

void draw_emblem(ImDrawList *dl) { emblem(dl, P(1210, 660), S(27)); }

void draw_toast(ImDrawList *dl, float dt)
{
	if (g_app.message_serial != g_ui.seen_message) {
		g_ui.seen_message = g_app.message_serial;
		snprintf(g_ui.toast, sizeof g_ui.toast, "%s", g_app.message);
		g_ui.toast_time = TOAST_SECONDS;
	}
	// Duas linhas, como "Achievement unlocked / 20G - ...": o que e, e o detalhe.
	char first[200], second[200];
	const char *line1 = nullptr, *line2 = nullptr;
	float progress = -1;
	if (g_app.installing) {
		progress = g_app.install_total > 0 ? (float)((double)g_app.install_done / (double)g_app.install_total) : 0;
		snprintf(first, sizeof first, "Baixando %s", g_app.install_name);
		char done[24], total[24];
		human_size(g_app.install_done, done, sizeof done);
		human_size(g_app.install_total, total, sizeof total);
		snprintf(second, sizeof second, "%d%% - %s de %s - arquivo %d de %d", (int)(progress * 100), done, total,
			g_app.install_file, g_app.install_files);
		line1 = first;
		line2 = second;
	} else if (g_ui.toast_time > 0) {
		g_ui.toast_time -= dt;
		line1 = "EPTX Live";
		line2 = g_ui.toast;
	}
	if (!line1)
		return;
	const float alpha = progress >= 0 ? 1 : clamp01(g_ui.toast_time * 2);

	// A capsula: aro claro em volta, corpo grafite com reflexo em cima e o orbe na ponta esquerda.
	const float h = S(66);
	const float w = fminf(fmaxf(text_width(g_regular, 21, line1), text_width(g_regular, 18, line2)) + h + S(40), S(820));
	const ImVec2 a(g_width / 2 - w / 2, P(0, 586).y), b(g_width / 2 + w / 2, P(0, 586).y + h);
	const float r = h / 2;
	soft_shadow(dl, a, b, r, ImVec2(0, S(4)), S(10), 0.5f * alpha);
	round_gradient(dl, ImVec2(a.x - S(3), a.y - S(3)), ImVec2(b.x + S(3), b.y + S(3)), r + S(3),
		rgba(225, 228, 230, alpha), rgba(70, 74, 78, alpha));
	round_gradient(dl, a, b, r, rgba(74, 78, 82, 0.97f * alpha), rgba(16, 17, 18, 0.97f * alpha));
	round_gradient(dl, ImVec2(a.x + S(6), a.y + S(2)), ImVec2(b.x - S(6), a.y + h * 0.48f), r * 0.9f,
		rgba(255, 255, 255, 0.24f * alpha), rgba(255, 255, 255, 0.03f * alpha));

	// Orbe: anel cinza com o progresso em verde por cima; num aviso, o anel todo verde.
	const ImVec2 c(a.x + r, a.y + r);
	const float orb_r = r - S(8);
	int start = mark(dl);
	dl->AddCircleFilled(c, orb_r, IM_COL32_WHITE, 40);
	shade_vertical(dl, start, ImVec2(c.x, c.y - orb_r), ImVec2(c.x, c.y + orb_r), rgba(70, 74, 78, alpha),
		rgba(20, 22, 24, alpha));
	dl->AddCircle(c, orb_r * 0.74f, rgba(255, 255, 255, 0.16f * alpha), 40, S(6));
	const float sweep = progress >= 0 ? clamp01(progress) : 1;
	if (sweep > 0.005f) {
		dl->PathArcTo(c, orb_r * 0.74f, -PI / 2, -PI / 2 + 2 * PI * sweep, 48);
		start = mark(dl);
		dl->PathStroke(IM_COL32_WHITE, 0, S(6));
		shade_vertical(dl, start, ImVec2(c.x, c.y - orb_r), ImVec2(c.x, c.y + orb_r), rgba(190, 240, 80, alpha),
			rgba(70, 150, 10, alpha));
	}
	if (progress >= 0)
		draw_icon(dl, ICON_DOWNLOAD, ImVec2(c.x, c.y - S(1)), orb_r * 0.36f, alpha);
	else
		emblem(dl, c, orb_r * 0.5f, alpha);
	start = mark(dl);
	dl->AddEllipseFilled(ImVec2(c.x, c.y - orb_r * 0.45f), ImVec2(orb_r * 0.75f, orb_r * 0.4f), IM_COL32_WHITE, 0, 32);
	shade_vertical(dl, start, ImVec2(c.x, c.y - orb_r), ImVec2(c.x, c.y), rgba(255, 255, 255, 0.35f * alpha),
		rgba(255, 255, 255, 0.0f));

	const float tx = a.x + h + S(8), max_w = b.x - tx - S(24);
	dl->AddText(g_regular, S(21), ImVec2(tx, a.y + S(9)), rgba(255, 255, 255, alpha), fit(g_regular, 21, line1, max_w));
	dl->AddText(g_regular, S(18), ImVec2(tx, a.y + S(36)), rgba(205, 210, 214, alpha), fit(g_regular, 18, line2, max_w));
}

// --- paineis por cima (detalhe do jogo, biblioteca, perfil, sobre) ----------------------------

// Painel grafite do Guide: degrade, aro fino claro e sombra.
void slate(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, float alpha)
{
	soft_shadow(dl, a, b, rounding, ImVec2(0, S(8)), S(18), 0.6f * alpha);
	round_gradient(dl, ImVec2(a.x - S(1.5f), a.y - S(1.5f)), ImVec2(b.x + S(1.5f), b.y + S(1.5f)), rounding + S(1.5f),
		rgba(150, 158, 166, alpha), rgba(40, 44, 48, alpha));
	round_gradient(dl, a, b, rounding, rgba(80, 90, 100, alpha), rgba(46, 52, 58, alpha));
	round_gradient(dl, ImVec2(a.x + S(2), a.y + S(2)), ImVec2(b.x - S(2), a.y + S(60)), rounding,
		rgba(255, 255, 255, 0.10f * alpha), rgba(255, 255, 255, 0), ImDrawFlags_RoundCornersTop);
}

void panel_frame(ImDrawList *dl, const char *title, float alpha)
{
	dl->AddRectFilled(ImVec2(0, 0), ImVec2(g_width, g_height), rgba(0, 0, 0, 0.55f * alpha));
	const ImVec2 a = P(140, 90), b = P(1140, 600);
	slate(dl, a, b, S(10), alpha);
	round_gradient(dl, a, ImVec2(b.x, a.y + S(8)), S(10), rgba(196, 232, 60, alpha), rgba(110, 170, 20, alpha),
		ImDrawFlags_RoundCornersTop);
	text(dl, g_semibold, 34, P(180, 118), rgba(255, 255, 255, alpha), fit(g_semibold, 34, title, S(900)));
}

// Texto branco com sombra escura em cima da barra verde, como "Skin" na foto.
void bar_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, bool selected, const char *s, float alpha)
{
	if (selected)
		dl->AddText(font, S(size), ImVec2(pos.x + S(1), pos.y + S(1.5f)), rgba(20, 50, 0, 0.7f * alpha), s);
	dl->AddText(font, S(size), pos, rgba(240, 244, 246, alpha), s);
}

void menu_items(ImDrawList *dl, const char *const *items, int count, int selected, float x, float y, float width,
	float alpha)
{
	for (int i = 0; i < count; i++) {
		const ImVec2 a = P(x, y + (float)i * 52), b = P(x + width, y + (float)i * 52 + 42);
		if (i == selected)
			aero_bar(dl, a, b, alpha);
		else
			dl->AddRectFilled(a, b, rgba(255, 255, 255, 0.05f * alpha), S(5));
		bar_text(dl, g_regular, 24, ImVec2(a.x + S(14), a.y + S(7)), i == selected, items[i], alpha);
	}
}

const char *const STORE_MENU[] = { "Baixar e instalar", "Voltar" };
const char *const PROFILE_MENU[] = { "Editor de Avatar", "Sair do EPTX", "Voltar" };
const char *const LAUNCH_MENU[] = { "Jogar (experimental)", "Voltar" };

bool installed(const store_item &s)
{
	for (int i = 0; i < g_app.library_count; i++)
		if (strcasecmp(g_app.library[i].title_id, s.title_id) == 0)
			return true;
	return false;
}

void draw_store_item(ImDrawList *dl, float alpha)
{
	const store_item &s = g_app.store[g_ui.store_index];
	panel_frame(dl, s.name, alpha);
	const ImVec2 a = P(180, 180), b = P(420, 420);
	if (g_store_art[g_ui.store_index].valid)
		dl->AddImage(tex_id(g_store_art[g_ui.store_index]), a, b, ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, alpha));
	else {
		dl->AddRectFilledMultiColor(a, b, rgba(190, 226, 40, alpha), rgba(160, 210, 30, alpha), rgba(84, 146, 14, alpha),
			rgba(100, 160, 20, alpha));
		draw_icon(dl, ICON_CONTROLLER, P(300, 290), S(50), alpha);
	}
	char line[128], size[24];
	human_size(s.size, size, sizeof size);
	snprintf(line, sizeof line, "%s   |   %s   |   Title ID %s", s.kind, size, s.title_id);
	text(dl, g_regular, 22, P(460, 186), rgba(220, 224, 228, alpha), line, false);
	const char *where = !s.installable ? "Nao instalavel: ISO precisa virar GOD antes."
		: installed(s)                 ? "Ja esta na biblioteca deste console."
		: s.shows_in_dash              ? "Aparece em Meus Jogos da dash original."
									   : "Vai para Hdd1:\\Games (precisa de dash homebrew).";
	text(dl, g_regular, 22, P(460, 226), rgba(200, 230, 140, alpha), where, false);
	if (s.installable)
		menu_items(dl, STORE_MENU, 2, g_ui.menu, 460, 300, 400, alpha);
	else
		menu_items(dl, STORE_MENU + 1, 1, 0, 460, 300, 400, alpha);
}

void draw_library(ImDrawList *dl, float alpha, float dt)
{
	panel_frame(dl, "Biblioteca de Jogos", alpha);
	if (!g_app.library_count) {
		text(dl, g_regular, 24, P(180, 190), rgba(220, 224, 228, alpha),
			"Nenhum jogo instalado ainda. Baixe pela Loja de Jogos.", false);
		return;
	}
	constexpr int ROWS = 6;
	if ((float)g_ui.library_index < g_ui.library_scroll)
		g_ui.library_scroll = (float)g_ui.library_index;
	if ((float)g_ui.library_index > g_ui.library_scroll + ROWS - 1)
		g_ui.library_scroll = (float)(g_ui.library_index - ROWS + 1);
	static float shown = 0;
	shown = approach(shown, g_ui.library_scroll, dt);
	dl->PushClipRect(P(150, 170), P(1130, 590), true);
	for (int i = 0; i < g_app.library_count; i++) {
		const float y = 180 + ((float)i - shown) * 66;
		if (y < 120 || y > 600)
			continue;
		const library_item &it = g_app.library[i];
		const ImVec2 a = P(180, y), b = P(1100, y + 58);
		if (i == g_ui.library_index)
			aero_bar(dl, a, b, alpha);
		const ImVec2 ia = P(186, y + 4), ib = P(236, y + 54);
		if (g_library_art[i].valid)
			dl->AddImage(tex_id(g_library_art[i]), ia, ib, ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, alpha));
		else
			dl->AddRectFilled(ia, ib, rgba(255, 255, 255, 0.15f * alpha));
		const bool chosen = i == g_ui.library_index;
		bar_text(dl, g_regular, 24, P(252, y + 4), chosen, fit(g_regular, 24, it.name, S(700)), alpha);
		bar_text(dl, g_regular, 17, P(252, y + 34), chosen, it.content_type == 0x7000 ? "Games on Demand" : "Arcade",
			0.8f * alpha);
	}
	dl->PopClipRect();
}

void draw_about(ImDrawList *dl, float alpha)
{
	panel_frame(dl, "Sobre o EPTX Live", alpha);
	static const char *const lines[] = {
		"Dash homebrew do EPTX Live, no estilo da NXE. Nao fala com a Microsoft.",
		"",
		"Feito com:",
		"  xsdk - MIT. Partes de libxenon e Xenia sob BSD de 3 clausulas.",
		"  Dear ImGui (imgui-xenon) - MIT, (c) Omar Cornut.",
		"  Fonte Selawik - SIL Open Font License 1.1, (c) Microsoft.",
		"  stb_image - dominio publico, Sean Barrett.",
		"",
		"Os textos completos das licencas estao na pasta licenses do pacote.",
	};
	for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
		text(dl, g_regular, 22, P(180, 180 + (float)i * 34), rgba(220, 224, 228, alpha), lines[i], false);
}

// As direcoes que directions() devolve, um bit cada.
enum { DIR_UP = 1, DIR_DOWN = 2, DIR_LEFT = 4, DIR_RIGHT = 8 };

// --- editor de avatar ------------------------------------------------------------------------
//
// As partes numa lista a esquerda (cima e baixo), a opcao de cada uma trocando
// com esquerda e direita, e o boneco grande num palco a direita, mudando na hora.

void draw_avatar_editor(ImDrawList *dl, float alpha)
{
	dl->AddRectFilled(ImVec2(0, 0), ImVec2(g_width, g_height), rgba(0, 0, 0, 0.55f * alpha));

	// O palco: luz redonda atras do boneco e o disco onde ele pisa.
	const ImVec2 stage = P(900, 590);
	for (int i = 6; i >= 1; i--)
		dl->AddCircleFilled(P(900, 370), S(60.0f * (float)i), rgba(200, 236, 255, 0.05f * alpha), 64);
	dl->AddEllipseFilled(ImVec2(stage.x, stage.y + S(6)), ImVec2(S(170), S(34)), rgba(0, 0, 0, 0.35f * alpha), 0, 64);
	int start = mark(dl);
	dl->AddEllipseFilled(stage, ImVec2(S(160), S(30)), IM_COL32_WHITE, 0, 64);
	shade_vertical(dl, start, ImVec2(0, stage.y - S(30)), ImVec2(0, stage.y + S(30)), rgba(240, 246, 250, alpha),
		rgba(150, 170, 186, alpha));
	dl->AddEllipse(stage, ImVec2(S(160), S(30)), rgba(170, 220, 60, 0.9f * alpha), 0, 64, S(3));
	// Respira de leve, como os avatares parados da NXE.
	const float breathe = 1.0f + 0.006f * sinf(g_ui.time * 2.2f);
	draw_look(dl, ImVec2(stage.x, stage.y), S(390) * breathe, g_ui.edit_look, alpha);

	const ImVec2 a = P(110, 80), b = P(640, 640);
	slate(dl, a, b, S(10), alpha);
	round_gradient(dl, a, ImVec2(b.x, a.y + S(8)), S(10), rgba(196, 232, 60, alpha), rgba(110, 170, 20, alpha),
		ImDrawFlags_RoundCornersTop);
	text(dl, g_semibold, 32, P(140, 102), rgba(255, 255, 255, alpha), "Editor de Avatar");
	if (g_app.avatar_saving)
		text(dl, g_regular, 20, P(470, 112), rgba(200, 230, 140, alpha), "Salvando...", false);

	for (int i = 0; i < LOOK_FIELDS; i++) {
		const float y = 152 + (float)i * 29.5f;
		const ImVec2 ra = P(126, y), rb = P(624, y + 27);
		const bool chosen = i == g_ui.edit_field;
		if (chosen)
			aero_bar(dl, ra, rb, alpha);
		bar_text(dl, g_regular, 20, ImVec2(ra.x + S(10), ra.y + S(3)), chosen, LOOK_FIELD_NAMES[i], alpha);
		const int value = g_ui.edit_look.v[i];
		const char *name = look_option_name(i, value);
		float right = 600;
		if (chosen) { // setas de troca
			bar_text(dl, g_semibold, 20, ImVec2(P(right, 0).x, ra.y + S(3)), true, ">", alpha);
			right -= 8;
		}
		const float w = text_width(g_regular, 20, name) / g_sx;
		bar_text(dl, g_regular, 20, ImVec2(P(right - w, 0).x, ra.y + S(3)), chosen, name, alpha);
		if (chosen)
			bar_text(dl, g_semibold, 20, ImVec2(P(right - w - 22, 0).x, ra.y + S(3)), true, "<", alpha);
		if (const Rgb *c = look_option_colour(i, value)) {
			const ImVec2 dot = P(right - w - (chosen ? 40 : 18), y + 13.5f);
			dl->AddCircleFilled(dot, S(8), rgba(255, 255, 255, 0.9f * alpha), 20);
			dl->AddCircleFilled(dot, S(6.5f), rgba(c->r, c->g, c->b, alpha), 20);
		}
	}
}

void open_avatar_editor()
{
	g_ui.view = VIEW_AVATAR_EDITOR;
	g_ui.edit_field = 0;
	memcpy(g_ui.edit_look.v, AVATAR_DEFAULT_LOOK, sizeof g_ui.edit_look.v);
	if (g_app.has_look)
		g_ui.edit_look = g_app.look;
}

void avatar_editor_input(uint16_t pressed, int dir)
{
	if (dir & DIR_DOWN)
		g_ui.edit_field = (g_ui.edit_field + 1) % LOOK_FIELDS;
	if (dir & DIR_UP)
		g_ui.edit_field = (g_ui.edit_field + LOOK_FIELDS - 1) % LOOK_FIELDS;
	uint8_t &value = g_ui.edit_look.v[g_ui.edit_field];
	const int options = AVATAR_OPTIONS[g_ui.edit_field];
	if (dir & DIR_RIGHT)
		value = (uint8_t)((value + 1) % options);
	if (dir & DIR_LEFT)
		value = (uint8_t)((value + options - 1) % options);
	if (pressed & INPUT_GAMEPAD_X) { // aleatorio
		static uint32_t seed = (uint32_t)time(nullptr) | 1;
		for (int i = 0; i < LOOK_FIELDS; i++) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			g_ui.edit_look.v[i] = (uint8_t)(seed % AVATAR_OPTIONS[i]);
		}
	}
	if (pressed & INPUT_GAMEPAD_Y)
		memcpy(g_ui.edit_look.v, AVATAR_DEFAULT_LOOK, sizeof g_ui.edit_look.v);
	if (pressed & INPUT_GAMEPAD_A && !g_app.avatar_saving) {
		int len = 0;
		uint8_t *png = look_png(g_ui.edit_look, &len);
		g_ui.view = VIEW_HOME;
		if (!png) {
			snprintf(g_app.message, sizeof g_app.message, "Nao consegui desenhar o avatar para salvar.");
			g_app.message_serial++;
			return;
		}
		const avatar_look look = g_ui.edit_look;
		app_unlock();
		app_save_avatar(&look, png, len);
		app_lock();
	}
}

// --- teclado do XAM ----------------------------------------------------------------------------

// KB_WAITING: um teclado (g_kb_next) espera a tela do sistema fechar para abrir. O XAM recusa
// com 5 (acesso negado) enquanto qualquer tela dele esta aberta, ate uns 0,4 s depois de um
// teclado fechar (xsdk:docs/HARDWARE.md, "XAM`s dialogs").
enum Keyboard { KB_NONE, KB_GAMERTAG, KB_PASSWORD, KB_WAITING };
Keyboard g_kb = KB_NONE;
Keyboard g_kb_next;
constexpr uint32_t XAM_ACCESS_DENIED = 5; // ERROR_ACCESS_DENIED, que o xsdk nao nomeia
int g_kb_wait_frames; // quadros sem tela do sistema antes de tentar
int g_kb_tries;

void open_keyboard(Keyboard which);
void open_keyboard_soon(Keyboard which)
{
	g_kb = KB_WAITING;
	g_kb_next = which;
	g_kb_wait_frames = 0;
}
XAM_OVERLAPPED g_kb_overlapped;
uint16_t g_kb_text[64];
char g_kb_gamertag[40];

void to_utf16(const char *s, uint16_t *out, size_t cap)
{
	size_t i = 0;
	for (; s[i] && i + 1 < cap; i++)
		out[i] = (uint8_t)s[i];
	out[i] = 0;
}

// O teclado so devolve o que da para digitar no controle; o que nao for ASCII vira '?'.
void from_utf16(const uint16_t *s, char *out, size_t cap)
{
	size_t i = 0;
	for (; s[i] && i + 1 < cap; i++)
		out[i] = s[i] < 0x80 ? (char)s[i] : '?';
	out[i] = 0;
}

uint32_t keyboard_user()
{
	uint32_t user = XSDK_XAM_USER_NONE;
	char gamertag[XSDK_XAM_GAMERTAG_BYTES];
	if (xsdk_xam_signed_in_user(&user, gamertag) != ERROR_SUCCESS)
		user = XSDK_XAM_USER_NONE;
	return user;
}

void open_keyboard(Keyboard which)
{
	static uint16_t title[64], description[128], initial[64];
	if (which == KB_GAMERTAG) {
		to_utf16("Entrar no EPTX", title, 64);
		to_utf16("Gamertag da sua conta no servidor EPTX", description, 128);
		to_utf16(g_kb_gamertag, initial, 64);
	} else {
		to_utf16("Entrar no EPTX", title, 64);
		to_utf16("Senha da conta EPTX", description, 128);
		initial[0] = 0;
	}
	memset(g_kb_text, 0, sizeof g_kb_text);
	const uint32_t result =
		xsdk_xam_keyboard(keyboard_user(), title, description, initial, g_kb_text, 63, &g_kb_overlapped);
	if (result != ERROR_IO_PENDING && result != ERROR_SUCCESS) {
		xsdk_log_write("eptx: teclado nao abriu: 0x%08X", (unsigned)result);
		// Tenta de novo daqui a pouco: o XAM recusa enquanto uma tela dele ainda fecha.
		if (result == XAM_ACCESS_DENIED && ++g_kb_tries < 8) {
			open_keyboard_soon(which);
			return;
		}
		g_kb = KB_NONE;
		app_lock();
		snprintf(g_app.message, sizeof g_app.message, "O teclado do console nao abriu (0x%08X). Tente de novo.",
			(unsigned)result);
		g_app.message_serial++;
		app_unlock();
		return;
	}
	g_kb = which;
}

// O A no perfil: comeca pelo teclado da gamertag, com as tentativas zeradas.
void start_login()
{
	g_kb_tries = 0;
	open_keyboard(KB_GAMERTAG);
}

void poll_keyboard(bool system_ui)
{
	if (g_kb == KB_NONE)
		return;
	// So abre o teclado depois que a tela do sistema sumiu de vez (um quarto de segundo sem ela).
	if (g_kb == KB_WAITING) {
		if (system_ui)
			g_kb_wait_frames = 0;
		else if (++g_kb_wait_frames >= 15)
			open_keyboard(g_kb_next);
		return;
	}
	const uint32_t result = xsdk_xam_wait(&g_kb_overlapped, 0, nullptr);
	if (result == ERROR_IO_INCOMPLETE)
		return;
	const Keyboard was = g_kb;
	g_kb = KB_NONE;
	if (result != ERROR_SUCCESS)
		return; // cancelou
	if (was == KB_GAMERTAG) {
		from_utf16(g_kb_text, g_kb_gamertag, sizeof g_kb_gamertag);
		xsdk_log_write("eptx: gamertag digitada: \"%s\"", g_kb_gamertag);
		if (g_kb_gamertag[0]) {
			g_kb_tries = 0;
			open_keyboard_soon(KB_PASSWORD);
		}
	} else {
		char password[64];
		from_utf16(g_kb_text, password, sizeof password);
		if (password[0])
			app_sign_in(g_kb_gamertag, password);
		memset(password, 0, sizeof password);
		memset(g_kb_text, 0, sizeof g_kb_text);
	}
}

// --- controle -------------------------------------------------------------------------------------

struct Pad {
	uint16_t held = 0, previous = 0;
	float repeat_time = 0;
	int repeat_dir = 0; // botao direcional que esta repetindo
};
Pad g_pad;

// Direcional e analogico esquerdo, com repeticao ao segurar, como na dash.
int directions(const INPUT_GAMEPAD &pad, float dt)
{
	int now = 0;
	if (pad.buttons & INPUT_GAMEPAD_DPAD_UP || pad.thumb_ly > 16000)
		now |= DIR_UP;
	if (pad.buttons & INPUT_GAMEPAD_DPAD_DOWN || pad.thumb_ly < -16000)
		now |= DIR_DOWN;
	if (pad.buttons & INPUT_GAMEPAD_DPAD_LEFT || pad.thumb_lx < -16000)
		now |= DIR_LEFT;
	if (pad.buttons & INPUT_GAMEPAD_DPAD_RIGHT || pad.thumb_lx > 16000)
		now |= DIR_RIGHT;
	if (!(now & g_pad.repeat_dir)) {
		g_pad.repeat_dir = now & -now; // um de cada vez
		g_pad.repeat_time = 0.38f;
		return g_pad.repeat_dir;
	}
	g_pad.repeat_time -= dt;
	if (g_pad.repeat_time <= 0) {
		g_pad.repeat_time = 0.11f;
		return g_pad.repeat_dir;
	}
	return 0;
}

void launch(const char *path, const char *what)
{
	xsdk_log_write("eptx: abrindo %s: %s", what, path);
	if (xsdk_launch(path, nullptr, 0) != 0) {
		app_lock();
		snprintf(g_app.message, sizeof g_app.message, "Nao consegui abrir %s (errno %d).", what, errno);
		g_app.message_serial++;
		app_unlock();
	}
}

void activate_tile(const Tile &tile)
{
	switch (tile.kind) {
	case TILE_PROFILE:
		if (g_app.session == SESSION_SIGNED_IN) {
			g_ui.view = VIEW_PROFILE;
			g_ui.menu = 0;
		} else if (g_app.session == SESSION_SIGNED_OUT) {
			start_login();
		}
		break;
	case TILE_LIBRARY:
		g_ui.view = VIEW_LIBRARY;
		g_ui.library_index = 0;
		break;
	case TILE_DOWNLOADS:
		g_ui.channel = CHANNEL_STORE;
		break;
	case TILE_DASHBOARD:
		// Sai do titulo; com o EPTX como dash padrao do DashLaunch, veja o README antes.
		app_unlock();
		launch("\\SystemRoot\\dash.xex", "a dashboard original");
		app_lock();
		break;
	case TILE_STORE_ITEM:
		g_ui.view = VIEW_STORE_ITEM;
		g_ui.store_index = tile.index;
		g_ui.menu = g_app.store[tile.index].installable ? 0 : 1;
		break;
	case TILE_REFRESH:
		app_unlock();
		app_refresh();
		app_lock();
		break;
	case TILE_ABOUT:
		g_ui.view = VIEW_ABOUT;
		break;
	case TILE_STORE_EMPTY:
	case TILE_FRIENDS_EMPTY:
		if (g_app.session == SESSION_SIGNED_OUT)
			start_login();
		break;
	case TILE_FRIEND:
	case TILE_SERVER:
		break;
	}
}

// Chamada com g_app travado. Os pedidos a app.c destravam antes, porque eles travam sozinhos.
void guide_go(int channel, int tile);

void sign_out_now()
{
	app_unlock();
	app_sign_out();
	app_lock();
}

void guide_input(uint16_t pressed, int dir)
{
	constexpr int ITEMS = 7; // GUIDE_COUNT
	if (dir & DIR_DOWN)
		g_ui.menu = (g_ui.menu + 1) % ITEMS;
	if (dir & DIR_UP)
		g_ui.menu = (g_ui.menu + ITEMS - 1) % ITEMS;
	if (pressed & (INPUT_GAMEPAD_B | INPUT_GAMEPAD_BACK)) {
		g_ui.view = VIEW_HOME;
		return;
	}
	if (pressed & INPUT_GAMEPAD_Y) {
		guide_go(CHANNEL_MY, 0);
		return;
	}
	if (pressed & INPUT_GAMEPAD_X && g_app.session == SESSION_SIGNED_IN) {
		g_ui.view = VIEW_HOME;
		sign_out_now();
		return;
	}
	if (!(pressed & INPUT_GAMEPAD_A))
		return;
	switch (g_ui.menu) {
	case 0: // Dashboard EPTX
		guide_go(CHANNEL_MY, -1);
		break;
	case 1: // Loja de Jogos
		guide_go(CHANNEL_STORE, -1);
		break;
	case 2: // Amigos
		guide_go(CHANNEL_FRIENDS, -1);
		break;
	case 3: // Biblioteca
		g_ui.view = VIEW_LIBRARY;
		g_ui.library_index = 0;
		break;
	case 4: // Downloads
		guide_go(CHANNEL_MY, 2);
		break;
	case 5: // Dashboard Original
		g_ui.view = VIEW_HOME;
		app_unlock();
		launch("\\SystemRoot\\dash.xex", "a dashboard original");
		app_lock();
		break;
	case 6: // Entrar ou sair
		g_ui.view = VIEW_HOME;
		if (g_app.session == SESSION_SIGNED_IN)
			sign_out_now();
		else if (g_app.session == SESSION_SIGNED_OUT)
			start_login();
		break;
	}
}

bool handle_input(const INPUT_GAMEPAD &pad, float dt, const Tile *tiles, int tile_count)
{
	const uint16_t pressed = pad.buttons & ~g_pad.previous;
	g_pad.previous = pad.buttons;
	const int dir = directions(pad, dt);

	if (g_ui.view == VIEW_GUIDE) {
		guide_input(pressed, dir);
		return true;
	}
	if (g_ui.view == VIEW_AVATAR_EDITOR) {
		if (pressed & (INPUT_GAMEPAD_B | INPUT_GAMEPAD_BACK))
			g_ui.view = VIEW_PROFILE; // cancela: o avatar salvo continua o mesmo
		else
			avatar_editor_input(pressed, dir);
		return true;
	}
	if (g_ui.view == VIEW_HOME && pressed & INPUT_GAMEPAD_BACK) {
		g_ui.view = VIEW_GUIDE;
		g_ui.menu = 0;
		return true;
	}

	if (g_ui.view == VIEW_HOME) {
		if (dir & DIR_DOWN && g_ui.channel + 1 < CHANNEL_COUNT)
			g_ui.channel++;
		if (dir & DIR_UP && g_ui.channel > 0)
			g_ui.channel--;
		int &tile = g_ui.tile[g_ui.channel];
		if (tile >= tile_count)
			tile = tile_count - 1;
		if (dir & DIR_RIGHT && tile + 1 < tile_count)
			tile++;
		if (dir & DIR_LEFT && tile > 0)
			tile--;
		if (pressed & INPUT_GAMEPAD_A && tile_count > 0)
			activate_tile(tiles[tile]);
		if (pressed & INPUT_GAMEPAD_Y) {
			app_unlock();
			app_refresh();
			app_lock();
		}
		return true;
	}

	if (pressed & (INPUT_GAMEPAD_B | INPUT_GAMEPAD_BACK)) {
		g_ui.view = g_ui.view == VIEW_CONFIRM_LAUNCH ? VIEW_LIBRARY : VIEW_HOME;
		return true;
	}
	switch (g_ui.view) {
	case VIEW_STORE_ITEM: {
		const bool installable = g_app.store[g_ui.store_index].installable;
		if (installable && dir & DIR_DOWN)
			g_ui.menu = 1;
		if (installable && dir & DIR_UP)
			g_ui.menu = 0;
		if (pressed & INPUT_GAMEPAD_A) {
			const int64_t id = g_app.store[g_ui.store_index].id;
			const bool go = installable && g_ui.menu == 0;
			g_ui.view = VIEW_HOME;
			if (go) {
				app_unlock();
				app_install(id);
				app_lock();
			}
		}
		break;
	}
	case VIEW_PROFILE:
		if (dir & DIR_DOWN && g_ui.menu < 2)
			g_ui.menu++;
		if (dir & DIR_UP && g_ui.menu > 0)
			g_ui.menu--;
		if (pressed & INPUT_GAMEPAD_A) {
			g_ui.view = VIEW_HOME;
			if (g_ui.menu == 0) {
				open_avatar_editor();
			} else if (g_ui.menu == 1) {
				app_unlock();
				app_sign_out();
				app_lock();
			}
		}
		break;
	case VIEW_LIBRARY:
		if (dir & DIR_DOWN && g_ui.library_index + 1 < g_app.library_count)
			g_ui.library_index++;
		if (dir & DIR_UP && g_ui.library_index > 0)
			g_ui.library_index--;
		if (pressed & INPUT_GAMEPAD_A && g_app.library_count > 0) {
			g_ui.view = VIEW_CONFIRM_LAUNCH;
			g_ui.menu = 0;
		}
		break;
	case VIEW_CONFIRM_LAUNCH:
		if (dir & DIR_DOWN)
			g_ui.menu = 1;
		if (dir & DIR_UP)
			g_ui.menu = 0;
		if (pressed & INPUT_GAMEPAD_A) {
			if (g_ui.menu == 0 && g_ui.library_index < g_app.library_count) {
				char path[300];
				snprintf(path, sizeof path, "%s", g_app.library[g_ui.library_index].path);
				app_unlock();
				launch(path, "o jogo");
				app_lock();
			}
			g_ui.view = VIEW_LIBRARY;
		}
		break;
	case VIEW_ABOUT:
		if (pressed & INPUT_GAMEPAD_A)
			g_ui.view = VIEW_HOME;
		break;
	case VIEW_HOME:
	case VIEW_GUIDE:
	case VIEW_AVATAR_EDITOR:
		break;
	}
	return true;
}

void draw_footer(ImDrawList *dl)
{
	float x = 36;
	const ImU32 label = rgba(60, 64, 68);
	switch (g_ui.view) {
	case VIEW_HOME:
		button_hint(dl, x, 680, "A", BUTTON_A, "Selecionar", label);
		button_hint(dl, x, 680, "Y", BUTTON_Y, "Atualizar", label);
		break;
	case VIEW_GUIDE:
		break; // o Guide tem a linha de botoes dele
	case VIEW_AVATAR_EDITOR: {
		const ImU32 light = rgba(220, 222, 224);
		button_hint(dl, x, 680, "A", BUTTON_A, "Salvar", light);
		button_hint(dl, x, 680, "B", BUTTON_B, "Cancelar", light);
		button_hint(dl, x, 680, "X", BUTTON_X, "Aleatorio", light);
		button_hint(dl, x, 680, "Y", BUTTON_Y, "Padrao", light);
		break;
	}
	case VIEW_ABOUT:
		button_hint(dl, x, 680, "B", BUTTON_B, "Voltar", rgba(220, 222, 224));
		break;
	default:
		button_hint(dl, x, 680, "A", BUTTON_A, "Selecionar", rgba(220, 222, 224));
		button_hint(dl, x, 680, "B", BUTTON_B, "Voltar", rgba(220, 222, 224));
		break;
	}
}

// --- o Guide do EPTX -----------------------------------------------------------------------------
//
// O botao do Xbox abre o Guide do proprio sistema por cima de qualquer titulo, e o
// XAM fica com o controle enquanto ele esta aberto: o titulo nem ve esse botao. O
// Guide do EPTX, no mesmo visual, abre pelo Back.

enum GuideItem { GUIDE_HOME, GUIDE_STORE, GUIDE_FRIENDS, GUIDE_LIBRARY, GUIDE_DOWNLOADS, GUIDE_DASHBOARD,
	GUIDE_SESSION, GUIDE_COUNT };

void blade(ImDrawList *dl, float x0, float x1, float y0, float y1, const char *label, float alpha)
{
	const ImVec2 a = P(x0, y0), b = P(x1, y1);
	soft_shadow(dl, a, b, S(6), ImVec2(S(2), S(4)), S(6), 0.4f * alpha);
	int start = mark(dl);
	dl->AddRectFilled(a, b, IM_COL32_WHITE, S(6));
	shade(dl, start, a, b, rgba(236, 238, 240, alpha), rgba(200, 204, 208, alpha), rgba(170, 174, 178, alpha),
		rgba(214, 218, 222, alpha));
	dl->AddRect(a, b, rgba(120, 126, 132, 0.8f * alpha), S(6), 0, S(1));
	// O nome corre de cima para baixo, como "Games" e "Media" no Guide do sistema.
	const ImVec2 pivot = P((x0 + x1) / 2, y0 + 24);
	start = mark(dl);
	dl->AddText(g_regular, S(21), ImVec2(pivot.x, pivot.y - S(12)), rgba(70, 76, 82, alpha), label);
	rotate_since(dl, start, pivot, PI / 2);
}

int friends_online()
{
	int n = 0;
	for (int i = 0; i < g_app.friend_count; i++)
		n += g_app.friends[i].online;
	return n;
}

void draw_guide(ImDrawList *dl, float alpha)
{
	dl->AddRectFilled(ImVec2(0, 0), ImVec2(g_width, g_height), rgba(0, 0, 0, 0.62f * alpha));
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;

	blade(dl, 284, 330, 222, 500, "Jogos", alpha);
	blade(dl, 326, 372, 212, 510, "Loja", alpha);
	blade(dl, 906, 952, 212, 510, "Amigos", alpha);
	blade(dl, 948, 994, 222, 500, "Sistema", alpha);

	const ImVec2 a = P(365, 203), b = P(913, 517);
	slate(dl, a, b, S(8), alpha);

	// Faixa da esquerda com o gamertag na vertical e o anel girando no alto.
	round_gradient(dl, a, P(412, 517), S(8), rgba(52, 60, 68, alpha), rgba(34, 40, 46, alpha),
		ImDrawFlags_RoundCornersLeft);
	const ImVec2 ring = P(388, 226);
	dl->AddCircle(ring, S(11), rgba(255, 255, 255, 0.15f * alpha), 32, S(3));
	const float spin = g_ui.time * 3.0f;
	dl->PathArcTo(ring, S(11), spin, spin + PI * 1.2f, 24);
	dl->PathStroke(rgba(140, 210, 40, alpha), 0, S(3));
	const char *tag = signed_in ? g_app.gamertag : "Offline";
	const ImVec2 pivot = P(388, 254);
	int start = mark(dl);
	dl->AddText(g_regular, S(24), ImVec2(pivot.x, pivot.y - S(13)), rgba(230, 234, 238, alpha), tag);
	rotate_since(dl, start, pivot, PI / 2);

	// Barra de cima: bateria e controle, a foto do perfil no meio, o relogio a direita.
	const ImVec2 bat_a = P(376, 175), bat_b = P(404, 192);
	dl->AddRect(bat_a, bat_b, rgba(235, 238, 240, alpha), S(2), 0, S(2));
	dl->AddRectFilled(P(404, 179), P(408, 188), rgba(235, 238, 240, alpha));
	for (int i = 0; i < 3; i++)
		dl->AddRectFilled(P(379 + (float)i * 8, 178), P(385 + (float)i * 8, 189), rgba(235, 238, 240, alpha));
	draw_icon(dl, ICON_CONTROLLER, P(434, 184), S(10), 0.9f * alpha);
	soft_shadow(dl, P(616, 147), P(664, 195), S(3), ImVec2(0, S(3)), S(5), 0.5f * alpha);
	if (signed_in && g_app.has_look)
		look_headshot(dl, P(616, 147), S(48), g_app.look, alpha);
	else if (signed_in && g_app.avatar_gif && my_avatar())
		avatar_card(dl, P(616, 147), S(48), *my_avatar(), alpha);
	else if (signed_in && my_avatar())
		avatar_headshot(dl, P(616, 147), S(48), *my_avatar(), alpha);
	else if (signed_in)
		gamerpic(dl, P(616, 147), S(48), g_app.gamertag, alpha);
	else {
		dl->AddRectFilled(P(616, 147), P(664, 195), rgba(90, 98, 106, alpha));
		draw_icon(dl, ICON_PERSON, P(640, 171), S(17), alpha);
	}
	char clock[16] = "";
	const time_t now = time(nullptr);
	struct tm local;
	if (localtime_r(&now, &local))
		strftime(clock, sizeof clock, "%H:%M", &local);
	text(dl, g_regular, 22, ImVec2(P(909, 0).x - text_width(g_regular, 22, clock), P(0, 172).y),
		rgba(235, 238, 240, alpha), clock, false);

	// Os itens, com o numero e o icone a direita.
	char right[GUIDE_COUNT][24] = {};
	snprintf(right[GUIDE_FRIENDS], 24, "%d", friends_online());
	snprintf(right[GUIDE_LIBRARY], 24, "%d", g_app.library_count);
	if (g_app.installing && g_app.install_total > 0)
		snprintf(right[GUIDE_DOWNLOADS], 24, "%d%%", (int)(g_app.install_done * 100 / g_app.install_total));
	const char *labels[GUIDE_COUNT] = { "Dashboard EPTX", "Loja de Jogos", "Amigos", "Biblioteca de Jogos", "Downloads",
		"Dashboard Original", signed_in ? "Sair do EPTX" : "Entrar no EPTX" };
	const Icon icons[GUIDE_COUNT] = { ICON_NONE, ICON_BAG, ICON_FRIENDS, ICON_CONTROLLER, ICON_DOWNLOAD, ICON_HOME,
		ICON_PERSON };
	for (int i = 0; i < GUIDE_COUNT; i++) {
		const float y = 214 + (float)i * 41;
		const ImVec2 ra = P(424, y), rb = P(898, y + 37);
		const bool chosen = i == g_ui.menu;
		if (chosen)
			aero_bar(dl, ra, rb, alpha);
		bar_text(dl, g_regular, 25, ImVec2(ra.x + S(8), ra.y + S(4)), chosen, labels[i], alpha);
		if (icons[i] != ICON_NONE)
			draw_icon(dl, icons[i], P(876, y + 19), S(9), alpha);
		if (right[i][0])
			bar_text(dl, g_regular, 23, ImVec2(P(856, 0).x - text_width(g_regular, 23, right[i]), ra.y + S(5)), chosen,
				right[i], alpha);
	}

	float x = 378;
	const ImU32 label = rgba(230, 232, 234, alpha);
	button_hint(dl, x, 538, "A", BUTTON_A, "Selecionar", label, alpha);
	button_hint(dl, x, 538, "B", BUTTON_B, "Voltar", label, alpha);
	if (signed_in)
		button_hint(dl, x, 538, "X", BUTTON_X, "Sair", label, alpha);
	button_hint(dl, x, 538, "Y", BUTTON_Y, "Dashboard EPTX", label, alpha);
}

// Fecha o Guide indo para um canal.
void guide_go(int channel, int tile)
{
	g_ui.view = VIEW_HOME;
	g_ui.channel = channel;
	if (tile >= 0)
		g_ui.tile[channel] = tile;
}

// --- o quadro -------------------------------------------------------------------------------------

Tile g_tiles[CHANNEL_COUNT][MAX_TILES];
int g_tile_count[CHANNEL_COUNT];

void frame(const INPUT_GAMEPAD &pad, bool input_allowed, float dt)
{
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	int want_art[8], want_count = 0;
	int want_lib[8], want_lib_count = 0;
	int want_friend[8], want_friend_count = 0;

	app_lock();
	g_uploads_this_frame = 0;
	if (g_friends_serial != g_app.friends_serial) {
		free_arts(g_friend_art, APP_MAX_FRIENDS);
		g_friends_serial = g_app.friends_serial;
	}
	if (g_avatar_serial != g_app.avatar_serial) {
		// Saiu da conta ou trocou de avatar: a textura velha nao serve mais.
		free_arts(&g_avatar_art, 1);
		g_avatar_serial = g_app.avatar_serial;
	}
	if (g_store_serial != g_app.store_serial) {
		free_arts(g_store_art, APP_MAX_STORE);
		g_store_serial = g_app.store_serial;
	}
	if (g_library_serial != g_app.library_serial) {
		free_arts(g_library_art, APP_MAX_LIBRARY);
		g_library_serial = g_app.library_serial;
		if (g_ui.library_index >= g_app.library_count)
			g_ui.library_index = g_app.library_count ? g_app.library_count - 1 : 0;
	}
	if (g_ui.store_index >= g_app.store_count && g_ui.view == VIEW_STORE_ITEM)
		g_ui.view = VIEW_HOME;
	for (int c = 0; c < CHANNEL_COUNT; c++)
		g_tile_count[c] = build_tiles(c, g_tiles[c]);

	if (input_allowed)
		handle_input(pad, dt, g_tiles[g_ui.channel], g_tile_count[g_ui.channel]);

	g_ui.channel_pos = approach(g_ui.channel_pos, (float)g_ui.channel, dt, 10);
	for (int c = 0; c < CHANNEL_COUNT; c++) {
		if (g_ui.tile[c] >= g_tile_count[c])
			g_ui.tile[c] = g_tile_count[c] - 1;
		g_ui.tile_pos[c] = approach(g_ui.tile_pos[c], (float)g_ui.tile[c], dt);
	}
	g_ui.view_fade = approach(g_ui.view_fade, g_ui.view == VIEW_HOME ? 0.0f : 1.0f, dt, 14);

	// Capas: sobe as prontas e pede as dos blocos perto da tela.
	for (int i = 0; i < g_app.store_count; i++)
		take_art(g_app.store[i], g_store_art[i]);
	for (int i = 0; i < g_app.library_count; i++)
		take_art(g_app.library[i], g_library_art[i]);
	const int focus = g_ui.tile[CHANNEL_STORE];
	for (int i = focus - 1; i <= focus + 6 && want_count < 8; i++)
		if (i >= 0 && i < g_app.store_count && g_app.store[i].art == ART_NONE)
			want_art[want_count++] = i;
	for (int i = 0; i < g_app.library_count && want_lib_count < 8; i++)
		if (g_app.library[i].art == ART_NONE)
			want_lib[want_lib_count++] = i;
	take_image(g_app.avatar_art, g_app.avatar_image, g_avatar_art);
	for (int i = 0; i < g_app.friend_count; i++) {
		take_art(g_app.friends[i], g_friend_art[i]);
		if (g_app.friends[i].art == ART_NONE && g_app.friends[i].avatar_updated > 0 && want_friend_count < 8)
			want_friend[want_friend_count++] = i;
	}

	// GIF do avatar: parado no primeiro quadro, roda inteiro enquanto o bloco dele esta escolhido.
	const bool home = g_ui.view == VIEW_HOME;
	animate(g_avatar_art,
		home && g_ui.channel == CHANNEL_MY && g_ui.tile[CHANNEL_MY] >= 0 && g_tile_count[CHANNEL_MY] > 0 &&
			g_tiles[CHANNEL_MY][g_ui.tile[CHANNEL_MY]].kind == TILE_PROFILE,
		dt);
	int chosen_friend = -1;
	if (home && g_ui.channel == CHANNEL_FRIENDS && g_ui.tile[CHANNEL_FRIENDS] >= 0 &&
		g_ui.tile[CHANNEL_FRIENDS] < g_tile_count[CHANNEL_FRIENDS]) {
		const Tile &t = g_tiles[CHANNEL_FRIENDS][g_ui.tile[CHANNEL_FRIENDS]];
		if (t.kind == TILE_FRIEND)
			chosen_friend = t.index;
	}
	for (int i = 0; i < g_app.friend_count; i++)
		animate(g_friend_art[i], i == chosen_friend, dt);

	draw_background(dl);
	draw_channels(dl);
	// O canal atual e o vizinho que esta chegando: os blocos cruzam na troca.
	for (int c = 0; c < CHANNEL_COUNT; c++) {
		const float d = (float)c - g_ui.channel_pos;
		if (fabsf(d) >= 1)
			continue;
		dl->PushClipRect(P(0, 200 - 0), P(1280, 720), true);
		const ImVec2 shift(0, S(-d * 260));
		const int vtx = dl->VtxBuffer.Size;
		draw_tiles(dl, g_tiles[c], g_tile_count[c], c);
		// Desloca e esmaece o que acabou de desenhar, para o canal entrar ou sair por baixo.
		const float fade = 1 - fabsf(d);
		for (int v = vtx; v < dl->VtxBuffer.Size; v++) {
			ImDrawVert &vert = dl->VtxBuffer[v];
			vert.pos.x += shift.x;
			vert.pos.y += shift.y;
			const int a = (int)((float)((vert.col >> IM_COL32_A_SHIFT) & 0xFF) * fade);
			vert.col = (vert.col & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
		}
		dl->PopClipRect();
	}
	draw_profile_corner(dl);
	draw_emblem(dl);

	if (g_ui.view_fade > 0.01f) {
		const float alpha = g_ui.view_fade;
		switch (g_ui.view) {
		case VIEW_STORE_ITEM:
			draw_store_item(dl, alpha);
			break;
		case VIEW_LIBRARY:
		case VIEW_CONFIRM_LAUNCH:
			draw_library(dl, alpha, dt);
			if (g_ui.view == VIEW_CONFIRM_LAUNCH)
				menu_items(dl, LAUNCH_MENU, 2, g_ui.menu, 720, 470, 380, alpha);
			break;
		case VIEW_PROFILE:
			panel_frame(dl, g_app.gamertag, alpha);
			text(dl, g_regular, 22, P(180, 186), rgba(220, 224, 228, alpha), "Conectado ao servidor EPTX em", false);
			text(dl, g_regular, 22, P(180, 218), rgba(200, 230, 140, alpha), g_app.server, false);
			menu_items(dl, PROFILE_MENU, 3, g_ui.menu, 180, 300, 400, alpha);
			if (g_app.has_look)
				draw_look(dl, P(900, 560), S(330), g_app.look, alpha);
			break;
		case VIEW_AVATAR_EDITOR:
			draw_avatar_editor(dl, alpha);
			break;
		case VIEW_GUIDE:
			draw_guide(dl, alpha);
			break;
		case VIEW_ABOUT:
			draw_about(dl, alpha);
			break;
		case VIEW_HOME:
			break;
		}
	}
	draw_footer(dl);
	draw_toast(dl, dt);
	app_unlock();

	for (int i = 0; i < want_count; i++)
		app_load_art(want_art[i]);
	for (int i = 0; i < want_lib_count; i++)
		app_load_library_art(want_lib[i]);
	for (int i = 0; i < want_friend_count; i++)
		app_load_friend_avatar(want_friend[i]);
}

ImFont *add_font(const unsigned char *data, unsigned int size)
{
	ImFontConfig config;
	config.FontDataOwnedByAtlas = false; // os bytes vivem no executavel
	return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(data), (int)size, 20.0f, &config);
}

double now_seconds()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

} // namespace

int main()
{
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("eptx: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("eptx: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	uint32_t width = 0, height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &width, &height);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("eptx: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	g_width = (float)width;
	g_height = (float)height;
	g_sx = g_width / 1280.0f;
	g_sy = g_height / 720.0f;
	xsdk_log_write("eptx: tela %ux%u", (unsigned)width, (unsigned)height);

	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr; // game: e so leitura dentro do pacote
	io.LogFilename = nullptr;
	g_regular = add_font(font_regular, font_regular_size);
	g_semibold = add_font(font_semibold, font_semibold_size);
	g_light = add_font(font_light, font_light_size);
	(void)g_light;
	if (!ImGui_ImplXsdk_Init()) {
		xsdk_log_write("eptx: ImGui_ImplXsdk_Init failed");
		return 1;
	}

	app_start();

	double last = now_seconds();
	for (;;) {
		const double t = now_seconds();
		const float dt = fminf((float)(t - last), 0.1f);
		last = t;
		g_ui.time += dt;

		INPUT_STATE input = {};
		if (XamInputGetState(0, 0, &input) != ERROR_SUCCESS)
			input = INPUT_STATE{};
		const bool system_ui = xsdk_system_ui_open();
		poll_keyboard(system_ui);
		const bool input_allowed = !system_ui && g_kb == KB_NONE;
		// Entregar o controle ao backend faz ele parar de ler o controle 1 por conta propria.
		ImGui_ImplXsdk_ProcessGamepad(&input.gamepad);

		ImGui_ImplXsdk_NewFrame(width, height);
		ImGui::NewFrame();
		frame(input.gamepad, input_allowed, dt);
		ImGui::Render();

		uint32_t address;
		status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("eptx: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			break;
		}
		xsdk_gpu_set_target(width, height);
		ImGui_ImplXsdk_RenderDrawData(ImGui::GetDrawData());
		xsdk_gpu_resolve(address, width, height, 0xFF000000);
		xsdk_present_end();
	}

	ImGui_ImplXsdk_Shutdown();
	ImGui::DestroyContext();
	xsdk_present_close();
	xsdk_gpu_close();
	return 0;
}
