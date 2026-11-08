// O XUI do FSD sem o runtime da Microsoft; veja xui.h.

#include "xui.h"

#include "imgui_internal.h" // ImDrawListSharedData, para o pixel branco do atlas

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace xui {

Host g_host;

namespace {

// --- XML ------------------------------------------------------------------------------
//
// So o que os .xui tem: elementos, texto, atributos simples e as entidades basicas.

struct Xml {
	std::string tag, text;
	std::vector<std::pair<std::string, std::string>> attrs;
	std::vector<Xml *> kids;
	~Xml()
	{
		for (Xml *k : kids)
			delete k;
	}
	const Xml *child(const char *name) const
	{
		for (const Xml *k : kids)
			if (k->tag == name)
				return k;
		return nullptr;
	}
	const char *attr(const char *name) const
	{
		for (const auto &a : attrs)
			if (a.first == name)
				return a.second.c_str();
		return nullptr;
	}
};

void unescape(std::string &s)
{
	if (s.find('&') == std::string::npos)
		return;
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] != '&') {
			out += s[i];
			continue;
		}
		const size_t end = s.find(';', i);
		if (end == std::string::npos) {
			out += s[i];
			continue;
		}
		const std::string ent = s.substr(i + 1, end - i - 1);
		if (ent == "amp")
			out += '&';
		else if (ent == "lt")
			out += '<';
		else if (ent == "gt")
			out += '>';
		else if (ent == "quot")
			out += '"';
		else if (ent == "apos")
			out += '\'';
		else if (!ent.empty() && ent[0] == '#') {
			const unsigned long c = ent[1] == 'x' ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
			// UTF-8
			if (c < 0x80)
				out += (char)c;
			else if (c < 0x800) {
				out += (char)(0xC0 | c >> 6);
				out += (char)(0x80 | (c & 0x3F));
			} else {
				out += (char)(0xE0 | c >> 12);
				out += (char)(0x80 | (c >> 6 & 0x3F));
				out += (char)(0x80 | (c & 0x3F));
			}
		} else {
			out += s.substr(i, end - i + 1);
		}
		i = end;
	}
	s.swap(out);
}

struct XmlReader {
	const char *p, *end;

	void skip_space()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
			p++;
	}
	bool starts(const char *s) const { return (size_t)(end - p) >= strlen(s) && memcmp(p, s, strlen(s)) == 0; }
	void skip_past(const char *s)
	{
		const size_t n = strlen(s);
		while (p < end && !((size_t)(end - p) >= n && memcmp(p, s, n) == 0))
			p++;
		p = p + n <= end ? p + n : end;
	}

	// Le um elemento comecando em '<'. Nulo no fim ou em erro.
	Xml *element()
	{
		for (;;) {
			skip_space();
			if (p >= end || *p != '<')
				return nullptr;
			if (starts("<?")) {
				skip_past("?>");
				continue;
			}
			if (starts("<!--")) {
				skip_past("-->");
				continue;
			}
			break;
		}
		if (starts("</"))
			return nullptr;
		p++;
		Xml *x = new Xml;
		while (p < end && *p != '>' && *p != '/' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
			x->tag += *p++;
		// Atributos.
		for (;;) {
			skip_space();
			if (p >= end) {
				delete x;
				return nullptr;
			}
			if (*p == '/') {
				skip_past(">");
				return x;
			}
			if (*p == '>') {
				p++;
				break;
			}
			std::string name, value;
			while (p < end && *p != '=' && *p != '>' && *p != ' ')
				name += *p++;
			skip_space();
			if (p < end && *p == '=') {
				p++;
				skip_space();
				const char quote = p < end ? *p : '"';
				p++;
				while (p < end && *p != quote)
					value += *p++;
				p++;
			}
			unescape(value);
			x->attrs.emplace_back(name, value);
		}
		// Conteudo: texto e filhos.
		for (;;) {
			const char *text_start = p;
			while (p < end && *p != '<')
				p++;
			x->text.append(text_start, (size_t)(p - text_start));
			if (p >= end)
				break;
			if (starts("<!--")) {
				skip_past("-->");
				continue;
			}
			if (starts("<![CDATA[")) {
				p += 9;
				const char *cdata = p;
				skip_past("]]>");
				x->text.append(cdata, (size_t)(p - 3 - cdata));
				continue;
			}
			if (starts("</")) {
				skip_past(">");
				break;
			}
			Xml *kid = element();
			if (!kid)
				break;
			x->kids.push_back(kid);
		}
		// Texto de elemento com filhos e so indentacao.
		if (!x->kids.empty())
			x->text.clear();
		else
			unescape(x->text);
		return x;
	}
};

// --- valores -------------------------------------------------------------------------------

int parse_floats(const char *s, float *out, int cap)
{
	int n = 0;
	while (*s && n < cap) {
		char *next;
		const float v = strtof(s, &next);
		if (next == s)
			break;
		out[n++] = v;
		s = next;
		while (*s == ',' || *s == ' ')
			s++;
	}
	return n;
}

Value value_of(const std::string &s)
{
	Value v;
	v.s = s;
	v.n = parse_floats(s.c_str(), v.f, 4);
	return v;
}

uint32_t color_of(const std::string &s) { return (uint32_t)strtoul(s.c_str(), nullptr, 0); }

// 0xAARRGGBB do XUI para a palavra do ImGui.
ImU32 im_color(uint32_t argb, float alpha = 1)
{
	const int a = (int)((float)(argb >> 24) * alpha + 0.5f);
	return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, a < 0 ? 0 : a > 255 ? 255 : a);
}

