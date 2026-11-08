extends CanvasLayer
## In-match HUD laid out like CS2's: health/armor bottom-left, ammo bottom-right, timer and score top-center,
## money top-left, kill feed top-right; plus buy menu (B), scoreboard (Tab), build bar and pause menu (Esc).

const Defs = preload("res://scripts/gen/defs.gd")
const UI = preload("res://scripts/ui.gd")

var match_node: Node
var _root: Control
var _hp: Label
var _armor: Label
var _money: Label
var _ammo: Label
var _weapon_icon: TextureRect
var _timer: Label
var _base := Color.WHITE       # HUD number colour: white (CS2) or 1.6 amber (modes.hud_style = classic)
var _styled := false
const AMBER := Color(1.0, 0.72, 0.22)
var _score_ct: Label
var _score_t: Label
var _alive_ct: HBoxContainer
var _alive_t: HBoxContainer
var _feed: VBoxContainer
var _center: Label
var _banner: PanelContainer
var _banner_text: Label
var _banner_sub: Label
var _progress: ProgressBar
var _progress_label: Label
var _crosshair: Control
var _scope: ColorRect
var _hurt: ColorRect
var _buy: PanelContainer
var _buy_money: Label
var _score_board: PanelContainer
var _board_list: VBoxContainer
var _build_bar: HBoxContainer
var _mat_boxes := {}
var _build_hint: Label
var _pause: PanelContainer
var _spec: Label
var _bomb_icon: TextureRect
var _center_until := 0.0
var _spectating: Fighter
var _fps: Label

