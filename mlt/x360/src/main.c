// O laco do jogo: carga, menu, partida, cameras. O mesmo no 360 e no PC; a plataforma e o desenho ficam atras de
// plat.h e gfx.h.
//
// Teste automatico no PC (variaveis de ambiente):
//   BS_AUTOPLAY=classic|build|portal  os 10 lutadores sao bots; a camera segue um deles
//   BS_QUIT=segundos                  fecha sozinho
//   BS_SHOTS=pasta                    salva um quadro a cada 5 s
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "net.h"

Game G;

float frand(void)
{
	G.rng ^= G.rng << 13;
	G.rng ^= G.rng >> 17;
	G.rng ^= G.rng << 5;
	return (G.rng & 0xFFFFFF) / 16777216.0f;
}

float frand_range(float lo, float hi)
{
	return lo + (hi - lo) * frand();
}

float frandn(float sigma)
{
	const float u = fmaxf(frand(), 1e-6f), v = frand();
	return sigma * sqrtf(-2.0f * logf(u)) * cosf(2 * PI_F * v);
}

static void mix(float *out, uint32_t frames)
{
	audio_mix(out, frames);
}

static bool load(char *err, size_t eb)
{
	uint32_t w, h;
	if (!gfx_open(&w, &h, err, eb))
		return false;
	const double t0 = plat_seconds();
	if (!bs_pack_load(&G.map, plat_find_file("dust2.pak"), err, eb))
		return false;
	if (!assets_load(&G.as, plat_find_file("assets.pak"), err, eb))
		return false;
	if (!bs_collider_init(&G.col, &G.map)) {
		snprintf(err, eb, "sem memoria para a colisao");
		return false;
	}
	if (!gfx_map_upload(&G.map, err, eb))
		return false;
	G.model_tex = calloc(G.as.texture_count + 1, sizeof(int));
	if (!G.model_tex) {
		snprintf(err, eb, "sem memoria");
		return false;
	}
	for (uint32_t i = 0; i < G.as.texture_count; i++)
		G.model_tex[i] = G.as.textures[i].data ? gfx_texture(&G.as.textures[i]) : GFX_WHITE;
	G.atlas_tex = gfx_texture_rgba(G.as.atlas_w, G.as.atlas_h, G.as.atlas_rgba);
	for (int i = 0; i < 3; i++)
		G.build_tex[i] = G.as.build_textures[i] >= 0 ? G.model_tex[G.as.build_textures[i]] : GFX_WHITE;
	if (!world_init(err, eb))
		return false;
	audio_init();
	audio_volumes(G.settings.volume, G.settings.music);
	// o mesmo pacote dos dois lados: tamanho e o comeco de cada arquivo
	uint32_t hash = 2166136261u;
	const char *names[2] = { "dust2.pak", "assets.pak" };
	for (int k = 0; k < 2; k++) {
		FILE *f = fopen(plat_find_file(names[k]), "rb");
		if (!f)
			continue;
		static uint8_t buf[65536];
		const size_t n = fread(buf, 1, sizeof buf, f);
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fclose(f);
		for (size_t i = 0; i < n; i++)
			hash = (hash ^ buf[i]) * 16777619u;
		hash = (hash ^ (uint32_t)size) * 16777619u;
	}
	snprintf(G.pack_hash, sizeof G.pack_hash, "%08x", (unsigned)hash);
	if (!plat_audio_start(mix))
		plat_log("sem saida de audio; o jogo segue mudo");
	plat_log("carregado em %.1f s: %u modelos, %u animacoes, %u texturas, %u sons", plat_seconds() - t0,
		(unsigned)G.as.model_count, (unsigned)G.as.anim_count, (unsigned)G.as.texture_count,
		(unsigned)G.as.sound_count);
	return true;
}

static void base_camera(GfxCamera *c)
{
	const BsInfo *in = &G.map.info;
	c->fog_start = in->fog_start;
	c->fog_end = in->fog_end;
	c->sun_dir = in->sun_dir;
	c->ambient = 0.55f;
	c->sun = 0.65f;
	memcpy(c->sky_rgb, in->sky_rgb, sizeof c->sky_rgb);
	memcpy(c->fog_rgb, in->fog_rgb, sizeof c->fog_rgb);
}

// O fov da configuracao e o horizontal do CS2 em 4:3; a camera usa o vertical.
static float vertical_fov(float deg)
{
	return 2.0f * atanf(tanf(deg * PI_F / 360.0f) * 0.75f);
}

