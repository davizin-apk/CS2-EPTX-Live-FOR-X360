#include "player.h"

#include <string.h>

// player.gd e fighter.gd, em metros.
#define U2M 0.0254f
static const float ACCEL = 5.5f;
static const float AIR_ACCEL = 12.0f;
static const float FRICTION = 5.2f;
static const float STOP_SPEED = 100.0f * U2M;
static const float AIR_CAP = 30.0f * U2M;
static const float GRAVITY = 800.0f * U2M;
static const float JUMP_SPEED = 301.993f * U2M;
static const float RUN_SPEED = 250.0f * U2M; // faca na mao
static const float WALK_SCALE = 0.52f;
static const float CROUCH_SCALE = 0.34f;
static const float RADIUS = 0.38f;
static const float STAND_HEIGHT = 1.83f;
static const float CROUCH_HEIGHT = 1.3f;
static const float EYE_STAND = 1.64f;
static const float EYE_CROUCH = 1.18f;
static const float FLOOR_COS = 0.6947f; // 46 graus
static const float STEP = 18.0f * U2M;
static const float SNAP = 0.35f;
static const float GROUND_EPS = 0.03f;
// Nenhum passo de movimento anda mais que isto, para o centro de uma esfera nunca atravessar um chao fino.
static const float SUBSTEP = 0.12f;
static const float FLY_SPEED = 9.0f;

void bs_player_spawn(BsPlayer *p, const BsSpawn *spawn)
{
	memset(p, 0, sizeof *p);
	p->position = spawn->position;
	p->yaw = spawn->yaw;
	p->height = STAND_HEIGHT;
	p->eye = EYE_STAND;
}

Vec3 bs_player_eye(const BsPlayer *p)
{
	return v3(p->position.x, p->position.y + p->eye, p->position.z);
}

typedef struct {
	bool ground;
	Vec3 ground_normal;
} Contact;

// Empurra as tres esferas para fora dos triangulos e tira da velocidade o que entra na parede. O pe conta como no
// chao quando encosta num triangulo pisavel, mesmo sem entrar nele.
static void resolve(BsPlayer *p, BsCollider *c, Contact *out)
{
	out->ground = false;
	out->ground_normal = v3(0, 1, 0);
	const float m = RADIUS + 0.25f;
	bs_collider_gather(c, v3(p->position.x - m, p->position.y - 0.25f, p->position.z - m),
		v3(p->position.x + m, p->position.y + p->height + 0.25f, p->position.z + m));
	p->candidates = c->candidate_count;
	p->overflowed = c->overflowed;
	for (int iter = 0; iter < 4; iter++) {
		bool pushed = false;
		const float heights[3] = { RADIUS, p->height * 0.5f, p->height - RADIUS };
		for (int s = 0; s < 3; s++) {
			for (uint32_t k = 0; k < c->candidate_count; k++) {
				const uint32_t t = c->candidates[k];
				const Vec3 center = v3(p->position.x, p->position.y + heights[s], p->position.z);
				const Vec3 q = bs_collider_closest(c, t, center);
				const Vec3 d = v3_sub(center, q);
				const float dist2 = v3_dot(d, d);
				const float reach = s == 0 ? RADIUS + GROUND_EPS : RADIUS;
				if (dist2 >= reach * reach)
					continue;
				const float dist = sqrtf(dist2);
				Vec3 n;
				if (dist > 1e-5f) {
					n = v3_scale(d, 1.0f / dist);
				} else {
					n = bs_collider_normal(c, t);
					if (n.y < 0.0f)
						n = v3_scale(n, -1.0f);
				}
				const float depth = RADIUS - dist;
				if (s == 0 && n.y > FLOOR_COS) {
					out->ground = true;
					out->ground_normal = n;
					if (depth > 0.0f) {
						// chao pisavel: sobe reto, para nao escorregar parado numa rampa
						p->position.y += fminf(depth / n.y, RADIUS);
						pushed = true;
					}
					if (p->velocity.y < 0.0f)
						p->velocity.y = 0.0f;
					continue;
				}
				if (depth <= 0.0f)
					continue;
				p->position = v3_add(p->position, v3_scale(n, depth));
				const float into = v3_dot(p->velocity, n);
				if (into < 0.0f)
					p->velocity = v3_sub(p->velocity, v3_scale(n, into));
				pushed = true;
			}
		}
		if (!pushed)
			break;
	}
}