func _ready() -> void:
	_root = Control.new()
	_root.set_anchors_preset(Control.PRESET_FULL_RECT)
	_root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_root)
	_hurt = ColorRect.new()
	_hurt.color = Color(0.8, 0, 0, 0)
	_hurt.set_anchors_preset(Control.PRESET_FULL_RECT)
	_hurt.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_root.add_child(_hurt)
	_scope = ColorRect.new()
	_scope.set_anchors_preset(Control.PRESET_FULL_RECT)
	_scope.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var sm := ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = "shader_type canvas_item;\nvoid fragment(){ vec2 uv = UV - 0.5; uv.x *= 16.0/9.0; float r = length(uv); float ring = smoothstep(0.42, 0.43, r); float cross = (abs(uv.x) < 0.0012 || abs(uv.y) < 0.0012) ? 1.0 : 0.0; COLOR = vec4(0.0, 0.0, 0.0, max(ring, cross)); }"
	sm.shader = sh
	_scope.material = sm
	_scope.visible = false
	_root.add_child(_scope)
	_crosshair = _make_crosshair()
	_root.add_child(_crosshair)

	# bottom-left: health and armor
	var bl := HBoxContainer.new()
	bl.position = Vector2(36, 0)
	bl.anchor_top = 1.0
	bl.anchor_bottom = 1.0
	bl.offset_top = -86
	bl.add_theme_constant_override("separation", 26)
	_root.add_child(bl)
	_hp = _stat(bl, "+", "100")
	_armor = _stat(bl, "◈", "0")
	# bottom-right: ammo and weapon
	var br := VBoxContainer.new()
	br.anchor_left = 1.0
	br.anchor_right = 1.0
	br.anchor_top = 1.0
	br.anchor_bottom = 1.0
	br.offset_left = -360
	br.offset_top = -150
	br.offset_right = -36
	br.alignment = BoxContainer.ALIGNMENT_END
	_root.add_child(br)
	_weapon_icon = TextureRect.new()
	_weapon_icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_weapon_icon.custom_minimum_size = Vector2(320, 46)
	_weapon_icon.modulate = Color(1, 1, 1, 0.9)
	_weapon_icon.size_flags_horizontal = Control.SIZE_SHRINK_END
	br.add_child(_weapon_icon)
	_ammo = UI.label("12 / 24", 40, Color.WHITE, true)
	_ammo.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_ammo.add_theme_constant_override("outline_size", 6)
	_ammo.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.6))
	br.add_child(_ammo)
	# top-left: money
	_money = UI.label("$800", 30, Color(0.45, 0.85, 0.45), true)
	_money.position = Vector2(36, 120)
	_money.add_theme_constant_override("outline_size", 6)
	_money.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.6))
	_root.add_child(_money)
	_bomb_icon = TextureRect.new()
	_bomb_icon.texture = Game.icon("img_c4", 40)
	_bomb_icon.position = Vector2(36, 170)
	_bomb_icon.modulate = Color(1, 0.75, 0.3)
	_root.add_child(_bomb_icon)
	# top-center: alive avatars, scores and the round timer
	var top := HBoxContainer.new()
	top.anchor_left = 0.5
	top.anchor_right = 0.5
	top.offset_left = -360
	top.offset_right = 360
	top.offset_top = 10
	top.alignment = BoxContainer.ALIGNMENT_CENTER
	top.add_theme_constant_override("separation", 8)
	_root.add_child(top)
	_alive_ct = HBoxContainer.new()
	_alive_ct.alignment = BoxContainer.ALIGNMENT_END
	_alive_ct.custom_minimum_size = Vector2(190, 0)
	top.add_child(_alive_ct)
	_score_ct = _score_box(top, UI.CT)
	var tbox := UI.panel(Color(0.05, 0.06, 0.08, 0.85))
	tbox.custom_minimum_size = Vector2(110, 0)
	top.add_child(tbox)
	_timer = UI.label("1:55", 28, Color.WHITE, true)
	_timer.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	tbox.add_child(_timer)
	_score_t = _score_box(top, UI.T)
	_alive_t = HBoxContainer.new()
	_alive_t.custom_minimum_size = Vector2(190, 0)
	top.add_child(_alive_t)
	# top-right: kill feed
	_feed = VBoxContainer.new()
	_feed.anchor_left = 1.0
	_feed.anchor_right = 1.0
	_feed.offset_left = -520
	_feed.offset_right = -24
	_feed.offset_top = 24
	_feed.alignment = BoxContainer.ALIGNMENT_BEGIN
	_feed.add_theme_constant_override("separation", 4)
	_root.add_child(_feed)
	# center notices
	_center = UI.label("", 26, Color.WHITE, true)
	_center.anchor_left = 0.5
	_center.anchor_right = 0.5
	_center.offset_left = -500
	_center.offset_right = 500
	_center.offset_top = 210
	_center.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_center.add_theme_constant_override("outline_size", 8)
	_center.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.7))
	_root.add_child(_center)
	_banner = UI.panel(Color(0.05, 0.06, 0.08, 0.86))
	_banner.anchor_left = 0.5
	_banner.anchor_right = 0.5
	_banner.offset_left = -420
	_banner.offset_right = 420
	_banner.offset_top = 120
	var bv := VBoxContainer.new()
	_banner.add_child(bv)
	_banner_text = UI.label("", 40, Color.WHITE, true)
	_banner_text.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	bv.add_child(_banner_text)
	_banner_sub = UI.label("", 20, UI.DIM)
	_banner_sub.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	bv.add_child(_banner_sub)
	_banner.visible = false
	_root.add_child(_banner)
	# plant/defuse progress
	var pv := VBoxContainer.new()
	pv.anchor_left = 0.5
	pv.anchor_right = 0.5
	pv.anchor_top = 0.5
	pv.anchor_bottom = 0.5
	pv.offset_left = -200
	pv.offset_right = 200
	pv.offset_top = 90
	_root.add_child(pv)
	_progress_label = UI.label("", 18, Color.WHITE, true)
	_progress_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	pv.add_child(_progress_label)
	_progress = ProgressBar.new()
	_progress.show_percentage = false
	_progress.custom_minimum_size = Vector2(400, 8)
	var fill := StyleBoxFlat.new()
	fill.bg_color = UI.ACCENT
	_progress.add_theme_stylebox_override("fill", fill)
	var back := StyleBoxFlat.new()
	back.bg_color = Color(0, 0, 0, 0.5)
	_progress.add_theme_stylebox_override("background", back)
	pv.add_child(_progress)
	# spectating label
	_spec = UI.label("", 22, Color.WHITE, true)
	_spec.anchor_left = 0.5
	_spec.anchor_right = 0.5
	_spec.anchor_top = 1.0
	_spec.anchor_bottom = 1.0
	_spec.offset_left = -400
	_spec.offset_right = 400
	_spec.offset_top = -150
	_spec.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_root.add_child(_spec)
	_fps = UI.label("", 16, Color(1, 1, 1, 0.75), true)
	_fps.position = Vector2(36, 86)
	_root.add_child(_fps)
	_make_build_bar()
	_make_buy_menu()
	_make_scoreboard()
	_make_pause()