static void set_camera(Vec3 eye, float yaw, float pitch, float fov_y, float near_z)
{
	GfxCamera *c = &G.cam;
	base_camera(c);
	c->eye = eye;
	c->view = mat4_view(eye, yaw, pitch);
	c->proj = mat4_perspective(fov_y, (float)GFX_WIDTH / GFX_HEIGHT, near_z, G.map.info.fog_end + 30.0f);
	audio_listener(eye, yaw);
}

// Atras de quem esta sendo assistido, sem entrar na parede.
static void chase_camera(const Fighter *f)
{
	const Vec3 head = v3_add(f->body.position, v3(0, 1.7f, 0));
	const Vec3 back = v3(sinf(f->body.yaw) * cosf(-0.25f), 0.45f, cosf(f->body.yaw));
	Vec3 eye = v3_add(head, v3_scale(v3_normalize(back), 2.8f));
	const RayHit h = world_ray(head, eye, RAY_WORLD | RAY_PIECES, -1);
	if (h.hit)
		eye = v3_add(head, v3_scale(v3_sub(h.point, head), 0.85f));
	const Vec3 d = v3_sub(head, eye);
	set_camera(eye, atan2f(-d.x, -d.z), atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z)), vertical_fov(G.settings.fov), 0.05f);
}

static void match_frame(void)
{
	Match *m = &G.m;
	const Fighter *p = m->player >= 0 ? &m->f[m->player] : NULL;
	// quem esta sendo assistido (espectador e modo automatico)
	if (!p || !p->alive) {
		if (!m->f[m->spectate].alive || G.act.left || G.act.right || G.act.click) {
			const int dir = G.act.left ? -1 : 1;
			for (int i = 1; i <= m->count; i++) {
				const int j = (m->spectate + dir * i + m->count * 2) % m->count;
				if (m->f[j].alive && (!p || m->f[j].team == p->team || !m->f[m->spectate].alive)) {
					m->spectate = j;
					break;
				}
			}
		}
	}
	if (p && p->alive) {
		const float fov = p->scoped ? 25.0f * PI_F / 180.0f : vertical_fov(G.settings.fov);
		set_camera(bs_player_eye(&p->body), p->body.yaw, p->body.pitch, fov, 0.05f);
	} else {
		chase_camera(&m->f[m->spectate]);
	}
	gfx_camera(&G.cam, 0);
	G.stats = gfx_draw_map();
	match_draw();
	if (p && p->alive) {
		GfxCamera vm = G.cam;
		vm.proj = mat4_perspective(vertical_fov(68.0f), (float)GFX_WIDTH / GFX_HEIGHT, 0.01f, 20.0f);
		gfx_camera(&vm, 1);
		match_draw_viewmodel();
	}
	ui_begin();
	hud_draw();
	buy_update();
	tacmap_update();
	tacmap_draw();
	pause_update();
	ui_flush();
}

static void menu_frame(void)
{
	// o Dust II atras do menu, olhando o agente no spawn, balancando devagar
	const BsSpawn *sp = &G.map.spawns[G.settings.side == 1 ? G.map.spawn_count - 1 : 0];
	const Vec3 fwd = v3(-sinf(sp->yaw), 0, -cosf(sp->yaw));
	const float sway = sinf((float)G.time * 0.15f) * 0.12f;
	const Vec3 eye = v3_add(v3_add(sp->position, v3(0, 1.55f, 0)), v3_scale(fwd, -0.4f));
	set_camera(eye, sp->yaw + 0.4f + sway, -0.08f, vertical_fov(80.0f), 0.05f);
	gfx_camera(&G.cam, 0);
	G.stats = gfx_draw_map();
	menu_draw();
	ui_begin();
	menu_draw_ui();
	ui_flush();
}

