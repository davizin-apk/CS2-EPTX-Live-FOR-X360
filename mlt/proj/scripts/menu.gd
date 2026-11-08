extends Node3D
## Main menu in CS2's style: your agent standing on Dust II behind a dark top bar with a PLAY button,
## a play panel with mode tiles, the map, side and bot difficulty, and a green GO button.

const Defs = preload("res://scripts/gen/defs.gd")
const UI = preload("res://scripts/ui.gd")

var world: World
var cam: Camera3D
var agent: Node3D
var ui: Control
var play_panel: Control
var settings_panel: Control
var music: AudioStreamPlayer
var _t := 0.0
var _cam_base: Transform3D
var _sel := {}
var _mode_btns := {}
var _side_btns := {}
var _diff_btns := {}

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	world = World.new()
	add_child(world)
	world.setup("de_dust2", false)
	_sel = {"mode": Game.settings.mode, "side": Game.settings.side, "difficulty": Game.settings.difficulty}
	if OS.get_environment("BOTSTRIKE_NOUI") != "1":
		_build_ui()
	await get_tree().physics_frame  # collision must be in the physics space before the camera raycasts
	await get_tree().physics_frame
	_place_agent()
	music = AudioStreamPlayer.new()
	music.stream = Game.sound("mus_menu")
	if music.stream is AudioStreamWAV:
		var w: AudioStreamWAV = music.stream
		w.loop_mode = AudioStreamWAV.LOOP_FORWARD
		w.loop_end = int(w.get_length() * w.mix_rate)
	elif music.stream is AudioStreamMP3:
		(music.stream as AudioStreamMP3).loop = true
	music.volume_db = linear_to_db(maxf(Game.settings.music, 0.001))
	add_child(music)
	music.play()
	await get_tree().create_timer(1.0).timeout
	Game.melty_report("menu-shown", agent != null)
	if Game.test_mode:
		await get_tree().create_timer(2.0 if Game._shot_dir == "" else 6.0).timeout
		_sel.mode = OS.get_environment("BOTSTRIKE_MODE") if Defs.MODES.has(OS.get_environment("BOTSTRIKE_MODE")) else "classic16"
		_sel.side = "CT"
		_go()

func _place_agent() -> void:
	var sp: Array = world.spawns.CT
	var base: Transform3D = sp[0] if sp.size() > 0 else Transform3D.IDENTITY
	agent = Game.instance(Defs.TEAMS[_sel.side if _sel.side != "random" else "CT"].agent_asset) if OS.get_environment("BOTSTRIKE_NOAGENT") != "1" else null
	cam = Camera3D.new()
	cam.fov = 55
	add_child(cam)
	if agent:
		add_child(agent)
		agent.global_position = base.origin
		for n in agent.find_children("*", "Node3D", true, false):
			var nn := String(n.name).to_lower()
			if nn.contains("defusekit") or nn.contains("firstperson"):
				n.visible = false
		var ap := Game.animate_agent(agent)
		if ap:
			for a in ap.get_animation_list():
				if String(a).contains("idle"):
					ap.get_animation(a).loop_mode = Animation.LOOP_LINEAR
					ap.play(a)
					break
		var gun := Game.instance("mdl_m4a1s" if _sel.side != "T" else "mdl_ak47")
		if gun:
			agent.add_child(gun)
			gun.position = Game.grip_offset(1)
	# the camera stands in the most open direction around the agent, a few meters away
	var chest := base.origin + Vector3(0, 1.3, 0)
	var space := get_world_3d().direct_space_state
	var best_dir := -base.basis.z
	var best_len := 0.0
	for i in 16:
		var d := Vector3(sin(TAU * i / 16.0), 0, cos(TAU * i / 16.0))
		var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(chest, chest + d * 8.0, Game.LAYER_WORLD | Game.LAYER_CLIP))
		var dist: float = 8.0 if hit.is_empty() else chest.distance_to(hit.position)
		if dist > best_len:
			best_len = dist
			best_dir = d
	var side := best_dir.cross(Vector3.UP)
	var cam_pos := chest + best_dir * minf(3.4, best_len - 0.4) + Vector3(0, 0.15, 0) + side * 0.7
	cam.look_at_from_position(cam_pos, chest - Vector3(0, 0.1, 0) + side * 0.7, Vector3.UP)
	if agent:
		agent.look_at(Vector3(cam_pos.x, base.origin.y, cam_pos.z), Vector3.UP)
		agent.rotate_y(PI) # Source models face +X -> Godot +Z
		agent.rotate_y(deg_to_rad(-20))
	_cam_base = cam.global_transform
	cam.current = true
	if OS.get_environment("BOTSTRIKE_PICK") != "":
		await get_tree().process_frame
		_dev_pick(Vector2(float(OS.get_environment("BOTSTRIKE_PICK").get_slice(",", 0)), float(OS.get_environment("BOTSTRIKE_PICK").get_slice(",", 1))))