func _stat(parent: Control, glyph: String, val: String) -> Label:
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", 8)
	var g := UI.label(glyph, 34, Color(1, 1, 1, 0.85), true)
	h.add_child(g)
	var l := UI.label(val, 44, Color.WHITE, true)
	l.add_theme_constant_override("outline_size", 6)
	l.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.6))
	h.add_child(l)
	parent.add_child(h)
	return l

func _score_box(parent: Control, c: Color) -> Label:
	var p := UI.panel(c.darkened(0.25))
	p.custom_minimum_size = Vector2(64, 0)
	var l := UI.label("0", 28, Color.WHITE, true)
	l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	p.add_child(l)
	parent.add_child(p)
	return l

func _make_crosshair() -> Control:
	var c := Control.new()
	c.anchor_left = 0.5
	c.anchor_top = 0.5
	c.anchor_right = 0.5
	c.anchor_bottom = 0.5
	c.mouse_filter = Control.MOUSE_FILTER_IGNORE
	for r in [Rect2(-12, -1, 8, 2), Rect2(4, -1, 8, 2), Rect2(-1, -12, 2, 8), Rect2(-1, 4, 2, 8)]:
		var o := ColorRect.new()
		o.color = Color(0, 0, 0, 0.6)
		o.position = r.position - Vector2(1, 1)
		o.size = r.size + Vector2(2, 2)
		c.add_child(o)
		var b := ColorRect.new()
		b.color = Color(0.3, 1, 0.3)
		b.position = r.position
		b.size = r.size
		c.add_child(b)
	return c

func _make_build_bar() -> void:
	_build_bar = HBoxContainer.new()
	_build_bar.anchor_left = 0.5
	_build_bar.anchor_right = 0.5
	_build_bar.anchor_top = 1.0
	_build_bar.anchor_bottom = 1.0
	_build_bar.offset_left = -420
	_build_bar.offset_right = 420
	_build_bar.offset_top = -100
	_build_bar.alignment = BoxContainer.ALIGNMENT_CENTER
	_build_bar.add_theme_constant_override("separation", 6)
	_root.add_child(_build_bar)
	for id in Defs.BUILD_PIECES:
		var row: Dictionary = Defs.BUILD_PIECES[id]
		var p := UI.panel(Color(0.05, 0.06, 0.08, 0.75))
		p.custom_minimum_size = Vector2(108, 64)
		p.name = id
		var v := VBoxContainer.new()
		v.alignment = BoxContainer.ALIGNMENT_CENTER
		p.add_child(v)
		var k := UI.label("%s / %s" % [row.key, row.alt_key], 14, UI.DIM, true)
		k.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		v.add_child(k)
		var n := UI.label(String(row.name).to_upper(), 18, Color.WHITE, true)
		n.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		v.add_child(n)
		_build_bar.add_child(p)
	var mv := VBoxContainer.new()
	mv.add_theme_constant_override("separation", 0)
	_build_bar.add_child(mv)
	for id in Defs.BUILD_MATERIALS:
		var l := UI.label("", 17, Color(String(Defs.BUILD_MATERIALS[id].color)).lightened(0.25), true)
		mv.add_child(l)
		_mat_boxes[id] = l
	_build_hint = UI.label("", 14, UI.DIM)
	_build_hint.anchor_left = 0.5
	_build_hint.anchor_right = 0.5
	_build_hint.anchor_top = 1.0
	_build_hint.anchor_bottom = 1.0
	_build_hint.offset_left = -420
	_build_hint.offset_right = 420
	_build_hint.offset_top = -124
	_build_hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_root.add_child(_build_hint)

