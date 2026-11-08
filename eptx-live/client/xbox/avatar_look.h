/*
 * O avatar de cada conta EPTX, como uma lista de escolhas: um numero por parte
 * (pele, cabelo, roupa...). A dash desenha o boneco a partir dela e o editor so
 * troca os numeros. O servidor guarda a lista como esta, sem saber o que cada
 * numero quer dizer; o padrao dele (src/lib/avatarLook.ts) tem que ser igual a
 * AVATAR_DEFAULT_LOOK.
 *
 * O boneco e um desenho proprio do EPTX, feito com formas: nao usa nada dos
 * avatares da Microsoft.
 */
#ifndef EPTX_AVATAR_LOOK_H
#define EPTX_AVATAR_LOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	LOOK_SKIN,
	LOOK_HAIR,
	LOOK_HAIR_COLOUR,
	LOOK_EYES,
	LOOK_MOUTH,
	LOOK_GLASSES,
	LOOK_BEARD,
	LOOK_TOP,
	LOOK_TOP_COLOUR,
	LOOK_BOTTOM,
	LOOK_BOTTOM_COLOUR,
	LOOK_SHOES,
	LOOK_SHOE_COLOUR,
	LOOK_BUILD,
	LOOK_HEIGHT,
	LOOK_HAT,
	LOOK_FIELDS,
};

/* Quantas opcoes cada parte tem; o editor da a volta nelas. */
static const uint8_t AVATAR_OPTIONS[LOOK_FIELDS] = { 6, 7, 8, 4, 4, 4, 4, 5, 10, 3, 8, 3, 8, 3, 3, 4 };

/*
 * O avatar de toda conta nova: cabelo curto castanho-escuro de lado, oculos
 * redondos, camisa de botao azul-jeans, calca caqui e tenis laranja.
 */
static const uint8_t AVATAR_DEFAULT_LOOK[LOOK_FIELDS] = { 1, 1, 1, 0, 0, 1, 0, 1, 3, 0, 1, 0, 3, 1, 1, 0 };

typedef struct {
	uint8_t v[LOOK_FIELDS];
} avatar_look;

#ifdef __cplusplus
}
#endif

#endif
