// O jogo: o Bot Strike original (match.gd, fighter.gd, player.gd, bot.gd, build.gd, portal.gd, tacmap.gd, hud.gd,
// menu.gd) em C. As regras e os numeros vem das mesmas planilhas (defs.h, gerado delas).
#ifndef BS_GAME_H
#define BS_GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "assets.h"
#include "audio.h"
#include "collide.h"
#include "defs.h"
#include "gfx.h"
#include "mathx.h"
#include "pack.h"
#include "plat.h"
#include "player.h"

#define U2M 0.0254f
#define PI_F 3.14159265f

enum {
	TEAM_CT = 0,
	TEAM_T = 1,
	MAX_FIGHTERS = 10,
	MAX_PATH = 160,
	MAX_PIECES = 192,
	MAX_PORTALS = 2 * MAX_FIGHTERS,
	MAX_FX = 768,
	MAX_SHELLS = 24,
	MAX_FEED = 5,
	SLOT_COUNT = 6, // 1 primaria, 2 pistola, 3 faca, 4 portal gun, 5 bomba
};

// ---------------------------------------------------------------- animacao e modelos

typedef struct {
	const Anim *cur, *prev;
	float t, prev_t;
	float blend, blend_len;
	const Anim *queued; // toca quando cur acabar (o idle depois do tiro)
	bool paused;
} AnimPlayer;

// Um modelo com pele calculada na CPU, com os seus vertices na GPU.
typedef struct {
	const Model *model;
	int mesh;
	MeshVertex *out;
	Xf *pose; // um por slot
	Xf *palette; // um por bind
} Skin;

// ---------------------------------------------------------------- mundo

typedef struct {
	bool hit;
	float t;
	Vec3 point, normal;
	int kind; // HIT_WORLD, HIT_PIECE, HIT_FIGHTER
	int index;
	uint8_t flags;
} RayHit;

enum {
	HIT_NONE,
	HIT_WORLD,
	HIT_PIECE,
	HIT_FIGHTER,
};

enum {
	RAY_WORLD = 1, // o mapa (sem os clips de jogador)
	RAY_CLIP = 2, // os clips tambem
	RAY_PIECES = 4,
	RAY_FIGHTERS = 8,
};

typedef struct {
	const NavData *nav;
	float *box; // min xyz, max xyz por poligono
	Vec3 *center;
	uint32_t *adj_start, *adj; // vizinhos por aresta compartilhada
	uint32_t *adj_edge; // os dois vertices da aresta, por vizinho
	bool *reachable; // ligado aos spawns (o radar desenha so esses)
	// A*
	float *g;
	uint32_t *came, *open, *stamp;
	uint32_t search;
} Nav;

// ---------------------------------------------------------------- construcao

typedef struct {
	bool used;
	int piece; // BUILD_PIECES_*
	int mat; // BUILD_MATERIALS_*
	int team;
	int64_t key;
	int cell[3];
	int card, ramp_card;
	float oy;
	Xf xf;
	bool grounded;
	float max_hp, hp, build_time, built;
	bool edits[9];
	Vec3 lo, hi; // caixa no mundo
	// geometria em triangulos (para colisao, tiro e desenho)
	float *tris;
	uint32_t tri_count;
	int mesh; // na GPU
	bool dying;
	float die_at;
} Piece;

typedef struct {
	int piece;
	Xf xf;
	int key_hint;
	int64_t key;
	int cell[3];
	int card;
	float oy;
	bool ok;
} Placement;

// ---------------------------------------------------------------- portais

typedef struct {
	bool used;
	int owner;
	int color; // 0 azul, 1 laranja
	Xf xf; // colunas: x, y, normal; origem no centro do oval
	bool standing;
	int surface_piece; // -1, ou a peca em que esta (se quebrar, o portal some)
	int twin;
} Portal;

// ---------------------------------------------------------------- lutadores

enum {
	GOAL_SITE,
	GOAL_BOMB,
	GOAL_PLANT,
	GOAL_GUARD,
	GOAL_DEFUSE,
	GOAL_RETAKE,
};

typedef struct {
	int skill;
	Vec3 goal;
	int goal_kind;
	int site; // 0 A, 1 B
	Vec3 path[MAX_PATH];
	int path_count, path_i;
	int target;
	double seen_since;
	Vec3 last_seen;
	double next_think, next_repath, hold_until, unstick_until;
	float stuck_t;
	Vec3 last_pos;
	float build_cd;
	float aim_err_x, aim_err_y;
	float aim_height;
	int stuck_count;
	Vec3 unstick;
	bool portal_done;
	float portal_delay;
	int portal_goal; // portal em que ele entra como atalho, ou -1
	int target_portal; // o alvo e visto por este portal, ou -1
	bool autopilot;
} Brain;