# ---------------------------------------------------------------- buy menu
func _make_buy_menu() -> void:
	_buy = UI.panel(Color(0.06, 0.07, 0.09, 0.95))
	_buy.anchor_left = 0.5
	_buy.anchor_right = 0.5
	_buy.anchor_top = 0.5
	_buy.anchor_bottom = 0.5
	_buy.offset_left = -560
	_buy.offset_right = 560
	_buy.offset_top = -330
	_buy.offset_bottom = 330
	_buy.visible = false
	_root.add_child(_buy)

func _fill_buy() -> void:
	for c in _buy.get_children():
		c.queue_free()
	var f: Fighter = match_node.player
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 14)
	_buy.add_child(v)
	var head := HBoxContainer.new()
	v.add_child(head)
	var t := UI.label("BUY MENU", 30, Color.WHITE, true)
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(t)
	_buy_money = UI.label("$%d" % f.money, 30, Color(0.45, 0.85, 0.45), true)
	head.add_child(_buy_money)
	var cats := [["PISTOLS", ["glock", "usp"]], ["RIFLES", ["ak47", "m4a1s", "awp"]], ["GEAR", ["kevlar", "kevlar_helmet", "defuser"]]]
	if match_node.mode.portals:
		cats.append(["PORTAL  ·  slot 4: left mouse blue portal, right mouse the map for the orange one", ["portalgun"]])
	for cat in cats:
		var ln := UI.label(cat[0], 16, UI.DIM, true)
		v.add_child(ln)
		var g := HBoxContainer.new()
		g.add_theme_constant_override("separation", 10)
		v.add_child(g)
		for id in cat[1]:
			var is_w := Defs.WEAPONS.has(id)
			var row: Dictionary = f.wd(id) if is_w else Defs.EQUIPMENT[id]
			if row.team != "any" and row.team != f.team:
				continue
			var b := UI.button("", 18)
			b.custom_minimum_size = Vector2(250, 130)
			var bv := VBoxContainer.new()
			bv.set_anchors_preset(Control.PRESET_FULL_RECT)
			bv.mouse_filter = Control.MOUSE_FILTER_IGNORE
			bv.alignment = BoxContainer.ALIGNMENT_CENTER
			b.add_child(bv)
			var ic := TextureRect.new()
			ic.texture = Game.icon(row.icon_asset, 44)
			ic.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
			ic.custom_minimum_size = Vector2(0, 52)
			ic.mouse_filter = Control.MOUSE_FILTER_IGNORE
			if ic.texture == null and id == "portalgun":
				ic.texture = _portal_icon()
			bv.add_child(ic)
			var nl := UI.label(row.name, 18, Color.WHITE, true)
			nl.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
			bv.add_child(nl)
			var pl := UI.label("$%d" % int(row.price), 16, Color(0.45, 0.85, 0.45) if f.money >= int(row.price) else Color(0.85, 0.35, 0.3))
			pl.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
			bv.add_child(pl)
			b.pressed.connect(func():
				if match_node.buy_allowed(f) and f.buy(id):
					Game.play_ui("snd_knife", -18)
					(f as PlayerFighter).refresh_held()
					_fill_buy())
			g.add_child(b)
	var hint := UI.label("Press B or Esc to close.  Buying works in your spawn during buy time.", 15, UI.DIM)
	v.add_child(hint)