int main(void)
{
	G.rng = 0x9E3779B9u;
	G.settings = (Settings){ 1.6f, 90.0f, 0.5f, 0.35f, BOT_SKILLS_NORMAL, 0, MODES_CLASSIC, 0.05f, -0.03f, 0.08f, false, 0.35f, 8.0f };
	char err[320];
	if (!plat_open(err, sizeof err)) {
		plat_fatal(err);
		return 1;
	}
	if (!load(err, sizeof err)) {
		plat_fatal(err);
		plat_close();
		return 1;
	}
	net_start(G.pack_hash);
	const char *autoplay = getenv("BS_AUTOPLAY");
	const char *autohost = getenv("BS_NET_AUTOHOST");
	const char *autojoin = getenv("BS_NET_AUTOJOIN");
	double next_auto = 0;
	const char *quit = getenv("BS_QUIT");
	const char *shots = getenv("BS_SHOTS");
	const double quit_at = quit ? atof(quit) : 0;
	double next_shot = 5.0;
	int shot_n = 0;
	if (autoplay && *autoplay) {
		int mode = MODES_CLASSIC;
		for (int i = 0; i < MODES_COUNT; i++)
			if (!strcmp(MODES[i].id, autoplay))
				mode = i;
		G.autoplay = true;
		G.state = STATE_MATCH;
		match_start(mode, 0, BOT_SKILLS_NORMAL, true);
	} else {
		menu_enter();
	}
	double last = plat_seconds(), fps_t = last;
	int frames = 0;
	while (!plat_quit_requested() && !G.want_quit) {
		const double now = plat_seconds();
		G.dt = (float)fmin(now - last, 0.05);
		last = now;
		G.prev_pad = G.pad;
		plat_poll(&G.pad);
		const bool menus = G.state == STATE_MENU || G.paused || G.m.buy_open || G.m.tacmap_open;
		input_build(&G.pad, &G.prev_pad, &G.act, menus);
		// em rede a partida nao para na pausa: os outros continuam jogando
		const bool sim = !G.paused || G.net_role != NET_ROLE_NONE;
		if (sim)
			G.time += G.dt;
		if (G.state == STATE_MENU) {
			netgame_menu_poll();
			// testes automaticos da rede no PC: um cria a sala e convida, o outro aceita
			if ((autohost || autojoin) && G.time >= next_auto && net_online()) {
				next_auto = G.time + 1.0;
				NetView v;
				net_view(&v);
				if (autohost && !v.in_room) {
					for (int i = 0; i < MODES_COUNT; i++)
						if (!strcmp(MODES[i].id, autohost))
							G.settings.mode = i;
					net_room_create(MODES[G.settings.mode].id);
				} else if (autohost && v.room.host_id == v.user_id) {
					for (int i = 0; i < v.friend_count; i++)
						if (!strcmp(v.friends[i].state, "online"))
							net_room_invite(v.friends[i].user_id);
					bool ready = v.room.member_count >= 2;
					for (int i = 0; i < v.room.member_count; i++)
						ready &= v.room.members[i].udp;
					if (ready) {
						net_room_state(MODES[G.settings.mode].id, true);
						audio_stop_kind(AUDIO_MUSIC);
						G.state = STATE_MATCH;
						match_start(G.settings.mode, 0, G.settings.difficulty, false);
						netgame_host_begin();
					}
				} else if (autojoin && !v.in_room && v.invite_count) {
					net_room_join(v.invites[0].room_id);
				}
			}
			if (G.state == STATE_MENU)
				menu_update(G.dt);
		} else if (G.net_role == NET_ROLE_CLIENT) {
			// o host parou ou sumiu: volta ao menu
			NetView v;
			net_view(&v);
			if (!v.in_room || !v.room.playing || G.time - G.m.net_last_state > 8.0) {
				plat_log("rede: a partida do host acabou (%s)", !v.in_room ? "sem sala" : (!v.room.playing ? "host parou" : "sem estado"));
				match_end();
				menu_enter();
			} else {
				match_update(G.dt);
			}
		} else if (sim) {
			match_update(G.dt);
		}
		// presenca: o que aparece para os amigos (na dash do EPTX e no jogo deles)
		{
			char detail[64];
			if (G.state == STATE_MENU)
				snprintf(detail, sizeof detail, "Menu");
			else
				snprintf(detail, sizeof detail, "%s - CT %d x %d T%s", MODES[G.m.mode].name, G.m.score[0], G.m.score[1],
					G.net_role != NET_ROLE_NONE ? " (em sala)" : "");
			net_presence(detail);
		}
		if (!gfx_begin())
			break;
		if (G.state == STATE_MATCH && G.m.active)
			match_frame();
		else
			menu_frame();
		if (shots && G.time >= next_shot) {
			char path[512];
			snprintf(path, sizeof path, "%s/shot_%02d.bmp", shots, shot_n++);
			gfx_screenshot(path);
			plat_log("captura %s (%.0f fps, %u draws, %u triangulos do mapa)", path, G.fps, (unsigned)G.stats.draws,
				(unsigned)G.stats.triangles);
			next_shot += 5.0;
		}
		gfx_end();
		frames++;
		if (now - fps_t >= 1.0) {
			G.fps = (float)(frames / (now - fps_t));
			frames = 0;
			fps_t = now;
		}
		if (quit_at > 0 && G.time >= quit_at)
			break;
	}
	plat_log("fim: %.0f s de jogo, rodada %d, CT %d x T %d", G.time, G.m.round, G.m.score[0], G.m.score[1]);
	audio_shutdown();
	plat_audio_stop();
	gfx_close();
	plat_close();
	return 0;
}
