/*
 * PNG e GIF para RGBA. So os decodificadores de PNG e GIF do stb_image entram:
 * capas do catalogo e icones do cabecalho STFS sao sempre PNG, e o GIF e o avatar
 * animado salvo no 360sona.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "image.h"

enum { MAX_SIDE = 512 };

bool image_decode_png(const uint8_t *data, size_t len, app_image *out)
{
	int width = 0, height = 0, channels = 0;
	out->rgba = NULL;
	out->width = out->height = 0;
	if (!data || len == 0 || len > 0x7FFFFFFF)
		return false;
	/* Um icone de 64x64 e o normal; recusar o absurdo protege a memoria do console. */
	if (!stbi_info_from_memory(data, (int)len, &width, &height, &channels) || width > MAX_SIDE || height > MAX_SIDE)
		return false;
	uint8_t *rgba = stbi_load_from_memory(data, (int)len, &width, &height, &channels, 4);
	if (!rgba)
		return false;
	out->rgba = rgba;
	out->width = width;
	out->height = height;
	out->frames = 1;
	out->delays = NULL;
	return true;
}

void image_free(app_image *image)
{
	if (image->rgba)
		stbi_image_free(image->rgba);
	free(image->delays);
	image->rgba = NULL;
	image->delays = NULL;
	image->width = image->height = image->frames = 0;
}

/* Quantos quadros o GIF tem, andando pelos blocos sem decodificar nada. */
static int gif_frame_count(const uint8_t *p, size_t len)
{
	if (len < 13)
		return 0;
	size_t i = 13 + (p[10] & 0x80 ? 3u << ((p[10] & 7) + 1) : 0);
	int frames = 0;
	while (i < len) {
		const uint8_t kind = p[i++];
		if (kind == 0x3B)
			break;
		if (kind == 0x21) {
			i++;
		} else if (kind == 0x2C) {
			if (i + 9 > len)
				break;
			const uint8_t local = p[i + 8];
			i += 9 + (local & 0x80 ? 3u << ((local & 7) + 1) : 0) + 1;
			frames++;
		} else {
			break;
		}
		while (i < len && p[i])
			i += (size_t)p[i] + 1;
		i++;
	}
	return frames;
}

/* Reduz um quadro w x h para dw x dh tirando a media de cada caixa de pixels. */
static void shrink(const uint8_t *src, int w, int h, uint8_t *dst, int dw, int dh)
{
	for (int y = 0; y < dh; y++) {
		const int y0 = y * h / dh, y1 = (y + 1) * h / dh > y0 ? (y + 1) * h / dh : y0 + 1;
		for (int x = 0; x < dw; x++) {
			const int x0 = x * w / dw, x1 = (x + 1) * w / dw > x0 ? (x + 1) * w / dw : x0 + 1;
			unsigned sum[4] = { 0, 0, 0, 0 }, n = 0;
			for (int sy = y0; sy < y1; sy++)
				for (int sx = x0; sx < x1; sx++, n++)
					for (int c = 0; c < 4; c++)
						sum[c] += src[((size_t)sy * (size_t)w + (size_t)sx) * 4 + (size_t)c];
			for (int c = 0; c < 4; c++)
				dst[((size_t)y * (size_t)dw + (size_t)x) * 4 + (size_t)c] = (uint8_t)(sum[c] / n);
		}
	}
}

bool image_decode_gif(const uint8_t *data, size_t len, int max_side, int max_frames, app_image *out)
{
	memset(out, 0, sizeof *out);
	if (!data || len < 13 || len > 0x7FFFFFFF)
		return false;
	const int total = gif_frame_count(data, len);
	if (total < 1)
		return false;
	/* Com quadros demais, pula alguns e soma o tempo deles no quadro que fica. */
	const int step = (total + max_frames - 1) / max_frames;
	const int kept_max = (total + step - 1) / step;

	stbi__context s;
	stbi__start_mem(&s, data, (int)len);
	stbi__gif *g = (stbi__gif *)calloc(1, sizeof(stbi__gif));
	uint8_t *full[2] = { NULL, NULL }; /* os dois ultimos quadros inteiros, para o descarte "volta ao anterior" */
	uint8_t *frames = NULL;
	uint16_t *delays = (uint16_t *)calloc((size_t)kept_max, sizeof(uint16_t));
	int dw = 0, dh = 0, kept = 0, comp = 0;
	bool ok = g && delays;
	for (int layer = 0; ok; layer++) {
		uint8_t *two_back = layer >= 2 ? full[layer % 2] : NULL;
		uint8_t *u = stbi__gif_load_next(&s, g, &comp, 4, two_back);
		if (!u || u == (uint8_t *)&s)
			break; /* fim (ou quadro quebrado: fica o que ja deu certo) */
		const size_t stride = (size_t)g->w * (size_t)g->h * 4;
		if (!frames) {
			const int big = g->w > g->h ? g->w : g->h;
			dw = big > max_side ? g->w * max_side / big : g->w;
			dh = big > max_side ? g->h * max_side / big : g->h;
			dw = dw < 1 ? 1 : dw;
			dh = dh < 1 ? 1 : dh;
			frames = (uint8_t *)malloc((size_t)dw * (size_t)dh * 4 * (size_t)kept_max);
			full[0] = (uint8_t *)malloc(stride);
			full[1] = (uint8_t *)malloc(stride);
			if (!frames || !full[0] || !full[1]) {
				ok = false;
				break;
			}
		}
		if (layer % step == 0 && kept < kept_max)
			shrink(u, g->w, g->h, frames + (size_t)kept++ * (size_t)dw * (size_t)dh * 4, dw, dh);
		/* GIF sem atraso roda a 10 quadros por segundo nos navegadores; aqui tambem. */
		const int delay = g->delay < 20 ? 100 : g->delay;
		if (kept > 0) {
			const int sum = delays[kept - 1] + delay;
			delays[kept - 1] = (uint16_t)(sum > 60000 ? 60000 : sum);
		}
		memcpy(full[layer % 2], u, stride);
	}
	if (g) {
		STBI_FREE(g->out);
		STBI_FREE(g->history);
		STBI_FREE(g->background);
	}
	free(g);
	free(full[0]);
	free(full[1]);
	if (!ok || kept == 0) {
		free(frames);
		free(delays);
		return false;
	}
	out->rgba = frames;
	out->width = dw;
	out->height = dh;
	out->frames = kept;
	out->delays = delays;
	return true;
}

/* O corpo do avatar que o editor desenha sai daqui para o servidor. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

uint8_t *image_encode_png(const uint8_t *rgba, int width, int height, int *out_len)
{
	*out_len = 0;
	return stbi_write_png_to_mem(rgba, width * 4, width, height, 4, out_len);
}