// Anda delta em passos curtos, resolvendo a colisao a cada passo; a velocidade ja cortada vale para o resto.
static void slide(BsPlayer *p, BsCollider *c, float dt, Contact *contact)
{
	const Vec3 delta = v3_scale(p->velocity, dt);
	int steps = (int)ceilf(v3_length(delta) / SUBSTEP);
	if (steps < 1)
		steps = 1;
	if (steps > 32)
		steps = 32;
	const float h = dt / (float)steps;
	for (int i = 0; i < steps; i++) {
		p->position = v3_add(p->position, v3_scale(p->velocity, h));
		resolve(p, c, contact);
	}
}

// Desce ate max_drop em passos curtos, parando no primeiro chao pisavel.
static bool drop_to_ground(BsPlayer *p, BsCollider *c, float max_drop, Contact *contact)
{
	const Vec3 keep_velocity = p->velocity;
	for (float dropped = 0.0f; dropped < max_drop; dropped += 0.08f) {
		p->position.y -= 0.08f;
		resolve(p, c, contact);
		if (contact->ground) {
			p->velocity = keep_velocity;
			p->velocity.y = 0.0f;
			return true;
		}
	}
	return false;
}

static float horizontal_distance(Vec3 a, Vec3 b)
{
	const float dx = a.x - b.x, dz = a.z - b.z;
	return sqrtf(dx * dx + dz * dz);
}

static void accelerate(BsPlayer *p, Vec3 wish, float wish_speed, float accel, float accel_speed, float dt)
{
	const float len = v3_length(wish);
	if (len < 0.01f)
		return;
	const Vec3 dir = v3_scale(wish, 1.0f / len);
	const float add = wish_speed - v3_dot(p->velocity, dir);
	if (add <= 0.0f)
		return;
	const float a = fminf(accel * accel_speed * dt * len, add);
	p->velocity = v3_add(p->velocity, v3_scale(dir, a));
}

static void friction(BsPlayer *p, float dt)
{
	const float sp = sqrtf(p->velocity.x * p->velocity.x + p->velocity.z * p->velocity.z);
	if (sp < 0.01f) {
		p->velocity.x = p->velocity.z = 0.0f;
		return;
	}
	const float drop = fmaxf(sp, STOP_SPEED) * FRICTION * dt;
	const float ns = fmaxf(sp - drop, 0.0f) / sp;
	p->velocity.x *= ns;
	p->velocity.z *= ns;
}

// Cabe de pe aqui? (para levantar depois de agachar)
static bool head_room(BsPlayer *p, BsCollider *c)
{
	const Vec3 top = v3(p->position.x, p->position.y + STAND_HEIGHT - RADIUS, p->position.z);
	bs_collider_gather(c, v3(top.x - RADIUS, top.y - RADIUS, top.z - RADIUS),
		v3(top.x + RADIUS, top.y + RADIUS, top.z + RADIUS));
	for (uint32_t k = 0; k < c->candidate_count; k++) {
		const Vec3 d = v3_sub(top, bs_collider_closest(c, c->candidates[k], top));
		if (v3_dot(d, d) < RADIUS * RADIUS * 0.9f)
			return false;
	}
	return true;
}

void bs_player_crouch(BsPlayer *p, BsCollider *c, bool want)
{
	if (!want && p->crouching && !head_room(p, c))
		want = true;
	p->crouching = want;
	const float target_h = want ? CROUCH_HEIGHT : STAND_HEIGHT;
	p->height += (target_h - p->height) * 0.4f;
	p->eye += ((want ? EYE_CROUCH : EYE_STAND) - p->eye) * 0.25f;
}

