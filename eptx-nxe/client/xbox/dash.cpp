// EPTX NXE Edition: a dash do EPTX sobre a interface do Freestyle Dash.
//
// O fundo, as legendas das abas, os blocos em perspectiva, o canto do perfil, os
// botoes, os paineis e as linhas de lista sao as cenas do FSD (main.xui e skin.xui
// da skin Default), desenhadas pelo xui.cpp. O que e do EPTX (abas Home, Games,
// Friends, Store, Profile e Multiplayer, o avatar, o progresso) entra por cima, no
// espaco de cada elemento.
//
// A tela nunca fala com o servidor; ela pede tarefas a app.c e desenha g_app.

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

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
#include "sound.h"
#include "xui.h"

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
//
// As abas desta edicao, empilhadas como o FSD empilha as dele: a atual embaixo, as
// proximas acima e menores. Cada aba e uma fileira de blocos do FSD (Tab2 de ScnMain).

enum Tab { TAB_HOME, TAB_GAMES, TAB_FRIENDS, TAB_STORE, TAB_PROFILE, TAB_MULTIPLAYER, TAB_COUNT };
const char *const TAB_NAMES[TAB_COUNT] = { "Home", "Games", "Friends", "Store", "Profile", "Multiplayer" };

enum TileKind { TILE_PROFILE, TILE_DOWNLOADS, TILE_LIBRARY, TILE_GAME, TILE_STORE_ITEM, TILE_STORE_SEARCH,
	TILE_STORE_EMPTY, TILE_FRIEND, TILE_ADD_FRIEND, TILE_REQUESTS, TILE_FRIENDS_EMPTY, TILE_ONLINE_FRIENDS,
	TILE_AVATAR_EDITOR, TILE_SERVER, TILE_SIGN, TILE_ABOUT, TILE_REFRESH, TILE_DASHBOARD, TILE_ROOM, TILE_CREATE_ROOM,
	TILE_INVITES };

struct Tile {
	TileKind kind;
	int index; // na loja, nos amigos, na biblioteca
};

enum View { VIEW_HOME, VIEW_STORE_ITEM, VIEW_STORE_SEARCH, VIEW_LIBRARY, VIEW_CONFIRM_LAUNCH, VIEW_PROFILE, VIEW_ABOUT,
	VIEW_GUIDE, VIEW_AVATAR_EDITOR, VIEW_FRIEND, VIEW_REQUESTS, VIEW_ADD_FRIEND, VIEW_ROOM, VIEW_CREATE_ROOM,
	VIEW_INVITES, VIEW_CONFIRM };

// O que o painel de confirmacao confirma.
enum Confirm { CONFIRM_REMOVE_FRIEND, CONFIRM_SIGN_OUT, CONFIRM_LEAVE_ROOM };

struct UiState {
	int tab = TAB_HOME;
	int tab_target = TAB_HOME; // sem dar a volta: as legendas andam pelo caminho curto
	float tab_pos = TAB_HOME;
	int tile[TAB_COUNT] = {};
	float tile_pos[TAB_COUNT] = {};
	View view = VIEW_HOME;
	View confirm_back = VIEW_HOME;
	float view_fade = 0;
	int menu = 0;        // linha escolhida no painel aberto
	float scroll = 0;    // primeira linha visivel numa lista longa
	int store_index = 0; // jogo aberto no painel da loja
	int friend_index = 0;
	int library_index = 0;
	Confirm confirm = CONFIRM_REMOVE_FRIEND;
	uint32_t seen_message = 0;
	float toast_time = 0;
	char toast[200] = "";
	float time = 0;
	avatar_look edit_look = {}; // o avatar no editor, antes de salvar
	char xbox_profile[16] = ""; // o perfil do Xbox logado, como o XAM diz
	bool profile_known = false;
	float profile_poll = 0;
	int edit_field = 0;
};
UiState g_ui;

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
		sound_play("NotifyPopup");
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

// Texto branco com sombra escura em cima da barra verde, como "Skin" na foto.
void bar_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, bool selected, const char *s, float alpha)
{
	if (selected)
		dl->AddText(font, S(size), ImVec2(pos.x + S(1), pos.y + S(1.5f)), rgba(20, 50, 0, 0.7f * alpha), s);
	dl->AddText(font, S(size), pos, rgba(240, 244, 246, alpha), s);
}

