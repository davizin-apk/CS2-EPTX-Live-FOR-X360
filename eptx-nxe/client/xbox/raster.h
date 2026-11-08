// Rasterizador de software para listas do ImGui: pinta os triangulos de um
// ImDrawList num quadro RGBA na memoria, sem GPU. A dash usa para gravar o
// corpo do avatar em PNG, e o preview no PC para gerar as telas.
//
// O quadro guarda cor pre-multiplicada pelo alfa, para o fundo poder ficar
// transparente; raster_to_rgba8 desfaz isso no fim.

#ifndef EPTX_RASTER_H
#define EPTX_RASTER_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "imgui.h"

struct RasterTexture {
	int w, h;
	const uint8_t *px;
	int bytes; // 4 para RGBA, 1 para so alfa (branco)
};

struct RasterTarget {
	int w, h;
	float *px; // w*h*4, RGBA pre-multiplicado
};

inline void raster_sample(const RasterTexture &img, float u, float v, float out[4])
{
	const float x = u * (float)img.w - 0.5f, y = v * (float)img.h - 0.5f;
	const int x0 = (int)floorf(x), y0 = (int)floorf(y);
	const float fx = x - (float)x0, fy = y - (float)y0;
	float acc[4] = { 0, 0, 0, 0 };
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) {
			const int sx = x0 + i < 0 ? 0 : x0 + i >= img.w ? img.w - 1 : x0 + i;
			const int sy = y0 + j < 0 ? 0 : y0 + j >= img.h ? img.h - 1 : y0 + j;
			const float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
			const uint8_t *p = &img.px[((size_t)sy * (size_t)img.w + (size_t)sx) * (size_t)img.bytes];
			for (int c = 0; c < 4; c++) {
				const uint8_t value = img.bytes == 4 ? p[c] : c == 3 ? p[0] : 255;
				acc[c] += wgt * (float)value / 255.0f;
			}
		}
	memcpy(out, acc, sizeof acc);
}

inline void raster_triangle(RasterTarget &f, const ImDrawVert *v[3], const RasterTexture *tex, const ImVec4 &clip)
{
	const float x0 = fminf(fminf(v[0]->pos.x, v[1]->pos.x), v[2]->pos.x);
	const float x1 = fmaxf(fmaxf(v[0]->pos.x, v[1]->pos.x), v[2]->pos.x);
	const float y0 = fminf(fminf(v[0]->pos.y, v[1]->pos.y), v[2]->pos.y);
	const float y1 = fmaxf(fmaxf(v[0]->pos.y, v[1]->pos.y), v[2]->pos.y);
	const int ix0 = (int)fmaxf(floorf(fmaxf(x0, clip.x)), 0), ix1 = (int)fminf(ceilf(fminf(x1, clip.z)), (float)f.w);
	const int iy0 = (int)fmaxf(floorf(fmaxf(y0, clip.y)), 0), iy1 = (int)fminf(ceilf(fminf(y1, clip.w)), (float)f.h);
	const ImVec2 a = v[0]->pos, b = v[1]->pos, c = v[2]->pos;
	const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	if (fabsf(area) < 1e-6f)
		return;
	for (int y = iy0; y < iy1; y++)
		for (int x = ix0; x < ix1; x++) {
			// Amostra um pouco fora do centro, num ponto que nunca cai exatamente numa aresta:
			// assim a borda que dois triangulos dividem e pintada por um so, sem regra de desempate.
			const float px = (float)x + 0.5f + 0.000731f, py = (float)y + 0.5f + 0.000419f;
			const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
			const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
			const float w2 = 1 - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0)
				continue;
			float col[4];
			for (int k = 0; k < 4; k++) {
				const int shift = k * 8;
				col[k] = (w0 * (float)((v[0]->col >> shift) & 0xFF) + w1 * (float)((v[1]->col >> shift) & 0xFF) +
							 w2 * (float)((v[2]->col >> shift) & 0xFF)) /
					255.0f;
			}
			if (tex) {
				float t[4];
				raster_sample(*tex, w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x,
					w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y, t);
				for (int k = 0; k < 4; k++)
					col[k] *= t[k];
			}
			float *dst = &f.px[((size_t)y * (size_t)f.w + (size_t)x) * 4];
			for (int k = 0; k < 3; k++)
				dst[k] = col[k] * col[3] + dst[k] * (1 - col[3]);
			dst[3] = col[3] + dst[3] * (1 - col[3]);
		}
}

// texture_of devolve a textura de um comando (ou nullptr para cor lisa).
template <typename Lookup> void raster_draw_list(RasterTarget &f, const ImDrawList *dl, Lookup texture_of)
{
	for (const ImDrawCmd &cmd : dl->CmdBuffer) {
		if (cmd.UserCallback)
			continue;
		const RasterTexture *tex = texture_of(cmd);
		for (unsigned i = 0; i < cmd.ElemCount; i += 3) {
			const ImDrawVert *v[3];
			for (int k = 0; k < 3; k++)
				v[k] = &dl->VtxBuffer[(int)cmd.VtxOffset + dl->IdxBuffer[(int)(cmd.IdxOffset + i) + k]];
			raster_triangle(f, v, tex, cmd.ClipRect);
		}
	}
}

// Para bytes RGBA comuns, tirando a pre-multiplicacao.
inline void raster_to_rgba8(const RasterTarget &f, uint8_t *out)
{
	for (size_t i = 0; i < (size_t)f.w * (size_t)f.h; i++) {
		const float *p = &f.px[i * 4];
		const float a = p[3];
		for (int k = 0; k < 3; k++) {
			const float c = a > 1e-5f ? p[k] / a : 0;
			out[i * 4 + k] = (uint8_t)(fminf(fmaxf(c, 0), 1) * 255.0f + 0.5f);
		}
		out[i * 4 + 3] = (uint8_t)(fminf(fmaxf(a, 0), 1) * 255.0f + 0.5f);
	}
}

#endif