uint32_t mix_argb(uint32_t a, uint32_t b, float t)
{
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
		out |= (uint32_t)(int)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

Vec3 vec3_of(const Value &v, Vec3 fallback)
{
	if (v.n >= 2)
		fallback.x = v.f[0], fallback.y = v.f[1];
	if (v.n >= 3)
		fallback.z = v.f[2];
	return fallback;
}

void apply_value(State &s, const std::string &name, const Value &v)
{
	if (name == "Position")
		s.pos = vec3_of(v, s.pos);
	else if (name == "Scale")
		s.scale = vec3_of(v, s.scale);
	else if (name == "Pivot")
		s.pivot = vec3_of(v, s.pivot);
	else if (name == "Rotation") {
		if (v.n == 4)
			s.rot = { v.f[0], v.f[1], v.f[2], v.f[3] };
	} else if (name == "Width")
		s.w = v.n ? v.f[0] : s.w;
	else if (name == "Height")
		s.h = v.n ? v.f[0] : s.h;
	else if (name == "Opacity")
		s.opacity = v.n ? v.f[0] : s.opacity;
	else if (name == "Show")
		s.show = v.s != "false";
	else if (name == "ImagePath")
		s.image_path = v.s;
	else if (name == "Text")
		s.text = v.s;
	else if (name == "TextColor")
		s.text_color = color_of(v.s);
	else if (name == "DropShadowColor")
		s.shadow_color = color_of(v.s);
	else if (name == "PointSize")
		s.point_size = v.n ? v.f[0] : s.point_size;
	else if (name == "TextStyle")
		s.text_style = (uint32_t)(v.n ? v.f[0] : 0);
}

// Interpola de a para b; o que nao e numero fica em a ate o proximo quadro-chave.
void apply_between(State &s, const std::string &name, const Value &a, const Value &b, float t)
{
	if (t <= 0 || a.n == 0 || a.n != b.n) {
		apply_value(s, name, t >= 1 ? b : a);
		return;
	}
	if (name == "TextColor" || name == "DropShadowColor") {
		const uint32_t c = mix_argb(color_of(a.s), color_of(b.s), t);
		(name == "TextColor" ? s.text_color : s.shadow_color) = c;
		return;
	}
	if (name == "Text" || name == "ImagePath" || name == "Show" || name == "TextStyle") {
		apply_value(s, name, t >= 1 ? b : a);
		return;
	}
	Value v;
	v.n = a.n;
	for (int i = 0; i < a.n; i++)
		v.f[i] = a.f[i] + (b.f[i] - a.f[i]) * t;
	if (name == "Rotation" && v.n == 4) {
		// nlerp, pelo caminho curto
		const float dot = a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2] + a.f[3] * b.f[3];
		const float sign = dot < 0 ? -1.0f : 1.0f;
		float len = 0;
		for (int i = 0; i < 4; i++) {
			v.f[i] = a.f[i] + (b.f[i] * sign - a.f[i]) * t;
			len += v.f[i] * v.f[i];
		}
		len = sqrtf(len);
		if (len > 1e-6f)
			for (int i = 0; i < 4; i++)
				v.f[i] /= len;
	}
	apply_value(s, name, v);
}

Kind kind_of(const std::string &tag)
{
	if (tag == "XuiScene" || tag == "XuiTabScene" || tag == "XuiCanvas")
		return K_SCENE;
	if (tag == "XuiGroup")
		return K_GROUP;
	if (tag == "XuiImage")
		return K_IMAGE;
	if (tag == "XuiText")
		return K_TEXT;
	if (tag == "XuiTextPresenter")
		return K_TEXT_PRESENTER;
	if (tag == "XuiImagePresenter")
		return K_IMAGE_PRESENTER;
	if (tag == "XuiFigure")
		return K_FIGURE;
	if (tag == "XuiNineGrid")
		return K_NINEGRID;
	if (tag == "XuiVisual")
		return K_VISUAL;
	if (tag == "XuiSoundXAudio")
		return K_SOUND;
	if (tag == "XuiControl" || tag == "XuiButton" || tag == "XuiNavButton" || tag == "XuiListItem" ||
		tag == "XuiRadioButton" || tag == "XuiProgressBar" || tag == "XuiSlider" || tag == "XuiEdit" ||
		tag == "XuiCheckbox" || tag == "XuiList")
		return K_CONTROL;
	return K_OTHER;
}

// --- figuras ---------------------------------------------------------------------------------

// "N, x,y, c1x,c1y, c2x,c2y, tipo, ..." (sete numeros por ponto), amostrando as curvas.
void parse_points(Figure &fig, const std::string &text, bool closed)
{
	std::vector<float> f;
	const char *s = text.c_str();
	while (*s) {
		char *next;
		const float v = strtof(s, &next);
		if (next == s)
			break;
		f.push_back(v);
		s = next;
		while (*s == ',' || *s == ' ')
			s++;
	}
	if (f.empty())
		return;
	const int count = (int)f[0];
	if (count < 2 || (int)f.size() < 1 + count * 7)
		return;
	struct P {
		ImVec2 p, in, out;
	};
	std::vector<P> pts((size_t)count);
	for (int i = 0; i < count; i++) {
		const float *q = &f[(size_t)(1 + i * 7)];
		pts[(size_t)i] = { ImVec2(q[0], q[1]), ImVec2(q[2], q[3]), ImVec2(q[4], q[5]) };
	}
	auto same = [](ImVec2 a, ImVec2 b) { return fabsf(a.x - b.x) < 0.01f && fabsf(a.y - b.y) < 0.01f; };
	const int segments = closed ? count : count - 1;
	for (int i = 0; i < segments; i++) {
		const P &a = pts[(size_t)i], &b = pts[(size_t)((i + 1) % count)];
		fig.outline.push_back(a.p);
		const bool straight = (same(a.out, a.p) || same(a.out, b.p)) && (same(b.in, b.p) || same(b.in, a.p));
		if (straight)
			continue;
		for (int k = 1; k < 10; k++) {
			const float t = (float)k / 10, u = 1 - t;
			const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
			fig.outline.push_back(ImVec2(w0 * a.p.x + w1 * a.out.x + w2 * b.in.x + w3 * b.p.x,
				w0 * a.p.y + w1 * a.out.y + w2 * b.in.y + w3 * b.p.y));
		}
	}
	if (!closed)
		fig.outline.push_back(pts.back().p);
	fig.min_x = fig.min_y = 1e9f;
	fig.max_x = fig.max_y = -1e9f;
	for (const ImVec2 &p : fig.outline) {
		fig.min_x = fminf(fig.min_x, p.x);
		fig.min_y = fminf(fig.min_y, p.y);
		fig.max_x = fmaxf(fig.max_x, p.x);
		fig.max_y = fmaxf(fig.max_y, p.y);
	}
}