typedef struct {
	int id; // WEAPONS_*
	int mag, reserve;
} Gun;

typedef struct {
	bool used;
	int index;
	bool is_player, is_bot;
	int team;
	char name[24];
	BsPlayer body;
	int hp, armor;
	bool helmet, kit, bomb;
	int money;
	int mats[3];
	int build_mat;
	bool alive, frozen;
	int kills, deaths;
	bool has[SLOT_COUNT];
	Gun guns[SLOT_COUNT];
	int slot, last_slot;
	double next_fire, reload_until;
	Gun *reload_gun;
	float recoil;
	double portal_cd;
	// modelo em terceira pessoa
	Skin skin;
	AnimPlayer anim;
	int anim_kind;
	float topple; // 0 em pe, 1 tombado
	float walk_speed;
	Brain bot;
	// jogador
	float bob, kick;
	bool scoped;
	int build_piece; // -1 sem construcao
	int editing; // peca sendo editada, ou -1
	int edit_sel[9];
	int edit_sel_count;
	bool edit_paint;
	double next_build;
	bool held_fire;
	int remote; // no host: o slot da sala do jogador remoto neste lugar, ou -1
	uint8_t net_epoch; // muda quando o host move o corpo (nascer, portal): o cliente pula para la
	Vec3 net_target; // no cliente: onde o host diz que ele esta
} Fighter;

// ---------------------------------------------------------------- efeitos

enum {
	FX_TRACER,
	FX_FLASH,
	FX_DECAL,
	FX_PARTICLE,
	FX_EXPLOSION,
};

typedef struct {
	bool used;
	int kind;
	Vec3 a, b, vel;
	float color[4];
	float born, life, size, spin;
	bool viewmodel;
} Fx;

typedef struct {
	bool used;
	int model; // 0 9mm, 1 7.62, 2 .50
	Xf xf;
	Vec3 vel, spin;
	float born;
} Shell;

// ---------------------------------------------------------------- partida

enum {
	PHASE_FREEZE,
	PHASE_LIVE,
	PHASE_PLANTED,
	PHASE_OVER,
	PHASE_MATCH_OVER,
};

typedef struct {
	char killer[24], victim[24];
	int killer_team, victim_team;
	int weapon;
	bool headshot;
	float born;
} FeedLine;

typedef struct {
	bool active;
	int mode; // MODES_*
	int player_team;
	int difficulty;
	Fighter f[MAX_FIGHTERS];
	int count;
	int player; // indice, ou -1 (autoplay)
	int phase;
	double phase_end, round_start;
	int round;
	int score[2];
	int loss_streak[2];
	int plan_site;
	int bomb_carrier;
	bool bomb_dropped;
	Vec3 bomb_drop_pos;
	bool bomb_planted;
	Vec3 bomb_pos;
	int bomb_site;
	double bomb_explode_at, next_beep;
	int planter, defuser;
	float plant_progress, defuse_progress;
	int spectate;
	Piece pieces[MAX_PIECES];
	Portal portals[MAX_PORTALS];
	Fx fx[MAX_FX];
	Shell shells[MAX_SHELLS];
	FeedLine feed[MAX_FEED];
	char notice[96];
	double notice_until;
	char banner[64], banner_sub[96];
	bool banner_on;
	double match_over_restart;
	float hurt;
	int music_voice, wind_voice;
	bool tacmap_open;
	bool buy_open;
	int buy_sel;
	bool board_open;
	bool net_can_buy; // no cliente: o host diz se da para comprar
	double net_last_state; // no cliente: quando chegou o ultimo estado do host
} Match;

// ---------------------------------------------------------------- comandos (controle e teclado juntos)

typedef struct {
	float move_x, move_y, look_x, look_y;
	bool fire, fire_pressed, alt_pressed, alt;
	bool jump, crouch, walk;
	bool reload, use, use_pressed, next_weapon, prev_weapon;
	int slot_pressed; // 1..5, ou 0
	int build_select; // peca escolhida (tecla), ou -1
	bool build_cycle; // RB: entra no modo de construcao / proxima peca
	bool material_cycle;
	bool edit_pressed;
	bool buy_pressed, drop_pressed, board, pause_pressed;
	// menus
	bool up, down, left, right, accept, back;
	float cursor_x, cursor_y; // ponteiro (mouse, ou o analogico nos menus)
	bool click, right_click;
	float wheel;
	bool pad_aim; // a mira esta vindo do analogico (no PC, ate o mouse mexer)
} Actions;

