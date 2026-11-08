// Comandos, desenho 2D e as telas: HUD (hud.gd), compra, placar, pausa, mapa tatico (tacmap.gd) e menu (menu.gd).
// Tudo numa grade de 1280x720, com as fontes e os icones do proprio CS2.
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "net.h"

// cores do ui.gd
static const float WHITE[4] = { 1, 1, 1, 1 };
static const float TEXT[4] = { 0.92f, 0.93f, 0.95f, 1 };
static const float DIM[4] = { 0.62f, 0.65f, 0.7f, 1 };
static const float ACCENT[4] = { 0.36f, 0.62f, 0.98f, 1 };
static const float GO[4] = { 0.3f, 0.68f, 0.31f, 1 };
static const float CT_C[4] = { 0.36f, 0.47f, 0.68f, 1 };
static const float T_C[4] = { 0.87f, 0.61f, 0.21f, 1 };
static const float PANEL[4] = { 0.11f, 0.12f, 0.15f, 0.94f };
static const float SHADE[4] = { 0.05f, 0.06f, 0.08f, 0.86f };

enum {
	ALIGN_LEFT,
	ALIGN_CENTER,
	ALIGN_RIGHT,
};

// ---------------------------------------------------------------- comandos

static bool key(const PadState *p, int vk)
{
	return p->keyboard && p->keys[vk & 0xFF];
}
static bool key_pressed(const PadState *p, const PadState *q, int vk)
{
	return key(p, vk) && !key(q, vk);
}
static bool btn(const PadState *p, uint32_t b)
{
	return p->connected && (p->buttons & b);
}
static bool btn_pressed(const PadState *p, const PadState *q, uint32_t b)
{
	return btn(p, b) && !btn(q, b);
}

void input_build(const PadState *p, const PadState *q, Actions *a, bool menus)
{
	static double repeat_at[4];
	memset(a, 0, sizeof *a);
	a->build_select = -1;
	const float dt = G.dt;
	// andar
	a->move_x = p->lx + (float)key(p, 'D') - (float)key(p, 'A');
	a->move_y = p->ly + (float)key(p, 'W') - (float)key(p, 'S');
	a->move_x = clampf(a->move_x, -1, 1);
	a->move_y = clampf(a->move_y, -1, 1);
	// olhar: o analogico com curva (devagar no centro para mirar), o mouse direto
	const float rate = 3.0f;
	a->look_x = p->rx * fabsf(p->rx) * rate * dt + p->look_x;
	a->look_y = (p->ry * fabsf(p->ry) * rate * 0.7f * dt + p->look_y) * (G.settings.invert_y ? -1.0f : 1.0f);
	// a assistencia de mira so vale para o controle: o mouse mexendo desliga ate o analogico voltar
	static bool pad_aim = true;
	if (fabsf(p->rx) + fabsf(p->ry) + fabsf(p->lx) + fabsf(p->ly) > 0.0f || p->rt > 0.3f)
		pad_aim = true;
	if (p->look_x != 0.0f || p->look_y != 0.0f || p->mouse_left)
		pad_aim = false;
	a->pad_aim = pad_aim;
	a->fire = p->rt > 0.3f || p->mouse_left;
	a->fire_pressed = (p->rt > 0.3f && q->rt <= 0.3f) || (p->mouse_left && !q->mouse_left);
	a->alt = p->lt > 0.5f || p->mouse_right;
	a->alt_pressed = (p->lt > 0.5f && q->lt <= 0.5f) || (p->mouse_right && !q->mouse_right);
	a->jump = btn(p, PAD_A) || key(p, 0x20);
	a->crouch = btn(p, PAD_B) || key(p, 0x11);
	a->walk = btn(p, PAD_LS) || key(p, 0x10);
	a->reload = btn_pressed(p, q, PAD_X) || key_pressed(p, q, 'R');
	a->use = btn(p, PAD_X) || key(p, 'E');
	a->next_weapon = btn_pressed(p, q, PAD_Y) || p->wheel < -0.5f;
	a->prev_weapon = btn_pressed(p, q, PAD_LB) || p->wheel > 0.5f;
	a->material_cycle = btn_pressed(p, q, PAD_LB);
	a->build_cycle = btn_pressed(p, q, PAD_RB);
	a->edit_pressed = btn_pressed(p, q, PAD_RS) || key_pressed(p, q, 'F');
	for (int k = 0; k < 5; k++)
		if (key_pressed(p, q, '1' + k))
			a->slot_pressed = k + 1;
	// as teclas de construcao da planilha (Q/X/C/V ou F1-F4)
	for (int i = 0; i < BUILD_PIECES_COUNT; i++) {
		const char *k = BUILD_PIECES[i].key, *alt = BUILD_PIECES[i].alt_key;
		if ((k[0] && !k[1] && key_pressed(p, q, k[0])) || (alt[0] == 'F' && key_pressed(p, q, 0x70 + atoi(alt + 1) - 1)))
			a->build_select = i;
	}
	if (key_pressed(p, q, 'Q') && a->build_select < 0)
		a->slot_pressed = 0; // Q fora do Build Fight: ultima arma (abaixo)
	a->buy_pressed = btn_pressed(p, q, PAD_UP) || key_pressed(p, q, 'B');
	a->drop_pressed = btn_pressed(p, q, PAD_DOWN) || key_pressed(p, q, 'G');
	a->board = btn(p, PAD_BACK) || key(p, 0x09);
	a->pause_pressed = btn_pressed(p, q, PAD_START) || key_pressed(p, q, 0x1B);
	// menus: cruz, analogico ou setas, com repeticao
	const bool held[4] = { btn(p, PAD_UP) || p->ly > 0.6f || key(p, 0x26) || key(p, 'W'),
		btn(p, PAD_DOWN) || p->ly < -0.6f || key(p, 0x28) || key(p, 'S'),
		btn(p, PAD_LEFT) || p->lx < -0.6f || key(p, 0x25) || key(p, 'A'),
		btn(p, PAD_RIGHT) || p->lx > 0.6f || key(p, 0x27) || key(p, 'D') };
	static bool was[4];
	bool fire_dir[4];
	for (int i = 0; i < 4; i++) {
		fire_dir[i] = false;
		if (held[i] && !was[i]) {
			fire_dir[i] = true;
			repeat_at[i] = G.time + 0.35;
		} else if (held[i] && G.time >= repeat_at[i]) {
			fire_dir[i] = true;
			repeat_at[i] = G.time + 0.09;
		}
		was[i] = held[i];
	}
	if (menus) {
		a->up = fire_dir[0];
		a->down = fire_dir[1];
		a->left = fire_dir[2];
		a->right = fire_dir[3];
	}
	a->accept = btn_pressed(p, q, PAD_A) || key_pressed(p, q, 0x0D) || key_pressed(p, q, 0x20);
	a->back = btn_pressed(p, q, PAD_B) || key_pressed(p, q, 0x1B);
	a->click = (p->mouse_left && !q->mouse_left) || btn_pressed(p, q, PAD_A);
	a->right_click = (p->mouse_right && !q->mouse_right) || btn_pressed(p, q, PAD_B);
	a->wheel = p->wheel + (btn_pressed(p, q, PAD_RB) ? -1.0f : 0.0f) + (btn_pressed(p, q, PAD_LB) ? 1.0f : 0.0f);
	// ponteiro: o mouse no PC; no controle, o analogico esquerdo o move
	static float cx = 640, cy = 360;
	if (p->mouse_x != q->mouse_x || p->mouse_y != q->mouse_y) {
		cx = p->mouse_x;
		cy = p->mouse_y;
	}
	cx = clampf(cx + p->lx * 700.0f * dt, 0, GFX_WIDTH);
	cy = clampf(cy - p->ly * 700.0f * dt, 0, GFX_HEIGHT);
	a->cursor_x = cx;
	a->cursor_y = cy;
}

// ---------------------------------------------------------------- desenho 2D (um lote por quadro)

enum {
	MAX_2D = 30000,
};
static FxVertex batch[MAX_2D];
static uint32_t batch_n;

void ui_begin(void)
{
	batch_n = 0;
}

void ui_flush(void)
{
	gfx_draw_2d(batch, batch_n, G.atlas_tex);
	batch_n = 0;
}

static void quad_uv(float x, float y, float w, float h, float u0, float v0, float u1, float v1, const float c[4])
{
	if (batch_n + 6 > MAX_2D)
		ui_flush();
	const float xs[4] = { x, x + w, x + w, x }, ys[4] = { y, y, y + h, y + h };
	const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
	static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++) {
		const int j = idx[i];
		batch[batch_n++] = (FxVertex){ xs[j], ys[j], 0, c[0], c[1], c[2], c[3], us[j], vs[j] };
	}
}