void parse_figure(Def *d, const Xml *props)
{
	Figure &fig = d->fig;
	const Xml *closed = props->child("Closed");
	if (const Xml *points = props->child("Points"))
		parse_points(fig, points->text, !closed || closed->text != "false");
	if (const Xml *fill = props->child("Fill")) {
		const Xml *fp = fill->child("Properties");
		fig.fill = true;
		if (fp) {
			if (const Xml *t = fp->child("FillType"))
				fig.fill_type = atoi(t->text.c_str());
			if (const Xml *c = fp->child("FillColor"))
				fig.fill_color = color_of(c->text);
			if (const Xml *r = fp->child("Rotation"))
				fig.fill_rot = strtof(r->text.c_str(), nullptr);
			float v[4];
			if (const Xml *t = fp->child("Translation"))
				if (parse_floats(t->text.c_str(), v, 4) >= 2)
					fig.fill_tx = v[0], fig.fill_ty = v[1];
			if (const Xml *s = fp->child("Scale"))
				if (parse_floats(s->text.c_str(), v, 4) >= 2)
					fig.fill_sx = v[0], fig.fill_sy = v[1];
			const Xml *g = fp->child("Gradient");
			const Xml *gp = g ? g->child("Properties") : nullptr;
			if (gp) {
				const Xml *radial = gp->child("Radial");
				if (radial && radial->text == "true" && fig.fill_type >= 2)
					fig.fill_type = 3;
				for (const Xml *k : gp->kids) {
					const char *index = k->attr("index");
					if (!index)
						continue;
					const size_t i = (size_t)atoi(index);
					if (fig.stops.size() <= i)
						fig.stops.resize(i + 1, Stop{ 0, 0 });
					if (k->tag == "StopColor")
						fig.stops[i].color = color_of(k->text);
					else if (k->tag == "StopPos")
						fig.stops[i].pos = strtof(k->text.c_str(), nullptr);
				}
			}
		} else {
			fig.fill_type = 1;
		}
		if (fig.fill_type >= 2 && fig.stops.size() < 2)
			fig.fill_type = 1;
		if (fig.fill_type == 0)
			fig.fill = false;
	}
	if (const Xml *stroke = props->child("Stroke"))
		if (const Xml *sp = stroke->child("Properties")) {
			if (const Xml *w = sp->child("StrokeWidth"))
				fig.stroke_width = strtof(w->text.c_str(), nullptr);
			if (const Xml *c = sp->child("StrokeColor"))
				fig.stroke_color = color_of(c->text);
		}
}

void parse_timelines(Def *d, const Xml *timelines)
{
	for (const Xml *k : timelines->kids) {
		if (k->tag == "NamedFrames") {
			for (const Xml *f : k->kids) {
				NamedFrame nf;
				if (const Xml *x = f->child("Name"))
					nf.name = x->text;
				if (const Xml *x = f->child("Time"))
					nf.time = atoi(x->text.c_str());
				if (const Xml *x = f->child("Command"))
					nf.command = x->text;
				if (const Xml *x = f->child("CommandParams"))
					nf.target = x->text;
				d->last_frame = nf.time > d->last_frame ? nf.time : d->last_frame;
				d->frames.push_back(nf);
			}
		} else if (k->tag == "Timeline") {
			Timeline tl;
			for (const Xml *x : k->kids) {
				if (x->tag == "Id")
					tl.target = x->text;
				else if (x->tag == "TimelineProp")
					tl.props.push_back(x->text);
				else if (x->tag == "KeyFrame") {
					Keyframe kf;
					float ease_in = 0, ease_out = 0;
					for (const Xml *y : x->kids) {
						if (y->tag == "Time")
							kf.time = atoi(y->text.c_str());
						else if (y->tag == "Interpolation")
							kf.interp = atoi(y->text.c_str());
						else if (y->tag == "EaseIn")
							ease_in = strtof(y->text.c_str(), nullptr);
						else if (y->tag == "EaseOut")
							ease_out = strtof(y->text.c_str(), nullptr);
						else if (y->tag == "Prop")
							kf.values.push_back(value_of(y->text));
					}
					kf.ease = fminf((fabsf(ease_in) + fabsf(ease_out)) / 200.0f, 1.0f);
					if (kf.interp == 2 && kf.ease <= 0)
						kf.ease = 1;
					d->last_frame = kf.time > d->last_frame ? kf.time : d->last_frame;
					tl.keys.push_back(kf);
				}
			}
			for (size_t i = 0; i < tl.props.size(); i++)
				if (tl.props[i] == "File")
					tl.file_prop = (int)i;
			d->timelines.push_back(tl);
		}
	}
}