static var _picon: Texture2D
## A small blue-and-orange oval pair for the Portal Gun (CS2 has no icon for it).
func _portal_icon() -> Texture2D:
	if _picon:
		return _picon
	var img := Image.create(96, 48, false, Image.FORMAT_RGBA8)
	for y in 48:
		for x in 96:
			for c in [[24.0, Color(0.15, 0.55, 1.0)], [72.0, Color(1.0, 0.5, 0.08)]]:
				var d := Vector2((x - c[0]) / 13.0, (y - 24.0) / 21.0).length()
				if d < 1.0:
					img.set_pixel(x, y, Color(c[1] as Color, 1.0 if d > 0.7 else 0.45))
	_picon = ImageTexture.create_from_image(img)
	return _picon

func _toggle_buy() -> void:
	var f: Fighter = match_node.player
	if f == null:
		return
	if not _buy.visible and not match_node.buy_allowed(f):
		notice("Buy time is over", 1.2)
		return
	_buy.visible = not _buy.visible
	if _buy.visible:
		_fill_buy()
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if _buy.visible else Input.MOUSE_MODE_CAPTURED

# ---------------------------------------------------------------- scoreboard
func _make_scoreboard() -> void:
	_score_board = UI.panel(Color(0.06, 0.07, 0.09, 0.92))
	_score_board.anchor_left = 0.5
	_score_board.anchor_right = 0.5
	_score_board.anchor_top = 0.5
	_score_board.anchor_bottom = 0.5
	_score_board.offset_left = -480
	_score_board.offset_right = 480
	_score_board.offset_top = -320
	_score_board.offset_bottom = 320
	_score_board.visible = false
	_board_list = VBoxContainer.new()
	_score_board.add_child(_board_list)
	_root.add_child(_score_board)

func _fill_board() -> void:
	for c in _board_list.get_children():
		c.queue_free()
	for team in ["CT", "T"]:
		var hdr := UI.label("%s   %d" % [Defs.TEAMS[team].name.to_upper(), match_node.score[team]], 24, UI.CT.lightened(0.3) if team == "CT" else UI.T, true)
		_board_list.add_child(hdr)
		var cols := HBoxContainer.new()
		for t in [["PLAYER", 380], ["MONEY", 140], ["K", 90], ["D", 90], ["HP", 90]]:
			var l := UI.label(t[0], 14, UI.DIM, true)
			l.custom_minimum_size = Vector2(t[1], 0)
			cols.add_child(l)
		_board_list.add_child(cols)
		for f in match_node.fighters:
			if f.team != team:
				continue
			var r := HBoxContainer.new()
			var col := Color.WHITE if f.alive else Color(1, 1, 1, 0.4)
			for t in [[f.display_name + ("  ✦" if f.has_bomb else ""), 380], ["$%d" % f.money if f.team == match_node.player_team else "", 140], [str(f.kills), 90], [str(f.deaths), 90], [str(f.hp) if f.alive else "DEAD", 90]]:
				var l := UI.label(t[0], 18, col, f == match_node.player)
				l.custom_minimum_size = Vector2(t[1], 0)
				r.add_child(l)
			_board_list.add_child(r)
		_board_list.add_child(HSeparator.new())

