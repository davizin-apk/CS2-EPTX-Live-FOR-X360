// Gerado pelo tools/pack_assets.py a partir das planilhas do Bot Strike (C:/mlt/proj/design).
// Nao edite: mude a planilha e gere de novo.
#ifndef BS_DEFS_H
#define BS_DEFS_H

#include <stdbool.h>

typedef struct {
	const char * id;
	const char * name;
	int slot;
	int price;
	const char * team;
	int damage;
	float armor_pen;
	float head_mult;
	float fire_interval;
	int mag;
	int reserve;
	float reload_time;
	int move_speed;
	float spread;
	float recoil;
	float range_m;
	bool auto_;
	int scope_fov;
	const char * model_asset;
	const char * icon_asset;
	const char * sound_asset;
	const char * anim_set;
	const char * shell_asset;
	const char * only_mode;
} WeaponsDef;
enum {
	WEAPONS_KNIFE = 0,
	WEAPONS_GLOCK = 1,
	WEAPONS_USP = 2,
	WEAPONS_AK47 = 3,
	WEAPONS_M4A1S = 4,
	WEAPONS_AWP = 5,
	WEAPONS_PORTALGUN = 6,
	WEAPONS_COUNT = 7,
};
static const WeaponsDef WEAPONS[WEAPONS_COUNT] = {
	{ "knife", "Knife", 3, 0, "any", 40, 0.85f, 1.0f, 0.4f, 0, 0, 0.0f, 250, 0.0f, 0.0f, 1.8f, true, 0, "mdl_knife", "img_knife", "snd_knife", "knife", "none", "any" },
	{ "glock", "Glock-18", 2, 200, "T", 30, 0.47f, 4.0f, 0.15f, 20, 120, 2.2f, 240, 0.012f, 0.6f, 60.0f, false, 0, "mdl_glock", "img_glock", "snd_glock", "glock", "shell_9mm", "any" },
	{ "usp", "USP-S", 2, 200, "CT", 35, 0.505f, 4.0f, 0.17f, 12, 24, 2.2f, 240, 0.008f, 0.7f, 60.0f, false, 0, "mdl_usp", "img_usp", "snd_usp", "pistol", "shell_9mm", "any" },
	{ "ak47", "AK-47", 1, 2700, "T", 36, 0.775f, 4.0f, 0.1f, 30, 90, 2.5f, 215, 0.006f, 1.0f, 120.0f, true, 0, "mdl_ak47", "img_ak47", "snd_ak47", "ak", "shell_762", "any" },
	{ "m4a1s", "M4A1-S", 1, 2900, "CT", 38, 0.7f, 4.0f, 0.1f, 20, 80, 3.1f, 225, 0.005f, 0.75f, 120.0f, true, 0, "mdl_m4a1s", "img_m4a1s", "snd_m4a1s", "rifle", "shell_762", "any" },
	{ "awp", "AWP", 1, 4750, "any", 115, 0.975f, 4.0f, 1.46f, 5, 30, 3.7f, 200, 0.002f, 2.5f, 200.0f, false, 20, "mdl_awp", "img_awp", "snd_awp", "awp", "shell_50cal", "any" },
	{ "portalgun", "Portal Gun", 4, 1000, "any", 0, 0.0f, 1.0f, 0.45f, 0, 0, 0.0f, 240, 0.0f, 0.3f, 150.0f, false, 0, "none", "none", "snd_usp", "none", "none", "portal" },
};

typedef struct {
	const char * id;
	const char * name;
	int price;
	const char * team;
	const char * effect;
	const char * icon_asset;
} EquipmentDef;
enum {
	EQUIPMENT_KEVLAR = 0,
	EQUIPMENT_KEVLAR_HELMET = 1,
	EQUIPMENT_DEFUSER = 2,
	EQUIPMENT_COUNT = 3,
};
static const EquipmentDef EQUIPMENT[EQUIPMENT_COUNT] = {
	{ "kevlar", "Kevlar Vest", 650, "any", "armor100", "img_armor" },
	{ "kevlar_helmet", "Kevlar + Helmet", 1000, "any", "armor100_helmet", "img_armor_helmet" },
	{ "defuser", "Defuse Kit", 400, "CT", "defuse5", "img_defuser" },
};

