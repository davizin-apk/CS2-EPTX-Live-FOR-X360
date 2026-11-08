// O jogador em primeira pessoa: o movimento estilo Source do player.gd (aceleracao, atrito, air strafe), com a
// capsula do fighter.gd, degraus de ate 18 unidades do Source e o "grude" no chao ao descer rampas.
#ifndef BS_PLAYER_H
#define BS_PLAYER_H

#include <stdbool.h>

#include "collide.h"
#include "mathx.h"
#include "pack.h"

typedef struct {
	float move_x, move_y; // -1 a 1; move_y positivo e para frente
	float look_x, look_y; // radianos girados neste quadro; look_y positivo olha para cima
	bool jump, crouch, walk;
	float max_speed; // m/s com a arma na mao; 0 usa o da faca
} BsInput;

typedef struct {
	Vec3 position; // os pes
	Vec3 velocity;
	float yaw, pitch;
	float height, eye;
	bool on_ground;
	bool crouching;
	bool fly; // voo livre, sem colisao: para olhar o mapa se a colisao der problema
	// Diagnostico, para o HUD e para o teste no PC.
	uint32_t candidates;
	bool overflowed;
} BsPlayer;

void bs_player_spawn(BsPlayer *p, const BsSpawn *spawn);
void bs_player_update(BsPlayer *p, BsCollider *c, const BsInput *in, float dt);
// So o deslocamento (colisao, degrau e grude no chao) com a velocidade que ja esta em p: os bots montam a propria.
void bs_player_move(BsPlayer *p, BsCollider *c, float dt, bool was_ground, bool jumped);
// Agacha ou levanta (so levanta se cabe), como o player.gd.
void bs_player_crouch(BsPlayer *p, BsCollider *c, bool want);
Vec3 bs_player_eye(const BsPlayer *p);

#endif