# ---------------------------------------------------------------- pause menu
func _make_pause() -> void:
	_pause = UI.panel(Color(0.06, 0.07, 0.09, 0.95))
	_pause.anchor_left = 0.5
	_pause.anchor_right = 0.5
	_pause.anchor_top = 0.5
	_pause.anchor_bottom = 0.5
	_pause.offset_left = -220
	_pause.offset_right = 220
	_pause.offset_top = -210
	_pause.offset_bottom = 210
	_pause.visible = false
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 12)
	_pause.add_child(v)
	v.add_child(UI.label("PAUSED", 30, Color.WHITE, true))
	var resume := UI.button("RESUME", 20)
	resume.pressed.connect(func(): _set_pause(false))
	v.add_child(resume)
	var sens_l := UI.label("Sensitivity  %.2f" % Game.settings.sensitivity, 16, UI.DIM)
	v.add_child(sens_l)
	var sens := HSlider.new()
	sens.min_value = 0.2
	sens.max_value = 6.0
	sens.step = 0.05
	sens.value = Game.settings.sensitivity
	sens.value_changed.connect(func(x):
		Game.settings.sensitivity = x
		sens_l.text = "Sensitivity  %.2f" % x
		Game.save_settings())
	v.add_child(sens)
	v.add_child(UI.label("Graphics", 16, UI.DIM))
	var q := HBoxContainer.new()
	v.add_child(q)
	var qbtns := {}
	for id in Defs.GRAPHICS:
		var b := UI.button(String(Defs.GRAPHICS[id].name).to_upper(), 16)
		b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		qbtns[id] = b
		q.add_child(b)
	for id in qbtns:
		qbtns[id].pressed.connect(func():
			Game.settings.quality = id
			Game.save_settings()
			for k in qbtns:
				UI.toggle(qbtns[k], k == id))
		UI.toggle(qbtns[id], Game.settings.quality == id)
	var quit := UI.button("QUIT TO MAIN MENU", 20)
	quit.pressed.connect(func(): match_node.back_to_menu())
	v.add_child(quit)
	_root.add_child(_pause)

func _set_pause(on: bool) -> void:
	_pause.visible = on
	match_node.paused = on
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if on else Input.MOUSE_MODE_CAPTURED

func blocking() -> bool:
	return _buy.visible or _pause.visible

func _unhandled_input(e: InputEvent) -> void:
	if Game.test_mode:
		return
	if e is InputEventKey and e.pressed and not e.echo:
		if e.keycode == KEY_ESCAPE:
			if match_node.tacmap:
				match_node.close_tacmap()
			elif _buy.visible:
				_toggle_buy()
			else:
				_set_pause(not _pause.visible)
			get_viewport().set_input_as_handled()
		elif e.keycode == KEY_B and not _pause.visible and match_node.tacmap == null:
			_toggle_buy()
			get_viewport().set_input_as_handled()
	if e is InputEventMouseButton and e.pressed and Input.mouse_mode == Input.MOUSE_MODE_VISIBLE and not blocking() and match_node.tacmap == null:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

# ---------------------------------------------------------------- events from the match
func round_started() -> void:
	_banner.visible = false
	_buy.visible = false
	if match_node.player and not Game.test_mode:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	_spectating = null
	_spec.text = ""
	var mode_name: String = match_node.mode.name
	notice("ROUND %d  -  %s" % [match_node.round_num, mode_name.to_upper()], 3.0)

func round_over(winner: String, why: String) -> void:
	_banner.visible = true
	_banner_text.text = ("COUNTER-TERRORISTS WIN" if winner == "CT" else "TERRORISTS WIN")
	_banner_text.add_theme_color_override("font_color", UI.CT.lightened(0.35) if winner == "CT" else UI.T)
	_banner_sub.text = why

func match_over(winner: String) -> void:
	_banner.visible = true
	var you: String = match_node.player_team
	_banner_text.text = "DRAW" if winner == "" else ("VICTORY" if winner == you else "DEFEAT")
	_banner_sub.text = "Final score  CT %d - %d T" % [match_node.score.CT, match_node.score.T]

func kill_feed(killer: Fighter, victim: Fighter, weapon_id: String, headshot: bool) -> void:
	var p := UI.panel(Color(0, 0, 0, 0.55) if killer != match_node.player else Color(0.45, 0.08, 0.08, 0.7))
	var h := HBoxContainer.new()
	h.alignment = BoxContainer.ALIGNMENT_END
	h.add_theme_constant_override("separation", 10)
	p.add_child(h)
	if killer and killer != victim:
		h.add_child(UI.label(killer.display_name, 18, UI.CT.lightened(0.35) if killer.team == "CT" else UI.T, true))
	var icon_id: String = Defs.WEAPONS[weapon_id].icon_asset if Defs.WEAPONS.has(weapon_id) else "img_c4"
	var ic := TextureRect.new()
	ic.texture = Game.icon(icon_id, 26)
	ic.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	h.add_child(ic)
	if headshot:
		h.add_child(UI.label("◎", 18, Color.WHITE, true))
	h.add_child(UI.label(victim.display_name, 18, UI.CT.lightened(0.35) if victim.team == "CT" else UI.T, true))
	_feed.add_child(p)
	p.size_flags_horizontal = Control.SIZE_SHRINK_END
	get_tree().create_timer(7.0).timeout.connect(p.queue_free)
	while _feed.get_child_count() > 6:
		_feed.get_child(0).free()