void bs_player_update(BsPlayer *p, BsCollider *c, const BsInput *in, float dt)
{
	if (dt > 0.05f)
		dt = 0.05f; // um quadro travado nao vira um salto atraves da parede
	p->yaw -= in->look_x;
	p->pitch = clampf(p->pitch + in->look_y, -1.55f, 1.55f);
	const float cy = cosf(p->yaw), sy = sinf(p->yaw);
	// Basis(UP, yaw) * (x, 0, -y), como o player.gd monta o wish com WASD
	Vec3 wish = v3(in->move_x * cy - in->move_y * sy, 0.0f, -in->move_x * sy - in->move_y * cy);
	if (v3_length(wish) > 1.0f)
		wish = v3_normalize(wish);

	if (p->fly) {
		const Vec3 f = look_dir(p->yaw, p->pitch);
		const Vec3 r = v3(cy, 0, -sy);
		Vec3 move = v3_add(v3_scale(f, in->move_y), v3_scale(r, in->move_x));
		if (in->jump)
			move.y += 1.0f;
		if (in->crouch)
			move.y -= 1.0f;
		p->position = v3_add(p->position, v3_scale(move, FLY_SPEED * (in->walk ? 0.3f : 1.0f) * dt));
		p->velocity = v3(0, 0, 0);
		p->on_ground = false;
		return;
	}

	bs_player_crouch(p, c, in->crouch);
	const bool want_crouch = p->crouching;

	const float run = in->max_speed > 0.0f ? in->max_speed : RUN_SPEED;
	const float speed = run * (want_crouch ? CROUCH_SCALE : (in->walk ? WALK_SCALE : 1.0f));
	const bool was_ground = p->on_ground;
	bool jumped = false;
	if (was_ground) {
		friction(p, dt);
		accelerate(p, wish, speed, ACCEL, speed, dt);
		if (in->jump) {
			p->velocity.y = JUMP_SPEED;
			jumped = true;
		}
	} else {
		accelerate(p, wish, fminf(speed, AIR_CAP), AIR_ACCEL, speed, dt);
		p->velocity.y -= GRAVITY * dt;
	}

	bs_player_move(p, c, dt, was_ground, jumped);
}

void bs_player_move(BsPlayer *p, BsCollider *c, float dt, bool was_ground, bool jumped)
{
	if (dt > 0.05f)
		dt = 0.05f;
	const Vec3 start = p->position;
	const Vec3 start_velocity = p->velocity;
	Contact contact;
	slide(p, c, dt, &contact);

	// Degrau: se andando no chao a parede segurou, tenta subir STEP, andar e descer de novo; fica com o que andou mais.
	const float wanted = horizontal_distance(v3_add(start, v3_scale(start_velocity, dt)), start);
	if (was_ground && !jumped && wanted > 0.001f && horizontal_distance(p->position, start) < wanted * 0.9f) {
		const Vec3 a_position = p->position, a_velocity = p->velocity;
		const Contact a_contact = contact;
		p->position = start;
		p->velocity = v3(0, STEP / dt, 0);
		slide(p, c, dt, &contact);
		p->velocity = v3(start_velocity.x, 0.0f, start_velocity.z);
		slide(p, c, dt, &contact);
		const bool landed = drop_to_ground(p, c, STEP + 0.1f, &contact);
		if (!landed || horizontal_distance(p->position, start) <= horizontal_distance(a_position, start)) {
			p->position = a_position;
			p->velocity = a_velocity;
			contact = a_contact;
		}
	}

	// Grude no chao ao descer rampa ou degrau, como o floor_snap_length do fighter.gd.
	if (was_ground && !jumped && !contact.ground && p->velocity.y <= 0.0f) {
		const Vec3 keep = p->position, keep_velocity = p->velocity;
		if (!drop_to_ground(p, c, SNAP, &contact)) {
			p->position = keep;
			p->velocity = keep_velocity;
			contact.ground = false;
		}
	}
	p->on_ground = contact.ground && !jumped;
}
