// Confere o dust2.pak no PC antes de ele ir para o console, com o mesmo leitor, a mesma colisao e o mesmo
// movimento que o .xex usa. Para cada spawn: cai no chao, fica parado, anda para frente, para os lados e pula, e
// diz se o jogador atravessou o chao, ficou preso ou saiu do mapa.
//
//   zig cc -O2 -std=c17 -I src tools/pc/check.c src/pack.c src/collide.c src/player.c -o out/check.exe
//   out/check.exe out/dust2.pak

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "collide.h"
#include "pack.h"
#include "player.h"

static double now_ms(void)
{
	return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}

// O chao de colisao mais alto embaixo de (x, z), abaixo de y: a altura onde os pes deveriam parar.
static float floor_below(const BsPack *pack, float x, float y, float z)
{
	float best = -1e9f;
	for (uint32_t i = 0; i < pack->triangle_count; i++) {
		const float *v = pack->triangles + i * 9;
		const float d = (v[5] - v[2]) * (v[6] - v[0]) - (v[3] - v[0]) * (v[8] - v[2]);
		if (fabsf(d) < 1e-9f)
			continue;
		// baricentricas no plano XZ
		const float l1 = ((v[5] - v[8]) * (x - v[6]) + (v[6] - v[3]) * (z - v[8])) / d;
		const float l2 = ((v[8] - v[2]) * (x - v[6]) + (v[0] - v[6]) * (z - v[8])) / d;
		const float l3 = 1.0f - l1 - l2;
		if (l1 < 0 || l2 < 0 || l3 < 0)
			continue;
		const float h = l1 * v[1] + l2 * v[4] + l3 * v[7];
		if (h <= y && h > best)
			best = h;
	}
	return best;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "uso: check <dust2.pak>\n");
		return 2;
	}
	static BsPack pack;
	char err[256];
	const double t0 = now_ms();
	if (!bs_pack_load(&pack, argv[1], err, sizeof err)) {
		printf("FALHOU: %s\n", err);
		return 1;
	}
	printf("pacote ok em %.0f ms: %u draws, %u vertices, %u indices, %u texturas, %u materiais, %u spawns\n",
		now_ms() - t0, pack.draw_count, pack.vertex_count, pack.index_count, pack.texture_count, pack.material_count,
		pack.spawn_count);
	printf("colisao: %u triangulos, grade %ux%ux%u, %u referencias\n", pack.triangle_count, pack.grid.nx,
		pack.grid.ny, pack.grid.nz, pack.grid.ref_count);

	// Cada textura le inteira e tem os bytes que os niveis somam.
	uint32_t tex_bytes = 0;
	for (uint32_t i = 0; i < pack.texture_count; i++) {
		void *buf = malloc(pack.textures[i].data_bytes);
		if (!buf || !bs_pack_read_texture(&pack, &pack.textures[i], buf)) {
			printf("FALHOU: textura %u nao le\n", i);
			return 1;
		}
		tex_bytes += pack.textures[i].data_bytes;
		free(buf);
	}
	printf("texturas: %.1f MB lidas\n", tex_bytes / 1e6);

	static BsCollider col;
	if (!bs_collider_init(&col, &pack)) {
		printf("FALHOU: sem memoria para o colisor\n");
		return 1;
	}
	const float dt = 1.0f / 60.0f;
	int problems = 0;
	uint32_t worst_candidates = 0;
	double worst_frame = 0.0, total_ms = 0.0;
	long frames = 0;
	for (uint32_t s = 0; s < pack.spawn_count; s++) {
		BsPlayer p;
		bs_player_spawn(&p, &pack.spawns[s]);
		const Vec3 start = p.position;
		float lowest = p.position.y;
		bool fell = false, overflowed = false;
		Vec3 rest = start;
		// 0-2 s parado, 2-6 s para frente, 6-8 s para a direita, 8-10 s para tras pulando
		for (int f = 0; f < 600; f++) {
			BsInput in = { 0 };
			const float t = f * dt;
			if (t >= 2.0f && t < 6.0f)
				in.move_y = 1.0f;
			else if (t >= 6.0f && t < 8.0f)
				in.move_x = 1.0f;
			else if (t >= 8.0f) {
				in.move_y = -1.0f;
				in.jump = (f % 50) == 0;
			}
			if (t >= 2.0f && t < 6.0f)
				in.look_x = 0.004f; // vai virando de leve, para varrer mais parede
			const double a = now_ms();
			bs_player_update(&p, &col, &in, dt);
			const double ms = now_ms() - a;
			total_ms += ms;
			frames++;
			if (ms > worst_frame)
				worst_frame = ms;
			if (p.candidates > worst_candidates)
				worst_candidates = p.candidates;
			overflowed |= p.overflowed;
			if (p.position.y < lowest)
				lowest = p.position.y;
			if (p.position.y < pack.info.min.y - 2.0f)
				fell = true;
			if (f == 119)
				rest = p.position;
		}
		const float ground = floor_below(&pack, start.x, start.y + 0.3f, start.z);
		const float settle = start.y - rest.y;
		const float off = rest.y - ground; // onde os pes pararam menos o chao embaixo do spawn
		const float dx = p.position.x - start.x, dz = p.position.z - start.z;
		const float moved = sqrtf(dx * dx + dz * dz);
		const bool stuck = moved < 1.0f;
		const bool floating = !p.on_ground && settle < 0.0f;
		bool sunk = fabsf(off) > 0.08f;
		if (sunk) {
			// O que segura o pe: o triangulo mais perto da esfera de baixo, parado depois de 2 s.
			const Vec3 foot = v3(rest.x, rest.y + 0.38f, rest.z);
			bs_collider_gather(&col, v3(foot.x - 0.5f, foot.y - 0.5f, foot.z - 0.5f),
				v3(foot.x + 0.5f, foot.y + 0.5f, foot.z + 0.5f));
			float best = 1e9f;
			uint32_t bt = 0;
			for (uint32_t k = 0; k < col.candidate_count; k++) {
				const Vec3 d = v3_sub(foot, bs_collider_closest(&col, col.candidates[k], foot));
				const float l = v3_length(d);
				if (l < best) {
					best = l;
					bt = col.candidates[k];
				}
			}
			const Vec3 n = bs_collider_normal(&col, bt);
			const Vec3 q = bs_collider_closest(&col, bt, foot);
			printf("    apoio: triangulo %u, normal (%.2f %.2f %.2f), flags %u, toque a %.2f m do centro do pe, "
				   "%.2f m abaixo do centro\n",
				bt, n.x, n.y, n.z, pack.triangle_flags[bt], best, foot.y - q.y);
			// Um apoio de verdade embaixo (pisavel e por baixo) e chao, mesmo que a sonda pontual nao ache.
			sunk = !(n.y > 0.69f && best < 0.42f && foot.y - q.y > 0.2f);
		}
		printf("spawn %2u (%s) spawn %.2f m acima do chao, parou a %+.2f m dele, andou %.1f m, chao=%d%s%s%s%s\n", s,
			pack.spawns[s].team ? "T " : "CT", start.y - ground, off, moved, p.on_ground,
			fell ? "  ATRAVESSOU O CHAO" : "", stuck ? "  PRESO" : "", sunk ? "  FORA DO CHAO" : "",
			overflowed ? "  (candidatos demais)" : "");
		(void)floating;
		(void)settle;
		problems += fell || stuck || sunk;
	}
	printf("movimento: %.3f ms por quadro em media, pior %.2f ms, ate %u triangulos perto\n", total_ms / frames,
		worst_frame, worst_candidates);
	bs_collider_free(&col);
	bs_pack_free(&pack);
	if (problems) {
		printf("%d spawn(s) com problema\n", problems);
		return 1;
	}
	printf("TUDO OK\n");
	return 0;
}
