// Colisao contra os triangulos do mapa: a grade do pacote diz quais triangulos ficam perto, e o jogador e tres
// esferas empilhadas (pe, cintura, cabeca), como a capsula do fighter.gd.
#ifndef BS_COLLIDE_H
#define BS_COLLIDE_H

#include <stdbool.h>
#include <stdint.h>

#include "mathx.h"
#include "pack.h"

enum {
	BS_COLLIDER_MAX_CANDIDATES = 8192,
};

// Triangulos que mudam durante a partida (as pecas de construcao), em grupos com a caixa de cada um.
typedef struct {
	Vec3 lo, hi;
	uint32_t first, count; // em triangulos
	int owner; // a peca
} BsDynGroup;

typedef struct {
	const BsPack *pack;
	uint32_t *stamp; // um por triangulo, para nao testar o mesmo triangulo duas vezes por consulta
	uint32_t stamp_id;
	uint32_t candidates[BS_COLLIDER_MAX_CANDIDATES];
	uint32_t candidate_count;
	bool overflowed;
	// Os triangulos dinamicos vem depois dos do mapa: o indice pack->triangle_count + k e o k-esimo.
	const float *dyn;
	uint32_t dyn_count;
	const BsDynGroup *groups;
	uint32_t group_count;
} BsCollider;

bool bs_collider_init(BsCollider *c, const BsPack *pack);
void bs_collider_free(BsCollider *c);
void bs_collider_set_dynamic(BsCollider *c, const float *tris, uint32_t count, const BsDynGroup *groups,
	uint32_t group_count);
// Junta os triangulos cuja caixa toca [lo, hi].
void bs_collider_gather(BsCollider *c, Vec3 lo, Vec3 hi);
// O ponto do triangulo i mais perto de p.
Vec3 bs_collider_closest(const BsCollider *c, uint32_t i, Vec3 p);
// A normal do triangulo i.
Vec3 bs_collider_normal(const BsCollider *c, uint32_t i);
// Os tres vertices (9 floats) do triangulo i, do mapa ou dinamico.
const float *bs_collider_tri(const BsCollider *c, uint32_t i);
// O primeiro triangulo que o segmento from + dir*t, t em [0, max_t], cruza; pula os do mapa com algum bit de
// skip_flags (BS_COLLISION_CLIP: as balas passam pelos clips de jogador). Devolve o indice ou UINT32_MAX.
uint32_t bs_collider_ray(BsCollider *c, Vec3 from, Vec3 dir, float max_t, uint8_t skip_flags, bool dynamic, float *t);

#endif