func _dev_pick(frac: Vector2) -> void:
	# developer aid: list map meshes whose bounds the view ray through a screen point crosses
	var vp := get_viewport().get_visible_rect().size
	var from := cam.project_ray_origin(vp * frac)
	var dir := cam.project_ray_normal(vp * frac)
	var hits := []
	for mi in world.find_children("*", "MeshInstance3D", true, false):
		var m: MeshInstance3D = mi
		if not m.visible or m.mesh == null:
			continue
		var box := m.global_transform * m.get_aabb()
		var hit = box.intersects_ray(from, dir)
		if hit != null:
			var names := []
			for s in m.mesh.get_surface_count():
				var mat := m.get_active_material(s)
				names.append("%s%s" % [mat.resource_name if mat else "?", "(T%d)" % mat.transparency if mat is BaseMaterial3D else ""])
			hits.append([from.distance_to(hit), m.name, names, box.size])
	hits.sort_custom(func(a, b): return a[0] < b[0])
	for h in hits.slice(0, 25):
		Game.log_line("pick %.1fm %s %s size %s" % h)

func _process(dt: float) -> void:
	_t += dt
	if cam:
		cam.global_transform = _cam_base.translated_local(Vector3(sin(_t * 0.15) * 0.08, sin(_t * 0.21) * 0.03, 0))

# ---------------------------------------------------------------- UI
func _build_ui() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	ui = Control.new()
	ui.set_anchors_preset(Control.PRESET_FULL_RECT)
	ui.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(ui)
	# soft vignette on the left so the text reads
	var shade := TextureRect.new()
	var grad := GradientTexture2D.new()
	var g := Gradient.new()
	g.set_color(0, Color(0.02, 0.03, 0.05, 0.85))
	g.set_color(1, Color(0.02, 0.03, 0.05, 0.0))
	grad.gradient = g
	grad.fill_from = Vector2(0, 0)
	grad.fill_to = Vector2(1, 0)
	shade.texture = grad
	shade.set_anchors_preset(Control.PRESET_FULL_RECT)
	shade.anchor_right = 0.55
	shade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	ui.add_child(shade)
	# top bar
	var bar := PanelContainer.new()
	var bs := StyleBoxFlat.new()
	bs.bg_color = Color(0.03, 0.04, 0.06, 0.88)
	bs.border_color = Color(1, 1, 1, 0.06)
	bs.border_width_bottom = 1
	bar.add_theme_stylebox_override("panel", bs)
	bar.anchor_right = 1.0
	bar.offset_bottom = 76
	ui.add_child(bar)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	bar.add_child(row)
	var play := UI.button("  ▶   PLAY  ", 24, Color(1, 1, 1, 0.1), Color(1, 1, 1, 0.22))
	play.custom_minimum_size = Vector2(190, 76)
	play.pressed.connect(func(): _show(play_panel))
	row.add_child(play)
	var sep := VSeparator.new()
	row.add_child(sep)
	var logo := UI.label("  BOT STRIKE", 26, Color.WHITE, true)
	logo.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(logo)
	var gear := UI.button("⚙", 28, Color(0, 0, 0, 0), Color(1, 1, 1, 0.12))
	gear.custom_minimum_size = Vector2(76, 76)
	gear.pressed.connect(func(): _show(settings_panel))
	row.add_child(gear)
	var power := UI.button("⏻", 28, Color(0, 0, 0, 0), Color(0.8, 0.2, 0.2, 0.5))
	power.custom_minimum_size = Vector2(76, 76)
	power.pressed.connect(func(): get_tree().quit())
	row.add_child(power)
	# lower-left card
	var card := VBoxContainer.new()
	card.anchor_top = 1.0
	card.anchor_bottom = 1.0
	card.offset_left = 48
	card.offset_top = -220
	card.add_theme_constant_override("separation", 6)
	ui.add_child(card)
	card.add_child(UI.label("5v5 VS BOTS", 46, Color.WHITE, true))
	card.add_child(UI.label("Dust II  ·  Classic 1.6, Competitive, Build Fight or Portal Strike", 22, Color(0.85, 0.87, 0.9)))
	card.add_child(UI.label("Maps, agents, guns and sounds are read from your own Counter-Strike 2 install.", 16, UI.DIM))
	var quick := UI.button("  PLAY NOW  ", 22, UI.GO, UI.GO.lightened(0.15), Color.WHITE)
	quick.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	quick.custom_minimum_size = Vector2(220, 52)
	quick.pressed.connect(func(): _show(play_panel))
	card.add_child(quick)
	_build_play_panel()
	_build_settings_panel()