Def *convert(File &file, const Xml *x)
{
	Def *d = new Def;
	file.all.push_back(d);
	d->tag = x->tag;
	d->kind = kind_of(x->tag);
	if (const Xml *props = x->child("Properties")) {
		for (const Xml *p : props->kids) {
			const std::string &n = p->tag;
			if (n == "Id")
				d->id = p->text;
			else if (n == "Visual")
				d->visual = p->text;
			else if (n == "ClassOverride")
				d->class_override = p->text;
			else if (n == "Anchor")
				d->anchor = atoi(p->text.c_str());
			else if (n == "SizeMode")
				d->size_mode = atoi(p->text.c_str());
			else if (n == "ColorWriteFlags")
				d->color_writes = (atoi(p->text.c_str()) & 7) != 0;
			else if (n == "TextureFileName")
				d->grid_texture = p->text;
			else if (n == "LeftOffset")
				d->grid[0] = strtof(p->text.c_str(), nullptr);
			else if (n == "TopOffset")
				d->grid[1] = strtof(p->text.c_str(), nullptr);
			else if (n == "RightOffset")
				d->grid[2] = strtof(p->text.c_str(), nullptr);
			else if (n == "BottomOffset")
				d->grid[3] = strtof(p->text.c_str(), nullptr);
			else
				apply_value(d->base, n, value_of(p->text));
		}
		if (d->kind == K_FIGURE)
			parse_figure(d, props);
	}
	for (const Xml *k : x->kids) {
		if (k->tag == "Timelines")
			parse_timelines(d, k);
		else if (k->tag.compare(0, 3, "Xui") == 0)
			d->children.push_back(convert(file, k));
	}
	return d;
}

// --- matrizes ---------------------------------------------------------------------------------

struct Mat {
	float m[4][4];
};

Mat identity()
{
	Mat r = {};
	for (int i = 0; i < 4; i++)
		r.m[i][i] = 1;
	return r;
}

Mat mul(const Mat &a, const Mat &b)
{
	Mat r = {};
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			for (int k = 0; k < 4; k++)
				r.m[i][j] += a.m[i][k] * b.m[k][j];
	return r;
}

Mat translate(float x, float y, float z)
{
	Mat r = identity();
	r.m[0][3] = x, r.m[1][3] = y, r.m[2][3] = z;
	return r;
}

Mat scale(float x, float y, float z)
{
	Mat r = identity();
	r.m[0][0] = x, r.m[1][1] = y, r.m[2][2] = z;
	return r;
}

Mat rotate(const Quat &q)
{
	Mat r = identity();
	const float x = q.x, y = q.y, z = q.z, w = q.w;
	r.m[0][0] = 1 - 2 * (y * y + z * z), r.m[0][1] = 2 * (x * y - z * w), r.m[0][2] = 2 * (x * z + y * w);
	r.m[1][0] = 2 * (x * y + z * w), r.m[1][1] = 1 - 2 * (x * x + z * z), r.m[1][2] = 2 * (y * z - x * w);
	r.m[2][0] = 2 * (x * z - y * w), r.m[2][1] = 2 * (y * z + x * w), r.m[2][2] = 1 - 2 * (x * x + y * y);
	return r;
}

ImVec2 project(const Mat &m, float x, float y)
{
	const float px = m.m[0][0] * x + m.m[0][1] * y + m.m[0][3];
	const float py = m.m[1][0] * x + m.m[1][1] * y + m.m[1][3];
	float pz = m.m[2][0] * x + m.m[2][1] * y + m.m[2][3];
	const float f = g_host.focal;
	if (pz < -f * 0.9f)
		pz = -f * 0.9f;
	const float k = f / (f + pz);
	return ImVec2((g_host.vanish_x + (px - g_host.vanish_x) * k) * g_host.sx,
		(g_host.vanish_y + (py - g_host.vanish_y) * k) * g_host.sy);
}

// Leva os vertices desenhados em coordenadas locais (desde start) para a tela, e aplica a opacidade.
void place_since(ImDrawList *dl, int start, const Mat &m, float alpha)
{
	for (int i = start; i < dl->VtxBuffer.Size; i++) {
		ImDrawVert &v = dl->VtxBuffer[i];
		v.pos = project(m, v.pos.x, v.pos.y);
		if (alpha < 0.999f) {
			const int a = (int)((float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) * alpha + 0.5f);
			v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
		}
	}
}

// --- desenho --------------------------------------------------------------------------------------

uint32_t gradient_at(const std::vector<Stop> &stops, float t)
{
	if (t <= stops.front().pos)
		return stops.front().color;
	for (size_t i = 1; i < stops.size(); i++)
		if (t <= stops[i].pos) {
			const float span = stops[i].pos - stops[i - 1].pos;
			return mix_argb(stops[i - 1].color, stops[i].color, span > 1e-6f ? (t - stops[i - 1].pos) / span : 1);
		}
	return stops.back().color;
}