func notice(text: String, dur := 2.5) -> void:
	_center.text = text
	_center_until = Time.get_ticks_msec() / 1000.0 + dur

func hurt_flash(_attacker: Fighter, dmg: int) -> void:
	_hurt.color.a = clampf(0.1 + dmg / 120.0, 0, 0.45)

func spectate(f: Fighter) -> void:
	_spectating = f

# ---------------------------------------------------------------- per frame
func _process(dt: float) -> void:
	if not _styled and match_node != null and match_node.get("mode") is Dictionary and not match_node.mode.is_empty():
		_styled = true
		if match_node.mode.hud_style == "classic":
			_apply_classic_style()
	var m = match_node
	var t := Time.get_ticks_msec() / 1000.0
	_fps.text = "FPS %d" % Engine.get_frames_per_second()
	if t > _center_until:
		_center.text = ""
	_hurt.color.a = maxf(0.0, _hurt.color.a - dt * 0.8)
	var tl: float = m.time_left()
	_timer.text = "%d:%02d" % [int(tl) / 60, int(tl) % 60]
	_timer.add_theme_color_override("font_color", Color(1, 0.3, 0.25) if m.phase == m.Phase.PLANTED else _base)
	if m.phase == m.Phase.FREEZE:
		_timer.add_theme_color_override("font_color", Color(0.8, 0.85, 0.95))
	_score_ct.text = str(m.score.CT)
	_score_t.text = str(m.score.T)
	_update_alive(_alive_ct, "CT")
	_update_alive(_alive_t, "T")
	var f: Fighter = m.player
	var shown: Fighter = f
	if f == null or not f.alive:
		shown = _spectating if _spectating and _spectating.alive else null
		if shown == null:
			m.next_spectate()
	if shown and shown != f:
		_spec.text = "SPECTATING  %s   (click for next)" % shown.display_name
		var cam := _spec_cam()
		cam.current = true
		var head := shown.global_position + Vector3(0, 1.75, 0)
		var want := head + Basis(Vector3.UP, shown.yaw) * Vector3(0.45, 0.35, 2.6)
		var hit := shown.get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(head, want, Game.LAYER_WORLD | Game.LAYER_CLIP | Game.LAYER_BUILD))
		cam.global_position = want if hit.is_empty() else head.lerp(hit.position, 0.85)
		cam.look_at(shown.global_position + Vector3(0, 1.45, 0) + shown.aim_dir() * 6.0)
	elif f and f.alive:
		_spec.text = ""
	if shown == null:
		return
	_hp.text = str(shown.hp)
	_hp.add_theme_color_override("font_color", Color(1, 0.35, 0.3) if shown.hp <= 20 else _base)
	_armor.text = str(shown.armor)
	_money.text = "$%d" % shown.money
	_bomb_icon.visible = shown.has_bomb
	var w := shown.current()
	var d := shown.current_def()
	if shown.slot == 5:
		_ammo.text = "C4"
		_weapon_icon.texture = Game.icon("img_c4", 46)
	elif d.id == "portalgun":
		_ammo.text = "PORTAL"
		_weapon_icon.texture = _portal_icon()
	elif not w.is_empty():
		_ammo.text = ("%d / %d" % [w.mag, w.reserve]) if int(d.mag) > 0 else ""
		_weapon_icon.texture = Game.icon(d.icon_asset, 46)
	var building: bool = m.building_enabled()
	_build_bar.visible = building and shown == f
	_build_hint.text = ""
	if building:
		for id in _mat_boxes:
			var cur: bool = shown.build_mat == id
			_mat_boxes[id].text = ("▶ " if cur else "   ") + "%s %d" % [String(Defs.BUILD_MATERIALS[id].name).to_upper(), shown.mats[id]]
			_mat_boxes[id].modulate = Color.WHITE if cur else Color(1, 1, 1, 0.55)
		var pl := f as PlayerFighter
		for c in _build_bar.get_children():
			if c is PanelContainer:
				var on: bool = pl != null and pl.build_piece == String(c.name)
				c.add_theme_stylebox_override("panel", UI.box(UI.ACCENT if on else Color(0.05, 0.06, 0.08, 0.75)))
		if pl and pl.alive:
			if pl.editing:
				_build_hint.text = "EDITING  ·  click or drag tiles to cut them out  ·  F confirm  ·  right mouse reset"
			elif pl.build_piece != "":
				_build_hint.text = "left mouse place (hold for turbo build)  ·  right mouse switch material  ·  F edit"
			else:
				_build_hint.text = "F edit your team's piece you are looking at"
	var scoped := f is PlayerFighter and (f as PlayerFighter).scoped and f.alive
	_scope.visible = scoped
	_crosshair.visible = not scoped and shown == f and not blocking()
	# plant / defuse progress
	if m.planter == shown:
		_progress.visible = true
		_progress_label.text = "PLANTING THE BOMB"
		_progress.value = 100.0 * m.plant_progress / float(m.mode.plant_time)
	elif m.defuser == shown:
		_progress.visible = true
		_progress_label.text = "DEFUSING" + (" (KIT)" if shown.has_kit else "")
		_progress.value = 100.0 * m.defuse_progress / (float(m.mode.defuse_kit_time) if shown.has_kit else float(m.mode.defuse_time))
	else:
		_progress.visible = false
		_progress_label.text = ""
		if m.phase == m.Phase.PLANTED and shown.team == "CT" and shown.global_position.distance_to(m.bomb_pos) < 1.8:
			_progress_label.text = "Hold E to defuse"
		elif shown.has_bomb and shown.slot == 5 and m.world.site_at(shown.global_position) != "":
			_progress_label.text = "Hold LEFT MOUSE to plant"
		elif shown.has_bomb and m.phase == m.Phase.LIVE and shown.slot != 5 and m.world.site_at(shown.global_position) != "":
			_progress_label.text = "Press 5 to take out the bomb"
		elif shown == f and d.id == "portalgun":
			_progress_label.text = "LEFT MOUSE blue portal  ·  RIGHT MOUSE map for the orange portal"
	if _buy.visible and _buy_money:
		_buy_money.text = "$%d" % f.money
	_score_board.visible = Input.is_physical_key_pressed(KEY_TAB) and not Game.test_mode
	if _score_board.visible:
		_fill_board()

var _cam: Camera3D
func _spec_cam() -> Camera3D:
	if _cam == null:
		_cam = Camera3D.new()
		_cam.fov = 80
		match_node.add_child(_cam)
	return _cam

func _update_alive(box: HBoxContainer, team: String) -> void:
	var fs: Array = match_node.fighters.filter(func(x): return x.team == team)
	while box.get_child_count() < fs.size():
		var r := ColorRect.new()
		r.custom_minimum_size = Vector2(30, 38)
		box.add_child(r)
	for i in fs.size():
		var c: Color = UI.CT if team == "CT" else UI.T
		(box.get_child(i) as ColorRect).color = c if fs[i].alive else Color(0.2, 0.2, 0.22, 0.6)


## Classic 1.6 look: amber numbers with a dark outline (the mode's hud_style column).
func _apply_classic_style() -> void:
	_base = AMBER
	for l in [_hp, _armor, _ammo, _timer]:
		l.add_theme_color_override("font_color", AMBER)
	_money.add_theme_color_override("font_color", AMBER)
	for l in [_hp, _armor]:
		var glyph := (l.get_parent() as HBoxContainer).get_child(0) as Label
		glyph.add_theme_color_override("font_color", AMBER)