func _show(p: Control) -> void:
	for c in [play_panel, settings_panel]:
		c.visible = c == p and not c.visible

func _overlay(title: String) -> Array:
	var o := PanelContainer.new()
	o.add_theme_stylebox_override("panel", UI.box(Color(0.04, 0.05, 0.07, 0.93), 0, Color(0, 0, 0, 0)))
	o.anchor_right = 1.0
	o.anchor_bottom = 1.0
	o.offset_top = 76
	o.visible = false
	ui.add_child(o)
	var m := MarginContainer.new()
	for side in ["left", "right", "top", "bottom"]:
		m.add_theme_constant_override("margin_" + side, 56)
	o.add_child(m)
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", 22)
	m.add_child(v)
	var head := HBoxContainer.new()
	v.add_child(head)
	var t := UI.label(title, 36, Color.WHITE, true)
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(t)
	var close := UI.button("✕", 22, Color(0, 0, 0, 0), Color(1, 1, 1, 0.12))
	close.pressed.connect(func(): o.visible = false)
	head.add_child(close)
	return [o, v]

func _build_play_panel() -> void:
	var r := _overlay("PLAY  ·  VS BOTS")
	play_panel = r[0]
	var v: VBoxContainer = r[1]
	v.add_child(UI.label("GAME MODE", 16, UI.DIM, true))
	# Two columns so all four mode tiles (Classic 1.6 included) fit a 1080p screen.
	var modes := GridContainer.new()
	modes.columns = 2
	modes.add_theme_constant_override("h_separation", 16)
	modes.add_theme_constant_override("v_separation", 12)
	v.add_child(modes)
	for id in Defs.MODES:
		var row: Dictionary = Defs.MODES[id]
		var b := UI.button("", 20)
		b.custom_minimum_size = Vector2(520, 120)
		var bv := VBoxContainer.new()
		bv.set_anchors_preset(Control.PRESET_FULL_RECT)
		bv.offset_left = 22
		bv.offset_top = 18
		bv.offset_right = -22
		bv.mouse_filter = Control.MOUSE_FILTER_IGNORE
		b.add_child(bv)
		bv.add_child(UI.label(String(row.name).to_upper(), 26, Color.WHITE, true))
		var tl := UI.label(row.tagline, 17, Color(0.85, 0.87, 0.9))
		tl.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		bv.add_child(tl)
		var extra := "First to %d rounds  ·  %d v %d" % [int(row.rounds_to_win), int(row.team_size), int(row.team_size)]
		if row.building:
			extra += "  ·  Q wall  X floor  C stairs  V cone  ·  F edit"
		if row.portals:
			extra += "  ·  buy the Portal Gun (4)"
		bv.add_child(UI.label(extra, 15, UI.DIM))
		b.pressed.connect(func(): _pick("mode", id))
		modes.add_child(b)
		_mode_btns[id] = b
	v.add_child(UI.label("MAP", 16, UI.DIM, true))
	var map_tile := PanelContainer.new()
	map_tile.add_theme_stylebox_override("panel", UI.box(UI.ACCENT.darkened(0.55), 2, UI.ACCENT))
	map_tile.custom_minimum_size = Vector2(320, 90)
	map_tile.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	var mv := VBoxContainer.new()
	map_tile.add_child(mv)
	mv.add_child(UI.label("DUST II", 26, Color.WHITE, true))
	mv.add_child(UI.label("de_dust2  ·  from your CS2", 15, UI.DIM))
	v.add_child(map_tile)
	var opts := HBoxContainer.new()
	opts.add_theme_constant_override("separation", 60)
	v.add_child(opts)
	var side_box := VBoxContainer.new()
	opts.add_child(side_box)
	side_box.add_child(UI.label("YOUR SIDE", 16, UI.DIM, true))
	var sides := HBoxContainer.new()
	side_box.add_child(sides)
	for s in [["CT", "COUNTER-TERRORIST"], ["T", "TERRORIST"], ["random", "RANDOM"]]:
		var b := UI.button(s[1], 17)
		b.custom_minimum_size = Vector2(0, 46)
		b.pressed.connect(func(): _pick("side", s[0]))
		sides.add_child(b)
		_side_btns[s[0]] = b
	var diff_box := VBoxContainer.new()
	opts.add_child(diff_box)
	diff_box.add_child(UI.label("BOT DIFFICULTY", 16, UI.DIM, true))
	var diffs := HBoxContainer.new()
	diff_box.add_child(diffs)
	for id in Defs.BOT_SKILLS:
		var b := UI.button(String(Defs.BOT_SKILLS[id].name).to_upper(), 17)
		b.custom_minimum_size = Vector2(120, 46)
		b.pressed.connect(func(): _pick("difficulty", id))
		diffs.add_child(b)
		_diff_btns[id] = b
	var spacer := Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	v.add_child(spacer)
	var foot := HBoxContainer.new()
	foot.alignment = BoxContainer.ALIGNMENT_END
	v.add_child(foot)
	var keys := UI.label("WASD move · SPACE jump · CTRL crouch · SHIFT walk · B buy · R reload · E defuse · 5 bomb · TAB scores", 15, UI.DIM)
	keys.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	foot.add_child(keys)
	var go := UI.button("GO", 30, UI.GO, UI.GO.lightened(0.15), Color.WHITE)
	go.custom_minimum_size = Vector2(260, 70)
	go.pressed.connect(_go)
	foot.add_child(go)
	_refresh_picks()