// ---------------------------------------------------------------- o jogo todo

enum {
	STATE_MENU,
	STATE_MATCH,
};

enum {
	NET_ROLE_NONE,
	NET_ROLE_HOST, // a partida roda aqui; os outros humanos mandam comandos
	NET_ROLE_CLIENT, // a partida roda no host; aqui so o proprio movimento e o desenho
};

typedef struct {
	float sensitivity, fov, volume, music;
	int difficulty, side, mode; // side: 0 CT, 1 T, 2 aleatorio
	float vm_x, vm_y, vm_z;
	bool invert_y;
	// assistencia de mira do controle (so contra bots): forca 0..1 e o cone em graus em volta da mira
	float aim_assist, aim_assist_fov;
} Settings;

typedef struct {
	BsPack map;
	Assets as;
	BsCollider col;
	Nav nav;
	Settings settings;
	int state;
	double time; // segundos de jogo (para quando pausado)
	float dt;
	bool paused;
	int pause_sel;
	Match m;
	PadState pad, prev_pad;
	Actions act;
	GfxCamera cam;
	// HUD
	int atlas_tex;
	int build_tex[3];
	int *model_tex; // textura da GPU de cada textura dos assets
	// menu
	int menu_sel, menu_page;
	Skin menu_agent;
	AnimPlayer menu_anim;
	// viewmodel
	Skin vm[2][16];
	int vm_set_ids[16];
	AnimPlayer vm_anim;
	int vm_set, vm_team;
	// malhas fixas (armas em terceira pessoa, capsulas, C4, portal gun)
	int rigid_mesh[64];
	const Model *rigid_model[64];
	int rigid_count;
	// estatisticas
	float fps;
	GfxStats stats;
	bool autoplay;
	double quit_at;
	bool want_quit;
	uint32_t rng;
	// rede (EPTX)
	int net_role;
	int net_match_id;
	char pack_hash[40];
	char login_user[40], login_pass[64];
	int login_field;
} Game;

extern Game G;

// main.c
float frand(void);
float frand_range(float lo, float hi);
float frandn(float sigma);

// anim.c
void anim_play(AnimPlayer *p, const Anim *a, float blend);
void anim_queue(AnimPlayer *p, const Anim *a);
void anim_update(AnimPlayer *p, float dt);
bool anim_done(const AnimPlayer *p);
bool skin_init(Skin *s, const Model *m);
void skin_free(Skin *s);
void skin_pose(Skin *s, const AnimPlayer *p);
void skin_draw(Skin *s, const Xf *world, const float tint[4]);
int rigid_mesh_of(const char *name);
void rigid_draw(const char *name, const Xf *world, const float tint[4]);
Vec3 skin_attach(const Skin *s, const char *name, const Xf *world, bool *found);

// world.c
bool world_init(char *err, size_t eb);
RayHit world_ray(Vec3 from, Vec3 to, uint32_t mask, int ignore_fighter);
bool world_site_at(Vec3 p, int *site);
Vec3 world_random_in_site(int site);
Vec3 nav_closest(Vec3 p);
int nav_path(Vec3 from, Vec3 to, Vec3 *out, int max);
void world_update_dynamic(void);

// fighter.c
int weapon_index(const char *id);
void fighter_reset_loadout(Fighter *f);
void fighter_give(Fighter *f, int weapon);
bool fighter_buy(Fighter *f, const char *id);
void fighter_switch(Fighter *f, int slot);
const WeaponsDef *fighter_weapon(const Fighter *f);
Gun *fighter_gun(Fighter *f);
Vec3 fighter_eye(const Fighter *f);
Vec3 fighter_aim(const Fighter *f);
void fighter_fire(Fighter *f);
void fighter_start_reload(Fighter *f);
void fighter_take_hit(Fighter *f, Fighter *attacker, int weapon, int part);
void fighter_die(Fighter *f, Fighter *killer, int weapon, bool headshot);
void fighter_respawn(Fighter *f, Vec3 at, float yaw);
float fighter_max_speed(const Fighter *f);
void fighter_update_anim(Fighter *f, float dt);
void fighter_draw(Fighter *f);
void player_update(Fighter *f, float dt);
void player_remote(Fighter *f, const Actions *a, float dt);
void player_remote_choice(Fighter *f, const Actions *a);
void player_draw_viewmodel(Fighter *f);
void player_viewmodel_play(const char *clip);
void bot_setup(Fighter *f, int skill);
void bot_round_start(Fighter *f);
void bot_buy(Fighter *f);
void bot_update(Fighter *f, float dt);
void bot_hurt(Fighter *f, Fighter *attacker);

