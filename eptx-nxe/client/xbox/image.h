#ifndef EPTX_IMAGE_H
#define EPTX_IMAGE_H

#include <stddef.h>
#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decodifica um PNG em memoria para RGBA de 8 bits por canal. */
bool image_decode_png(const uint8_t *data, size_t len, app_image *out);
/* O mesmo, com outro teto de lado: a skin do FSD tem fundos de 1280x720, que vem do pacote e nao da rede. */
bool image_decode_png_limit(const uint8_t *data, size_t len, int max_side, app_image *out);

/*
 * Decodifica um GIF animado quadro a quadro, reduzindo cada um para caber em
 * max_side e guardando no maximo max_frames (os outros sao pulados, e o tempo
 * deles vai para o quadro anterior). Os quadros ficam em sequencia em out->rgba.
 */
bool image_decode_gif(const uint8_t *data, size_t len, int max_side, int max_frames, app_image *out);
void image_free(app_image *image);

/* Codifica RGBA em PNG. Devolve um buffer de malloc (free), ou NULL. */
uint8_t *image_encode_png(const uint8_t *rgba, int width, int height, int *out_len);

#ifdef __cplusplus
}
#endif

#endif