void draw_figure(ImDrawList *dl, const Figure &fig, float w, float h)
{
	if (fig.outline.size() < 3)
		return;
	// O caminho vem no espaco em que foi desenhado; estica para o tamanho do elemento.
	const float kx = fig.max_x > 1e-3f ? w / fig.max_x : 1, ky = fig.max_y > 1e-3f ? h / fig.max_y : 1;
	std::vector<ImVec2> pts;
	const bool gradient = fig.fill && fig.fill_type >= 2;
	for (size_t i = 0; i < fig.outline.size(); i++) {
		const ImVec2 a(fig.outline[i].x * kx, fig.outline[i].y * ky);
		pts.push_back(a);
		if (!gradient)
			continue;
		// Num degrade, as bordas retas ganham pontos no meio para a cor seguir as paradas.
		const ImVec2 &n = fig.outline[(i + 1) % fig.outline.size()];
		const ImVec2 b(n.x * kx, n.y * ky);
		for (int k = 1; k < 8; k++) {
			const float t = (float)k / 8;
			pts.push_back(ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
		}
	}
	if (fig.fill && !gradient) {
		dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), im_color(fig.fill_color));
	} else if (gradient) {
		const float x0 = fig.min_x * kx, y0 = fig.min_y * ky, x1 = fig.max_x * kx, y1 = fig.max_y * ky;
		const ImVec2 c((x0 + x1) / 2, (y0 + y1) / 2);
		const float bw = fmaxf(x1 - x0, 1e-3f), bh = fmaxf(y1 - y0, 1e-3f);
		const float angle = fig.fill_rot * 3.14159265f / 180.0f, cs = cosf(angle), sn = sinf(angle);
		auto colour = [&](ImVec2 p) {
			const float u = (p.x - x0) / bw - 0.5f, v = (p.y - y0) / bh - 0.5f;
			float t;
			if (fig.fill_type == 3) {
				const float gx = (u + fig.fill_tx) * fig.fill_sx, gy = (v + fig.fill_ty) * fig.fill_sy;
				t = 2 * sqrtf(gx * gx + gy * gy);
			} else {
				t = u * fig.fill_sx * cs - v * fig.fill_sy * sn - fig.fill_tx;
			}
			return im_color(gradient_at(fig.stops, t));
		};
		// Aneis do centro ate a borda, com a cor calculada em cada vertice.
		constexpr int RINGS = 12;
		const int n = (int)pts.size();
		const ImVec2 uv = dl->_Data->TexUvWhitePixel;
		dl->PrimReserve(RINGS * n * 6, (RINGS + 1) * n);
		const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
		for (int r = 0; r <= RINGS; r++) {
			const float k = (float)r / RINGS;
			for (int i = 0; i < n; i++) {
				const ImVec2 p(c.x + (pts[(size_t)i].x - c.x) * k, c.y + (pts[(size_t)i].y - c.y) * k);
				dl->PrimWriteVtx(p, uv, colour(p));
			}
		}
		for (int r = 0; r < RINGS; r++)
			for (int i = 0; i < n; i++) {
				const int j = (i + 1) % n;
				const ImDrawIdx a = (ImDrawIdx)(base + r * n + i), b = (ImDrawIdx)(base + r * n + j),
								cc = (ImDrawIdx)(base + (r + 1) * n + j), d = (ImDrawIdx)(base + (r + 1) * n + i);
				dl->PrimWriteIdx(a), dl->PrimWriteIdx(b), dl->PrimWriteIdx(cc);
				dl->PrimWriteIdx(a), dl->PrimWriteIdx(cc), dl->PrimWriteIdx(d);
			}
	}
	if (fig.stroke_width > 0)
		dl->AddPolyline(pts.data(), (int)pts.size(), im_color(fig.stroke_color), ImDrawFlags_Closed, fig.stroke_width);
}

void draw_image(ImDrawList *dl, const Texture &tex, int size_mode, float w, float h)
{
	if (tex.id == ImTextureID_Invalid || tex.w <= 0 || tex.h <= 0)
		return;
	ImVec2 a(0, 0), b(w, h), uv0(0, 0), uv1(1, 1);
	if (size_mode == 1) {
		// tamanho real, no canto
		b = ImVec2(tex.w, tex.h);
	} else if (size_mode == 8 || size_mode == 16) {
		// 8: cabe inteira mantendo a proporcao; 16: cobre tudo, cortando
		const float k = size_mode == 8 ? fminf(w / tex.w, h / tex.h) : fmaxf(w / tex.w, h / tex.h);
		const float iw = tex.w * k, ih = tex.h * k;
		if (size_mode == 8) {
			a = ImVec2((w - iw) / 2, (h - ih) / 2);
			b = ImVec2(a.x + iw, a.y + ih);
		} else {
			uv0 = ImVec2((iw - w) / 2 / iw, (ih - h) / 2 / ih);
			uv1 = ImVec2(1 - uv0.x, 1 - uv0.y);
		}
	}
	dl->AddImage(tex.id, a, b, uv0, uv1);
}

void draw_ninegrid(ImDrawList *dl, const Texture &tex, const float *grid, float w, float h)
{
	if (tex.id == ImTextureID_Invalid || tex.w <= 0 || tex.h <= 0)
		return;
	const float l = grid[0], t = grid[1], r = grid[2], bt = grid[3];
	const float xs[4] = { 0, fminf(l, w / 2), fmaxf(w - r, w / 2), w };
	const float ys[4] = { 0, fminf(t, h / 2), fmaxf(h - bt, h / 2), h };
	const float us[4] = { 0, l / tex.w, 1 - r / tex.w, 1 };
	const float vs[4] = { 0, t / tex.h, 1 - bt / tex.h, 1 };
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			if (xs[i + 1] > xs[i] && ys[j + 1] > ys[j])
				dl->AddImage(tex.id, ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j + 1]), ImVec2(us[i], vs[j]),
					ImVec2(us[i + 1], vs[j + 1]));
}

