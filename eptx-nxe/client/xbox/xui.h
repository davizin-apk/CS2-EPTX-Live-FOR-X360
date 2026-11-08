// XUI no xsdk: le as cenas .xui do Freestyle Dash (o XML que o XUI Tool do XDK grava)
// e as desenha com o Dear ImGui, sem o runtime XUI da Microsoft, que so vem com o XDK.
//
// Cobre o que as cenas e a skin do FSD usam: XuiScene, XuiGroup, XuiImage, XuiText,
// XuiFigure (cor solida e degrade linear ou radial), XuiNineGrid, os presenters e os
// controles com Visual da skin.xui (Btn_Button_A, ContentItem_Active, ...), com as
// timelines (quadros nomeados, comandos stop/goto/gotoandplay e interpolacao linear,
// em degrau e suavizada) e a profundidade (Position z) em perspectiva.
//
// Fica de fora o que o ImGui nao faz: mistura aditiva, mascara por canal de cor
// (ColorWriteFlags so de alfa: o elemento e pulado), som, HTML e as listas do XUI.
#ifndef EPTX_XUI_H
#define EPTX_XUI_H

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace xui {

struct Vec3 {
	float x = 0, y = 0, z = 0;
};
struct Quat {
	float x = 0, y = 0, z = 0, w = 1;
};

struct Texture {
	ImTextureID id = ImTextureID_Invalid;
	float w = 0, h = 0;
};

// O que o engine pede a quem o usa. path vem como no .xui, com '\' e relativo a skin.
struct Host {
	bool (*texture)(const char *path, Texture *out) = nullptr;
	ImFont *(*font)(const char *name) = nullptr;
	// Um XuiSoundXAudio tocou: a timeline de um estado chegou num quadro-chave com File
	// ("Audio\btn_Focus.xma", "sharedres://btn_Focus.xma"...).
	void (*sound)(const char *file) = nullptr;
	// A camera: ponto de fuga e distancia focal, em pixels de 1280x720.
	float vanish_x = 0, vanish_y = 0, focal = 1100;
	// Pixels por ponto de fonte do XUI.
	float font_scale = 1.0f;
	// Tela real dividida por 1280x720.
	float sx = 1, sy = 1;
};
extern Host g_host;

enum Kind { K_OTHER, K_SCENE, K_GROUP, K_IMAGE, K_TEXT, K_TEXT_PRESENTER, K_IMAGE_PRESENTER, K_FIGURE, K_NINEGRID,
	K_CONTROL, K_VISUAL, K_SOUND };

struct Stop {
	float pos;
	uint32_t color;
};

// As propriedades que mudam com a timeline, ja convertidas.
struct State {
	// O XUI Tool so grava o que difere do padrao: sem Width/Height, o elemento tem 60x30.
	float w = 60, h = 30;
	Vec3 pos, scale{ 1, 1, 1 }, pivot;
	Quat rot;
	float opacity = 1;
	bool show = true;
	std::string image_path, text;
	uint32_t text_color = 0xffffffff, shadow_color = 0;
	float point_size = 14;
	uint32_t text_style = 0;
};

struct Figure {
	std::vector<ImVec2> outline; // ja amostrada, nas coordenadas do .xui
	float min_x = 0, min_y = 0, max_x = 1, max_y = 1;
	bool fill = false;
	int fill_type = 1; // 1 solido, 2 degrade linear, 3 radial (0 sem preenchimento)
	uint32_t fill_color = 0;
	std::vector<Stop> stops;
	// A transformacao do pincel, com p de -0,5 a 0,5 na caixa da figura. Linear: gira(p * scale,
	// rotation).x - translation.x. Radial: 2 * |(p + translation) * scale| (raio 1 = meio tamanho),
	// com o centro no canto de dentro: e assim que os cantos das molduras (translation +-0,5,
	// verde ate 0,49) fecham a curva com a mesma espessura das bordas.
	float fill_tx = 0, fill_ty = 0, fill_sx = 1, fill_sy = 1, fill_rot = 0;
	float stroke_width = 0;
	uint32_t stroke_color = 0;
};

// Um valor do .xui com os numeros ja lidos ("100,250,0" vira 3 floats).
struct Value {
	std::string s;
	float f[4] = {};
	int n = 0;
};

struct Keyframe {
	int time = 0;
	int interp = 0; // 0 linear, 1 degrau, 2 suavizada
	float ease = 0; // 0..1, quanto da curva suave entra
	std::vector<Value> values;
};

struct Timeline {
	std::string target;
	std::vector<std::string> props;
	std::vector<Keyframe> keys;
	int file_prop = -1; // indice de "File" em props: a timeline de um som
};

struct NamedFrame {
	std::string name;
	int time = 0;
	std::string command, target;
};

struct Def {
	std::string tag, id, visual, class_override;
	Kind kind = K_OTHER;
	State base;
	int anchor = 0, size_mode = 0;
	bool color_writes = true;
	Figure fig;
	std::string grid_texture;
	float grid[4] = {}; // esquerda, cima, direita, baixo
	std::vector<Def *> children;
	std::vector<Timeline> timelines;
	std::vector<NamedFrame> frames;
	int last_frame = 0;
};

// Uma cena carregada de um arquivo: as definicoes, donas da memoria.
struct File {
	std::vector<Def *> all;
	Def *root = nullptr;
	~File();
	Def *find(const char *id) const; // primeira definicao com esse Id, em qualquer profundidade
	Def *visual(const char *id) const;
};

bool load(File &file, const char *path);
bool parse(File &file, const char *xml, size_t len);

// Uma instancia viva, com timeline tocando.
struct Node {
	const Def *def = nullptr;
	Node *parent = nullptr;
	Node *visual = nullptr; // a arvore do XuiVisual da skin, desenhada antes dos filhos
	std::vector<Node *> children;
	State s;
	Vec3 anchor_pos; // ajuste das ancoras quando o visual tem outro tamanho que o controle
	float anchor_w = 0, anchor_h = 0;
	float time = 0;
	int stop_at = -1;
	bool playing = false;
	// Sons so quando um estado foi tocado por nome (o Normal da criacao fica quieto).
	bool sound_armed = false;
	bool fresh = false; // o quadro em time ainda nao tocou o som dele
	// O que o codigo troca, por cima da definicao e da timeline.
	bool has_image = false;
	Texture image; // capa, avatar
	bool has_text = false;
	std::string text;
	bool has_image_path = false;
	std::string image_path;
	bool hidden = false;
	// Desenho do codigo por cima do elemento, em coordenadas locais (0..w, 0..h): sai na
	// mesma perspectiva e com a mesma opacidade, que o engine aplica depois.
	std::function<void(ImDrawList *, float w, float h)> on_draw;
	~Node();
};

// Instancia def; skin resolve os Visual. Os filhos tocam a partir do quadro 0.
Node *instantiate(const Def *def, const File *skin, Node *parent = nullptr);

Node *find(Node *root, const char *id); // busca em profundidade, so nos filhos (nao nos visuais)
Node *find_path(Node *root, const char *path); // "Tab2/CustomMenuItem/ContentTab_MainText"

// Toca a partir do quadro nomeado ate o proximo stop (ou o fim). Falso se o nome nao existe.
bool play(Node *node, const char *frame);
// No visual de um controle: "Normal", "Focus", "Press", "InitFocus", "KillFocus".
bool play_visual(Node *node, const char *frame);

void set_text(Node *node, const char *text);
void set_image(Node *node, const Texture &texture);
void set_image_path(Node *node, const char *path);

void update(Node *root, float dt);

// Desenha root com a origem em (x, y) de 1280x720, multiplicando a opacidade por alpha.
void draw(ImDrawList *dl, Node *root, float x = 0, float y = 0, float alpha = 1);

// Aplica um valor de propriedade como no .xui ("Position", "100,250,0").
void apply(State &s, const std::string &name, const std::string &value);

} // namespace xui

#endif