typedef struct {
	const char * id;
	const char * name;
	const char * tagline;
	int team_size;
	int rounds_to_win;
	int max_rounds;
	int round_time;
	int freeze_time;
	int buy_time;
	int bomb_time;
	float plant_time;
	int defuse_time;
	int defuse_kit_time;
	int start_money;
	int max_money;
	int win_bonus;
	int loss_bonus;
	int kill_reward;
	bool building;
	bool portals;
	const char * map;
} ModesDef;
enum {
	MODES_CLASSIC = 0,
	MODES_BUILD = 1,
	MODES_PORTAL = 2,
	MODES_COUNT = 3,
};
static const ModesDef MODES[MODES_COUNT] = {
	{ "classic", "Competitive vs Bots", "5v5 bomb defusal on Dust II. You and 4 bots against 5 bots.", 5, 7, 12, 115, 10, 20, 40, 3.2f, 10, 5, 800, 16000, 3250, 1400, 300, false, false, "de_dust2" },
	{ "build", "Build Fight vs Bots", "5v5 bomb defusal where everyone can build walls, floors and ramps.", 5, 7, 12, 135, 10, 20, 45, 3.2f, 10, 5, 800, 16000, 3250, 1400, 300, true, false, "de_dust2" },
	{ "portal", "Portal Strike vs Bots", "5v5 bomb defusal with a buyable Portal Gun: fire one portal, open the map and drop the other anywhere.", 5, 7, 12, 125, 10, 20, 40, 3.2f, 10, 5, 800, 16000, 3250, 1400, 300, false, true, "de_dust2" },
};

typedef struct {
	const char * id;
	const char * name;
	const char * agent_asset;
	const char * spawn_class;
	const char * color;
	const char * default_pistol;
	const char * bot_names[5];
} TeamsDef;
enum {
	TEAMS_CT = 0,
	TEAMS_T = 1,
	TEAMS_COUNT = 2,
};
static const TeamsDef TEAMS[TEAMS_COUNT] = {
	{ "CT", "Counter-Terrorists", "mdl_ct", "info_player_counterterrorist", "#5d79ae", "usp", { "Shane", "Vitaliy", "Karl", "Ethan", "Brian" } },
	{ "T", "Terrorists", "mdl_t", "info_player_terrorist", "#de9b35", "glock", { "Rock", "Ivan", "Marcus", "Yuri", "Dmitri" } },
};

typedef struct {
	const char * id;
	const char * name;
	float reaction_s;
	float aim_error_deg;
	float aim_settle_s;
	int fov_deg;
	int see_range_m;
	float build_chance;
} BotSkillsDef;
enum {
	BOT_SKILLS_EASY = 0,
	BOT_SKILLS_NORMAL = 1,
	BOT_SKILLS_HARD = 2,
	BOT_SKILLS_COUNT = 3,
};
static const BotSkillsDef BOT_SKILLS[BOT_SKILLS_COUNT] = {
	{ "easy", "Easy", 0.7f, 6.0f, 1.2f, 100, 40, 0.25f },
	{ "normal", "Normal", 0.4f, 3.5f, 0.8f, 120, 60, 0.5f },
	{ "hard", "Hard", 0.22f, 2.0f, 0.45f, 140, 80, 0.75f },
};

typedef struct {
	const char * id;
	const char * name;
	const char * key;
	const char * alt_key;
	int cost;
	int max_hp;
	float build_time;
	const char * shape;
	int edit_grid;
} BuildPiecesDef;
enum {
	BUILD_PIECES_WALL = 0,
	BUILD_PIECES_FLOOR = 1,
	BUILD_PIECES_RAMP = 2,
	BUILD_PIECES_CONE = 3,
	BUILD_PIECES_COUNT = 4,
};
static const BuildPiecesDef BUILD_PIECES[BUILD_PIECES_COUNT] = {
	{ "wall", "Wall", "Q", "F1", 10, 150, 1.0f, "wall", 3 },
	{ "floor", "Floor", "X", "F2", 10, 140, 1.0f, "floor", 2 },
	{ "ramp", "Stairs", "C", "F3", 10, 140, 1.0f, "ramp", 2 },
	{ "cone", "Cone", "V", "F4", 10, 140, 1.0f, "cone", 2 },
};

typedef struct {
	const char * id;
	const char * name;
	float hp_mult;
	float time_mult;
	float start_hp;
	const char * color;
	int start_amount;
	int per_kill;
	const char * texture_hint;
} BuildMaterialsDef;
enum {
	BUILD_MATERIALS_WOOD = 0,
	BUILD_MATERIALS_BRICK = 1,
	BUILD_MATERIALS_METAL = 2,
	BUILD_MATERIALS_COUNT = 3,
};
static const BuildMaterialsDef BUILD_MATERIALS[BUILD_MATERIALS_COUNT] = {
	{ "wood", "Wood", 1.0f, 1.0f, 0.1f, "b8854f", 300, 50, "wood_planks" },
	{ "brick", "Brick", 2.0f, 2.5f, 0.1f, "a5523f", 150, 30, "brick_ground" },
	{ "metal", "Metal", 3.333f, 4.5f, 0.1f, "8f9aa3", 100, 20, "metal_panels_01_color" },
};

#endif