// Corta com reticencias para caber em max_width.
std::string ellipsize(ImFont *font, float size, const std::string &s, float max_width)
{
	if (font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= max_width)
		return s;
	size_t len = s.size();
	while (len > 0) {
		do
			len--;
		while (len > 0 && (s[len] & 0xC0) == 0x80);
		const std::string out = s.substr(0, len) + "...";
		if (font->CalcTextSizeA(size, FLT_MAX, 0, out.c_str()).x <= max_width)
			return out;
	}
	return "...";
}

// TextStyle: 0x10 sombra, 0x200 a direita, 0x400 centralizado, 0x1 centro vertical,
// 0x1000 uma linha so, 0x4000 reticencias.
void draw_text(ImDrawList *dl, const State &s, const std::string &text, float w, float h)
{
	if (text.empty() || !g_host.font)
		return;
	ImFont *font = g_host.font("ConsoleFont");
	if (!font)
		return;
	// Sem PointSize valido (zero numa timeline, ou vazio), vale o padrao do XUI.
	const float size = (s.point_size > 0.5f ? s.point_size : 14.0f) * g_host.font_scale;
	const uint32_t style = s.text_style;
	const bool single = style & 0x1000;
	std::string shown = text;
	if (style & 0x4000)
		shown = ellipsize(font, size, text, w);
	const float wrap = single ? 0.0f : w;
	const ImVec2 box = font->CalcTextSizeA(size, FLT_MAX, wrap, shown.c_str());
	float x = 0, y = 0;
	if (style & 0x400)
		x = (w - box.x) / 2;
	else if (style & 0x200)
		x = w - box.x;
	if (style & 0x1)
		y = (h - box.y) / 2;
	if (style & 0x10 && (s.shadow_color >> 24))
		dl->AddText(font, size, ImVec2(x + 1.5f, y + 1.5f), im_color(s.shadow_color), shown.c_str(), nullptr, wrap);
	dl->AddText(font, size, ImVec2(x, y), im_color(s.text_color), shown.c_str(), nullptr, wrap);
}

// Quem fornece texto e imagem a um presenter: o controle (ou cena) acima dele.
const Node *owner(const Node *n)
{
	for (const Node *p = n->parent; p; p = p->parent)
		if (p->def->kind == K_CONTROL || p->def->kind == K_SCENE)
			return p;
	return nullptr;
}

std::string text_of(const Node *n) { return n->has_text ? n->text : n->s.text; }

void draw_node(ImDrawList *dl, const Node *n, const Mat &parent, float alpha);

void draw_children(ImDrawList *dl, const Node *n, const Mat &m, float alpha)
{
	if (n->visual)
		for (const Node *c : n->visual->children)
			draw_node(dl, c, m, alpha);
	for (const Node *c : n->children)
		draw_node(dl, c, m, alpha);
}

void draw_node(ImDrawList *dl, const Node *n, const Mat &parent, float alpha)
{
	const State &s = n->s;
	const float a = alpha * s.opacity;
	if (!s.show || n->hidden || a <= 0.004f || !n->def->color_writes)
		return;
	const Vec3 pos{ s.pos.x + n->anchor_pos.x, s.pos.y + n->anchor_pos.y, s.pos.z + n->anchor_pos.z };
	const float w = s.w + n->anchor_w, h = s.h + n->anchor_h;
	Mat m = mul(parent, translate(pos.x, pos.y, pos.z));
	m = mul(m, translate(s.pivot.x, s.pivot.y, s.pivot.z));
	m = mul(m, rotate(s.rot));
	m = mul(m, scale(s.scale.x, s.scale.y, s.scale.z));
	m = mul(m, translate(-s.pivot.x, -s.pivot.y, -s.pivot.z));

	const int start = dl->VtxBuffer.Size;
	switch (n->def->kind) {
	case K_IMAGE: {
		Texture tex;
		const std::string &path = n->has_image_path ? n->image_path : s.image_path;
		if (n->has_image)
			tex = n->image;
		else if (!path.empty() && g_host.texture)
			g_host.texture(path.c_str(), &tex);
		draw_image(dl, tex, n->def->size_mode, w, h);
		break;
	}
	case K_IMAGE_PRESENTER: {
		const Node *o = owner(n);
		Texture tex;
		if (o && o->has_image)
			tex = o->image;
		else if (o && g_host.texture) {
			const std::string &path = o->has_image_path ? o->image_path : o->s.image_path;
			if (!path.empty())
				g_host.texture(path.c_str(), &tex);
		}
		if (n->has_image)
			tex = n->image;
		draw_image(dl, tex, n->def->size_mode ? n->def->size_mode : 8, w, h);
		break;
	}
	case K_NINEGRID: {
		Texture tex;
		if (!n->def->grid_texture.empty() && g_host.texture)
			g_host.texture(n->def->grid_texture.c_str(), &tex);
		draw_ninegrid(dl, tex, n->def->grid, w, h);
		break;
	}
	case K_FIGURE:
		draw_figure(dl, n->def->fig, w, h);
		break;
	case K_TEXT:
		draw_text(dl, s, text_of(n), w, h);
		break;
	case K_TEXT_PRESENTER: {
		const Node *o = owner(n);
		draw_text(dl, s, n->has_text ? n->text : o ? text_of(o) : std::string(), w, h);
		break;
	}
	default:
		break;
	}
	place_since(dl, start, m, a);
	draw_children(dl, n, m, a);
	if (n->on_draw) {
		const int mine = dl->VtxBuffer.Size;
		n->on_draw(dl, w, h);
		place_since(dl, mine, m, a);
	}
}

// --- timelines ---------------------------------------------------------------------------------------

float smooth(float t) { return t * t * (3 - 2 * t); }

