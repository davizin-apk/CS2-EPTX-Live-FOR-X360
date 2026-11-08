// Vetores e matrizes do jogo. Mesmas convencoes do Godot: Y para cima, a camera olha para -Z, metros.
// Matrizes coluna a coluna, como o GLSL as le das constantes (xsdk:samples/cube).
#ifndef BS_MATHX_H
#define BS_MATHX_H

#include <math.h>

typedef struct {
	float x, y, z;
} Vec3;

typedef struct {
	float m[16];
} Mat4;

static inline Vec3 v3(float x, float y, float z)
{
	return (Vec3){ x, y, z };
}
static inline Vec3 v3_add(Vec3 a, Vec3 b)
{
	return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline Vec3 v3_sub(Vec3 a, Vec3 b)
{
	return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline Vec3 v3_scale(Vec3 a, float s)
{
	return v3(a.x * s, a.y * s, a.z * s);
}
static inline float v3_dot(Vec3 a, Vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline Vec3 v3_cross(Vec3 a, Vec3 b)
{
	return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_length(Vec3 a)
{
	return sqrtf(v3_dot(a, a));
}
static inline Vec3 v3_normalize(Vec3 a)
{
	const float l = v3_length(a);
	return l > 1e-8f ? v3_scale(a, 1.0f / l) : v3(0, 1, 0);
}
static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline Mat4 mat4_multiply(const Mat4 *a, const Mat4 *b)
{
	Mat4 p;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += a->m[k * 4 + r] * b->m[c * 4 + k];
			p.m[c * 4 + r] = s;
		}
	return p;
}

// Perspectiva com profundidade de 0 em near a 1 em far e y para cima, como o Xenos recebe (xsdk:samples/cube).
static inline Mat4 mat4_perspective(float fov_y, float aspect, float near_z, float far_z)
{
	const float f = 1.0f / tanf(fov_y * 0.5f);
	const float fz = far_z / (near_z - far_z);
	return (Mat4){ { f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, fz, -1, 0, 0, near_z * fz, 0 } };
}

// O inverso da camera do Godot: corpo girado por yaw em Y, cabeca por pitch em X, olho em eye.
static inline Mat4 mat4_view(Vec3 eye, float yaw, float pitch)
{
	const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
	// Colunas da camera no mundo: direita, cima, tras.
	const Vec3 right = v3(cy, 0, -sy);
	const Vec3 up = v3(sy * sp, cp, cy * sp);
	const Vec3 back = v3(sy * cp, -sp, cy * cp);
	return (Mat4){ { right.x, up.x, back.x, 0, right.y, up.y, back.y, 0, right.z, up.z, back.z, 0,
		-v3_dot(right, eye), -v3_dot(up, eye), -v3_dot(back, eye), 1 } };
}

// Para onde a camera olha.
static inline Vec3 look_dir(float yaw, float pitch)
{
	return v3(-sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
}

#endif