// match.c
void match_start(int mode, int side, int difficulty, bool autoplay);
void match_update(float dt);
void match_draw(void);
void match_draw_viewmodel(void);
void build_update_and_draw(float dt);
void build_clear(void);
void portals_draw(void);
void match_end(void);
bool match_buy_allowed(const Fighter *f);
void match_died(Fighter *victim, Fighter *killer, int weapon, bool headshot);
void match_plant_tick(Fighter *f, float dt);
void match_plant_cancel(Fighter *f);
void match_defuse_tick(Fighter *f, float dt);
void match_defuse_cancel(Fighter *f);
void match_drop_bomb(Fighter *f);
void match_bot_objective(Fighter *b);
bool match_is_busy(const Fighter *f);
void match_notice(const char *text, float dur);
void fx_tracer(Vec3 a, Vec3 b);
void fx_flash(Vec3 at, bool viewmodel);
void fx_impact(Vec3 p, Vec3 n, bool wood);
void fx_particles(Vec3 p, const float color[4], int n);
void fx_shell(int kind, Vec3 at, const Xf *view);
void sound_at(const char *id, Vec3 p, float vol_db);
void sound_ui(const char *id, float vol_db);
void music_play(const char *id, float vol_db);
// construcao
bool build_enabled(void);
Placement build_placement(const Fighter *f, int piece, float yaw, float pitch);
bool build_can(const Fighter *f, int piece, const Placement *p);
bool build_try(Fighter *f, int piece, const Placement *p);
void build_damage(int piece, float amount);
int build_tile_at(const Piece *p, Vec3 from, Vec3 dir);
Xf build_tile_xf(const Piece *p, int idx, Vec3 viewer, float *w, float *h);
bool build_apply_edit(Piece *p, const int *sel, int count);
void build_reset_edit(Piece *p);
void build_draw_ghost(const Fighter *f);
int build_edit_grid(int piece);
// portais
void portal_fire(Fighter *f);
void portal_place_orange(Fighter *f, Vec3 feet, float yaw);
bool portal_on_segment(Vec3 a, Vec3 b, Vec3 *point, Xf *through, int *portal);
Xf portal_through(const Portal *p);
Vec3 portal_normal(const Portal *p);
Vec3 portal_front(const Portal *p, float dist);
void portals_clear(void);
void portals_update(void);
bool portal_shortcut(Fighter *b, Vec3 goal, int *entry, Vec3 *approach);
void portal_bot_open(Fighter *b);
bool portal_linked(int i);
// mapa tatico
void tacmap_routes(int team, Vec3 routes[MAX_FIGHTERS][MAX_PATH + 2], int *lens, int *count);
int tacmap_recommend(int team, Vec3 *pos, float *yaw, const char **label, int max);
void tacmap_open(void);
void tacmap_update(void);
void tacmap_draw(void);

// netgame.c
void netgame_remote(Fighter *f, float dt);
void netgame_flush(void);
void netgame_host_begin(void);
void netgame_client_begin(int mode);
void netgame_update(float dt);
void netgame_end(void);
void netgame_menu_poll(void);
// eventos que o host repassa aos clientes
void netev_tracer(Vec3 a, Vec3 b);
void netev_impact(Vec3 p, Vec3 n, bool wood);
void netev_particles(Vec3 p, const float c[4], int n);
void netev_sound(const char *id, Vec3 p, float vol, bool ui);
void netev_music(const char *id, float vol);
void netev_feed(Fighter *k, Fighter *v, int weapon, bool headshot);
void netev_banner(int kind, int winner, const char *why);
// escolhas do cliente que vao para o host
void netcl_buy(const char *id);
void netcl_orange(Vec3 feet, float yaw);
bool netcl_choices(const Actions *a);

// ui.c
void input_build(const PadState *pad, const PadState *prev, Actions *a, bool menus);
void ui_begin(void);
void ui_flush(void);
void ui_rect(float x, float y, float w, float h, const float c[4]);
float ui_icon(const char *name, float x, float y, float h, const float c[4], bool right_align);
float ui_text(const char *s, float x, float y, float size, const float c[4], int align, bool bold);
float ui_text_width(const char *s, float size, bool bold);
void hud_draw(void);
void hud_round_started(void);
void hud_round_over(int winner, const char *why);
void hud_match_over(int winner);
void hud_feed(Fighter *killer, Fighter *victim, int weapon, bool headshot);
void buy_update(void);
void pause_update(void);
void menu_enter(void);
void menu_update(float dt);
void menu_draw(void);
void menu_draw_ui(void);

#endif
