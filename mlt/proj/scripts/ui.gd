extends RefCounted
## Small UI factory in CS2's dark, flat style. Fonts come from the player's own CS2 (panorama/fonts).

const BG := Color(0.07, 0.08, 0.1, 0.92)
const PANEL := Color(0.11, 0.12, 0.15, 0.94)
const LINE := Color(1, 1, 1, 0.08)
const TEXT := Color(0.92, 0.93, 0.95)
const DIM := Color(0.62, 0.65, 0.7)
const ACCENT := Color(0.36, 0.62, 0.98)
const GO := Color(0.3, 0.68, 0.31)
const CT := Color(0.36, 0.47, 0.68)
const T := Color(0.87, 0.61, 0.21)

static func label(text: String, size := 18, color := TEXT, bold := false) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_font_override("font", Game.font("font_ui" if bold else "font_body"))
	l.add_theme_font_size_override("font_size", size)
	l.add_theme_color_override("font_color", color)
	return l

static func box(color := PANEL, radius := 2, border := LINE) -> StyleBoxFlat:
	var s := StyleBoxFlat.new()
	s.bg_color = color
	s.set_corner_radius_all(radius)
	s.border_color = border
	s.set_border_width_all(1 if border.a > 0 else 0)
	s.content_margin_left = 14
	s.content_margin_right = 14
	s.content_margin_top = 8
	s.content_margin_bottom = 8
	return s

static func button(text: String, size := 18, base := Color(1, 1, 1, 0.06), hover := Color(1, 1, 1, 0.16), font_color := TEXT) -> Button:
	var b := Button.new()
	b.text = text
	b.focus_mode = Control.FOCUS_NONE
	b.add_theme_font_override("font", Game.font("font_ui"))
	b.add_theme_font_size_override("font_size", size)
	b.add_theme_color_override("font_color", font_color)
	b.add_theme_color_override("font_hover_color", Color.WHITE)
	b.add_theme_color_override("font_pressed_color", Color.WHITE)
	b.add_theme_stylebox_override("normal", box(base, 2, Color(0, 0, 0, 0)))
	b.add_theme_stylebox_override("hover", box(hover, 2, Color(0, 0, 0, 0)))
	b.add_theme_stylebox_override("pressed", box(hover.lightened(0.1), 2, Color(0, 0, 0, 0)))
	b.add_theme_stylebox_override("disabled", box(base.darkened(0.4), 2, Color(0, 0, 0, 0)))
	b.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	return b

static func toggle(b: Button, on: bool) -> void:
	var c := ACCENT if on else Color(1, 1, 1, 0.06)
	b.add_theme_stylebox_override("normal", box(c if on else Color(1, 1, 1, 0.06), 2, Color(0, 0, 0, 0)))
	b.add_theme_stylebox_override("hover", box(c.lightened(0.15) if on else Color(1, 1, 1, 0.16), 2, Color(0, 0, 0, 0)))

static func panel(color := PANEL) -> PanelContainer:
	var p := PanelContainer.new()
	p.add_theme_stylebox_override("panel", box(color))
	return p