Node *child_by_id(Node *n, const std::string &id)
{
	for (Node *c : n->children)
		if (c->def->id == id)
			return c;
	return nullptr;
}

// Aplica as timelines de n (tempo n->time) nos filhos de targets.
void evaluate(Node *n, Node *targets)
{
	const float t = n->time;
	for (const Timeline &tl : n->def->timelines) {
		Node *target = child_by_id(targets, tl.target);
		if (!target || tl.keys.empty())
			continue;
		if (t < (float)tl.keys.front().time)
			continue;
		size_t k = 0;
		while (k + 1 < tl.keys.size() && (float)tl.keys[k + 1].time <= t)
			k++;
		const Keyframe &a = tl.keys[k];
		const Keyframe *b = k + 1 < tl.keys.size() ? &tl.keys[k + 1] : nullptr;
		float f = 0;
		if (b && b->time > a.time && a.interp != 1) {
			f = (t - (float)a.time) / (float)(b->time - a.time);
			if (a.interp == 2)
				f = f + (smooth(f) - f) * a.ease;
		}
		for (size_t p = 0; p < tl.props.size() && p < a.values.size(); p++) {
			if (b && p < b->values.size())
				apply_between(target->s, tl.props[p], a.values[p], b->values[p], f);
			else
				apply_value(target->s, tl.props[p], a.values[p]);
		}
	}
}

const NamedFrame *frame_named(const Def *d, const std::string &name)
{
	for (const NamedFrame &f : d->frames)
		if (f.name == name)
			return &f;
	return nullptr;
}

// Os sons das timelines de n com quadro-chave entre from e to (from so se fresh).
void fire_sounds(Node *n, float from, float to, bool fresh)
{
	if (!g_host.sound || !n->sound_armed)
		return;
	for (const Timeline &tl : n->def->timelines) {
		if (tl.file_prop < 0)
			continue;
		for (const Keyframe &k : tl.keys) {
			const float t = (float)k.time;
			if ((t > from || (fresh && t == from)) && t <= to && (size_t)tl.file_prop < k.values.size() &&
				!k.values[(size_t)tl.file_prop].s.empty())
				g_host.sound(k.values[(size_t)tl.file_prop].s.c_str());
		}
	}
}

void advance(Node *n, float dt)
{
	if (!n->playing || (n->def->frames.empty() && n->def->timelines.empty()))
		return;
	const bool fresh = n->fresh;
	n->fresh = false;
	const float before = n->time;
	float after = before + dt * 60.0f; // as timelines do XUI andam a 60 quadros por segundo
	// Um estado tocado por nome para no "End" dele (Normal ate EndNormal), com ou sem comando.
	if (n->stop_at >= 0 && after >= (float)n->stop_at && before <= (float)n->stop_at) {
		fire_sounds(n, before, (float)n->stop_at, fresh);
		n->time = (float)n->stop_at;
		n->playing = false;
		return;
	}
	// O primeiro quadro com comando no caminho decide.
	for (const NamedFrame &f : n->def->frames) {
		if (f.command.empty() || (float)f.time <= before || (float)f.time > after)
			continue;
		fire_sounds(n, before, (float)f.time, fresh);
		if (f.command == "stop") {
			n->time = (float)f.time;
			n->playing = false;
			return;
		}
		if (f.command == "goto" || f.command == "gotoandplay") {
			const NamedFrame *target = frame_named(n->def, f.target);
			n->time = target ? (float)target->time : 0;
			n->fresh = true;
			return;
		}
		if (f.command == "gotoandstop") {
			const NamedFrame *target = frame_named(n->def, f.target);
			n->time = target ? (float)target->time : 0;
			n->playing = false;
			return;
		}
	}
	if (after >= (float)n->def->last_frame) {
		after = (float)n->def->last_frame;
		n->playing = false;
	}
	fire_sounds(n, before, after, fresh);
	n->time = after;
}

void reset(Node *n)
{
	n->s = n->def->base;
	for (Node *c : n->children)
		reset(c);
	if (n->visual) {
		const float w = n->visual->s.w, h = n->visual->s.h;
		reset(n->visual);
		n->visual->s.w = w, n->visual->s.h = h;
	}
}

void tick(Node *n, float dt)
{
	advance(n, dt);
	if (n->visual) {
		advance(n->visual, dt);
		evaluate(n->visual, n->visual);
		for (Node *c : n->visual->children)
			tick(c, dt);
	}
	evaluate(n, n);
	for (Node *c : n->children)
		tick(c, dt);
}

// Ancoras: 1 esquerda, 2 cima, 4 direita, 8 baixo. Com as duas de um eixo, estica; so a
// do lado de la, acompanha; nenhuma, fica no meio.
void anchor(Node *n, float dw, float dh)
{
	const int a = n->def->anchor & 15;
	if (!a)
		return;
	// Com os bits de cima (0x40, 0x80, so nos brilhos dos botoes), o eixo sem ancora fica
	// onde esta em vez de ir para o meio: a linha de brilho continua no alto da moldura.
	const bool center = !(n->def->anchor & 0xF0);
	const bool l = a & 1, t = a & 2, r = a & 4, b = a & 8;
	if (l && r)
		n->anchor_w = dw;
	else if (r)
		n->anchor_pos.x = dw;
	else if (!l && center)
		n->anchor_pos.x = dw / 2;
	if (t && b)
		n->anchor_h = dh;
	else if (b)
		n->anchor_pos.y = dh;
	else if (!t && center)
		n->anchor_pos.y = dh / 2;
}