func _pick(k: String, v: String) -> void:
	_sel[k] = v
	_refresh_picks()

func _refresh_picks() -> void:
	for id in _mode_btns:
		UI.toggle(_mode_btns[id], _sel.mode == id)
	for id in _side_btns:
		UI.toggle(_side_btns[id], _sel.side == id)
	for id in _diff_btns:
		UI.toggle(_diff_btns[id], _sel.difficulty == id)

func _build_settings_panel() -> void:
	var r := _overlay("SETTINGS")
	settings_panel = r[0]
	var v: VBoxContainer = r[1]
	v.add_child(UI.label("GRAPHICS QUALITY", 16, UI.DIM, true))
	var q := HBoxContainer.new()
	v.add_child(q)
	var qbtns := {}
	for id in Defs.GRAPHICS:
		var b := UI.button(String(Defs.GRAPHICS[id].name).to_upper(), 17)
		b.custom_minimum_size = Vector2(140, 44)
		qbtns[id] = b
		q.add_child(b)
	for id in qbtns:
		qbtns[id].pressed.connect(func():
			Game.settings.quality = id
			Game.save_settings()
			for k in qbtns:
				UI.toggle(qbtns[k], k == id))
		UI.toggle(qbtns[id], Game.settings.quality == id)
	v.add_child(UI.label("Low turns off shadows and effects and renders at 60% resolution with FSR upscaling, for slower PCs.", 15, UI.DIM))
	_slider(v, "Render scale (0 = use the preset)", "render_scale", 0.0, 1.0, 0.05)
	_slider(v, "Mouse sensitivity (CS2 scale)", "sensitivity", 0.2, 6.0, 0.05)
	_slider(v, "Viewmodel right / left", "vm_x", -0.1, 0.15, 0.01)
	_slider(v, "Viewmodel up / down", "vm_y", -0.12, 0.06, 0.01)
	_slider(v, "Viewmodel back / forward", "vm_z", -0.05, 0.2, 0.01)
	_slider(v, "Field of view", "fov", 70, 110, 1)
	_slider(v, "Master volume", "volume", 0, 1, 0.05)
	_slider(v, "Music volume", "music", 0, 1, 0.05)
	var fs := CheckButton.new()
	fs.text = "Fullscreen"
	fs.button_pressed = Game.settings.fullscreen
	fs.add_theme_font_override("font", Game.font("font_ui"))
	fs.add_theme_font_size_override("font_size", 18)
	fs.toggled.connect(func(on):
		Game.settings.fullscreen = on
		Game.save_settings())
	v.add_child(fs)

func _slider(v: VBoxContainer, text: String, key: String, lo: float, hi: float, step: float) -> void:
	var l := UI.label("%s   %s" % [text, str(snappedf(Game.settings[key], step))], 18, Color(0.85, 0.87, 0.9))
	v.add_child(l)
	var s := HSlider.new()
	s.min_value = lo
	s.max_value = hi
	s.step = step
	s.value = Game.settings[key]
	s.custom_minimum_size = Vector2(520, 24)
	s.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	s.value_changed.connect(func(x):
		Game.settings[key] = x
		l.text = "%s   %s" % [text, str(snappedf(x, step))]
		if key == "music" and music:
			music.volume_db = linear_to_db(maxf(x, 0.001))
		Game.save_settings())
	v.add_child(s)

func _go() -> void:
	Game.settings.mode = _sel.mode
	Game.settings.side = _sel.side
	Game.settings.difficulty = _sel.difficulty
	Game.save_settings()
	Game.match_cfg = _sel.duplicate()
	get_tree().change_scene_to_file("res://scenes/match.tscn")