static void tri2d(float x0, float y0, float x1, float y1, float x2, float y2, const float c[4])
{
	if (batch_n + 3 > MAX_2D)
		ui_flush();
	const AtlasRect *w = assets_rect(&G.as, "white");
	const float u = (w->x + 2.0f) / G.as.atlas_w, v = (w->y + 2.0f) / G.as.atlas_h;
	batch[batch_n++] = (FxVertex){ x0, y0, 0, c[0], c[1], c[2], c[3], u, v };
	batch[batch_n++] = (FxVertex){ x1, y1, 0, c[0], c[1], c[2], c[3], u, v };
	batch[batch_n++] = (FxVertex){ x2, y2, 0, c[0], c[1], c[2], c[3], u, v };
}

void ui_rect(float x, float y, float w, float h, const float c[4])
{
	static const AtlasRect *white;
	if (!white)
		white = assets_rect(&G.as, "white");
	const float u = (white->x + 2.0f) / G.as.atlas_w, v = (white->y + 2.0f) / G.as.atlas_h;
	quad_uv(x, y, w, h, u, v, u, v, c);
}

static void line2d(float x0, float y0, float x1, float y1, float width, const float c[4])
{
	const float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
	if (l < 0.01f)
		return;
	const float nx = -dy / l * width / 2, ny = dx / l * width / 2;
	tri2d(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
	tri2d(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

float ui_icon(const char *name, float x, float y, float h, const float c[4], bool right_align)
{
	const AtlasRect *r = assets_rect(&G.as, name);
	if (!r)
		return 0;
	const float w = h * r->w / (float)r->h;
	if (right_align)
		x -= w;
	quad_uv(x, y, w, h, (float)r->x / G.as.atlas_w, (float)r->y / G.as.atlas_h, (float)(r->x + r->w) / G.as.atlas_w,
		(float)(r->y + r->h) / G.as.atlas_h, c);
	return w;
}

static const Font *font(bool bold)
{
	return bold ? &G.as.font_bold : &G.as.font_regular;
}

static float base_px(bool bold)
{
	return bold ? 44.0f : 30.0f;
}

float ui_text_width(const char *s, float size, bool bold)
{
	const Font *f = font(bold);
	const float k = size / base_px(bold);
	float w = 0;
	for (const unsigned char *c = (const unsigned char *)s; *c; c++)
		w += f->glyphs[*c].advance * k;
	return w;
}

// Texto com o topo da linha em y; devolve a largura.
float ui_text(const char *s, float x, float y, float size, const float c[4], int align, bool bold)
{
	const Font *f = font(bold);
	const float k = size / base_px(bold);
	const float w = ui_text_width(s, size, bold);
	if (align == ALIGN_CENTER)
		x -= w / 2;
	else if (align == ALIGN_RIGHT)
		x -= w;
	// sombra, como o contorno dos rotulos do hud.gd
	const float sh[4] = { 0, 0, 0, c[3] * 0.55f };
	for (int pass = 0; pass < 2; pass++) {
		float cx = x + (pass == 0 ? 2 : 0), cy = y + (pass == 0 ? 2 : 0);
		for (const unsigned char *ch = (const unsigned char *)s; *ch; ch++) {
			const Glyph *g = &f->glyphs[*ch];
			if (g->w)
				quad_uv(cx + g->ox * k, cy + g->oy * k, g->w * k, g->h * k, (float)g->x / G.as.atlas_w,
					(float)g->y / G.as.atlas_h, (float)(g->x + g->w) / G.as.atlas_w, (float)(g->y + g->h) / G.as.atlas_h,
					pass == 0 ? sh : c);
			cx += g->advance * k;
		}
	}
	return w;
}

static void panel(float x, float y, float w, float h, const float c[4])
{
	ui_rect(x, y, w, h, c);
	const float line[4] = { 1, 1, 1, 0.08f };
	ui_rect(x, y, w, 1, line);
	ui_rect(x, y + h - 1, w, 1, line);
}

// ---------------------------------------------------------------- HUD

static char banner_main[64], banner_sub[96];
static double banner_until;

void hud_round_started(void)
{
	G.m.banner_on = false;
	banner_until = 0;
}

void hud_round_over(int winner, const char *why)
{
	netev_banner(0, winner, why);
	snprintf(banner_main, sizeof banner_main, "%s WIN", winner ? "TERRORISTS" : "COUNTER-TERRORISTS");
	snprintf(banner_sub, sizeof banner_sub, "%s", why);
	banner_until = G.time + 5.5;
}

void hud_match_over(int winner)
{
	netev_banner(1, winner, "");
	if (winner < 0)
		snprintf(banner_main, sizeof banner_main, "DRAW");
	else
		snprintf(banner_main, sizeof banner_main, "%s WIN THE MATCH", winner ? "TERRORISTS" : "COUNTER-TERRORISTS");
	snprintf(banner_sub, sizeof banner_sub, "Final score  CT %d - %d T", G.m.score[0], G.m.score[1]);
	banner_until = G.time + 10.0;
}

void hud_feed(Fighter *k, Fighter *v, int w, bool headshot)
{
	netev_feed(k, v, w, headshot);
	Match *m = &G.m;
	memmove(&m->feed[1], &m->feed[0], (MAX_FEED - 1) * sizeof m->feed[0]);
	FeedLine *l = &m->feed[0];
	memset(l, 0, sizeof *l);
	snprintf(l->killer, sizeof l->killer, "%s", k ? k->name : "");
	snprintf(l->victim, sizeof l->victim, "%s", v->name);
	l->killer_team = k ? k->team : -1;
	l->victim_team = v->team;
	l->weapon = w;
	l->headshot = headshot;
	l->born = (float)G.time;
}

static const float *team_color(int t)
{
	return t ? T_C : CT_C;
}

static void crosshair(const Fighter *f)
{
	const float c[4] = { 0.3f, 1.0f, 0.4f, 0.95f };
	const float gap = 4 + fminf(f->recoil, 10) * 0.8f;
	ui_rect(640 - gap - 8, 359, 8, 2, c);
	ui_rect(640 + gap, 359, 8, 2, c);
	ui_rect(639, 360 - gap - 8, 2, 8, c);
	ui_rect(639, 360 + gap, 2, 8, c);
}

static void scope_overlay(void)
{
	// o anel preto da AWP: tudo fora de um circulo de raio 0.43 da altura
	const float k[4] = { 0, 0, 0, 1 };
	const float r = 720 * 0.43f;
	const int n = 64;
	for (int i = 0; i < n; i++) {
		const float a0 = 2 * PI_F * i / n, a1 = 2 * PI_F * (i + 1) / n;
		const float x0 = 640 + cosf(a0) * r, y0 = 360 + sinf(a0) * r, x1 = 640 + cosf(a1) * r, y1 = 360 + sinf(a1) * r;
		const float X0 = 640 + cosf(a0) * 1500, Y0 = 360 + sinf(a0) * 1500, X1 = 640 + cosf(a1) * 1500, Y1 = 360 + sinf(a1) * 1500;
		tri2d(x0, y0, X0, Y0, X1, Y1, k);
		tri2d(x0, y0, X1, Y1, x1, y1, k);
	}
	ui_rect(0, 359, 1280, 1.5f, k);
	ui_rect(639, 0, 1.5f, 720, k);
}

static void fmt_time(char *out, size_t n, float s)
{
	const int t = (int)ceilf(s);
	snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void alive_row(int team, float x, bool right)
{
	int k = 0;
	for (int i = 0; i < G.m.count; i++) {
		const Fighter *f = &G.m.f[i];
		if (f->team != team)
			continue;
		float c[4];
		memcpy(c, team_color(team), sizeof c);
		if (!f->alive)
			c[0] = c[1] = c[2] = 0.25f;
		const float px = right ? x + k * 34 : x - (k + 1) * 34;
		ui_rect(px, 14, 30, 30, c);
		ui_text(f->is_player ? "Y" : (const char[]){ f->name[0], 0 }, px + 15, 14, 22, WHITE, ALIGN_CENTER, true);
		k++;
	}
}

static void build_bar(const Fighter *f)
{
	static const char *names[4] = { "WALL", "FLOOR", "STAIRS", "CONE" };
	float x = 640 - 2 * 92;
	for (int i = 0; i < BUILD_PIECES_COUNT && i < 4; i++) {
		const bool on = f->build_piece == i;
		const float bg[4] = { on ? 0.25f : 0.08f, on ? 0.45f : 0.09f, on ? 0.8f : 0.12f, 0.85f };
		panel(x, 602, 86, 46, bg);
		ui_text(names[i], x + 43, 604, 17, WHITE, ALIGN_CENTER, true);
		char k[24];
		snprintf(k, sizeof k, "%s / %s", BUILD_PIECES[i].key, BUILD_PIECES[i].alt_key);
		ui_text(k, x + 43, 625, 14, DIM, ALIGN_CENTER, false);
		x += 92;
	}
	x = 640 - 3 * 76 / 2.0f;
	for (int m = 0; m < BUILD_MATERIALS_COUNT; m++) {
		const bool on = f->build_mat == m;
		const float bg[4] = { on ? 0.9f : 0.1f, on ? 0.75f : 0.1f, on ? 0.35f : 0.12f, on ? 0.9f : 0.8f };
		panel(x, 654, 70, 30, bg);
		char t[32];
		snprintf(t, sizeof t, "%s %d", BUILD_MATERIALS[m].name, f->mats[m]);
		ui_text(t, x + 35, 658, 15, on ? (const float[]){ 0, 0, 0, 1 } : WHITE, ALIGN_CENTER, true);
		x += 76;
	}
}

static void scoreboard(void)
{
	panel(240, 110, 800, 500, SHADE);
	ui_text("SCOREBOARD", 640, 120, 30, WHITE, ALIGN_CENTER, true);
	for (int team = 0; team < 2; team++) {
		const float y0 = 170 + team * 220;
		char hdr[64];
		snprintf(hdr, sizeof hdr, "%s  %d", team ? "TERRORISTS" : "COUNTER-TERRORISTS", G.m.score[team]);
		ui_rect(260, y0, 760, 30, team_color(team));
		ui_text(hdr, 270, y0 + 2, 22, WHITE, ALIGN_LEFT, true);
		ui_text("K", 820, y0 + 4, 18, WHITE, ALIGN_CENTER, true);
		ui_text("D", 880, y0 + 4, 18, WHITE, ALIGN_CENTER, true);
		ui_text("$", 960, y0 + 4, 18, WHITE, ALIGN_CENTER, true);
		int row = 0;
		for (int i = 0; i < G.m.count; i++) {
			const Fighter *f = &G.m.f[i];
			if (f->team != team)
				continue;
			const float y = y0 + 34 + row * 34;
			const float c[4] = { 1, 1, 1, f->alive ? 1.0f : 0.45f };
			ui_text(f->name, 270, y, 20, c, ALIGN_LEFT, f->is_player);
			char s[16];
			snprintf(s, sizeof s, "%d", f->kills);
			ui_text(s, 820, y, 20, c, ALIGN_CENTER, false);
			snprintf(s, sizeof s, "%d", f->deaths);
			ui_text(s, 880, y, 20, c, ALIGN_CENTER, false);
			if (G.m.player < 0 || f->team == G.m.f[G.m.player].team) {
				snprintf(s, sizeof s, "$%d", f->money);
				ui_text(s, 960, y, 20, c, ALIGN_CENTER, false);
			}
			row++;
		}
	}
}

void hud_draw(void)
{
	Match *m = &G.m;
	const Fighter *view = m->player >= 0 ? &m->f[m->player] : NULL;
	const bool dead = !view || !view->alive;
	const Fighter *shown = dead ? &m->f[m->spectate] : view;
	// dano: a tela fica vermelha
	if (m->hurt > 0) {
		const float c[4] = { 0.8f, 0, 0, m->hurt * 0.35f };
		ui_rect(0, 0, 1280, 720, c);
	}
	if (view && view->alive && view->scoped)
		scope_overlay();
	else if (view && view->alive && view->build_piece < 0)
		crosshair(view);
	// embaixo, a esquerda: vida e colete
	char s[96];
	snprintf(s, sizeof s, "+ %d", shown->hp);
	ui_text(s, 36, 640, 44, WHITE, ALIGN_LEFT, true);
	snprintf(s, sizeof s, "%d", shown->armor);
	const float iw = ui_icon(shown->helmet ? "img_armor_helmet" : "img_armor", 190, 648, 34, WHITE, false);
	ui_text(s, 190 + iw + 10, 640, 44, WHITE, ALIGN_LEFT, true);
	// embaixo, a direita: arma e municao
	if (shown->alive) {
		const WeaponsDef *w = fighter_weapon(shown);
		const char *icon = shown->slot == 5 ? "img_c4" : w->icon_asset;
		const float ic[4] = { 1, 1, 1, 0.9f };
		ui_icon(icon, 1244, 580, 46, ic, true);
		const Gun *g = shown->slot >= 1 && shown->slot <= 4 && shown->has[shown->slot] ? &shown->guns[shown->slot] : NULL;
		if (g && WEAPONS[g->id].mag > 0) {
			snprintf(s, sizeof s, "%d / %d", g->mag, g->reserve);
			ui_text(s, 1244, 632, 40, WHITE, ALIGN_RIGHT, true);
		}
	}
	// em cima, a esquerda: dinheiro e bomba
	if (view) {
		const float money[4] = { 0.45f, 0.85f, 0.45f, 1 };
		snprintf(s, sizeof s, "$%d", view->money);
		ui_text(s, 36, 120, 30, money, ALIGN_LEFT, true);
		if (view->bomb) {
			const float c4c[4] = { 1, 0.75f, 0.3f, 1 };
			ui_icon("img_c4", 36, 170, 40, c4c, false);
		}
		if (match_buy_allowed(view) && !m->buy_open)
			ui_text("BUY: B / D-pad up", 36, 220, 16, DIM, ALIGN_LEFT, false);
	}
	// em cima, no meio: vivos, placar e relogio
	alive_row(TEAM_CT, 640 - 120, false);
	alive_row(TEAM_T, 640 + 120, true);
	ui_rect(640 - 120, 10, 64, 40, (const float[]){ CT_C[0] * 0.75f, CT_C[1] * 0.75f, CT_C[2] * 0.75f, 0.95f });
	ui_rect(640 + 56, 10, 64, 40, (const float[]){ T_C[0] * 0.75f, T_C[1] * 0.75f, T_C[2] * 0.75f, 0.95f });
	snprintf(s, sizeof s, "%d", m->score[0]);
	ui_text(s, 640 - 88, 12, 28, WHITE, ALIGN_CENTER, true);
	snprintf(s, sizeof s, "%d", m->score[1]);
	ui_text(s, 640 + 88, 12, 28, WHITE, ALIGN_CENTER, true);
	ui_rect(640 - 55, 10, 110, 40, SHADE);
	const float left = m->phase == PHASE_PLANTED ? (float)(m->bomb_explode_at - G.time) : (float)(m->phase_end - G.time);
	if (m->phase == PHASE_PLANTED) {
		const float red[4] = { 1, 0.3f, 0.25f, 1 };
		ui_icon("img_c4", 640, 14, 32, red, false);
		ui_icon("img_c4", 640, 14, 32, red, true);
	} else {
		fmt_time(s, sizeof s, fmaxf(0, left));
		ui_text(s, 640, 12, 28, m->phase == PHASE_FREEZE ? DIM : WHITE, ALIGN_CENTER, true);
	}
	// em cima, a direita: mortes
	for (int i = 0; i < MAX_FEED; i++) {
		const FeedLine *l = &m->feed[i];
		if (!l->victim[0] || G.time - l->born > 6.0f)
			continue;
		const float y = 24 + i * 36;
		const float wv = ui_text_width(l->victim, 20, true), wk = ui_text_width(l->killer, 20, true);
		const AtlasRect *r = assets_rect(&G.as, WEAPONS[l->weapon].icon_asset);
		const float iw = r ? 24.0f * r->w / r->h : 24;
		const float total = wk + iw + wv + 40 + (l->headshot ? 30 : 0);
		const float x0 = 1256 - total;
		ui_rect(x0 - 8, y - 2, total + 16, 30, SHADE);
		float x = x0;
		if (l->killer[0])
			x += ui_text(l->killer, x, y, 20, team_color(l->killer_team), ALIGN_LEFT, true) + 10;
		ui_icon(WEAPONS[l->weapon].icon_asset, x, y + 2, 24, WHITE, false);
		x += iw + 10;
		if (l->headshot) {
			ui_text("HS", x, y, 18, (const float[]){ 1, 0.8f, 0.3f, 1 }, ALIGN_LEFT, true);
			x += 30;
		}
		ui_text(l->victim, x, y, 20, team_color(l->victim_team), ALIGN_LEFT, true);
	}
	// avisos, faixa de fim de rodada
	if (G.time < m->notice_until)
		ui_text(m->notice, 640, 210, 26, WHITE, ALIGN_CENTER, true);
	if (G.time < banner_until) {
		panel(220, 120, 840, 90, SHADE);
		ui_text(banner_main, 640, 128, 40, WHITE, ALIGN_CENTER, true);
		ui_text(banner_sub, 640, 176, 20, DIM, ALIGN_CENTER, false);
	}
	// plantar, desarmar
	float prog = -1;
	const char *what = "";
	if (view && m->planter == view->index) {
		prog = m->plant_progress / MODES[m->mode].plant_time;
		what = "PLANTING THE BOMB";
	} else if (view && m->defuser == view->index) {
		prog = m->defuse_progress / (view->kit ? MODES[m->mode].defuse_kit_time : MODES[m->mode].defuse_time);
		what = view->kit ? "DEFUSING (KIT)" : "DEFUSING";
	}
	if (prog >= 0) {
		ui_text(what, 640, 450, 18, WHITE, ALIGN_CENTER, true);
		ui_rect(440, 476, 400, 8, (const float[]){ 0, 0, 0, 0.5f });
		ui_rect(440, 476, 400 * clampf(prog, 0, 1), 8, ACCENT);
	} else if (view && view->alive && m->phase == PHASE_PLANTED && view->team == TEAM_CT &&
		v3_length(v3_sub(view->body.position, m->bomb_pos)) < 1.8f) {
		ui_text("Hold X / E to defuse", 640, 450, 18, WHITE, ALIGN_CENTER, true);
	}
	if (view && view->alive && MODES[m->mode].building && m->phase != PHASE_MATCH_OVER)
		build_bar(view);
	if (dead && m->count) {
		snprintf(s, sizeof s, "SPECTATING  %s", m->f[m->spectate].name);
		ui_text(s, 640, 560, 22, WHITE, ALIGN_CENTER, true);
	}
	snprintf(s, sizeof s, "%.0f FPS", G.fps);
	ui_text(s, 36, 86, 16, (const float[]){ 1, 1, 1, 0.75f }, ALIGN_LEFT, true);
	if (G.act.board)
		scoreboard();
}

// ---------------------------------------------------------------- compra

typedef struct {
	const char *id;
	bool weapon;
} BuyItem;

static int buy_items(BuyItem *out)
{
	int n = 0;
	for (int i = 0; i < WEAPONS_COUNT; i++) {
		if (WEAPONS[i].price <= 0)
			continue;
		if (strcmp(WEAPONS[i].only_mode, "any") && strcmp(WEAPONS[i].only_mode, MODES[G.m.mode].id))
			continue;
		out[n++] = (BuyItem){ WEAPONS[i].id, true };
	}
	for (int i = 0; i < EQUIPMENT_COUNT; i++)
		out[n++] = (BuyItem){ EQUIPMENT[i].id, false };
	return n;
}

void buy_update(void)
{
	Match *m = &G.m;
	if (m->player < 0)
		return;
	Fighter *f = &m->f[m->player];
	const Actions *a = &G.act;
	if (a->buy_pressed && !G.paused && !m->tacmap_open) {
		if (m->buy_open)
			m->buy_open = false;
		else if (match_buy_allowed(f))
			m->buy_open = true;
		else
			match_notice("You can only buy in your spawn during buy time", 1.5f);
		plat_capture_mouse(!m->buy_open);
	}
	if (!m->buy_open)
		return;
	if (!match_buy_allowed(f) || a->back) {
		m->buy_open = false;
		plat_capture_mouse(true);
		return;
	}
	BuyItem items[32];
	const int n = buy_items(items);
	if (a->up)
		m->buy_sel = (m->buy_sel + n - 1) % n;
	if (a->down)
		m->buy_sel = (m->buy_sel + 1) % n;
	// mouse em cima de uma linha
	const float x0 = 360, y0 = 150, rh = 38;
	int hover = -1;
	if (a->cursor_x >= x0 && a->cursor_x <= x0 + 560 && a->cursor_y >= y0 && a->cursor_y < y0 + n * rh)
		hover = (int)((a->cursor_y - y0) / rh);
	if (hover >= 0 && (G.pad.mouse_x != G.prev_pad.mouse_x || G.pad.mouse_y != G.prev_pad.mouse_y))
		m->buy_sel = hover;
	if (a->accept || (a->click && hover >= 0)) {
		if (G.net_role == NET_ROLE_CLIENT) {
			netcl_buy(items[m->buy_sel].id); // o host compra; o dinheiro volta no estado
			sound_ui("snd_knife", -18);
		} else if (fighter_buy(f, items[m->buy_sel].id))
			sound_ui("snd_knife", -18);
		else
			match_notice("Can't buy that", 1.0f);
	}
	// desenho
	panel(x0 - 20, y0 - 70, 600, n * rh + 110, PANEL);
	ui_text("BUY MENU", x0, y0 - 60, 30, WHITE, ALIGN_LEFT, true);
	char money[32];
	snprintf(money, sizeof money, "$%d", f->money);
	ui_text(money, x0 + 560, y0 - 58, 26, (const float[]){ 0.45f, 0.85f, 0.45f, 1 }, ALIGN_RIGHT, true);
	for (int i = 0; i < n; i++) {
		const float y = y0 + i * rh;
		int price = 0;
		const char *name = "", *icon = "", *team = "any";
		if (items[i].weapon) {
			const WeaponsDef *w = &WEAPONS[weapon_index(items[i].id)];
			price = w->price;
			name = w->name;
			icon = w->icon_asset;
			team = w->team;
		} else {
			for (int e = 0; e < EQUIPMENT_COUNT; e++)
				if (!strcmp(EQUIPMENT[e].id, items[i].id)) {
					price = EQUIPMENT[e].price;
					name = EQUIPMENT[e].name;
					icon = EQUIPMENT[e].icon_asset;
					team = EQUIPMENT[e].team;
				}
		}
		const bool mine = !strcmp(team, "any") || !strcmp(team, TEAMS[f->team].id);
		const bool afford = f->money >= price && mine;
		if (i == m->buy_sel)
			ui_rect(x0 - 10, y, 580, rh - 4, (const float[]){ 1, 1, 1, 0.12f });
		const float c[4] = { 1, 1, 1, afford ? 1.0f : 0.35f };
		if (!strcmp(items[i].id, "portalgun")) {
			const float blue[4] = { 0.3f, 0.6f, 1, c[3] };
			ui_icon("swirl", x0, y + 3, 28, blue, false);
		} else {
			ui_icon(icon, x0, y + 5, 26, c, false);
		}
		ui_text(name, x0 + 130, y + 4, 22, c, ALIGN_LEFT, true);
		char p[16];
		snprintf(p, sizeof p, "$%d", price);
		ui_text(p, x0 + 560, y + 4, 22, afford ? (const float[]){ 0.45f, 0.85f, 0.45f, 1 } : DIM, ALIGN_RIGHT, true);
	}
	ui_text("A / click: buy    B / Esc: close", x0, y0 + n * rh + 6, 16, DIM, ALIGN_LEFT, false);
	// o ponteiro do mouse
	ui_rect(a->cursor_x - 2, a->cursor_y - 2, 4, 4, WHITE);
}

// ---------------------------------------------------------------- pausa e configuracoes

static void setting_line(const char *label, char *value, float y, bool sel)
{
	if (sel)
		ui_rect(420, y - 2, 440, 34, (const float[]){ 1, 1, 1, 0.12f });
	ui_text(label, 440, y, 22, sel ? WHITE : TEXT, ALIGN_LEFT, sel);
	ui_text(value, 840, y, 22, sel ? ACCENT : DIM, ALIGN_RIGHT, true);
}

// As configuracoes (no menu e na pausa): devolve true quando o item muda.
static bool settings_rows(int *sel, int first_y, bool show)
{
	Settings *st = &G.settings;
	const Actions *a = &G.act;
	const int rows = 6;
	const int d = (a->right ? 1 : 0) - (a->left ? 1 : 0);
	bool changed = false;
	if (d) {
		switch (*sel) {
		case 0:
			st->sensitivity = clampf(st->sensitivity + d * 0.1f, 0.3f, 5.0f);
			break;
		case 1:
			st->fov = clampf(st->fov + d * 5, 60, 110);
			break;
		case 2:
			st->volume = clampf(st->volume + d * 0.05f, 0, 1);
			break;
		case 3:
			st->music = clampf(st->music + d * 0.05f, 0, 1);
			break;
		case 4:
			st->invert_y = !st->invert_y;
			break;
		case 5:
			st->vm_x = clampf(st->vm_x + d * 0.01f, -0.1f, 0.2f);
			break;
		}
		audio_volumes(st->volume, st->music);
		changed = true;
	}
	if (show) {
		char v[32];
		snprintf(v, sizeof v, "%.1f", st->sensitivity);
		setting_line("Sensitivity", v, first_y, *sel == 0);
		snprintf(v, sizeof v, "%.0f", st->fov);
		setting_line("Field of view", v, first_y + 38, *sel == 1);
		snprintf(v, sizeof v, "%.0f%%", st->volume * 100);
		setting_line("Volume", v, first_y + 76, *sel == 2);
		snprintf(v, sizeof v, "%.0f%%", st->music * 100);
		setting_line("Music", v, first_y + 114, *sel == 3);
		setting_line("Invert look", st->invert_y ? (char[]){ "ON" } : (char[]){ "OFF" }, first_y + 152, *sel == 4);
		snprintf(v, sizeof v, "%+.2f", st->vm_x);
		setting_line("Viewmodel right/left", v, first_y + 190, *sel == 5);
	}
	(void)rows;
	return changed;
}

void pause_update(void)
{
	const Actions *a = &G.act;
	if (a->pause_pressed && !G.m.buy_open && !G.m.tacmap_open) {
		G.paused = !G.paused;
		G.pause_sel = 0;
		plat_capture_mouse(!G.paused);
		return;
	}
	if (!G.paused)
		return;
	if (a->back) {
		G.paused = false;
		plat_capture_mouse(true);
		return;
	}
	// 0 continuar, 1-6 configuracoes, 7 sair
	const int n = 8;
	if (a->up)
		G.pause_sel = (G.pause_sel + n - 1) % n;
	if (a->down)
		G.pause_sel = (G.pause_sel + 1) % n;
	ui_rect(0, 0, 1280, 720, (const float[]){ 0, 0, 0, 0.55f });
	panel(400, 120, 480, 470, PANEL);
	ui_text("PAUSED", 640, 132, 34, WHITE, ALIGN_CENTER, true);
	if (G.pause_sel == 0)
		ui_rect(420, 188, 440, 34, (const float[]){ 1, 1, 1, 0.12f });
	ui_text("Resume", 640, 190, 24, G.pause_sel == 0 ? WHITE : TEXT, ALIGN_CENTER, true);
	int s = G.pause_sel - 1;
	if (G.pause_sel >= 1 && G.pause_sel <= 6)
		settings_rows(&s, 240, true);
	else {
		int none = -1;
		settings_rows(&none, 240, true);
	}
	if (G.pause_sel == 7)
		ui_rect(420, 512, 440, 34, (const float[]){ 1, 0.3f, 0.25f, 0.25f });
	ui_text("Quit to menu", 640, 514, 24, G.pause_sel == 7 ? WHITE : TEXT, ALIGN_CENTER, true);
	if (a->accept) {
		if (G.pause_sel == 0) {
			G.paused = false;
			plat_capture_mouse(true);
		} else if (G.pause_sel == 7) {
			G.paused = false;
			if (G.net_role == NET_ROLE_HOST)
				net_room_state(MODES[G.m.mode].id, false);
			else if (G.net_role == NET_ROLE_CLIENT)
				net_room_leave();
			match_end();
			menu_enter();
		}
	}
}

// ---------------------------------------------------------------- mapa tatico

static Vec3 tm_routes[MAX_FIGHTERS][MAX_PATH + 2];
static int tm_lens[MAX_FIGHTERS], tm_route_count;
static Vec3 tm_recs[3];
static float tm_rec_yaw[3];
static const char *tm_rec_label[3];
static int tm_rec_count;
static float tm_facing;
static bool tm_turned;
static float tm_scale, tm_cx, tm_cz, tm_side, tm_ox, tm_oy;

// O plano do mapa: (z, -x), como o tacmap.gd.
static void to_screen(Vec3 p, float *sx, float *sy)
{
	*sx = tm_ox + tm_side / 2 + (p.z - tm_cx) * tm_scale;
	*sy = tm_oy + tm_side / 2 + (-p.x - tm_cz) * tm_scale;
}

void tacmap_open(void)
{
	Match *m = &G.m;
	if (m->tacmap_open || m->player < 0)
		return;
	const Fighter *f = &m->f[m->player];
	m->tacmap_open = true;
	tacmap_routes(f->team, tm_routes, tm_lens, &tm_route_count);
	tm_rec_count = tacmap_recommend(f->team, tm_recs, tm_rec_yaw, tm_rec_label, 3);
	tm_facing = f->body.yaw;
	tm_turned = false;
	// enquadra os poligonos alcancaveis
	const Nav *nv = &G.nav;
	float lo_x = 1e30f, hi_x = -1e30f, lo_y = 1e30f, hi_y = -1e30f;
	for (uint32_t i = 0; i < nv->nav->poly_count; i++) {
		if (!nv->reachable[i])
			continue;
		const Vec3 c = nv->center[i];
		lo_x = fminf(lo_x, c.z);
		hi_x = fmaxf(hi_x, c.z);
		lo_y = fminf(lo_y, -c.x);
		hi_y = fmaxf(hi_y, -c.x);
	}
	tm_side = 720 * 0.86f;
	tm_ox = (1280 - tm_side) / 2;
	tm_oy = (720 - tm_side) / 2 + 20;
	tm_scale = tm_side / fmaxf(hi_x - lo_x, hi_y - lo_y) * 0.95f;
	tm_cx = (lo_x + hi_x) / 2;
	tm_cz = (lo_y + hi_y) / 2;
	plat_capture_mouse(false);
}

static Vec3 ground_at(float sx, float sy, bool *ok)
{
	const float pz = (sx - tm_ox - tm_side / 2) / tm_scale + tm_cx;
	const float nx = (sy - tm_oy - tm_side / 2) / tm_scale + tm_cz;
	const Vec3 q = v3(-nx, 0, pz);
	const Nav *nv = &G.nav;
	float best_y = -1e30f;
	*ok = false;
	for (uint32_t i = 0; i < nv->nav->poly_count; i++) {
		if (!nv->reachable[i])
			continue;
		const float *b = nv->box + i * 6;
		if (q.x < b[0] || q.x > b[3] || q.z < b[2] || q.z > b[5])
			continue;
		// dentro do poligono no plano
		const NavData *n = nv->nav;
		bool in = false;
		const uint32_t s = n->poly_start[i], e = n->poly_start[i + 1];
		for (uint32_t a = s, c = e - 1; a < e; c = a++) {
			const float *va = n->verts + n->poly_index[a] * 3, *vc = n->verts + n->poly_index[c] * 3;
			if ((va[2] > q.z) != (vc[2] > q.z) && q.x < (vc[0] - va[0]) * (q.z - va[2]) / (vc[2] - va[2]) + va[0])
				in = !in;
		}
		if (in && nv->center[i].y > best_y) {
			best_y = nv->center[i].y;
			*ok = true;
		}
	}
	return *ok ? nav_closest(v3(q.x, best_y + 0.3f, q.z)) : q;
}

void tacmap_update(void)
{
	Match *m = &G.m;
	const Actions *a = &G.act;
	if (!m->tacmap_open || m->player < 0)
		return;
	Fighter *f = &m->f[m->player];
	if (a->right_click || a->pause_pressed) {
		m->tacmap_open = false;
		plat_capture_mouse(true);
		return;
	}
	if (a->wheel != 0) {
		tm_facing += a->wheel * 15.0f * PI_F / 180.0f;
		tm_turned = true;
	}
	if (a->click) {
		for (int i = 0; i < tm_rec_count; i++) {
			float sx, sy;
			to_screen(tm_recs[i], &sx, &sy);
			if (fabsf(sx - a->cursor_x) < 22 && fabsf(sy - a->cursor_y) < 22) {
				portal_place_orange(f, tm_recs[i], tm_turned ? tm_facing : tm_rec_yaw[i]);
				return;
			}
		}
		bool ok;
		const Vec3 g = ground_at(a->cursor_x, a->cursor_y, &ok);
		if (!ok) {
			match_notice("Pick a walkable spot on the map", 1.2f);
			return;
		}
		// sai olhando para o ponto de rota inimiga mais perto
		float yaw = f->body.yaw, best = 1e30f;
		for (int r = 0; r < tm_route_count; r++)
			for (int k = 0; k < tm_lens[r]; k++) {
				const float d = v3_length(v3_sub(tm_routes[r][k], g));
				if (d < best && d > 0.5f) {
					best = d;
					const Vec3 dd = v3_sub(tm_routes[r][k], g);
					yaw = atan2f(-dd.x, -dd.z);
				}
			}
		portal_place_orange(f, g, tm_turned ? tm_facing : yaw);
	}
}

void tacmap_draw(void)
{
	Match *m = &G.m;
	if (!m->tacmap_open)
		return;
	const Actions *a = &G.act;
	ui_rect(0, 0, 1280, 720, (const float[]){ 0.02f, 0.03f, 0.04f, 0.82f });
	const Nav *nv = &G.nav;
	const NavData *n = nv->nav;
	float lo = 1e30f, hi = -1e30f;
	for (uint32_t i = 0; i < n->poly_count; i++)
		if (nv->reachable[i]) {
			lo = fminf(lo, nv->center[i].y);
			hi = fmaxf(hi, nv->center[i].y);
		}
	for (uint32_t i = 0; i < n->poly_count; i++) {
		if (!nv->reachable[i])
			continue;
		const float k = (nv->center[i].y - lo) / fmaxf(hi - lo, 1.0f);
		const float c[4] = { 0.22f + 0.3f * k, 0.25f + 0.3f * k, 0.28f + 0.3f * k, 1 };
		const uint32_t s = n->poly_start[i], e = n->poly_start[i + 1];
		float x0, y0;
		const float *v0 = n->verts + n->poly_index[s] * 3;
		to_screen(v3(v0[0], v0[1], v0[2]), &x0, &y0);
		for (uint32_t k2 = s + 1; k2 + 1 < e; k2++) {
			const float *v1 = n->verts + n->poly_index[k2] * 3, *v2 = n->verts + n->poly_index[k2 + 1] * 3;
			float x1, y1, x2, y2;
			to_screen(v3(v1[0], v1[1], v1[2]), &x1, &y1);
			to_screen(v3(v2[0], v2[1], v2[2]), &x2, &y2);
			tri2d(x0, y0, x1, y1, x2, y2, c);
		}
	}
	// bombsites
	for (int s = 0; s < 2; s++) {
		const float *b = G.as.sites[s];
		float x0, y0, x1, y1;
		to_screen(v3(b[0], 0, b[2]), &x0, &y0);
		to_screen(v3(b[3], 0, b[5]), &x1, &y1);
		const float c[4] = { 1, 0.85f, 0.3f, 0.25f };
		ui_rect(fminf(x0, x1), fminf(y0, y1), fabsf(x1 - x0), fabsf(y1 - y0), c);
		ui_text(s ? "B" : "A", (x0 + x1) / 2, (y0 + y1) / 2 - 14, 26, (const float[]){ 1, 0.85f, 0.3f, 1 }, ALIGN_CENTER, true);
	}
	// rotas inimigas previstas
	const float enemy[4] = { 1.0f, 0.3f, 0.25f, 0.9f };
	for (int r = 0; r < tm_route_count; r++) {
		for (int k = 1; k < tm_lens[r]; k++) {
			float x0, y0, x1, y1;
			to_screen(tm_routes[r][k - 1], &x0, &y0);
			to_screen(tm_routes[r][k], &x1, &y1);
			line2d(x0, y0, x1, y1, 3, enemy);
		}
		float x, y;
		to_screen(tm_routes[r][0], &x, &y);
		ui_rect(x - 5, y - 5, 10, 10, enemy);
	}
	// voce
	const Fighter *f = &m->f[m->player];
	float px, py;
	to_screen(f->body.position, &px, &py);
	ui_rect(px - 6, py - 6, 12, 12, ACCENT);
	// recomendacoes
	const float rec[4] = { 1.0f, 0.82f, 0.25f, 1 };
	for (int i = 0; i < tm_rec_count; i++) {
		float x, y;
		to_screen(tm_recs[i], &x, &y);
		for (int k = 0; k < 5; k++) {
			const float a0 = -PI_F / 2 + k * 2 * PI_F / 5, a1 = a0 + PI_F / 5, a2 = a0 + 2 * PI_F / 5;
			tri2d(x, y, x + cosf(a0) * 14, y + sinf(a0) * 14, x + cosf(a1) * 6, y + sinf(a1) * 6, rec);
			tri2d(x, y, x + cosf(a1) * 6, y + sinf(a1) * 6, x + cosf(a2) * 14, y + sinf(a2) * 14, rec);
		}
		ui_text(tm_rec_label[i], x, y + 14, 16, rec, ALIGN_CENTER, true);
	}
	ui_text("PORTAL MAP: click a star or any walkable spot for the orange portal", 640, 14, 20, WHITE, ALIGN_CENTER, true);
	ui_text("LB/RB or wheel: turn the exit    B / right mouse: close", 640, 40, 16, DIM, ALIGN_CENTER, false);
	// o ponteiro e a direcao de saida
	const float cx = a->cursor_x, cy = a->cursor_y;
	ui_rect(cx - 8, cy - 1, 16, 2, WHITE);
	ui_rect(cx - 1, cy - 8, 2, 16, WHITE);
	if (tm_turned) {
		const Vec3 d = v3(-sinf(tm_facing), 0, -cosf(tm_facing));
		line2d(cx, cy, cx + d.z * 26, cy - d.x * 26, 3, (const float[]){ 1, 0.5f, 0.08f, 1 });
	}
}


// ---------------------------------------------------------------- online (EPTX)

static const char *net_state_text(const NetView *v)
{
	return v->message[0] ? v->message : "EPTX";
}

static bool friend_online(const NetFriend *f)
{
	return !strcmp(f->state, "online") || !strcmp(f->state, "away");
}

// As linhas da pagina ONLINE, na ordem: acoes da sala, convites, amigos.
typedef struct {
	int kind; // 0 criar/modo, 1 comecar, 2 sair, 3 convite, 4 amigo
	int index;
} OnlineRow;

static int online_rows(const NetView *v, OnlineRow *rows)
{
	int n = 0;
	if (!v->in_room) {
		rows[n++] = (OnlineRow){ 0, 0 };
	} else {
		rows[n++] = (OnlineRow){ 0, 0 };
		rows[n++] = (OnlineRow){ 1, 0 };
		rows[n++] = (OnlineRow){ 2, 0 };
	}
	for (int i = 0; i < v->invite_count; i++)
		rows[n++] = (OnlineRow){ 3, i };
	for (int i = 0; i < v->friend_count && n < 48; i++)
		rows[n++] = (OnlineRow){ 4, i };
	return n;
}

static void start_hosted_match(const NetView *v)
{
	(void)v;
	Settings *st = &G.settings;
	net_room_state(MODES[st->mode].id, true);
	audio_stop_kind(AUDIO_MUSIC);
	G.state = STATE_MATCH;
	match_start(st->mode, st->side, st->difficulty, false);
	netgame_host_begin();
}

static void online_update(void)
{
	const Actions *a = &G.act;
	NetView v;
	net_view(&v);
	if (v.status == NET_NEED_LOGIN) {
		// formulario de login (PC): gamertag e senha
		char *field = G.login_field == 0 ? G.login_user : G.login_pass;
		const int cap = G.login_field == 0 ? (int)sizeof G.login_user : (int)sizeof G.login_pass;
		int len = (int)strlen(field);
		for (int i = 0; i < G.pad.text_len && len < cap - 1; i++)
			field[len++] = G.pad.text[i];
		field[len] = 0;
		if (G.pad.backspace && len > 0)
			field[len - 1] = 0;
		if (a->up || a->down || (G.pad.keyboard && G.pad.keys[0x09] && !G.prev_pad.keys[0x09]))
			G.login_field = 1 - G.login_field;
		if (G.pad.keyboard && G.pad.keys[0x0D] && !G.prev_pad.keys[0x0D]) {
			if (G.login_field == 0)
				G.login_field = 1;
			else if (G.login_user[0] && G.login_pass[0]) {
				net_login(G.login_user, G.login_pass);
				memset(G.login_pass, 0, sizeof G.login_pass);
			}
		}
		if (G.pad.keyboard && G.pad.keys[0x1B] && !G.prev_pad.keys[0x1B]) {
			G.menu_page = 0;
			G.menu_sel = 1;
		}
		return;
	}
	if (a->back) {
		G.menu_page = 0;
		G.menu_sel = 1;
		return;
	}
	if (v.status != NET_ONLINE)
		return;
	OnlineRow rows[64];
	const int n = online_rows(&v, rows);
	if (a->up)
		G.menu_sel = (G.menu_sel + n - 1) % n;
	if (a->down)
		G.menu_sel = (G.menu_sel + 1) % n;
	if (G.menu_sel >= n)
		G.menu_sel = n - 1;
	const OnlineRow *r = &rows[G.menu_sel];
	const bool host = v.in_room && v.room.host_id == v.user_id;
	const int d = (a->right ? 1 : 0) - (a->left ? 1 : 0);
	if (r->kind == 0 && d && (!v.in_room || host)) {
		G.settings.mode = (G.settings.mode + MODES_COUNT + d) % MODES_COUNT;
		if (v.in_room)
			net_room_state(MODES[G.settings.mode].id, false);
	}
	if (!a->accept)
		return;
	switch (r->kind) {
	case 0:
		if (!v.in_room)
			net_room_create(MODES[G.settings.mode].id);
		break;
	case 1:
		if (host)
			start_hosted_match(&v);
		break;
	case 2:
		net_room_leave();
		break;
	case 3:
		net_room_join(v.invites[r->index].room_id);
		break;
	case 4:
		if (host && friend_online(&v.friends[r->index]))
			net_room_invite(v.friends[r->index].user_id);
		break;
	}
}

static void online_draw(void)
{
	NetView v;
	net_view(&v);
	ui_text("ONLINE", 80, 190, 34, WHITE, ALIGN_LEFT, true);
	const float st_c[4] = { v.status == NET_ONLINE ? 0.45f : 0.9f, v.status == NET_ONLINE ? 0.85f : 0.75f,
		v.status == NET_ONLINE ? 0.45f : 0.3f, 1 };
	ui_text(net_state_text(&v), 80, 232, 18, st_c, ALIGN_LEFT, true);
	char line[160];
	if (v.server[0]) {
		if (v.udp_ms >= 0)
			snprintf(line, sizeof line, "Servidor %s   UDP %.0f ms (%d/%d)", v.server, v.udp_ms, v.udp_replies, v.udp_sent);
		else
			snprintf(line, sizeof line, "Servidor %s   UDP sem resposta (%d enviados)", v.server, v.udp_sent);
		ui_text(line, 80, 256, 15, DIM, ALIGN_LEFT, false);
	}
	if (v.status == NET_NEED_LOGIN) {
		const char *labels[2] = { "Gamertag", "Senha" };
		for (int i = 0; i < 2; i++) {
			const float y = 300 + i * 70;
			const bool sel = G.login_field == i;
			ui_text(labels[i], 80, y, 18, DIM, ALIGN_LEFT, false);
			ui_rect(76, y + 24, 380, 34, (const float[]){ 1, 1, 1, sel ? 0.16f : 0.07f });
			char shown[80];
			if (i == 0)
				snprintf(shown, sizeof shown, "%s%s", G.login_user, sel ? "_" : "");
			else {
				const int len = (int)strlen(G.login_pass);
				memset(shown, '*', (size_t)len);
				shown[len] = 0;
				if (sel)
					strcat(shown, "_");
			}
			ui_text(shown, 86, y + 28, 22, WHITE, ALIGN_LEFT, true);
		}
		ui_text("Enter: entrar    Tab: trocar de campo    Esc: voltar", 80, 460, 15, DIM, ALIGN_LEFT, false);
		ui_text("A conta e a mesma do EPTX (a do console e do celular).", 80, 484, 15, DIM, ALIGN_LEFT, false);
		return;
	}
	if (v.status != NET_ONLINE) {
		ui_text("Sem EPTX: a partida contra bots continua funcionando.", 80, 300, 16, DIM, ALIGN_LEFT, false);
		ui_text("B: voltar", 80, 640, 15, DIM, ALIGN_LEFT, false);
		return;
	}
	OnlineRow rows[64];
	const int n = online_rows(&v, rows);
	const bool host = v.in_room && v.room.host_id == v.user_id;
	float y = 292;
	for (int i = 0; i < n && y < 620; i++) {
		const OnlineRow *r = &rows[i];
		const bool sel = G.menu_sel == i;
		if (sel)
			ui_rect(60, y - 3, 400, 30, (const float[]){ 1, 1, 1, 0.14f });
		const float *c = sel ? WHITE : TEXT;
		switch (r->kind) {
		case 0:
			if (!v.in_room)
				snprintf(line, sizeof line, "CRIAR SALA  <  %s  >", MODES[G.settings.mode].name);
			else
				snprintf(line, sizeof line, "SALA: %s%s", v.room.mode, host ? "  (< > troca)" : "");
			ui_text(line, 80, y, 19, c, ALIGN_LEFT, true);
			break;
		case 1:
			ui_text(host ? "COMECAR (voces contra os bots)" : "Esperando o host comecar...", 80, y, 19,
				host ? GO : DIM, ALIGN_LEFT, true);
			break;
		case 2:
			ui_text("SAIR DA SALA", 80, y, 19, c, ALIGN_LEFT, true);
			break;
		case 3:
			snprintf(line, sizeof line, "CONVITE de %s (%s) - A: entrar", v.invites[r->index].host, v.invites[r->index].mode);
			ui_text(line, 80, y, 18, (const float[]){ 1, 0.82f, 0.25f, 1 }, ALIGN_LEFT, true);
			break;
		case 4: {
			const NetFriend *f = &v.friends[r->index];
			const bool on = friend_online(f);
			const float dot[4] = { on ? 0.3f : 0.4f, on ? 0.85f : 0.4f, on ? 0.35f : 0.4f, 1 };
			ui_rect(80, y + 8, 10, 10, dot);
			ui_text(f->gamertag, 100, y, 18, on ? c : DIM, ALIGN_LEFT, true);
			if (on) {
				snprintf(line, sizeof line, "%s%s%s", f->title[0] ? f->title : "Online", f->detail[0] ? " - " : "", f->detail);
				ui_text(line, 250, y + 2, 14, DIM, ALIGN_LEFT, false);
			}
			break;
		}
		}
		y += 32;
	}
	if (v.friend_count == 0)
		ui_text("Nenhum amigo no EPTX ainda (adicione pelo celular ou pela dash).", 80, y + 6, 14, DIM, ALIGN_LEFT, false);
	// a sala, na direita do painel
	if (v.in_room) {
		ui_rect(500, 190, 300, 30 + 30 * v.room.member_count, SHADE);
		ui_text("NA SALA", 512, 194, 18, WHITE, ALIGN_LEFT, true);
		for (int i = 0; i < v.room.member_count; i++) {
			const NetMember *m = &v.room.members[i];
			const bool same_pack = !m->pack[0] || !strcmp(m->pack, G.pack_hash);
			snprintf(line, sizeof line, "%s%s%s%s", m->gamertag, m->user_id == v.room.host_id ? " (host)" : "",
				m->udp ? "" : " - sem UDP", same_pack ? "" : " - PACOTE DIFERENTE");
			ui_text(line, 512, 222 + i * 30, 16, same_pack ? TEXT : (const float[]){ 1, 0.4f, 0.3f, 1 }, ALIGN_LEFT, false);
		}
	}
	ui_text(host ? "A: convidar o amigo selecionado / comecar    B: voltar" : "A: criar sala / aceitar convite    B: voltar",
		80, 640, 15, DIM, ALIGN_LEFT, false);
}

// ---------------------------------------------------------------- menu principal

void menu_enter(void)
{
	G.state = STATE_MENU;
	G.menu_page = 0;
	G.menu_sel = 0;
	plat_capture_mouse(false);
	audio_stop_kind(AUDIO_MUSIC);
	audio_play(assets_sound(&G.as, "mus_menu"), AUDIO_MUSIC, v3(0, 0, 0), 0.6f, 1, 1, true);
	const Model *m = assets_model(&G.as, G.settings.side == 1 ? "agent_t" : "agent_ct");
	if (G.menu_agent.model != m) {
		skin_free(&G.menu_agent);
		skin_init(&G.menu_agent, m);
	}
	anim_play(&G.menu_anim, assets_anim(&G.as, G.settings.side == 1 ? "agent_t.idle" : "agent_ct.idle"), 0);
}

static void menu_button(const char *text, float y, bool sel)
{
	if (sel)
		ui_rect(60, y - 4, 360, 46, (const float[]){ 1, 1, 1, 0.14f });
	ui_text(text, 80, y, 30, sel ? WHITE : TEXT, ALIGN_LEFT, true);
}

void menu_update(float dt)
{
	anim_update(&G.menu_anim, dt);
	const Actions *a = &G.act;
	Settings *st = &G.settings;
	static const char *sides[3] = { "Counter-Terrorists", "Terrorists", "Random" };
	if (G.menu_page == 0) {
		const int n = 4;
		if (a->up)
			G.menu_sel = (G.menu_sel + n - 1) % n;
		if (a->down)
			G.menu_sel = (G.menu_sel + 1) % n;
		if (a->accept) {
			if (G.menu_sel == 0) {
				G.menu_page = 1;
				G.menu_sel = 0;
			} else if (G.menu_sel == 1) {
				G.menu_page = 3;
				G.menu_sel = 0;
			} else if (G.menu_sel == 2) {
				G.menu_page = 2;
				G.menu_sel = 0;
			} else {
				G.want_quit = true;
			}
		}
	} else if (G.menu_page == 3) {
		online_update();
	} else if (G.menu_page == 1) {
		const int n = 4;
		if (a->up)
			G.menu_sel = (G.menu_sel + n - 1) % n;
		if (a->down)
			G.menu_sel = (G.menu_sel + 1) % n;
		const int d = (a->right ? 1 : 0) - (a->left ? 1 : 0);
		if (d) {
			if (G.menu_sel == 0)
				st->mode = (st->mode + MODES_COUNT + d) % MODES_COUNT;
			else if (G.menu_sel == 1) {
				st->side = (st->side + 3 + d) % 3;
				menu_enter();
				G.menu_page = 1;
				G.menu_sel = 1;
			} else if (G.menu_sel == 2)
				st->difficulty = (st->difficulty + BOT_SKILLS_COUNT + d) % BOT_SKILLS_COUNT;
		}
		if (a->back) {
			G.menu_page = 0;
			G.menu_sel = 0;
		} else if (a->accept && G.menu_sel == 3) {
			audio_stop_kind(AUDIO_MUSIC);
			G.state = STATE_MATCH;
			match_start(st->mode, st->side, st->difficulty, false);
			return;
		}
	} else {
		const int n = 6;
		if (a->up)
			G.menu_sel = (G.menu_sel + n - 1) % n;
		if (a->down)
			G.menu_sel = (G.menu_sel + 1) % n;
		settings_rows(&G.menu_sel, 0, false);
		if (a->back || a->accept) {
			G.menu_page = 0;
			G.menu_sel = 2;
		}
	}
	(void)sides;
}

void menu_draw(void)
{
	// o agente em pe no Dust II, com a arma
	if (G.menu_agent.model) {
		const BsSpawn *sp = &G.map.spawns[G.settings.side == 1 ? G.map.spawn_count - 1 : 0];
		const Vec3 fwd = v3(-sinf(sp->yaw), 0, -cosf(sp->yaw));
		const Vec3 at = v3_add(sp->position, v3_scale(fwd, 3.2f));
		const Xf body = xf_yaw(sp->yaw + PI_F, at);
		const Xf turn = xf_yaw(PI_F, v3(0, 0, 0));
		const Xf world = xf_mul(&body, &turn);
		skin_pose(&G.menu_agent, &G.menu_anim);
		const float tint[4] = { 1, 1, 1, 1 };
		skin_draw(&G.menu_agent, &world, tint);
		const Xf grip = xf_translate(v3(-0.1f, 1.29f, 0.02f));
		const Xf held = xf_mul(&world, &grip);
		rigid_draw(G.settings.side == 1 ? "w_ak47" : "w_m4a1s", &held, tint);
	}
}

void menu_draw_ui(void)
{
	const Settings *st = &G.settings;
	static const char *sides[3] = { "Counter-Terrorists", "Terrorists", "Random" };
	// a faixa escura da esquerda, como o menu do CS2
	ui_rect(0, 0, 480, 720, (const float[]){ 0.04f, 0.05f, 0.07f, 0.78f });
	ui_text("BOT STRIKE", 70, 60, 64, WHITE, ALIGN_LEFT, true);
	ui_text("DUST II  -  XBOX 360", 74, 132, 20, DIM, ALIGN_LEFT, true);
	if (G.menu_page == 0) {
		menu_button("PLAY", 240, G.menu_sel == 0);
		menu_button("ONLINE", 300, G.menu_sel == 1);
		menu_button("SETTINGS", 360, G.menu_sel == 2);
		menu_button("QUIT", 420, G.menu_sel == 3);
		NetView v;
		net_view(&v);
		const float c[4] = { v.status == NET_ONLINE ? 0.45f : 0.62f, v.status == NET_ONLINE ? 0.85f : 0.65f,
			v.status == NET_ONLINE ? 0.45f : 0.7f, 1 };
		ui_text(net_state_text(&v), 80, 500, 16, c, ALIGN_LEFT, true);
		int online = 0;
		for (int i = 0; i < v.friend_count; i++)
			online += friend_online(&v.friends[i]);
		if (v.status == NET_ONLINE) {
			char t[64];
			snprintf(t, sizeof t, "%d amigo%s online%s", online, online == 1 ? "" : "s", v.invite_count ? "  -  CONVITE!" : "");
			ui_text(t, 80, 524, 15, DIM, ALIGN_LEFT, false);
		}
	} else if (G.menu_page == 3) {
		online_draw();
	} else if (G.menu_page == 1) {
		ui_text("PLAY", 80, 200, 34, WHITE, ALIGN_LEFT, true);
		const char *labels[3] = { "Mode", "Side", "Bots" };
		const char *values[3] = { MODES[st->mode].name, sides[st->side], BOT_SKILLS[st->difficulty].name };
		for (int i = 0; i < 3; i++) {
			const float y = 260 + i * 70;
			const bool sel = G.menu_sel == i;
			if (sel)
				ui_rect(60, y - 4, 400, 62, (const float[]){ 1, 1, 1, 0.14f });
			ui_text(labels[i], 80, y, 18, DIM, ALIGN_LEFT, false);
			char v[96];
			snprintf(v, sizeof v, "<  %s  >", values[i]);
			ui_text(v, 80, y + 22, 24, sel ? WHITE : TEXT, ALIGN_LEFT, true);
		}
		ui_text(MODES[st->mode].tagline, 80, 480, 15, DIM, ALIGN_LEFT, false);
		const bool go = G.menu_sel == 3;
		ui_rect(60, 540, 400, 60, go ? GO : (const float[]){ GO[0] * 0.6f, GO[1] * 0.6f, GO[2] * 0.6f, 1 });
		ui_text("START", 260, 550, 34, WHITE, ALIGN_CENTER, true);
		ui_text("A: start    B: back    left/right: change", 80, 640, 15, DIM, ALIGN_LEFT, false);
	} else {
		ui_text("SETTINGS", 80, 200, 34, WHITE, ALIGN_LEFT, true);
		int sel = G.menu_sel;
		// a mesma lista da pausa, deslocada para a faixa
		const float save_x = 0;
		(void)save_x;
		char v[32];
		const char *names[6] = { "Sensitivity", "Field of view", "Volume", "Music", "Invert look", "Viewmodel right/left" };
		for (int i = 0; i < 6; i++) {
			switch (i) {
			case 0:
				snprintf(v, sizeof v, "%.1f", st->sensitivity);
				break;
			case 1:
				snprintf(v, sizeof v, "%.0f", st->fov);
				break;
			case 2:
				snprintf(v, sizeof v, "%.0f%%", st->volume * 100);
				break;
			case 3:
				snprintf(v, sizeof v, "%.0f%%", st->music * 100);
				break;
			case 4:
				snprintf(v, sizeof v, "%s", st->invert_y ? "ON" : "OFF");
				break;
			default:
				snprintf(v, sizeof v, "%+.2f", st->vm_x);
				break;
			}
			const float y = 260 + i * 46;
			if (sel == i)
				ui_rect(60, y - 4, 400, 40, (const float[]){ 1, 1, 1, 0.14f });
			ui_text(names[i], 80, y, 20, sel == i ? WHITE : TEXT, ALIGN_LEFT, sel == i);
			ui_text(v, 440, y, 20, sel == i ? ACCENT : DIM, ALIGN_RIGHT, true);
		}
		ui_text("left/right: change    A/B: back", 80, 640, 15, DIM, ALIGN_LEFT, false);
	}
	ui_text("Counter-Strike 2 and Dust II are (c) Valve. Fan-made, not affiliated with Valve.", 1260, 694, 13, DIM,
		ALIGN_RIGHT, false);
}