// As ancoras descem a arvore: um grupo esticado estica os filhos pelo quanto ele mudou.
// E assim que o FSD monta as molduras dos botoes (cantos de 16x16 e bordas num grupo).
void anchor_tree(Node *n, float dw, float dh)
{
	for (Node *c : n->children) {
		anchor(c, dw, dh);
		if (c->anchor_w != 0 || c->anchor_h != 0)
			anchor_tree(c, c->anchor_w, c->anchor_h);
	}
}

} // namespace

// --- API --------------------------------------------------------------------------------------------

File::~File()
{
	for (Def *d : all)
		delete d;
}

Def *File::find(const char *id) const
{
	for (Def *d : all)
		if (d->id == id)
			return d;
	return nullptr;
}

Def *File::visual(const char *id) const
{
	for (Def *d : all)
		if (d->kind == K_VISUAL && d->id == id)
			return d;
	return nullptr;
}

bool parse(File &file, const char *xml, size_t len)
{
	XmlReader r{ xml, xml + len };
	Xml *top = r.element();
	if (!top)
		return false;
	file.root = convert(file, top);
	delete top;
	return true;
}

bool load(File &file, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	fseek(fp, 0, SEEK_END);
	const long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0) {
		fclose(fp);
		return false;
	}
	char *data = (char *)malloc((size_t)size);
	const bool ok = data && fread(data, 1, (size_t)size, fp) == (size_t)size;
	fclose(fp);
	const bool parsed = ok && parse(file, data, (size_t)size);
	free(data);
	return parsed;
}

Node::~Node()
{
	delete visual;
	for (Node *c : children)
		delete c;
}

Node *instantiate(const Def *def, const File *skin, Node *parent)
{
	Node *n = new Node;
	n->def = def;
	n->parent = parent;
	n->s = def->base;
	for (const Def *c : def->children)
		n->children.push_back(instantiate(c, skin, n));
	if (!def->visual.empty() && skin) {
		if (const Def *v = skin->visual(def->visual.c_str())) {
			n->visual = instantiate(v, skin, n);
			n->visual->s.w = def->base.w;
			n->visual->s.h = def->base.h;
			anchor_tree(n->visual, def->base.w - v->base.w, def->base.h - v->base.h);
			if (!play(n->visual, "Normal"))
				n->visual->playing = !v->timelines.empty();
			n->visual->sound_armed = false; // o estado inicial nao toca som; so o que o codigo pedir depois
			n->visual->fresh = false;
		}
	}
	n->playing = !def->timelines.empty();
	return n;
}

Node *find(Node *root, const char *id)
{
	if (!root)
		return nullptr;
	if (root->def->id == id)
		return root;
	for (Node *c : root->children)
		if (Node *f = find(c, id))
			return f;
	return nullptr;
}

Node *find_path(Node *root, const char *path)
{
	Node *n = root;
	const char *s = path;
	while (n && *s) {
		const char *slash = strchr(s, '/');
		const std::string part = slash ? std::string(s, (size_t)(slash - s)) : std::string(s);
		n = child_by_id(n, part);
		s = slash ? slash + 1 : s + part.size();
	}
	return n;
}

bool play(Node *node, const char *frame)
{
	if (!node)
		return false;
	const NamedFrame *f = frame_named(node->def, frame);
	if (!f)
		return false;
	node->time = (float)f->time;
	node->playing = true;
	node->fresh = true;
	node->sound_armed = true;
	const NamedFrame *end = frame_named(node->def, std::string("End") + frame);
	node->stop_at = end && end->time > f->time ? end->time : -1;
	if (node->stop_at < 0) {
		// Sem "End" proprio (KillFocus segue para Normal): para no primeiro "End..." depois dele.
		for (const NamedFrame &g : node->def->frames)
			if (g.time > f->time && g.name.compare(0, 3, "End") == 0 && (node->stop_at < 0 || g.time < node->stop_at))
				node->stop_at = g.time;
	}
	return true;
}

bool play_visual(Node *node, const char *frame) { return node && play(node->visual, frame); }

void set_text(Node *node, const char *text)
{
	if (!node)
		return;
	node->has_text = text != nullptr;
	node->text = text ? text : "";
}

void set_image(Node *node, const Texture &texture)
{
	if (!node)
		return;
	node->has_image = texture.id != ImTextureID_Invalid;
	node->image = texture;
}

void set_image_path(Node *node, const char *path)
{
	if (!node)
		return;
	node->has_image_path = path != nullptr;
	node->image_path = path ? path : "";
}

void update(Node *root, float dt)
{
	reset(root);
	tick(root, dt);
}

void draw(ImDrawList *dl, Node *root, float x, float y, float alpha)
{
	// Tudo e desenhado em coordenadas locais e so depois projetado: o recorte do ImGui
	// nao pode descartar nada antes disso. Depois, os comandos voltam a recortar na tela.
	const int first_cmd = dl->CmdBuffer.Size;
	dl->PushClipRect(ImVec2(-16384, -16384), ImVec2(16384, 16384), false);
	draw_node(dl, root, translate(x, y, 0), alpha);
	dl->PopClipRect();
	const ImVec4 screen(0, 0, 1280 * g_host.sx, 720 * g_host.sy);
	for (int i = first_cmd > 0 ? first_cmd - 1 : 0; i < dl->CmdBuffer.Size; i++) {
		ImVec4 &c = dl->CmdBuffer[i].ClipRect;
		c.x = fmaxf(c.x, screen.x), c.y = fmaxf(c.y, screen.y);
		c.z = fminf(c.z, screen.z), c.w = fminf(c.w, screen.w);
	}
}

void apply(State &s, const std::string &name, const std::string &value) { apply_value(s, name, value_of(value)); }

} // namespace xui