bool installed(const store_item &s)
{
	for (int i = 0; i < g_app.library_count; i++)
		if (strcasecmp(g_app.library[i].title_id, s.title_id) == 0)
			return true;
	return false;
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
enum Keyboard { KB_NONE, KB_GAMERTAG, KB_PASSWORD, KB_WAITING, KB_FRIEND, KB_SEARCH };
void open_view(View v); // em "entrada", mais abaixo
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
		// A conta do perfil costuma ter a gamertag dele: ja vem escrita.
		to_utf16(g_kb_gamertag[0] ? g_kb_gamertag : g_ui.xbox_profile, initial, 64);
	} else if (which == KB_FRIEND) {
		to_utf16("Adicionar amigo", title, 64);
		to_utf16("Gamertag exata da pessoa no servidor EPTX", description, 128);
		initial[0] = 0;
	} else if (which == KB_SEARCH) {
		to_utf16("Pesquisar na Store", title, 64);
		to_utf16("Nome do jogo ou Title ID", description, 128);
		initial[0] = 0;
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

// O A no perfil: comeca pelo teclado da gamertag, com as tentativas zeradas. A tela pede
// com g_app travado; o teclado abre depois, em poll_keyboard, fora da trava (num erro,
// open_keyboard trava g_app para avisar).
void start_login()
{
	g_kb_tries = 0;
	open_keyboard_soon(KB_GAMERTAG);
}

void open_text_keyboard(Keyboard which)
{
	g_kb_tries = 0;
	open_keyboard_soon(which);
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
	} else if (was == KB_FRIEND || was == KB_SEARCH) {
		char typed[64];
		from_utf16(g_kb_text, typed, sizeof typed);
		if (!typed[0])
			return;
		app_lock();
		open_view(was == KB_FRIEND ? VIEW_ADD_FRIEND : VIEW_STORE_SEARCH);
		app_unlock();
		if (was == KB_FRIEND)
			app_lookup_gamertag(typed);
		else
			app_search_store(typed);
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

// --- a skin do FSD ----------------------------------------------------------------------
//
// As cenas e as imagens sao as do Freestyle Dash (Skins/Default do codigo que a TeamFSD
// liberou, GPLv3), desenhadas pelo xui.cpp. No pacote ficam em game:\Skin; o preview
// aponta g_skin_dir para a pasta do clone.

#ifndef SKIN_DIR
#define SKIN_DIR "game:/Skin"
#endif
const char *g_skin_dir = SKIN_DIR;
float g_dt;
Art g_lookup_art; // o avatar da gamertag procurada em Adicionar Amigo
uint32_t g_lookup_serial = ~0u;
Art g_profile_art; // a foto do perfil do Xbox logado
uint32_t g_profile_serial = ~0u;

#define UNLOCKED(call)                                                                                                 \
	do {                                                                                                               \
		app_unlock();                                                                                                  \
		call;                                                                                                          \
		app_lock();                                                                                                    \
	} while (0)

struct SkinTexture {
	Art art = {};
	bool tried = false;
};
std::map<std::string, SkinTexture> g_skin_textures;

// Le uma imagem da skin na primeira vez que alguma cena pede; depois fica na GPU.
bool skin_texture(const char *path, xui::Texture *out)
{
	SkinTexture &st = g_skin_textures[path];
	if (!st.tried) {
		st.tried = true;
		std::string full = std::string(g_skin_dir) + "/" + path;
		for (char &c : full)
			if (c == '\\')
				c = '/';
		FILE *fp = fopen(full.c_str(), "rb");
		if (fp) {
			fseek(fp, 0, SEEK_END);
			const long size = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			uint8_t *data = size > 0 ? (uint8_t *)malloc((size_t)size) : nullptr;
			app_image image = {};
			if (data && fread(data, 1, (size_t)size, fp) == (size_t)size &&
				image_decode_png_limit(data, (size_t)size, 2048, &image)) {
				if (!upload(&st.art, &image))
					xsdk_log_write("eptx: imagem da skin nao subiu: %s", path);
			}
			image_free(&image);
			free(data);
			fclose(fp);
		} else {
			xsdk_log_write("eptx: imagem da skin faltando: %s", full.c_str());
		}
	}
	if (!st.art.valid)
		return false;
	out->id = tex_id(st.art);
	out->w = (float)st.art.texture.width;
	out->h = (float)st.art.texture.height;
	return true;
}

ImFont *skin_font(const char *) { return g_regular; }
void skin_sound(const char *file); // com as dicas de botao, mais abaixo

xui::Texture art_texture(const Art &art)
{
	xui::Texture t;
	if (art.valid) {
		t.id = tex_id(art);
		t.w = (float)art.texture.width;
		t.h = (float)art.texture.height;
	}
	return t;
}

const xui::Def *def_child(const xui::Def *d, const char *id)
{
	if (d)
		for (const xui::Def *c : d->children)
			if (c->id == id)
				return c;
	return nullptr;
}

// Um controle com um Visual da skin.xui, sem cena em volta: painel, botao de lista, dica do controle.
std::deque<xui::Def> g_made_defs;
const xui::Def *control_def(const char *visual, float x, float y, float w, float h, const char *tag = "XuiButton")
{
	g_made_defs.emplace_back();
	xui::Def &d = g_made_defs.back();
	d.tag = tag;
	d.kind = xui::K_CONTROL;
	d.visual = visual;
	d.base.pos = { x, y, 0 };
	d.base.w = w;
	d.base.h = h;
	return &d;
}

xui::File g_skin_file, g_main_file;
bool g_skin_ok;
xui::Node *g_backdrop, *g_profile, *g_counter, *g_bullet, *g_panel, *g_loading;
xui::Node *g_captions[TAB_COUNT];
xui::Node *g_hints[5]; // A, B, X, Y, Back
const xui::Def *g_blade_def;

enum { HINT_A, HINT_B, HINT_X, HINT_Y, HINT_BACK };

bool load_skin()
{
	char path[256];
	snprintf(path, sizeof path, "%s/skin.xui", g_skin_dir);
	if (!xui::load(g_skin_file, path)) {
		xsdk_log_write("eptx: nao li %s", path);
		return false;
	}
	snprintf(path, sizeof path, "%s/main.xui", g_skin_dir);
	if (!xui::load(g_main_file, path)) {
		xsdk_log_write("eptx: nao li %s", path);
		return false;
	}
	xui::g_host.texture = skin_texture;
	xui::g_host.font = skin_font;
	xui::g_host.sound = skin_sound;
	xui::g_host.sx = g_sx;
	xui::g_host.sy = g_sy;
	// A camera que poe os blocos onde o preview.png da skin mostra.
	xui::g_host.vanish_x = 0;
	xui::g_host.vanish_y = 0;
	xui::g_host.focal = 1100;
	xui::g_host.font_scale = 1.0f;

	const xui::Def *main = g_main_file.find("MainScene");
	// ScnMain: a aba de jogos do FSD, de onde saem o bloco, o contador e os botoes.
	const xui::Def *scene = def_child(main, "Tab3");
	g_blade_def = def_child(scene, "Tab2");
	const xui::Def *caption = def_child(main, "Tab1Caption");
	const xui::Def *hint = def_child(scene, "Favourites");
	if (!main || !scene || !g_blade_def || !caption || !hint) {
		xsdk_log_write("eptx: main.xui nao tem as cenas esperadas do FSD");
		return false;
	}

	g_backdrop = xui::instantiate(g_main_file.find("CustomBackdrop"), &g_skin_file);
	// O orbe do FSD e o do XLink Kai saem: no canto fica o emblema do EPTX.
	for (const char *id : { "Animation0", "Animation3" })
		if (xui::Node *n = xui::find(g_backdrop, id))
			n->hidden = true;
	g_profile = xui::instantiate(g_main_file.find("ProfileScene"), &g_skin_file);
	g_bullet = xui::instantiate(def_child(main, "BulletFigure"), &g_skin_file);
	g_counter = xui::instantiate(def_child(scene, "TabCounter"), &g_skin_file);
	for (int i = 0; i < TAB_COUNT; i++) {
		g_captions[i] = xui::instantiate(caption, &g_skin_file);
		xui::set_text(g_captions[i], TAB_NAMES[i]);
	}
	static const char *const BUTTONS[5] = { "Btn_Button_A", "Btn_Button_B", "Btn_Button_X", "Btn_Button_Y",
		"Btn_Button_Back" };
	for (int i = 0; i < 5; i++) {
		g_made_defs.push_back(*hint);
		g_made_defs.back().visual = BUTTONS[i];
		g_hints[i] = xui::instantiate(&g_made_defs.back(), &g_skin_file);
	}
	// O painel solido da skin: o transparente (TransPanel_*) deixa os blocos de tras atrapalharem a leitura.
	g_panel = xui::instantiate(control_def("Panel_Background", 202, 112, 876, 484), &g_skin_file);
	g_loading = xui::instantiate(control_def("Animation_Loading", 0, 0, 96, 96, "XuiControl"), &g_skin_file);
	return g_backdrop && g_profile && g_bullet && g_counter;
}

// --- os blocos ------------------------------------------------------------------------------

constexpr int MAX_TILES = APP_MAX_STORE + 4;
Tile g_tiles[TAB_COUNT][MAX_TILES];
int g_tile_count[TAB_COUNT];

int build_tiles(int tab, Tile *tiles)
{
	int n = 0;
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;
	switch (tab) {
	case TAB_HOME:
		tiles[n++] = { TILE_PROFILE, 0 };
		if (g_app.invite_count)
			tiles[n++] = { TILE_INVITES, 0 };
		if (g_app.requests_in_count)
			tiles[n++] = { TILE_REQUESTS, 0 };
		tiles[n++] = { TILE_DOWNLOADS, 0 };
		tiles[n++] = { TILE_ONLINE_FRIENDS, 0 };
		tiles[n++] = { TILE_DASHBOARD, 0 };
		tiles[n++] = { TILE_REFRESH, 0 };
		tiles[n++] = { TILE_ABOUT, 0 };
		break;
	case TAB_GAMES:
		tiles[n++] = { TILE_LIBRARY, 0 };
		for (int i = 0; i < g_app.library_count && n < MAX_TILES - 1; i++)
			tiles[n++] = { TILE_GAME, i };
		tiles[n++] = { TILE_DOWNLOADS, 0 };
		break;
	case TAB_FRIENDS:
		tiles[n++] = { TILE_ADD_FRIEND, 0 };
		tiles[n++] = { TILE_REQUESTS, 0 };
		// Quem esta online primeiro, como na NXE.
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < g_app.friend_count && n < MAX_TILES; i++)
				if (g_app.friends[i].online == (pass == 0))
					tiles[n++] = { TILE_FRIEND, i };
		if (!g_app.friend_count)
			tiles[n++] = { TILE_FRIENDS_EMPTY, 0 };
		break;
	case TAB_STORE:
		if (signed_in)
			tiles[n++] = { TILE_STORE_SEARCH, 0 };
		for (int i = 0; i < g_app.store_count && n < MAX_TILES; i++)
			tiles[n++] = { TILE_STORE_ITEM, i };
		if (!g_app.store_count)
			tiles[n++] = { TILE_STORE_EMPTY, 0 };
		break;
	case TAB_PROFILE:
		tiles[n++] = { TILE_PROFILE, 0 };
		if (signed_in)
			tiles[n++] = { TILE_AVATAR_EDITOR, 0 };
		tiles[n++] = { TILE_SERVER, 0 };
		tiles[n++] = { TILE_SIGN, 0 };
		break;
	case TAB_MULTIPLAYER:
		tiles[n++] = { g_app.room.active ? TILE_ROOM : TILE_CREATE_ROOM, 0 };
		tiles[n++] = { TILE_INVITES, 0 };
		for (int i = 0; i < g_app.friend_count && n < MAX_TILES; i++)
			if (g_app.friends[i].online)
				tiles[n++] = { TILE_FRIEND, i };
		break;
	}
	return n;
}

int friends_online_count()
{
	int n = 0;
	for (int i = 0; i < g_app.friend_count; i++)
		n += g_app.friends[i].online;
	return n;
}

const char *session_status()
{
	switch (g_app.session) {
	case SESSION_SIGNED_IN:
		return g_app.server_ok ? "Online" : "Servidor sem resposta";
	case SESSION_SIGNING_IN:
		return "Entrando...";
	case SESSION_STARTING:
		return "Conectando...";
	case SESSION_NO_SERVER:
		return "Servidor fora do ar";
	case SESSION_SIGNED_OUT:
		break;
	}
	return "Offline";
}

ImU32 status_colour(float alpha = 1)
{
	if (g_app.session == SESSION_SIGNED_IN && g_app.server_ok)
		return rgba(110, 210, 40, alpha);
	if (g_app.session == SESSION_NO_SERVER || (g_app.session == SESSION_SIGNED_IN && !g_app.server_ok))
		return rgba(230, 70, 50, alpha);
	return rgba(160, 160, 160, alpha);
}

// O que um bloco mostra: o texto do FSD (ContentTab_MainText), uma linha nossa embaixo,
// o icone verde do FSD ou uma capa, e o fundo.
struct TileLook {
	char label[96] = "";
	char sub[128] = "";
	const char *icon = nullptr; // um dos icones verdes da skin
	const Art *cover = nullptr;
	int background = 2;         // Content_Bkg_N.png
};

const char *const ICON_DIR = "Images\\Interface\\Content Tabs\\ContentTabData\\Visuals\\Green\\";

TileLook describe(const Tile &t)
{
	TileLook l;
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;
	switch (t.kind) {
	case TILE_PROFILE:
		snprintf(l.label, sizeof l.label, "%s", signed_in ? g_app.gamertag : "Entrar no EPTX");
		snprintf(l.sub, sizeof l.sub, "%s", signed_in ? session_status()
				: g_app.session == SESSION_SIGNED_OUT ? "Aperte A para entrar"
													  : session_status());
		l.background = 1;
		break;
	case TILE_DOWNLOADS:
		snprintf(l.label, sizeof l.label, "Downloads");
		if (g_app.installing && g_app.install_total > 0)
			snprintf(l.sub, sizeof l.sub, "%s  %d%%", g_app.install_name,
				(int)(g_app.install_done * 100 / g_app.install_total));
		else
			snprintf(l.sub, sizeof l.sub, "Nada baixando");
		l.icon = "CopyDVD.png";
		l.background = 3;
		break;
	case TILE_LIBRARY:
		snprintf(l.label, sizeof l.label, "Biblioteca de Jogos");
		snprintf(l.sub, sizeof l.sub, "%d instalado(s) neste console", g_app.library_count);
		l.icon = "Xbox360.png";
		l.background = 2;
		break;
	case TILE_GAME: {
		const library_item &g = g_app.library[t.index];
		snprintf(l.label, sizeof l.label, "%s", g.name);
		snprintf(l.sub, sizeof l.sub, "%s", g.content_type == 0x7000 ? "Games on Demand" : "Arcade");
		l.cover = &g_library_art[t.index];
		l.icon = "Xbox360.png";
		l.background = 4;
		break;
	}
	case TILE_STORE_ITEM: {
		const store_item &s = g_app.store[t.index];
		char size[24];
		human_size(s.size, size, sizeof size);
		snprintf(l.label, sizeof l.label, "%s", s.name);
		snprintf(l.sub, sizeof l.sub, "%s  -  %s%s", s.kind, size, installed(s) ? "  -  instalado" : "");
		l.cover = &g_store_art[t.index];
		l.icon = "XBLA.png";
		l.background = 5;
		break;
	}
	case TILE_STORE_SEARCH:
		snprintf(l.label, sizeof l.label, "Pesquisar na Loja");
		snprintf(l.sub, sizeof l.sub, "%d jogo(s) no servidor", g_app.store_count);
		l.icon = "FileManager.png";
		l.background = 6;
		break;
	case TILE_STORE_EMPTY:
		snprintf(l.label, sizeof l.label, "%s", signed_in ? (g_app.store_loaded ? "Loja vazia" : "Carregando...")
														  : "Loja EPTX");
		snprintf(l.sub, sizeof l.sub, "%s", signed_in ? "Nenhum jogo no servidor" : "Entre para ver o catalogo");
		l.icon = "XBLA.png";
		break;
	case TILE_FRIEND: {
		const friend_item &f = g_app.friends[t.index];
		snprintf(l.label, sizeof l.label, "%s", f.gamertag);
		if (f.online)
			snprintf(l.sub, sizeof l.sub, "Online%s%s", f.title_name[0] ? " - " : "", f.title_name);
		else
			snprintf(l.sub, sizeof l.sub, "Offline");
		l.background = f.online ? 1 : 7;
		break;
	}
	case TILE_ADD_FRIEND:
		snprintf(l.label, sizeof l.label, "Adicionar Amigo");
		snprintf(l.sub, sizeof l.sub, "Procurar pela gamertag");
		l.icon = "KaiBuddy.png";
		l.background = 3;
		break;
	case TILE_REQUESTS:
		snprintf(l.label, sizeof l.label, "Solicitacoes (%d)", g_app.requests_in_count);
		snprintf(l.sub, sizeof l.sub, "%d enviada(s) aguardando", g_app.requests_out_count);
		l.icon = "KaiChat.png";
		l.background = 6;
		break;
	case TILE_FRIENDS_EMPTY:
		snprintf(l.label, sizeof l.label, "%s", signed_in ? "Nenhum amigo ainda" : "Amigos");
		snprintf(l.sub, sizeof l.sub, "%s", signed_in ? "Adicione pela gamertag" : "Entre para ver seus amigos");
		l.icon = "KaiBuddy.png";
		break;
	case TILE_ONLINE_FRIENDS:
		snprintf(l.label, sizeof l.label, "Amigos");
		snprintf(l.sub, sizeof l.sub, "%d de %d online", friends_online_count(), g_app.friend_count);
		l.icon = "KaiBuddy.png";
		l.background = 8;
		break;
	case TILE_AVATAR_EDITOR:
		snprintf(l.label, sizeof l.label, "Editor de Avatar");
		snprintf(l.sub, sizeof l.sub, "O boneco que seus amigos veem");
		l.icon = "Skins.png";
		l.background = 4;
		break;
	case TILE_SERVER:
		snprintf(l.label, sizeof l.label, "Servidor EPTX");
		snprintf(l.sub, sizeof l.sub, "%s  -  %s", g_app.server[0] ? g_app.server : "sem eptx.ini", session_status());
		l.icon = "System.png";
		l.background = 7;
		break;
	case TILE_SIGN:
		snprintf(l.label, sizeof l.label, "%s", signed_in ? "Sair do EPTX" : "Entrar no EPTX");
		snprintf(l.sub, sizeof l.sub, "%s", signed_in ? "Esquece a sessao deste console" : "Gamertag e senha");
		l.icon = "Settings.png";
		l.background = 8;
		break;
	case TILE_ABOUT:
		snprintf(l.label, sizeof l.label, "Sobre");
		snprintf(l.sub, sizeof l.sub, "EPTX NXE Edition e licencas");
		l.icon = "About.png";
		l.background = 2;
		break;
	case TILE_REFRESH:
		snprintf(l.label, sizeof l.label, "Atualizar");
		snprintf(l.sub, sizeof l.sub, "Loja, amigos, salas e biblioteca");
		l.icon = "RecentlyPlayed.png";
		l.background = 5;
		break;
	case TILE_DASHBOARD:
		snprintf(l.label, sizeof l.label, "Dashboard Original");
		snprintf(l.sub, sizeof l.sub, "Volta para a dash do console");
		l.icon = "Dvd.png";
		l.background = 4;
		break;
	case TILE_ROOM:
		snprintf(l.label, sizeof l.label, "Sala: %s", g_app.room.game);
		snprintf(l.sub, sizeof l.sub, "%d jogador(es)  -  %s", g_app.room.member_count,
			strcmp(g_app.room.state, "playing") == 0 ? "jogando" : "esperando");
		l.icon = "KaiArena.png";
		l.background = 1;
		break;
	case TILE_CREATE_ROOM:
		snprintf(l.label, sizeof l.label, "Criar Sala");
		snprintf(l.sub, sizeof l.sub, "%s", signed_in ? "Jogue com seus amigos" : "Entre para jogar online");
		l.icon = "KaiArena.png";
		l.background = 1;
		break;
	case TILE_INVITES:
		snprintf(l.label, sizeof l.label, "Convites (%d)", g_app.invite_count);
		snprintf(l.sub, sizeof l.sub, "%s", g_app.invite_count ? "Amigos te chamando para jogar" : "Nenhum convite");
		l.icon = "KaiPlayOnline.png";
		l.background = 6;
		break;
	}
	return l;
}

// Uma imagem quadrada e a foto de um perfil do Xbox (64x64), nao um corpo de avatar: vai num quadro.
bool square(const Art &art)
{
	const int w = art.w ? art.w : (int)art.texture.width, h = art.h ? art.h : (int)art.texture.height;
	return w > 0 && abs(w - h) <= 2;
}

// O avatar de um amigo dentro de uma caixa (desenhado, PNG, GIF, foto do perfil ou o gamerpic com a inicial).
void friend_figure(ImDrawList *dl, int index, ImVec2 feet, float height, bool chosen)
{
	const friend_item &f = g_app.friends[index];
	if (f.has_look) {
		draw_look(dl, feet, height, f.look, 1);
	} else if ((f.avatar_gif || square(g_friend_art[index])) && friend_avatar(index)) {
		const float side = height * 0.7f;
		avatar_card(dl, ImVec2(feet.x - side / 2, feet.y - side * 1.15f), side, *friend_avatar(index), 1, chosen);
	} else if (const Art *avatar = friend_avatar(index)) {
		draw_avatar_image(dl, feet, height, *avatar, 1);
	} else {
		const float side = height * 0.55f;
		gamerpic(dl, ImVec2(feet.x - side / 2, feet.y - side * 1.3f), side, f.gamertag, 1);
	}
}

void my_figure(ImDrawList *dl, ImVec2 feet, float height, bool chosen)
{
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;
	if (signed_in && g_app.has_look)
		draw_look(dl, feet, height, g_app.look, 1);
	else if (signed_in && my_avatar() && (g_app.avatar_gif || square(*my_avatar()))) {
		const float side = height * 0.62f;
		avatar_card(dl, ImVec2(feet.x - side / 2, feet.y - side * 1.25f), side, *my_avatar(), 1, chosen);
	} else
		draw_avatar(dl, feet, height, signed_in ? g_app.gamertag : nullptr, 1, signed_in ? my_avatar() : nullptr);
}

// O que vai por cima do bloco do FSD, no espaco dele (420x320 visiveis): a segunda linha,
// o avatar, a barra do download.
void tile_extra(ImDrawList *dl, const Tile &t, const char *sub, bool focused, float w, float h)
{
	(void)h;
	if (sub[0])
		dl->AddText(g_regular, 15, ImVec2(18, 287), rgba(235, 245, 220, 0.9f), fit(g_regular, 15, sub, w - 36));
	switch (t.kind) {
	case TILE_PROFILE:
		my_figure(dl, ImVec2(300, 300), 270, focused);
		break;
	case TILE_FRIEND:
		friend_figure(dl, t.index, ImVec2(210, 240), 200, focused);
		break;
	case TILE_DOWNLOADS:
		if (g_app.installing && g_app.install_total > 0) {
			const float k = (float)((double)g_app.install_done / (double)g_app.install_total);
			dl->AddRectFilled(ImVec2(18, 232), ImVec2(402, 240), rgba(20, 60, 0, 0.55f), 3);
			dl->AddRectFilled(ImVec2(18, 232), ImVec2(18 + 384 * k, 240), rgba(255, 255, 255, 0.95f), 3);
		}
		break;
	default:
		break;
	}
}

struct Blade {
	xui::Node *root = nullptr, *item = nullptr, *bg = nullptr, *icon = nullptr;
	bool focused = false;
};
std::vector<Blade> g_blades[TAB_COUNT];

Blade &blade_at(int tab, int i)
{
	std::vector<Blade> &v = g_blades[tab];
	if ((int)v.size() <= i)
		v.resize((size_t)i + 1);
	Blade &b = v[(size_t)i];
	if (!b.root) {
		b.root = xui::instantiate(g_blade_def, &g_skin_file);
		b.item = xui::find(b.root, "CustomMenuItem");
		b.bg = xui::find(b.root, "ContentTab_Background");
		b.icon = xui::find(b.root, "ContentTab_Icon");
	}
	return b;
}

void fill_blade(Blade &b, const Tile &t, bool focused)
{
	const TileLook look = describe(t);
	xui::set_text(b.item, look.label);
	char path[200];
	snprintf(path, sizeof path, "%sBackgrounds\\Content_Bkg_%d.png", ICON_DIR, look.background);
	xui::set_image_path(b.bg, path);
	if (b.icon) {
		b.icon->hidden = t.kind == TILE_PROFILE || t.kind == TILE_FRIEND;
		if (look.cover && look.cover->valid) {
			xui::set_image(b.icon, art_texture(*look.cover));
		} else {
			xui::set_image(b.icon, xui::Texture{});
			snprintf(path, sizeof path, "%sIcons\\%s", ICON_DIR, look.icon ? look.icon : "About.png");
			xui::set_image_path(b.icon, path);
		}
		// O icone do bloco focado cresce e volta, como no FSD (ContentItem_Active: InitFocus).
		if (focused != b.focused)
			xui::play_visual(b.icon, focused ? "InitFocus" : "KillFocus");
	}
	b.focused = focused;
	const std::string sub = look.sub;
	b.item->on_draw = [t, sub, focused](ImDrawList *dl, float w, float h) { tile_extra(dl, t, sub.c_str(), focused, w, h); };
}

// As posicoes da ScnMain do FSD (timelines 2To3, 3To4, ...): o focado em (100,250), cada
// seguinte 570 a direita, 115 abaixo e 300 mais fundo; o que sai vai para (-470,135,-300).
void blade_slot(float k, xui::Vec3 &pos, float &opacity)
{
	static const float X[] = { -470, 100, 670, 1200, 1750, 2300, 2870 };
	static const float Y[] = { 135, 250, 365, 480, 595, 710, 825 };
	static const float Z[] = { -300, 0, 300, 600, 900, 1200, 1500 };
	float f = k + 1;
	if (f < 0)
		f = 0;
	int i = (int)f;
	if (i > 5)
		i = 5;
	const float t = f - (float)i;
	pos.x = lerp(X[i], X[i + 1], t);
	pos.y = lerp(Y[i], Y[i + 1], t);
	pos.z = lerp(Z[i], Z[i + 1], t);
	opacity = k < 0 ? clamp01(1 + k) : clamp01(6 - k);
}

void draw_blades(ImDrawList *dl, int tab, float alpha)
{
	const int count = g_tile_count[tab];
	const float pos = g_ui.tile_pos[tab];
	// Do fundo para a frente, para o focado ficar por cima.
	for (int i = count - 1; i >= 0; i--) {
		const float k = (float)i - pos;
		if (k < -1 || k > 6)
			continue;
		Blade &b = blade_at(tab, i);
		fill_blade(b, g_tiles[tab][i], i == g_ui.tile[tab] && g_ui.tab == tab);
		xui::update(b.root, g_dt);
		float opacity;
		blade_slot(k, b.root->s.pos, opacity);
		b.root->s.opacity = opacity;
		xui::draw(dl, b.root, 0, 0, alpha);
	}
}

// As legendas das abas, nas posicoes do MainScene do FSD (1To2, 2To3, ...): a atual em
// y 150, as proximas em 102, 64 e 36, cada vez menores; a que chega por baixo vem de 200.
void caption_slot(float r, float &y, float &scale, float &opacity)
{
	static const float Y[TAB_COUNT] = { 150, 102, 64, 36, 11, 200 };
	static const float K[TAB_COUNT] = { 1, 0.8f, 0.6f, 0.4f, 0.1f, 0.5f };
	static const float A[TAB_COUNT] = { 1, 0.75f, 0.5f, 0.25f, 0, 0 };
	const int i = (int)r % TAB_COUNT, j = (i + 1) % TAB_COUNT;
	const float t = r - floorf(r);
	y = lerp(Y[i], Y[j], t);
	scale = lerp(K[i], K[j], t);
	opacity = lerp(A[i], A[j], t);
}

void draw_captions(ImDrawList *dl)
{
	for (int i = 0; i < TAB_COUNT; i++) {
		float r = fmodf((float)i - g_ui.tab_pos, (float)TAB_COUNT);
		if (r < 0)
			r += TAB_COUNT;
		xui::Node *c = g_captions[i];
		xui::update(c, g_dt);
		float y, k, a;
		caption_slot(r, y, k, a);
		c->s.pos.y = y;
		c->s.scale = { k, k, 1 };
		c->s.opacity = a;
		xui::draw(dl, c);
	}
	xui::update(g_bullet, g_dt);
	xui::draw(dl, g_bullet);
}

// O canto do perfil do FSD (ProfileScene): gamertag, a situacao no lugar do gamerscore e o avatar.
void draw_profile_corner(ImDrawList *dl)
{
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;
	xui::set_text(xui::find(g_profile, "GamerTag"), signed_in ? g_app.gamertag : "Offline");
	xui::set_text(xui::find(g_profile, "GamerScore"), session_status());
	// No lugar do icone de gamerscore fica o ponto da conexao, desenhado abaixo.
	if (xui::Node *icon = xui::find(g_profile, "GamerScoreIcon"))
		icon->hidden = true;
	xui::update(g_profile, g_dt);
	xui::draw(dl, g_profile);

	// O ProfileScene nao se mexe: o avatar e o ponto vao direto na tela, onde ficam o
	// GamerPic (64x64) e o icone. O rosto recorta com PushClipRect, que so vale na tela.
	const xui::Vec3 &o = g_profile->s.pos;
	const ImVec2 pic = P(o.x + 232, o.y + 5), dot = P(o.x + 210, o.y + 53);
	const float side = S(64);
	soft_shadow(dl, pic, ImVec2(pic.x + side, pic.y + side), S(3), ImVec2(S(2), S(4)), S(6), 0.4f);
	if (signed_in && g_app.has_look)
		look_headshot(dl, pic, side, g_app.look, 1);
	else if (signed_in && g_app.avatar_gif && my_avatar())
		avatar_card(dl, pic, side, *my_avatar(), 1);
	else if (signed_in && my_avatar())
		avatar_headshot(dl, pic, side, *my_avatar(), 1);
	else if (signed_in)
		gamerpic(dl, pic, side, g_app.gamertag, 1);
	else {
		dl->AddRectFilled(pic, ImVec2(pic.x + side, pic.y + side), rgba(255, 255, 255, 0.35f));
		draw_icon(dl, ICON_PERSON, ImVec2(pic.x + side / 2, pic.y + side / 2), side * 0.4f, 1);
	}
	// Verde online, vermelho sem servidor, cinza fora da conta.
	dl->AddCircleFilled(dot, S(7), rgba(0, 0, 0, 0.35f), 20);
	dl->AddCircleFilled(dot, S(5.5f), status_colour(), 20);
}

// Dicas de botao com os visuais Btn_Button_* da skin, na linha de baixo do FSD (y 650).
struct Hint {
	int button;
	const char *label;
};

void draw_hints(ImDrawList *dl, std::initializer_list<Hint> hints, float alpha = 1)
{
	float x = 95;
	for (const Hint &h : hints) {
		xui::Node *n = g_hints[h.button];
		xui::set_text(n, h.label);
		xui::draw(dl, n, x - 95, 0, alpha);
		x += 36 + g_regular->CalcTextSizeA(14, FLT_MAX, 0, h.label).x + 34;
	}
}

// Todo botao apertado toca o Press do visual dele (Btn_Button_*: o anel, e o a-button.xma
// pela timeline), apareca a dica na tela ou nao. Por isso as cinco andam todo quadro.
void press_hints(uint16_t pressed)
{
	static const uint16_t BITS[5] = { INPUT_GAMEPAD_A, INPUT_GAMEPAD_B, INPUT_GAMEPAD_X, INPUT_GAMEPAD_Y,
		INPUT_GAMEPAD_BACK };
	for (int i = 0; i < 5; i++)
		if (pressed & BITS[i] && g_hints[i])
			xui::play_visual(g_hints[i], "Press");
}

void update_hints()
{
	for (xui::Node *n : g_hints) {
		if (!n)
			continue;
		// O Press termina com o botao encolhido a zero: depois dele, volta ao Normal.
		if (n->visual && !n->visual->playing && n->visual->time > 2)
			xui::play_visual(n, "Normal");
		xui::update(n, g_dt);
	}
}

// "Audio\btn_Focus.xma", "sharedres://btn_Focus.xma" ou "btn_Focus.xma" viram "btn_Focus".
void skin_sound(const char *file)
{
	const char *base = file;
	for (const char *p = file; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	char name[40];
	snprintf(name, sizeof name, "%s", base);
	if (char *dot = strrchr(name, '.'))
		*dot = '\0';
	sound_play(name);
}

// --- paineis -----------------------------------------------------------------------------------
//
// O painel e o TransPanel_Background da skin (876x484, titulo no canto de cima), e as
// linhas sao os botoes de lista dela: Button_Simple para menus e Button_TwoLine_Icon
// para listas com icone e duas linhas.

constexpr float PANEL_X = 202, PANEL_Y = 112, PANEL_W = 876, PANEL_H = 484;

struct Row {
	xui::Node *node = nullptr;
	bool focused = false;
};
std::vector<Row> g_rows[2];
enum { ROW_SIMPLE, ROW_TWO_LINE };

Row &row_at(int kind, int i)
{
	std::vector<Row> &v = g_rows[kind];
	if ((int)v.size() <= i)
		v.resize((size_t)i + 1);
	Row &r = v[(size_t)i];
	if (!r.node) {
		r.node = xui::instantiate(kind == ROW_SIMPLE ? control_def("Button_Simple", 0, 0, 380, 44)
													 : control_def("Button_TwoLine_Icon", 0, 0, 800, 74),
			&g_skin_file);
		// A moldura do botao desabilitado nao tem timeline e ficaria acesa em toda linha; aqui nada e desabilitado.
		for (const char *id : { "disabledBox1", "disabledBox" })
			if (xui::Node *box = xui::find(r.node->visual, id))
				box->hidden = true;
	}
	return r;
}

void reset_rows()
{
	for (std::vector<Row> &v : g_rows)
		for (Row &r : v)
			if (r.node && r.focused) {
				xui::play_visual(r.node, "Normal");
				r.focused = false;
			}
}

void focus_row(Row &r, bool selected)
{
	if (selected != r.focused)
		xui::play_visual(r.node, selected ? "Focus" : "KillFocus");
	r.focused = selected;
}

void simple_row(ImDrawList *dl, int i, float x, float y, bool selected, const char *label, float alpha)
{
	Row &r = row_at(ROW_SIMPLE, i);
	focus_row(r, selected);
	xui::set_text(r.node, label);
	xui::update(r.node, g_dt);
	xui::draw(dl, r.node, x, y, alpha);
}

void two_line_row(ImDrawList *dl, int i, float x, float y, bool selected, const char *line1, const char *line2,
	const char *right, const xui::Texture *image, std::function<void(ImDrawList *, float, float)> picture, float alpha)
{
	Row &r = row_at(ROW_TWO_LINE, i);
	focus_row(r, selected);
	xui::set_text(xui::find(r.node->visual, "txt_line3"), line1);
	xui::set_text(xui::find(r.node->visual, "txt_line4"), line2 ? line2 : "");
	xui::set_text(xui::find(r.node->visual, "txt_line6"), right ? right : "");
	xui::Node *pic = xui::find(r.node->visual, "image_presenter");
	xui::set_image(pic, image ? *image : xui::Texture{});
	if (pic)
		pic->on_draw = picture;
	xui::update(r.node, g_dt);
	xui::draw(dl, r.node, x, y, alpha);
}

void panel_frame(ImDrawList *dl, const char *title, float alpha)
{
	dl->AddRectFilled(ImVec2(0, 0), ImVec2(g_width, g_height), rgba(0, 0, 0, 0.7f * alpha));
	xui::update(g_panel, g_dt);
	xui::draw(dl, g_panel, 0, 0, alpha);
	// O titulo no canto de cima, onde o FSD poe o dele (o presenter do visual fica vazio).
	text(dl, g_regular, 30, P(PANEL_X + 30, PANEL_Y + 18), rgba(255, 255, 255, alpha),
		fit(g_regular, 30, title, S(PANEL_W - 60)), false);
}

void panel_text(ImDrawList *dl, float x, float y, float size, ImU32 colour, const char *s, float width = 760)
{
	dl->AddText(g_regular, S(size), P(x, y), colour, s, nullptr, S(width));
}

void loading(ImDrawList *dl, float x, float y, float alpha)
{
	if (g_loading->visual && !g_loading->visual->playing)
		xui::play_visual(g_loading, "Normal");
	xui::update(g_loading, g_dt);
	xui::draw(dl, g_loading, x, y, alpha);
}

// A linha de baixo do painel: o que o servidor respondeu a ultima acao.
void action_status(ImDrawList *dl, float y, float alpha)
{
	switch (g_app.action) {
	case ACTION_BUSY:
		loading(dl, 236, y - 30, 0.8f * alpha);
		panel_text(dl, 340, y, 20, rgba(230, 232, 234, alpha), "Falando com o servidor...");
		break;
	case ACTION_OK:
		panel_text(dl, 236, y, 20, rgba(170, 230, 90, alpha), g_app.action_message);
		break;
	case ACTION_FAILED:
		panel_text(dl, 236, y, 20, rgba(255, 120, 100, alpha), g_app.action_message);
		break;
	case ACTION_IDLE:
		break;
	}
}

// Listas longas: mantem a linha escolhida na tela.
int list_first(int selected, int count, int visible)
{
	int first = (int)g_ui.scroll;
	if (selected < first)
		first = selected;
	if (selected >= first + visible)
		first = selected - visible + 1;
	if (first > count - visible)
		first = count - visible;
	if (first < 0)
		first = 0;
	g_ui.scroll = (float)first;
	return first;
}

// Itens de menu dos paineis. Cada um diz o que A faz.
enum Act { ACT_BACK, ACT_INSTALL, ACT_PLAY, ACT_EDIT_AVATAR, ACT_SIGN_OUT, ACT_SIGN_IN, ACT_INVITE_FRIEND,
	ACT_REMOVE_FRIEND, ACT_SEND_REQUEST, ACT_ACCEPT_THEIRS, ACT_SEARCH_AGAIN, ACT_INVITE, ACT_LEAVE_ROOM,
	ACT_CREATE_BOTSTRIKE, ACT_YES };

struct Item {
	Act act;
	int arg;
	char label[80];
};

// Quem e a gamertag procurada para esta conta.
enum Relation { REL_NONE, REL_FRIEND, REL_SENT, REL_RECEIVED, REL_SELF };

Relation relation(const char *gamertag, int64_t *request_id)
{
	if (strcasecmp(gamertag, g_app.gamertag) == 0)
		return REL_SELF;
	for (int i = 0; i < g_app.friend_count; i++)
		if (strcasecmp(g_app.friends[i].gamertag, gamertag) == 0)
			return REL_FRIEND;
	for (int i = 0; i < g_app.requests_out_count; i++)
		if (strcasecmp(g_app.requests_out[i].gamertag, gamertag) == 0)
			return REL_SENT;
	for (int i = 0; i < g_app.requests_in_count; i++)
		if (strcasecmp(g_app.requests_in[i].gamertag, gamertag) == 0) {
			if (request_id)
				*request_id = g_app.requests_in[i].id;
			return REL_RECEIVED;
		}
	return REL_NONE;
}

int panel_items(Item *items, int cap)
{
	int n = 0;
	auto add = [&](Act act, int arg, const char *fmt, ...) {
		if (n >= cap)
			return;
		Item &it = items[n++];
		it.act = act;
		it.arg = arg;
		va_list args;
		va_start(args, fmt);
		vsnprintf(it.label, sizeof it.label, fmt, args);
		va_end(args);
	};
	switch (g_ui.view) {
	case VIEW_STORE_ITEM: {
		const store_item &s = g_app.store[g_ui.store_index];
		if (s.installable && !g_app.installing)
			add(ACT_INSTALL, 0, installed(s) ? "Baixar de novo" : "Baixar e instalar");
		add(ACT_BACK, 0, "Voltar");
		break;
	}
	case VIEW_CONFIRM_LAUNCH:
		add(ACT_PLAY, 0, "Jogar (experimental)");
		add(ACT_BACK, 0, "Voltar");
		break;
	case VIEW_PROFILE:
		if (g_app.session == SESSION_SIGNED_IN) {
			add(ACT_EDIT_AVATAR, 0, "Editor de Avatar");
			add(ACT_SIGN_OUT, 0, "Sair do EPTX");
		} else {
			add(ACT_SIGN_IN, 0, "Entrar no EPTX");
		}
		add(ACT_BACK, 0, "Voltar");
		break;
	case VIEW_FRIEND:
		if (g_app.room.active)
			add(ACT_INVITE_FRIEND, 0, "Convidar para a sala");
		add(ACT_REMOVE_FRIEND, 0, "Remover amigo");
		add(ACT_BACK, 0, "Voltar");
		break;
	case VIEW_ADD_FRIEND:
		if (g_app.lookup == LOOKUP_FOUND) {
			int64_t id = 0;
			const Relation rel = relation(g_app.lookup_gamertag, &id);
			if (rel == REL_NONE)
				add(ACT_SEND_REQUEST, 0, "Enviar pedido de amizade");
			else if (rel == REL_RECEIVED)
				add(ACT_ACCEPT_THEIRS, (int)id, "Aceitar o pedido de %s", g_app.lookup_gamertag);
		}
		add(ACT_SEARCH_AGAIN, 0, "Procurar outra gamertag");
		add(ACT_BACK, 0, "Voltar");
		break;
	case VIEW_ROOM:
		if (g_app.room.active) {
			for (int pass = 0; pass < 2; pass++)
				for (int i = 0; i < g_app.friend_count; i++)
					if (g_app.friends[i].online == (pass == 0))
						add(ACT_INVITE, i, "Convidar %s%s", g_app.friends[i].gamertag,
							g_app.friends[i].online ? "" : " (offline)");
			add(ACT_LEAVE_ROOM, 0, g_app.room.is_host ? "Fechar a sala" : "Sair da sala");
		}
		add(ACT_BACK, 0, "Voltar");
		break;
	case VIEW_CREATE_ROOM:
		add(ACT_CREATE_BOTSTRIKE, 0, "Bot Strike - classico");
		add(ACT_BACK, 0, "Voltar");
		break;
	case VIEW_CONFIRM:
		add(ACT_YES, 0, "Sim");
		add(ACT_BACK, 0, "Nao");
		break;
	default:
		break;
	}
	return n;
}

// As linhas de menu numa coluna, rolando quando passam de visible.
void draw_menu(ImDrawList *dl, const Item *items, int count, float x, float y, int visible, float alpha)
{
	const int first = list_first(g_ui.menu, count, visible);
	for (int i = first; i < count && i < first + visible; i++)
		simple_row(dl, i - first, x, y + (float)(i - first) * 52, i == g_ui.menu, items[i].label, alpha);
}

const char *room_game_name(const char *game) { return strcmp(game, "botstrike") == 0 ? "Bot Strike" : game; }

void draw_view(ImDrawList *dl, float alpha)
{
	Item items[APP_MAX_FRIENDS + 4];
	const int count = panel_items(items, (int)(sizeof items / sizeof items[0]));
	char line[256], size[24];
	const ImU32 white = rgba(235, 238, 240, alpha), soft = rgba(190, 196, 202, alpha), green = rgba(170, 230, 90, alpha);
	switch (g_ui.view) {
	case VIEW_STORE_ITEM: {
		const store_item &s = g_app.store[g_ui.store_index];
		panel_frame(dl, s.name, alpha);
		const ImVec2 a = P(236, 196), b = P(436, 396);
		if (g_store_art[g_ui.store_index].valid)
			dl->AddImage(tex_id(g_store_art[g_ui.store_index]), a, b, ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, alpha));
		else {
			xui::Texture icon;
			char path[200];
			snprintf(path, sizeof path, "%sIcons\\XBLA.png", ICON_DIR);
			if (skin_texture(path, &icon))
				dl->AddImage(icon.id, a, b, ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, alpha));
		}
		dl->AddRect(a, b, rgba(255, 255, 255, 0.6f * alpha), 0, 0, S(2));
		human_size(s.size, size, sizeof size);
		snprintf(line, sizeof line, "%s   |   %s   |   Title ID %s", s.kind, size, s.title_id);
		panel_text(dl, 466, 196, 20, white, line);
		if (s.publisher[0]) {
			snprintf(line, sizeof line, "Editora: %s", s.publisher);
			panel_text(dl, 466, 226, 19, soft, line);
		}
		const char *where = !s.installable ? "Indisponivel: ISO precisa virar GOD antes de ir para o console."
			: installed(s)                 ? "Disponivel - ja esta na biblioteca deste console."
			: s.shows_in_dash              ? "Disponivel - aparece em Meus Jogos da dash original."
										   : "Disponivel - vai para Hdd1:\\Games (precisa de dash homebrew).";
		panel_text(dl, 466, 256, 19, s.installable ? green : rgba(255, 150, 120, alpha), where, 580);
		if (s.description[0])
			panel_text(dl, 466, 300, 17, soft, s.description, 580);
		if (g_app.installing) {
			snprintf(line, sizeof line, "Baixando %s: %d%%", g_app.install_name,
				g_app.install_total ? (int)(g_app.install_done * 100 / g_app.install_total) : 0);
			panel_text(dl, 236, 420, 18, white, line);
		}
		draw_menu(dl, items, count, 466, 446, 2, alpha);
		break;
	}
	case VIEW_STORE_SEARCH: {
		snprintf(line, sizeof line, "Pesquisa: %s", g_app.search_query);
		panel_frame(dl, line, alpha);
		if (g_app.search_loading) {
			loading(dl, 236, 200, alpha);
			panel_text(dl, 340, 230, 22, white, "Procurando no servidor...");
			break;
		}
		if (!g_app.search_count) {
			panel_text(dl, 236, 200, 22, white, "Nenhum jogo com esse nome ou Title ID.");
			panel_text(dl, 236, 236, 18, soft, "Aperte Y para pesquisar de novo.");
			break;
		}
		const int first = list_first(g_ui.menu, g_app.search_count, 5);
		for (int i = first; i < g_app.search_count && i < first + 5; i++) {
			const int k = g_app.search_index[i];
			const store_item &s = g_app.store[k];
			human_size(s.size, size, sizeof size);
			snprintf(line, sizeof line, "%s  -  %s", s.kind, size);
			const xui::Texture cover = art_texture(g_store_art[k]);
			two_line_row(dl, i - first, 238, 196 + (float)(i - first) * 80, i == g_ui.menu, s.name, line,
				installed(s) ? "instalado" : "", cover.id != ImTextureID_Invalid ? &cover : nullptr, nullptr, alpha);
		}
		break;
	}
	case VIEW_LIBRARY:
	case VIEW_CONFIRM_LAUNCH: {
		panel_frame(dl, "Biblioteca de Jogos", alpha);
		if (!g_app.library_count) {
			panel_text(dl, 236, 200, 22, white, "Nenhum jogo instalado ainda. Baixe pela Store.");
			break;
		}
		const int selected = g_ui.view == VIEW_LIBRARY ? g_ui.menu : g_ui.library_index;
		const int first = list_first(selected, g_app.library_count, 5);
		for (int i = first; i < g_app.library_count && i < first + 5; i++) {
			const library_item &it = g_app.library[i];
			const xui::Texture cover = art_texture(g_library_art[i]);
			two_line_row(dl, i - first, 238, 196 + (float)(i - first) * 80, i == selected, it.name,
				it.content_type == 0x7000 ? "Games on Demand" : "Arcade", it.title_id,
				cover.id != ImTextureID_Invalid ? &cover : nullptr, nullptr, alpha);
		}
		if (g_ui.view == VIEW_CONFIRM_LAUNCH) {
			dl->AddRectFilled(P(600, 430), P(1060, 580), rgba(10, 12, 14, 0.85f * alpha), S(6));
			for (int i = 0; i < count; i++)
				simple_row(dl, 8 + i, 640, 450 + (float)i * 52, i == g_ui.menu, items[i].label, alpha);
		}
		break;
	}
	case VIEW_PROFILE: {
		const bool signed_in = g_app.session == SESSION_SIGNED_IN;
		panel_frame(dl, signed_in ? g_app.gamertag : "Perfil", alpha);
		my_figure(dl, P(930, 560), S(330), false);
		snprintf(line, sizeof line, "Situacao: %s", session_status());
		dl->AddCircleFilled(P(244, 211), S(6), status_colour(alpha), 20);
		panel_text(dl, 258, 198, 21, white, line);
		snprintf(line, sizeof line, "Servidor: %s", g_app.server[0] ? g_app.server : "sem eptx.ini");
		panel_text(dl, 236, 232, 19, soft, line);
		if (signed_in) {
			snprintf(line, sizeof line, "Amigos: %d (%d online)   |   Pedidos: %d recebido(s), %d enviado(s)",
				g_app.friend_count, friends_online_count(), g_app.requests_in_count, g_app.requests_out_count);
			panel_text(dl, 236, 262, 19, soft, line);
			snprintf(line, sizeof line, "Sala: %s",
				g_app.room.active ? room_game_name(g_app.room.game) : "nenhuma");
			panel_text(dl, 236, 292, 19, soft, line);
		}
		// O perfil do Xbox a que esta conta esta presa, e a foto dele (que vai como avatar).
		snprintf(line, sizeof line, "Perfil do Xbox: %s%s", g_ui.xbox_profile,
			g_app.profile_xuid[0] ? "  (conta salva neste perfil)" : "  (pacote do perfil nao encontrado)");
		panel_text(dl, 300, 330, 18, soft, line);
		if (g_profile_art.valid)
			dl->AddImage(tex_id(g_profile_art), P(236, 318), P(284, 366), ImVec2(0, 0), ImVec2(1, 1),
				rgba(255, 255, 255, alpha));
		draw_menu(dl, items, count, 236, 384, 4, alpha);
		break;
	}
	case VIEW_FRIEND: {
		if (g_ui.friend_index >= g_app.friend_count) {
			g_ui.view = VIEW_HOME;
			break;
		}
		const friend_item &f = g_app.friends[g_ui.friend_index];
		panel_frame(dl, f.gamertag, alpha);
		{
			const int start = dl->VtxBuffer.Size;
			friend_figure(dl, g_ui.friend_index, P(930, 560), S(300), true);
			// friend_figure desenha com opacidade cheia; o painel aparece aos poucos.
			for (int v = start; v < dl->VtxBuffer.Size; v++) {
				ImDrawVert &vert = dl->VtxBuffer[v];
				const int a = (int)((float)((vert.col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
				vert.col = (vert.col & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
			}
		}
		dl->AddCircleFilled(P(244, 211), S(6), f.online ? rgba(110, 210, 40, alpha) : rgba(160, 160, 160, alpha), 20);
		snprintf(line, sizeof line, "%s%s%s", f.online ? "Online" : "Offline", f.title_name[0] ? " - " : "", f.title_name);
		panel_text(dl, 258, 198, 21, white, line);
		if (f.motto[0])
			panel_text(dl, 236, 234, 19, soft, f.motto, 560);
		draw_menu(dl, items, count, 236, 330, 3, alpha);
		action_status(dl, 540, alpha);
		break;
	}
	case VIEW_REQUESTS: {
		panel_frame(dl, "Solicitacoes de Amizade", alpha);
		if (!g_app.requests_in_count)
			panel_text(dl, 236, 200, 21, white, "Nenhum pedido recebido.");
		const int first = list_first(g_ui.menu, g_app.requests_in_count, 3);
		for (int i = first; i < g_app.requests_in_count && i < first + 3; i++) {
			const request_item &r = g_app.requests_in[i];
			two_line_row(dl, i - first, 238, 196 + (float)(i - first) * 80, i == g_ui.menu, r.gamertag,
				"Quer ser seu amigo", "", nullptr,
				[tag = std::string(r.gamertag)](ImDrawList *d, float w, float) {
					gamerpic(d, ImVec2(0, 0), w, tag.c_str(), 1);
				},
				alpha);
		}
		if (g_app.requests_out_count) {
			size_t w = (size_t)snprintf(line, sizeof line, "Enviados, esperando resposta: ");
			for (int i = 0; i < g_app.requests_out_count && w < sizeof line - 48; i++)
				w += (size_t)snprintf(line + w, sizeof line - w, "%s%s", i ? ", " : "", g_app.requests_out[i].gamertag);
			panel_text(dl, 236, 446, 18, soft, line);
		}
		action_status(dl, 540, alpha);
		break;
	}
	case VIEW_ADD_FRIEND: {
		panel_frame(dl, "Adicionar Amigo", alpha);
		snprintf(line, sizeof line, "Gamertag: %s", g_app.lookup_gamertag);
		panel_text(dl, 236, 196, 22, white, line);
		int64_t request = 0;
		const Relation rel = relation(g_app.lookup_gamertag, &request);
		switch (g_app.lookup) {
		case LOOKUP_LOADING:
			loading(dl, 236, 240, alpha);
			panel_text(dl, 340, 270, 20, white, "Procurando a conta no servidor...");
			break;
		case LOOKUP_FOUND: {
			static const char *const NOTES[] = { "Conta encontrada. Mande o pedido; ela aceita pela dash ou pelo app.",
				"Voces ja sao amigos.", "Voce ja mandou um pedido; falta a pessoa aceitar.",
				"Essa pessoa ja te mandou um pedido.", "Essa e a sua propria conta." };
			panel_text(dl, 236, 232, 19, rel == REL_NONE ? green : soft, NOTES[rel], 520);
			if (g_lookup_art.valid)
				draw_avatar_image(dl, P(930, 560), S(300), g_lookup_art, alpha);
			break;
		}
		case LOOKUP_NOT_FOUND:
			panel_text(dl, 236, 232, 19, rgba(255, 150, 120, alpha), "Nenhuma conta com essa gamertag no servidor EPTX.");
			break;
		case LOOKUP_FAILED:
			panel_text(dl, 236, 232, 19, rgba(255, 150, 120, alpha), "O servidor nao respondeu. Tente de novo.");
			break;
		case LOOKUP_NONE:
			break;
		}
		draw_menu(dl, items, count, 236, 330, 3, alpha);
		action_status(dl, 540, alpha);
		break;
	}
	case VIEW_ROOM: {
		const room_info &r = g_app.room;
		if (!r.active) {
			panel_frame(dl, "Sala", alpha);
			panel_text(dl, 236, 200, 21, white, "Voce nao esta em nenhuma sala.");
			draw_menu(dl, items, count, 236, 330, 3, alpha);
			action_status(dl, 540, alpha);
			break;
		}
		snprintf(line, sizeof line, "Sala de %s - %s", r.host, room_game_name(r.game));
		panel_frame(dl, line, alpha);
		snprintf(line, sizeof line, "Modo %s  |  %s  |  relay UDP na porta %d", r.mode,
			strcmp(r.state, "playing") == 0 ? "partida em andamento" : "esperando jogadores", r.relay_port);
		panel_text(dl, 236, 192, 18, soft, line);
		for (int i = 0; i < r.member_count; i++) {
			const room_member &m = r.members[i];
			const float y = 226 + (float)i * 32;
			dl->AddCircleFilled(P(244, y + 12), S(5), m.udp ? rgba(110, 210, 40, alpha) : rgba(160, 160, 160, alpha), 16);
			snprintf(line, sizeof line, "%d. %s%s  -  %s", m.slot + 1, m.gamertag,
				strcmp(m.gamertag, r.host) == 0 ? " (host)" : "", m.udp ? "conectado ao jogo" : "ainda fora do jogo");
			panel_text(dl, 258, y, 19, white, line);
		}
		panel_text(dl, 236, 360, 16, soft,
			"A partida roda dentro do jogo: abra o Bot Strike nos consoles da sala. A dash cuida da sala, dos convites "
			"e da presenca.",
			360);
		draw_menu(dl, items, count, 640, 226, 6, alpha);
		action_status(dl, 540, alpha);
		break;
	}
	case VIEW_CREATE_ROOM:
		panel_frame(dl, "Criar Sala", alpha);
		panel_text(dl, 236, 196, 19, soft,
			"Escolha o jogo. Ate 4 jogadores; quem cria e o host, e a partida roda no console dele. O servidor EPTX "
			"repassa os pacotes, sem abrir porta no roteador.",
			760);
		panel_text(dl, 236, 270, 17, soft,
			"So aparecem os jogos que ja falam com as salas do EPTX. Crossplay (360 e PC na mesma sala) depende de "
			"cada jogo usar o mesmo protocolo.",
			760);
		draw_menu(dl, items, count, 236, 360, 2, alpha);
		action_status(dl, 540, alpha);
		break;
	case VIEW_INVITES: {
		panel_frame(dl, "Convites para Jogar", alpha);
		if (!g_app.invite_count)
			panel_text(dl, 236, 200, 21, white, "Nenhum convite agora.");
		const int first = list_first(g_ui.menu, g_app.invite_count, 4);
		for (int i = first; i < g_app.invite_count && i < first + 4; i++) {
			const invite_item &iv = g_app.invites[i];
			snprintf(line, sizeof line, "%s convidou voce", iv.host);
			char sub[128];
			snprintf(sub, sizeof sub, "%s - %s - %d jogador(es)", room_game_name(iv.game), iv.mode, iv.members);
			two_line_row(dl, i - first, 238, 196 + (float)(i - first) * 80, i == g_ui.menu, line, sub, "",
				nullptr,
				[tag = std::string(iv.host)](ImDrawList *d, float w, float) {
					gamerpic(d, ImVec2(0, 0), w, tag.c_str(), 1);
				},
				alpha);
		}
		action_status(dl, 540, alpha);
		break;
	}
	case VIEW_CONFIRM: {
		const char *title = "Confirmar", *question = "";
		if (g_ui.confirm == CONFIRM_REMOVE_FRIEND) {
			title = "Remover amigo";
			snprintf(line, sizeof line, "Tirar %s da sua lista de amigos?",
				g_ui.friend_index < g_app.friend_count ? g_app.friends[g_ui.friend_index].gamertag : "");
			question = line;
		} else if (g_ui.confirm == CONFIRM_SIGN_OUT) {
			title = "Sair do EPTX";
			question = "Sair da conta neste console? A sessao salva e apagada; a senha nunca fica gravada.";
		} else {
			title = g_app.room.is_host ? "Fechar a sala" : "Sair da sala";
			question = g_app.room.is_host ? "Voce e o host: sair fecha a sala para todos." : "Sair da sala?";
		}
		panel_frame(dl, title, alpha);
		panel_text(dl, 236, 200, 21, white, question, 760);
		draw_menu(dl, items, count, 236, 300, 2, alpha);
		break;
	}
	case VIEW_ABOUT: {
		panel_frame(dl, "Sobre o EPTX NXE Edition", alpha);
		static const char *const lines[] = {
			"Dash do EPTX sobre a interface do Freestyle Dash, no visual da NXE. Nao fala com a Microsoft.",
			"",
			"Freestyle Dash 2.0 (TeamFSD) - GPL versao 3: as cenas e a arte da skin Default.",
			"xsdk - MIT. Partes de libxenon e Xenia sob BSD de 3 clausulas.",
			"Dear ImGui (imgui-xenon) - MIT, (c) Omar Cornut.",
			"Fonte Selawik - SIL Open Font License 1.1.",
			"stb_image - dominio publico, Sean Barrett.",
			"",
			"Os textos completos das licencas estao na pasta licenses do pacote.",
		};
		for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
			panel_text(dl, 236, 196 + (float)i * 32, 19, white, lines[i]);
		break;
	}
	case VIEW_HOME:
	case VIEW_GUIDE:
	case VIEW_AVATAR_EDITOR:
		break;
	}
}

// --- entrada ------------------------------------------------------------------------------------

void open_view(View v)
{
	if (v != VIEW_HOME && v != VIEW_GUIDE && v != g_ui.view)
		sound_play("snd_panelunfold");
	g_ui.view = v;
	g_ui.menu = 0;
	g_ui.scroll = 0;
	g_app.action = ACTION_IDLE;
	g_app.action_message[0] = '\0';
	reset_rows();
}

void go_tab(int target)
{
	g_ui.tab_target = target;
	g_ui.tab = ((target % TAB_COUNT) + TAB_COUNT) % TAB_COUNT;
}

// O tab mais perto de tab_target (sem dar a volta) que e igual a tab.
void go_tab_index(int tab)
{
	int target = g_ui.tab_target;
	int diff = (tab - g_ui.tab + TAB_COUNT) % TAB_COUNT;
	if (diff > TAB_COUNT / 2)
		diff -= TAB_COUNT;
	go_tab(target + diff);
}

void ask(Confirm what, View back)
{
	const View from = back;
	open_view(VIEW_CONFIRM);
	g_ui.confirm = what;
	g_ui.confirm_back = from;
	g_ui.menu = 1; // "Nao" ja escolhido
}

void sign_in_or_retry()
{
	if (g_app.session == SESSION_SIGNED_OUT)
		start_login();
	else if (g_app.session == SESSION_NO_SERVER)
		UNLOCKED(app_refresh());
}

void activate_tile(const Tile &tile)
{
	const bool signed_in = g_app.session == SESSION_SIGNED_IN;
	switch (tile.kind) {
	case TILE_PROFILE:
		if (signed_in)
			open_view(VIEW_PROFILE);
		else
			sign_in_or_retry();
		break;
	case TILE_DOWNLOADS:
		go_tab_index(TAB_STORE);
		break;
	case TILE_LIBRARY:
		open_view(VIEW_LIBRARY);
		break;
	case TILE_GAME:
		open_view(VIEW_CONFIRM_LAUNCH);
		g_ui.library_index = tile.index;
		break;
	case TILE_STORE_ITEM:
		open_view(VIEW_STORE_ITEM);
		g_ui.store_index = tile.index;
		break;
	case TILE_STORE_SEARCH:
		open_text_keyboard(KB_SEARCH);
		break;
	case TILE_FRIEND:
		open_view(VIEW_FRIEND);
		g_ui.friend_index = tile.index;
		break;
	case TILE_ADD_FRIEND:
		if (!signed_in) {
			sign_in_or_retry();
			break;
		}
		open_text_keyboard(KB_FRIEND);
		break;
	case TILE_REQUESTS:
		if (signed_in)
			open_view(VIEW_REQUESTS);
		else
			sign_in_or_retry();
		break;
	case TILE_ONLINE_FRIENDS:
		go_tab_index(TAB_FRIENDS);
		break;
	case TILE_AVATAR_EDITOR:
		open_avatar_editor();
		break;
	case TILE_SERVER:
	case TILE_REFRESH:
		UNLOCKED(app_refresh());
		break;
	case TILE_SIGN:
		if (signed_in)
			ask(CONFIRM_SIGN_OUT, VIEW_HOME);
		else
			sign_in_or_retry();
		break;
	case TILE_ABOUT:
		open_view(VIEW_ABOUT);
		break;
	case TILE_DASHBOARD:
		// Sai do titulo; com esta dash como padrao do DashLaunch, veja o README antes.
		UNLOCKED(launch("\\SystemRoot\\dash.xex", "a dashboard original"));
		break;
	case TILE_ROOM:
		open_view(VIEW_ROOM);
		break;
	case TILE_CREATE_ROOM:
		if (signed_in)
			open_view(VIEW_CREATE_ROOM);
		else
			sign_in_or_retry();
		break;
	case TILE_INVITES:
		if (signed_in)
			open_view(VIEW_INVITES);
		else
			sign_in_or_retry();
		break;
	case TILE_STORE_EMPTY:
	case TILE_FRIENDS_EMPTY:
		sign_in_or_retry();
		break;
	}
}

void guide_go(int tab, int tile)
{
	g_ui.view = VIEW_HOME;
	go_tab_index(tab);
	if (tile >= 0)
		g_ui.tile[tab] = tile;
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
		guide_go(TAB_HOME, 0);
		return;
	}
	if (pressed & INPUT_GAMEPAD_X && g_app.session == SESSION_SIGNED_IN) {
		ask(CONFIRM_SIGN_OUT, VIEW_HOME);
		return;
	}
	if (!(pressed & INPUT_GAMEPAD_A))
		return;
	switch (g_ui.menu) {
	case 0:
		guide_go(TAB_HOME, -1);
		break;
	case 1:
		guide_go(TAB_STORE, -1);
		break;
	case 2:
		guide_go(TAB_FRIENDS, -1);
		break;
	case 3:
		open_view(VIEW_LIBRARY);
		break;
	case 4:
		guide_go(TAB_GAMES, -1);
		break;
	case 5:
		g_ui.view = VIEW_HOME;
		UNLOCKED(launch("\\SystemRoot\\dash.xex", "a dashboard original"));
		break;
	case 6:
		if (g_app.session == SESSION_SIGNED_IN)
			ask(CONFIRM_SIGN_OUT, VIEW_HOME);
		else {
			g_ui.view = VIEW_HOME;
			sign_in_or_retry();
		}
		break;
	}
}

void run_item(const Item &it)
{
	switch (it.act) {
	case ACT_BACK:
		if (g_ui.view == VIEW_CONFIRM)
			open_view(g_ui.confirm_back);
		else if (g_ui.view == VIEW_CONFIRM_LAUNCH) {
			g_ui.view = VIEW_LIBRARY;
			g_ui.menu = g_ui.library_index;
		} else
			g_ui.view = VIEW_HOME;
		break;
	case ACT_INSTALL: {
		const int64_t id = g_app.store[g_ui.store_index].id;
		g_ui.view = VIEW_HOME;
		UNLOCKED(app_install(id));
		break;
	}
	case ACT_PLAY:
		if (g_ui.library_index < g_app.library_count) {
			char path[300];
			snprintf(path, sizeof path, "%s", g_app.library[g_ui.library_index].path);
			UNLOCKED(launch(path, "o jogo"));
		}
		g_ui.view = VIEW_LIBRARY;
		break;
	case ACT_EDIT_AVATAR:
		open_avatar_editor();
		break;
	case ACT_SIGN_OUT:
		ask(CONFIRM_SIGN_OUT, VIEW_PROFILE);
		break;
	case ACT_SIGN_IN:
		g_ui.view = VIEW_HOME;
		sign_in_or_retry();
		break;
	case ACT_INVITE_FRIEND:
		if (g_ui.friend_index < g_app.friend_count) {
			const int64_t user = g_app.friends[g_ui.friend_index].user_id;
			UNLOCKED(app_room_invite(user));
		}
		break;
	case ACT_REMOVE_FRIEND:
		ask(CONFIRM_REMOVE_FRIEND, VIEW_FRIEND);
		break;
	case ACT_SEND_REQUEST: {
		char tag[40];
		snprintf(tag, sizeof tag, "%s", g_app.lookup_gamertag);
		UNLOCKED(app_send_friend_request(tag));
		break;
	}
	case ACT_ACCEPT_THEIRS:
		UNLOCKED(app_accept_request(it.arg));
		break;
	case ACT_SEARCH_AGAIN:
		open_text_keyboard(KB_FRIEND);
		break;
	case ACT_INVITE: {
		const int64_t user = g_app.friends[it.arg].user_id;
		UNLOCKED(app_room_invite(user));
		break;
	}
	case ACT_LEAVE_ROOM:
		ask(CONFIRM_LEAVE_ROOM, VIEW_ROOM);
		break;
	case ACT_CREATE_BOTSTRIKE:
		open_view(VIEW_ROOM);
		g_app.action = ACTION_BUSY; // o painel da sala mostra "falando com o servidor" ate a resposta
		UNLOCKED(app_room_create("botstrike", "classic"));
		break;
	case ACT_YES: {
		const Confirm what = g_ui.confirm;
		if (what == CONFIRM_REMOVE_FRIEND && g_ui.friend_index < g_app.friend_count) {
			const int64_t friendship = g_app.friends[g_ui.friend_index].friendship_id;
			open_view(VIEW_HOME);
			UNLOCKED(app_remove_friend(friendship));
		} else if (what == CONFIRM_SIGN_OUT) {
			open_view(VIEW_HOME);
			UNLOCKED(app_sign_out());
		} else if (what == CONFIRM_LEAVE_ROOM) {
			open_view(VIEW_ROOM);
			UNLOCKED(app_room_leave());
		}
		break;
	}
	}
}

// Quantas linhas a lista do painel tem (as listas que nao sao menus de Item).
int list_count()
{
	switch (g_ui.view) {
	case VIEW_STORE_SEARCH:
		return g_app.search_loading ? 0 : g_app.search_count;
	case VIEW_LIBRARY:
		return g_app.library_count;
	case VIEW_REQUESTS:
		return g_app.requests_in_count;
	case VIEW_INVITES:
		return g_app.invite_count;
	default:
		return -1;
	}
}

void handle_input(const INPUT_GAMEPAD &pad, float dt)
{
	const uint16_t pressed = pad.buttons & ~g_pad.previous;
	g_pad.previous = pad.buttons;
	const int dir = directions(pad, dt);
	press_hints(pressed);

	if (g_ui.view == VIEW_GUIDE) {
		guide_input(pressed, dir);
		if (g_ui.view != VIEW_GUIDE)
			sound_play("HUD_close");
		return;
	}
	if (g_ui.view == VIEW_AVATAR_EDITOR) {
		if (pressed & (INPUT_GAMEPAD_B | INPUT_GAMEPAD_BACK))
			g_ui.view = VIEW_PROFILE; // cancela: o avatar salvo continua o mesmo
		else
			avatar_editor_input(pressed, dir);
		return;
	}
	if (g_ui.view == VIEW_HOME) {
		if (pressed & INPUT_GAMEPAD_BACK) {
			open_view(VIEW_GUIDE);
			sound_play("HUD_open");
			return;
		}
		// Cima e baixo trocam de aba (dando a volta, como o FSD); esquerda e direita, de bloco.
		// Os sons sao os que as timelines do FSD tocam nisso: MainScene (1To2... desce,
		// 2To1... sobe) e ScnMain (Sound_TabSwitch).
		if (dir & DIR_DOWN) {
			go_tab(g_ui.tab_target + 1);
			sound_play("snd_channeldown");
		}
		if (dir & DIR_UP) {
			go_tab(g_ui.tab_target - 1);
			sound_play("snd_channelup");
		}
		const int count = g_tile_count[g_ui.tab];
		int &tile = g_ui.tile[g_ui.tab];
		if (tile >= count)
			tile = count - 1;
		if (dir & DIR_RIGHT && tile + 1 < count) {
			tile++;
			sound_play("snd_panelright");
		}
		if (dir & DIR_LEFT && tile > 0) {
			tile--;
			sound_play("snd_panelleft");
		}
		if (pressed & INPUT_GAMEPAD_A && count > 0)
			activate_tile(g_tiles[g_ui.tab][tile]);
		if (pressed & INPUT_GAMEPAD_Y)
			UNLOCKED(app_refresh());
		return;
	}

	const int rows = list_count();
	if (rows >= 0) {
		// Listas: cima e baixo escolhem; A, X e Y fazem o que a linha de baixo da tela diz.
		if (dir & DIR_DOWN && g_ui.menu + 1 < rows)
			g_ui.menu++;
		if (dir & DIR_UP && g_ui.menu > 0)
			g_ui.menu--;
		if (pressed & (INPUT_GAMEPAD_B | INPUT_GAMEPAD_BACK)) {
			g_ui.view = VIEW_HOME;
			return;
		}
		const int i = g_ui.menu;
		switch (g_ui.view) {
		case VIEW_STORE_SEARCH:
			if (pressed & INPUT_GAMEPAD_A && i < rows) {
				const int index = g_app.search_index[i];
				open_view(VIEW_STORE_ITEM);
				g_ui.store_index = index;
			}
			if (pressed & INPUT_GAMEPAD_Y) {
				open_text_keyboard(KB_SEARCH);
			}
			break;
		case VIEW_LIBRARY:
			if (pressed & INPUT_GAMEPAD_A && i < rows) {
				g_ui.library_index = i;
				g_ui.view = VIEW_CONFIRM_LAUNCH;
				g_ui.menu = 0;
				reset_rows();
			}
			break;
		case VIEW_REQUESTS:
			if (i < rows && g_app.action != ACTION_BUSY) {
				const int64_t id = g_app.requests_in[i].id;
				if (pressed & INPUT_GAMEPAD_A)
					UNLOCKED(app_accept_request(id));
				else if (pressed & INPUT_GAMEPAD_X)
					UNLOCKED(app_decline_request(id));
			}
			break;
		case VIEW_INVITES:
			if (pressed & INPUT_GAMEPAD_A && i < rows && g_app.action != ACTION_BUSY) {
				const int64_t room = g_app.invites[i].room_id;
				open_view(VIEW_ROOM);
				UNLOCKED(app_room_join(room));
			}
			break;
		default:
			break;
		}
		if (g_ui.menu >= rows && rows > 0)
			g_ui.menu = rows - 1;
		return;
	}

	Item items[APP_MAX_FRIENDS + 4];
	const int count = panel_items(items, (int)(sizeof items / sizeof items[0]));
	if (pressed & (INPUT_GAMEPAD_B | INPUT_GAMEPAD_BACK)) {
		run_item(Item{ ACT_BACK, 0, "" });
		return;
	}
	if (dir & DIR_DOWN && g_ui.menu + 1 < count)
		g_ui.menu++;
	if (dir & DIR_UP && g_ui.menu > 0)
		g_ui.menu--;
	if (g_ui.menu >= count)
		g_ui.menu = count - 1;
	if (pressed & INPUT_GAMEPAD_A && count > 0 && g_app.action != ACTION_BUSY)
		run_item(items[g_ui.menu]);
}

void draw_footer(ImDrawList *dl)
{
	update_hints();
	switch (g_ui.view) {
	case VIEW_HOME:
		draw_hints(dl, { { HINT_A, "Selecionar" }, { HINT_Y, "Atualizar" }, { HINT_BACK, "Guide" } });
		break;
	case VIEW_GUIDE:
		break; // o Guide tem a linha de botoes dele
	case VIEW_AVATAR_EDITOR:
		draw_hints(dl, { { HINT_A, "Salvar" }, { HINT_B, "Cancelar" }, { HINT_X, "Aleatorio" }, { HINT_Y, "Padrao" } });
		break;
	case VIEW_REQUESTS:
		draw_hints(dl, { { HINT_A, "Aceitar" }, { HINT_X, "Recusar" }, { HINT_B, "Voltar" } });
		break;
	case VIEW_STORE_SEARCH:
		draw_hints(dl, { { HINT_A, "Abrir" }, { HINT_Y, "Nova pesquisa" }, { HINT_B, "Voltar" } });
		break;
	case VIEW_ABOUT:
		draw_hints(dl, { { HINT_B, "Voltar" } });
		break;
	default:
		draw_hints(dl, { { HINT_A, "Selecionar" }, { HINT_B, "Voltar" } });
		break;
	}
}

// Os paineis que acompanham amigos, pedidos e salas: com eles abertos, a thread consulta a cada poucos segundos.
bool watching()
{
	switch (g_ui.view) {
	case VIEW_FRIEND:
	case VIEW_REQUESTS:
	case VIEW_ADD_FRIEND:
	case VIEW_ROOM:
	case VIEW_INVITES:
		return true;
	case VIEW_HOME:
		return g_ui.tab == TAB_FRIENDS || g_ui.tab == TAB_MULTIPLAYER;
	default:
		return false;
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

// --- o quadro -------------------------------------------------------------------------------------

// Sem a skin (faltou game:\Skin no pacote): uma tela simples, para a pessoa saber o que houve.
void draw_no_skin(ImDrawList *dl)
{
	dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(g_width, g_height), rgba(20, 24, 28), rgba(20, 24, 28),
		rgba(40, 46, 52), rgba(40, 46, 52));
	char line[200];
	snprintf(line, sizeof line, "A skin do Freestyle Dash nao foi encontrada em %s.", g_skin_dir);
	text(dl, g_regular, 26, P(120, 300), rgba(255, 255, 255), line);
	text(dl, g_regular, 20, P(120, 344), rgba(200, 204, 208),
		"Copie a pasta Skin (main.xui, skin.xui e Images) para junto do default.xex.", false);
}

// O perfil do Xbox logado, perguntado ao XAM duas vezes por segundo (fora da trava de g_app).
// Mudou, a thread de rede troca para a conta EPTX desse perfil.
void poll_profile(float dt)
{
	g_ui.profile_poll -= dt;
	if (g_ui.profile_known && g_ui.profile_poll > 0)
		return;
	g_ui.profile_poll = 0.5f;
	uint32_t user = XSDK_XAM_USER_NONE;
	char tag[XSDK_XAM_GAMERTAG_BYTES] = "";
	if (xsdk_xam_signed_in_user(&user, tag) != ERROR_SUCCESS)
		tag[0] = '\0';
	if (g_ui.profile_known && strcmp(tag, g_ui.xbox_profile) == 0)
		return;
	const bool changed = g_ui.profile_known;
	g_ui.profile_known = true;
	snprintf(g_ui.xbox_profile, sizeof g_ui.xbox_profile, "%s", tag);
	xsdk_log_write("eptx: perfil do Xbox: %s", tag[0] ? tag : "(nenhum)");
	if (changed) {
		// Outra pessoa: nada do painel ou do teclado da anterior fica aberto.
		g_ui.view = VIEW_HOME;
		g_kb = KB_NONE;
		g_kb_gamertag[0] = '\0';
	}
	app_set_profile(tag);
}

// Sem perfil do Xbox logado a dash nao entra: a conta EPTX e o avatar vem do perfil.
void draw_profile_gate(ImDrawList *dl)
{
	panel_frame(dl, "Entre num perfil do Xbox", 1);
	const ImU32 white = rgba(235, 238, 240), soft = rgba(190, 196, 202);
	panel_text(dl, 236, 200, 22, white,
		"O EPTX NXE Edition usa o perfil logado no console: a conta EPTX fica salva nele, e a foto do perfil vira "
		"o seu avatar para os amigos.",
		760);
	panel_text(dl, 236, 290, 20, soft, "Aperte o botao Xbox do controle e escolha um perfil.", 760);
	panel_text(dl, 236, 326, 18, soft, "Cada perfil tem a sua conta: trocar de perfil troca de conta.", 760);
	emblem(dl, P(640, 470), S(40));
}

void frame(const INPUT_GAMEPAD &pad, bool input_allowed, float dt)
{
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	g_dt = dt;
	int want_art[16], want_count = 0;
	int want_lib[8], want_lib_count = 0;
	int want_friend[8], want_friend_count = 0;
	poll_profile(dt);
	const bool gated = !g_ui.xbox_profile[0];

	app_lock();
	g_uploads_this_frame = 0;
	if (g_profile_serial != g_app.profile_serial) {
		free_arts(&g_profile_art, 1);
		g_profile_serial = g_app.profile_serial;
	}
	take_image(g_app.profile_art, g_app.profile_image, g_profile_art);
	if (g_friends_serial != g_app.friends_serial) {
		free_arts(g_friend_art, APP_MAX_FRIENDS);
		g_friends_serial = g_app.friends_serial;
		if (g_ui.friend_index >= g_app.friend_count && g_ui.view == VIEW_FRIEND)
			g_ui.view = VIEW_HOME;
	}
	if (g_avatar_serial != g_app.avatar_serial) {
		// Saiu da conta ou trocou de avatar: a textura velha nao serve mais.
		free_arts(&g_avatar_art, 1);
		g_avatar_serial = g_app.avatar_serial;
	}
	if (g_lookup_serial != g_app.lookup_serial) {
		free_arts(&g_lookup_art, 1);
		g_lookup_serial = g_app.lookup_serial;
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
	for (int t = 0; t < TAB_COUNT; t++)
		g_tile_count[t] = build_tiles(t, g_tiles[t]);

	if (input_allowed && !gated)
		handle_input(pad, dt);

	g_ui.tab_pos = approach(g_ui.tab_pos, (float)g_ui.tab_target, dt, 10);
	for (int t = 0; t < TAB_COUNT; t++) {
		if (g_ui.tile[t] >= g_tile_count[t])
			g_ui.tile[t] = g_tile_count[t] - 1;
		if (g_ui.tile[t] < 0)
			g_ui.tile[t] = 0;
		g_ui.tile_pos[t] = approach(g_ui.tile_pos[t], (float)g_ui.tile[t], dt);
	}
	g_ui.view_fade = approach(g_ui.view_fade, g_ui.view == VIEW_HOME ? 0.0f : 1.0f, dt, 14);

	// Capas: sobe as prontas e pede as dos blocos perto da tela e as da pesquisa.
	for (int i = 0; i < g_app.store_count; i++)
		take_art(g_app.store[i], g_store_art[i]);
	for (int i = 0; i < g_app.library_count; i++)
		take_art(g_app.library[i], g_library_art[i]);
	int focus = g_ui.tile[TAB_STORE] - 1; // o primeiro bloco da loja e o da pesquisa
	for (int i = focus - 1; i <= focus + 6 && want_count < 8; i++)
		if (i >= 0 && i < g_app.store_count && g_app.store[i].art == ART_NONE)
			want_art[want_count++] = i;
	if (g_ui.view == VIEW_STORE_SEARCH)
		for (int i = 0; i < g_app.search_count && want_count < 16; i++) {
			const int k = g_app.search_index[i];
			if (g_app.store[k].art == ART_NONE)
				want_art[want_count++] = k;
		}
	for (int i = 0; i < g_app.library_count && want_lib_count < 8; i++)
		if (g_app.library[i].art == ART_NONE)
			want_lib[want_lib_count++] = i;
	take_image(g_app.avatar_art, g_app.avatar_image, g_avatar_art);
	take_image(g_app.lookup_art, g_app.lookup_image, g_lookup_art);
	for (int i = 0; i < g_app.friend_count; i++) {
		take_art(g_app.friends[i], g_friend_art[i]);
		if (g_app.friends[i].art == ART_NONE && g_app.friends[i].avatar_updated > 0 && want_friend_count < 8)
			want_friend[want_friend_count++] = i;
	}

	// GIFs: parados no primeiro quadro, rodam inteiros enquanto o bloco esta escolhido.
	const bool home = g_ui.view == VIEW_HOME;
	const Tile &chosen = g_tiles[g_ui.tab][g_ui.tile[g_ui.tab] < 0 ? 0 : g_ui.tile[g_ui.tab]];
	animate(g_avatar_art, (home && chosen.kind == TILE_PROFILE) || g_ui.view == VIEW_PROFILE, dt);
	const int chosen_friend = g_ui.view == VIEW_FRIEND ? g_ui.friend_index
		: home && chosen.kind == TILE_FRIEND         ? chosen.index
													 : -1;
	for (int i = 0; i < g_app.friend_count; i++)
		animate(g_friend_art[i], i == chosen_friend, dt);

	if (g_skin_ok && gated) {
		// So o fundo do FSD e o aviso: sem perfil, nenhuma aba abre.
		xui::update(g_backdrop, dt);
		xui::draw(dl, g_backdrop);
		draw_profile_gate(dl);
		draw_toast(dl, dt);
		app_unlock();
		app_watch(false);
		return;
	}
	if (g_skin_ok) {
		xui::update(g_backdrop, dt);
		xui::draw(dl, g_backdrop);
		draw_captions(dl);
		// A aba atual e a que esta chegando: os blocos de uma somem enquanto os da outra aparecem.
		for (int t = 0; t < TAB_COUNT; t++) {
			float d = fmodf((float)t - g_ui.tab_pos, (float)TAB_COUNT);
			if (d > TAB_COUNT / 2.0f)
				d -= TAB_COUNT;
			if (d <= -TAB_COUNT / 2.0f)
				d += TAB_COUNT;
			if (fabsf(d) < 1)
				draw_blades(dl, t, 1 - fabsf(d));
		}
		char where[32];
		snprintf(where, sizeof where, "%d de %d", g_ui.tile[g_ui.tab] + 1, g_tile_count[g_ui.tab]);
		xui::set_text(g_counter, where);
		xui::update(g_counter, dt);
		xui::draw(dl, g_counter);
		draw_profile_corner(dl);
	} else {
		draw_no_skin(dl);
	}
	draw_emblem(dl);

	if (g_ui.view_fade > 0.01f) {
		const float alpha = g_ui.view_fade;
		if (g_ui.view == VIEW_GUIDE)
			draw_guide(dl, alpha);
		else if (g_ui.view == VIEW_AVATAR_EDITOR)
			draw_avatar_editor(dl, alpha);
		else if (g_skin_ok)
			draw_view(dl, alpha);
	}
	if (g_skin_ok)
		draw_footer(dl);
	draw_toast(dl, dt);
	const bool watch = watching();
	app_unlock();

	app_watch(watch);
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
	// Sem a skin a dash segue: draw_no_skin explica o que falta.
	g_skin_ok = load_skin();
	xsdk_log_write("eptx: skin do FSD em %s: %s", g_skin_dir, g_skin_ok ? "carregada" : "faltando");
	sound_start(g_skin_dir);

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
