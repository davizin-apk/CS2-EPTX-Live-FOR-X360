extends Node
## Global state: where CS2 and the cache are, settings, asset loading, logging, Melty test mode.

const Defs = preload("res://scripts/gen/defs.gd")
const U2M := 0.0254
const PREP_VERSION := 6
const MELTY_SLUG := "bot-strike-dust-ii"

const LAYER_WORLD := 1
const LAYER_CLIP := 2
const LAYER_FIGHTER := 4
const LAYER_BUILD := 8

var cs2_root := ""
var csgo := ""
var cache := ""
var vrf := ""
var patch := ""
var settings := {"sensitivity": 1.6, "fov": 90.0, "volume": 0.5, "music": 0.35, "difficulty": "normal", "side": "CT", "mode": "classic", "fullscreen": true, "quality": "medium", "render_scale": 0.0, "vm_x": 0.05, "vm_y": -0.03, "vm_z": 0.08}
var match_cfg := {}
var map_scene: PackedScene
var world_ref: Node   # the World currently on screen, for live graphics changes
var test_mode := false
var melty_checks := ""
var _reported := {}
var _scenes := {}
var _sounds := {}
var _tex := {}
var _fonts := {}
var _log: FileAccess

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	_log = FileAccess.open("user://botstrike.log", FileAccess.WRITE)
	melty_start()
	test_mode = melty_checks != "" or OS.get_environment("BOTSTRIKE_AUTOPLAY") == "1"
	_find_paths()
	load_settings()
	if OS.get_environment("BOTSTRIKE_NOVSYNC") == "1":
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	if OS.get_environment("BOTSTRIKE_Q") != "":
		settings.quality = OS.get_environment("BOTSTRIKE_Q")
		apply_graphics()
	if test_mode:
		add_child(preload("res://scripts/testmode.gd").new())
	log_line("Bot Strike started. cs2=%s patch=%s vrf=%s cache=%s test=%s" % [cs2_root, patch, vrf, cache, test_mode])

var _last_frame := 0

var _shot_dir := OS.get_environment("BOTSTRIKE_SHOTS")
var _scene_t := 0.0
var _scene_name := ""
var _shots_done := {}

func _shots(dt: float) -> void:
	# developer capture mode: save what the game draws at fixed moments, then quit
	var sc := get_tree().current_scene
	if sc == null:
		return
	if sc.name != _scene_name:
		_scene_name = sc.name
		_scene_t = 0.0
	_scene_t += dt
	if int(_scene_t * 0.5) != int((_scene_t - dt) * 0.5):
		log_line("bench %s t=%d fps=%d draws=%d prims=%d process=%.1fms physics=%.1fms nav=%.1fms" % [sc.name, int(_scene_t), Engine.get_frames_per_second(), RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME), RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME), Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0, Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0, Performance.get_monitor(Performance.TIME_NAVIGATION_PROCESS) * 1000.0])
	if sc.name == "match" and _scene_t >= 20.0 and not _shots_done.has("census"):
		_shots_done["census"] = true
		var counts := {}
		var procs := {}
		for n in sc.find_children("*", "", true, false):
			counts[n.get_class()] = counts.get(n.get_class(), 0) + 1
			if n.is_processing() or n.is_physics_processing() or n.is_processing_internal():
				procs[n.get_class()] = procs.get(n.get_class(), 0) + 1
		log_line("census total=%d %s" % [sc.find_children("*", "", true, false).size(), str(counts)])
		log_line("census processing %s" % str(procs))
	var plan := {"menu": [4.0], "match": [14.0, 24.0, 30.2, 44.0]}
	for at in plan.get(String(sc.name), []):
		var key := "%s_%d" % [sc.name, int(at)]
		if _scene_t >= at and not _shots_done.has(key):
			_shots_done[key] = true
			get_viewport().get_texture().get_image().save_png(_shot_dir.path_join(key + ".png"))
			log_line("shot " + key)
	if OS.get_environment("BOTSTRIKE_FP") == "1" and sc.name == "match":
		var pl = sc.get("player")
		if pl and pl.alive:
			pl.frozen = false
			for ev in [[26.0, "fire"], [26.06, "shot:fire_a"], [26.12, "shot:fire_b"], [26.5, "fire"], [26.62, "shot:shells"], [29.0, "reload"], [29.8, "shot:reload_a"], [30.6, "shot:reload_b"]]:
				if _scene_t >= ev[0] and not _shots_done.has(str(ev[0])):
					_shots_done[str(ev[0])] = true
					if ev[1] == "fire":
						pl.next_fire = 0.0
						pl.fire()
					elif ev[1] == "reload":
						pl.weapons[pl.slot].mag = 3
						pl.start_reload()
					else:
						get_viewport().get_texture().get_image().save_png(_shot_dir.path_join(String(ev[1]).substr(5) + ".png"))
	if OS.get_environment("BOTSTRIKE_DEMO") != "" and sc.name == "match":
		_demo(sc, OS.get_environment("BOTSTRIKE_DEMO"))
	if _shots_done.has("match_44"):
		get_tree().quit()

## Developer capture of the build and portal features: drives the player seat through them and saves shots.
func _demo(sc: Node, kind: String) -> void:
	var pl = sc.get("player")
	if pl == null or not pl.alive:
		return
	var steps := []
	if kind == "build":
		steps = [[15.0, "wall"], [15.3, "place"], [15.5, "ramp"], [15.6, "pitch_down"], [16.2, "shot:build_ghost"], [16.4, "place"], [16.6, "floor"], [16.8, "place"], [17.0, "cone"], [17.2, "place"], [17.4, "unequip"], [17.5, "pitch_0"], [18.0, "shot:build_pieces"], [18.2, "edit"], [18.4, "sel"], [18.9, "shot:build_edit"], [19.1, "edit"], [19.8, "shot:build_door"], [20.0, "brick"], [20.2, "wall"], [20.3, "turn_left"], [20.5, "place"], [21.6, "shot:build_brick"]]
	else:
		steps = [[15.0, "portalgun"], [15.4, "fire"], [16.0, "map"], [17.0, "shot:portal_map"], [17.3, "orange"], [18.0, "shot:portal_after"], [18.2, "turn"], [19.0, "shot:portal_view"], [19.1, "close"], [19.7, "shot:portal_close"], [19.8, "target"], [20.1, "target"], [20.2, "ak"], [20.3, "shoot"], [20.35, "shot:portal_shoot"], [20.7, "hp"], [20.8, "portalgun"], [21.0, "walkin"], [23.0, "shot:portal_through"]]
	for ev in steps:
		var k := "demo%s" % str(ev[0])
		if _scene_t < ev[0] or _shots_done.has(k):
			continue
		_shots_done[k] = true
		var what: String = ev[1]
		pl.frozen = false
		log_line("demo " + what)
		match what:
			"wall", "floor", "ramp", "cone":
				pl._build_select(what)
			"place":
				pl._place()
			"pitch_down":
				pl.pitch = -0.25
			"pitch_0":
				pl.pitch = 0.0
			"unequip":
				pl._select(2)
			"brick":
				pl._build_select("wall")
				pl._cycle_material()
			"turn_left":
				pl.yaw += PI / 2
			"edit":
				pl._toggle_edit()
			"sel":
				if pl.editing:
					pl.edit_sel = [1, 4]
			"portalgun":
				pl.weapons[4] = {"id": "portalgun", "mag": 0, "reserve": 0}
				pl._select(4)
				pl.pitch = -0.05
			"fire":
				pl.next_fire = 0.0
				pl.fire()
			"map":
				sc.open_tacmap(pl)
			"orange":
				var tm = sc.tacmap
				if tm and not tm._recs.is_empty():
					sc.place_orange(pl, tm._recs[0].pos, tm._recs[0].yaw)
				elif tm:
					sc.place_orange(pl, sc.world.random_point_in_site("A"), 0.0)
			"turn":
				var bl = sc.all_portals().filter(func(x): return x.color_id == "blue")
				if not bl.is_empty():
					var d: Vector3 = bl[0].global_position - pl.eye_pos()
					pl.yaw = atan2(-d.x, -d.z)
					pl.pitch = atan2(d.y, Vector2(d.x, d.z).length())
			"close":
				var bl = sc.all_portals().filter(func(x): return x.color_id == "blue")
				if not bl.is_empty():
					var n: Vector3 = bl[0].normal()
					n = Vector3(n.x, 0, n.z).normalized()
					pl.global_position = bl[0].global_position + n * 3.5 - Vector3(0, 1.1, 0)
					pl.velocity = Vector3.ZERO
					var d: Vector3 = bl[0].global_position - (pl.global_position + Vector3(0, 1.64, 0))
					pl.yaw = atan2(-d.x, -d.z)
					pl.pitch = atan2(d.y, Vector2(d.x, d.z).length())
			"target":
				# an enemy standing out of the orange portal, right where a shot through the blue one comes out
				var bl = sc.all_portals().filter(func(x): return x.color_id == "blue")
				var foe = null
				for f in sc.fighters:
					if f != pl and f.alive and f.team != pl.team:
						foe = f
						break
				if not bl.is_empty() and foe:
					var m: Transform3D = bl[0].through()
					var nd: Vector3 = (m.basis * pl.aim_dir()).normalized()
					var chest: Vector3 = m * bl[0].global_position + nd * 4.0
					foe.global_position = chest - Vector3(0, 1.2, 0)
					foe.velocity = Vector3.ZERO
					foe.frozen = true
					set_meta("demo_foe", foe)
					log_line("demo target %s at %s hp %d" % [foe.display_name, str(foe.global_position), foe.hp])
			"ak":
				pl.weapons[1] = {"id": "ak47", "mag": 30, "reserve": 90}
				pl._select(1)
			"shoot":
				pl.next_fire = 0.0
				pl.recoil = 0.0
				pl.fire()
			"hp":
				var foe = get_meta("demo_foe", null)
				if foe:
					log_line("demo target hp now %d alive %s" % [foe.hp, str(foe.alive)])
					foe.frozen = false
			"walkin":
				var bl = sc.all_portals().filter(func(x): return x.color_id == "blue")
				if not bl.is_empty():
					pl.global_position = bl[0].global_position + bl[0].normal() * 0.3 - Vector3(0, 0.9 if absf(bl[0].normal().y) < 0.5 else 0.0, 0)
					pl.velocity = -bl[0].normal() * 2.0
			_:
				if what.begins_with("shot:"):
					get_viewport().get_texture().get_image().save_png(_shot_dir.path_join(what.substr(5) + ".png"))

func _process(_dt: float) -> void:
	if _shot_dir != "":
		_shots(_dt)
	var now := Time.get_ticks_msec()
	if _last_frame > 0 and now - _last_frame > 500:
		log_line("long frame: %d ms in %s" % [now - _last_frame, get_tree().current_scene.name if get_tree().current_scene else "?"])
	_last_frame = now

# ---------------------------------------------------------------- Melty test mode
func melty_start() -> void:
	var test := OS.get_environment("LOCALAPPDATA").path_join("Melty/test/%s.json" % MELTY_SLUG)
	if not FileAccess.file_exists(test):
		return # not a test run
	var data = JSON.parse_string(FileAccess.get_file_as_string(test))
	if data is Dictionary and data.has("checks"):
		melty_checks = data["checks"]
	melty_report("mod-loads", true)

func melty_report(check: String, passed: bool, why := "") -> void:
	if _reported.get(check, false):
		return
	_reported[check] = passed
	log_line("check %s %s %s" % [check, "pass" if passed else "fail", why])
	if melty_checks == "":
		return
	var file := FileAccess.open(melty_checks, FileAccess.READ_WRITE)
	if file == null:
		return
	file.seek_end()
	file.store_string(("pass " if passed else "fail ") + check + ("" if passed or why == "" else ": " + why) + "\n")
	file.close()

func log_line(s: String) -> void:
	print(s)
	if _log:
		_log.store_line("[%s] %s" % [Time.get_time_string_from_system(), s])
		_log.flush()

# ---------------------------------------------------------------- paths
func _find_paths() -> void:
	var arg := ""
	for a in OS.get_cmdline_args() + OS.get_cmdline_user_args():
		if a.begins_with("--cs2="):
			arg = a.substr(6).strip_edges().trim_suffix("\"").trim_prefix("\"")
	if arg == "":
		arg = OS.get_environment("BOTSTRIKE_CS2")
	var candidates: Array[String] = []
	if arg != "":
		candidates.append(arg)
	candidates.append_array(_steam_cs2_folders())
	for c in candidates:
		var root := _cs2_root_of(c.replace("\\", "/"))
		if root != "":
			cs2_root = root
			break
	if cs2_root != "":
		csgo = cs2_root.path_join("game/csgo")
		var inf := FileAccess.get_file_as_string(csgo.path_join("steam.inf"))
		for line in inf.split("\n"):
			if line.begins_with("PatchVersion="):
				patch = line.substr(13).strip_edges()
	cache = OS.get_user_data_dir().path_join("cs2cache")
	var exe_dir := OS.get_executable_path().get_base_dir()
	for v in [exe_dir.path_join("vrf/Source2Viewer-CLI.exe"), OS.get_environment("BOTSTRIKE_VRF"), "C:/mlt/tools/vrf/Source2Viewer-CLI.exe"]:
		if v != "" and FileAccess.file_exists(v):
			vrf = v
			break

func _cs2_root_of(p: String) -> String:
	p = p.trim_suffix("/")
	for root in [p, p.get_base_dir(), p.get_base_dir().get_base_dir()]:
		if root != "" and FileAccess.file_exists(root.path_join("game/csgo/pak01_dir.vpk")):
			return root
	return ""

func _steam_cs2_folders() -> Array[String]:
	var out: Array[String] = []
	for steam in ["C:/Program Files (x86)/Steam", "C:/Program Files/Steam"]:
		var vdf: String = steam.path_join("steamapps/libraryfolders.vdf")
		if not FileAccess.file_exists(vdf):
			continue
		for line in FileAccess.get_file_as_string(vdf).split("\n"):
			line = line.strip_edges()
			if line.begins_with("\"path\""):
				var parts := line.split("\"")
				if parts.size() > 3:
					out.append(parts[3].replace("\\\\", "/").path_join("steamapps/common/Counter-Strike Global Offensive"))
	return out

# ---------------------------------------------------------------- settings
func load_settings() -> void:
	var cfg := ConfigFile.new()
	if cfg.load("user://settings.cfg") == OK:
		for k in settings.keys():
			settings[k] = cfg.get_value("s", k, settings[k])
	apply_settings()

func save_settings() -> void:
	var cfg := ConfigFile.new()
	for k in settings.keys():
		cfg.set_value("s", k, settings[k])
	cfg.save("user://settings.cfg")
	apply_settings()

## Applies the graphics preset (graphics sheet) and render scale to the viewport and current world.
func apply_graphics() -> void:
	if not Defs.GRAPHICS.has(settings.quality):
		settings.quality = "medium"
	var g: Dictionary = Defs.GRAPHICS[settings.quality]
	var vp := get_viewport()
	var scale_3d: float = settings.render_scale if float(settings.render_scale) > 0.0 else float(g.render_scale)
	# on the OpenGL fallback (old GPUs like the Radeon HD 6000 series) render scaling and MSAA add a
	# full-screen copy every frame, which costs more than the pixels they save
	var gl := RenderingServer.get_current_rendering_method() == "gl_compatibility"
	if gl:
		scale_3d = 1.0
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR if scale_3d < 0.99 and g.fsr else Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = clampf(scale_3d, 0.4, 1.0)
	vp.msaa_3d = Viewport.MSAA_2X if int(g.msaa) >= 2 and not gl else Viewport.MSAA_DISABLED
	vp.mesh_lod_threshold = float(g.lod_threshold)
	RenderingServer.directional_shadow_atlas_set_size(int(g.shadow_atlas), true)
	if OS.get_environment("BOTSTRIKE_PLAINVP") == "1":
		vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
		vp.scaling_3d_scale = 1.0
		vp.msaa_3d = Viewport.MSAA_DISABLED
	if is_instance_valid(world_ref) and world_ref.has_method("apply_quality"):
		world_ref.apply_quality(g)

func apply_settings() -> void:
	apply_graphics()
	AudioServer.set_bus_volume_db(0, linear_to_db(maxf(settings.volume, 0.001)))
	if not test_mode and DisplayServer.get_name() != "headless":
		DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_EXCLUSIVE_FULLSCREEN if settings.fullscreen else DisplayServer.WINDOW_MODE_WINDOWED)

# ---------------------------------------------------------------- assets from the player's CS2
func asset_path(id: String) -> String:
	var row: Dictionary = Defs.ASSETS[id]
	if row.kind == "file":
		return csgo.path_join(row.path)
	var outs := String(row.out).split("|")
	var p := cache.path_join(outs[0])
	if outs.size() > 1:
		# alternatives (CS2 renames files between updates): the first real file wins
		for o in outs:
			var c := cache.path_join(o)
			var fa := FileAccess.open(c, FileAccess.READ)
			if fa and fa.get_length() > 1024:
				return c
	if row.kind == "sound" and not FileAccess.file_exists(p):
		# Source2Viewer writes a sound as .wav or .mp3 depending on how CS2 stores it
		for ext in [".wav", ".mp3"]:
			var alt: String = p.get_basename() + ext
			if FileAccess.file_exists(alt):
				return alt
	return p

func scene(id: String) -> PackedScene:
	if not Defs.ASSETS.has(id):
		return null   # "none": nothing to read from CS2 (the Portal Gun)
	if _scenes.has(id):
		return _scenes[id]
	var ps: PackedScene = null
	var path := asset_path(id)
	if FileAccess.file_exists(path):
		var doc := GLTFDocument.new()
		var st := GLTFState.new()
		if doc.append_from_file(path, st) == OK:
			var node := doc.generate_scene(st)
			if node:
				add_lods(node)
				ps = PackedScene.new()
				ps.pack(node)
				node.free()
	if ps == null:
		log_line("asset missing: %s (%s)" % [id, path])
	_scenes[id] = ps
	return ps

## Gives every dense mesh under root lower-detail levels, so far-away agents, guns and map pieces
## draw a fraction of their triangles (Godot picks the level by on-screen size).
static func add_lods(root: Node, min_tris := 1500) -> int:
	var done := 0
	for mi in root.find_children("*", "MeshInstance3D", true, false):
		var am := (mi as MeshInstance3D).mesh as ArrayMesh
		if am == null or am.get_blend_shape_count() > 0:
			continue
		var tris := 0
		for s in am.get_surface_count():
			var idx = am.surface_get_arrays(s)[Mesh.ARRAY_INDEX]
			tris += (idx.size() / 3) if idx != null else 0
		if tris < min_tris:
			continue
		var im := ImporterMesh.new()
		for s in am.get_surface_count():
			im.add_surface(am.surface_get_primitive_type(s), am.surface_get_arrays(s), [], {}, am.surface_get_material(s), am.surface_get_name(s), am.surface_get_format(s))
		im.generate_lods(25.0, 60.0, [])
		var lod_mesh := im.get_mesh()
		if lod_mesh:
			(mi as MeshInstance3D).mesh = lod_mesh
			done += 1
	return done

func instance(id: String) -> Node3D:
	var ps := scene(id)
	return ps.instantiate() if ps else null

func sound(id: String) -> AudioStream:
	if not Defs.ASSETS.has(id):
		return null   # "none": nothing to read from CS2 (the Portal Gun)
	if _sounds.has(id):
		return _sounds[id]
	var path := asset_path(id)
	var s: AudioStream = null
	if FileAccess.file_exists(path):
		if path.ends_with(".wav"):
			s = AudioStreamWAV.load_from_file(path)
		elif path.ends_with(".mp3"):
			s = AudioStreamMP3.load_from_file(path)
	_sounds[id] = s
	return s

func icon(id: String, height := 48) -> Texture2D:
	if not Defs.ASSETS.has(id):
		return null
	var key := "%s@%d" % [id, height]
	if _tex.has(key):
		return _tex[key]
	var t: Texture2D = null
	var path := asset_path(id)
	if FileAccess.file_exists(path):
		var img := Image.new()
		var buf := FileAccess.get_file_as_bytes(path)
		var ok := ERR_FILE_UNRECOGNIZED
		if path.ends_with(".svg"):
			ok = img.load_svg_from_buffer(buf, float(height) / 32.0)
		else:
			ok = img.load_png_from_buffer(buf)
		if ok == OK:
			t = ImageTexture.create_from_image(img)
	_tex[key] = t
	return t

func font(id: String) -> Font:
	if _fonts.has(id):
		return _fonts[id]
	var f: Font = ThemeDB.fallback_font
	var path := asset_path(id)
	if FileAccess.file_exists(path):
		var ff := FontFile.new()
		if ff.load_dynamic_font(path) == OK:
			f = ff
	_fonts[id] = f
	return f

func play_ui(id: String, vol := 0.0, bus := "Master") -> AudioStreamPlayer:
	var s := sound(id)
	if s == null:
		return null
	var p := AudioStreamPlayer.new()
	p.stream = s
	p.volume_db = vol
	p.bus = bus
	add_child(p)
	p.play()
	p.finished.connect(p.queue_free)
	return p

## Where a held gun sits on an agent, in the agent model's own space (it faces +Z).
## CS2's rifle idle and run poses keep both hands at chest height pointing forward.
static func grip_offset(slot: int) -> Vector3:
	match slot:
		1: return Vector3(-0.1, 1.29, 0.02)
		2: return Vector3(-0.05, 1.33, 0.38)
		3: return Vector3(-0.14, 1.22, 0.32)
	return Vector3(-0.1, 1.29, 0.1)

## The bone guns attach to: weapon_hand_r on older agents, hand_R on late-2026 ones.
func hand_bone(sk: Skeleton3D) -> int:
	for n in ["weapon_hand_r", "weapon_hand_R", "hand_R", "hand_r"]:
		var b := sk.find_bone(n)
		if b >= 0:
			return b
	return -1

# ---------------------------------------------------------------- agent animation
## Returns an AnimationPlayer for an agent model. Older CS2 builds export animations inside the
## model; newer ones keep them as separate clips, which are remapped here by bone name.
func animate_agent(model: Node) -> AnimationPlayer:
	var aps := model.find_children("*", "AnimationPlayer", true, false)
	if aps.size() > 0 and (aps[0] as AnimationPlayer).get_animation_list().size() > 0:
		return aps[0]
	var skels := model.find_children("*", "Skeleton3D", true, false)
	if skels.is_empty():
		return null
	var sk: Skeleton3D = skels[0]
	var lower := {}
	for b in sk.get_bone_count():
		lower[sk.get_bone_name(b).to_lower()] = sk.get_bone_name(b)
	var ap := AnimationPlayer.new()
	model.add_child(ap)
	ap.root_node = NodePath("..")
	var sk_path := model.get_path_to(sk)
	var lib := AnimationLibrary.new()
	for pair in [["idle", "anim_idle", true], ["run", "anim_run", true], ["fire", "anim_fire", false]]:
		var clip: Dictionary = _clip_animation(pair[1])
		if clip.is_empty():
			continue
		var src: Animation = clip.anim
		# clips are authored under a Z-up root_motion bone; the agent's root_motion is identity,
		# so the pelvis (root_motion's child) is carried into the agent's frame by that rest rotation
		var up: Basis = clip.up
		var a := Animation.new()
		a.length = src.length
		a.loop_mode = Animation.LOOP_LINEAR if pair[2] else Animation.LOOP_NONE
		for t in src.get_track_count():
			var path := String(src.track_get_path(t))
			var bone := path.get_slice(":", 1).to_lower()
			var kind := src.track_get_type(t)
			if not lower.has(bone) or bone == "root_motion":
				continue
			# rotations carry the motion; positions only for the pelvis so proportions stay the agent's
			if kind == Animation.TYPE_POSITION_3D and bone != "pelvis":
				continue
			if kind != Animation.TYPE_ROTATION_3D and kind != Animation.TYPE_POSITION_3D:
				continue
			var nt := a.add_track(kind)
			a.track_set_path(nt, NodePath("%s:%s" % [sk_path, lower[bone]]))
			a.track_set_interpolation_type(nt, src.track_get_interpolation_type(t))
			for k in src.track_get_key_count(t):
				var v = src.track_get_key_value(t, k)
				if bone == "pelvis":
					if kind == Animation.TYPE_POSITION_3D:
						v = up * (v as Vector3)
					else:
						v = (up.get_rotation_quaternion() * (v as Quaternion)).normalized()
				a.track_insert_key(nt, src.track_get_key_time(t, k), v)
		lib.add_animation(pair[0], a)
	ap.add_animation_library("", lib)
	return ap

var _clip_cache := {}
func _clip_animation(id: String) -> Dictionary:
	if _clip_cache.has(id):
		return _clip_cache[id]
	var out := {}
	var node := instance(id) if Defs.ASSETS.has(id) else null
	if node:
		var aps := node.find_children("*", "AnimationPlayer", true, false)
		var sks := node.find_children("*", "Skeleton3D", true, false)
		if aps.size() > 0 and sks.size() > 0:
			var list: PackedStringArray = (aps[0] as AnimationPlayer).get_animation_list()
			var sk: Skeleton3D = sks[0]
			var root := sk.find_bone("root_motion")
			if list.size() > 0:
				out = {"anim": (aps[0] as AnimationPlayer).get_animation(list[0]), "up": sk.get_bone_rest(root).basis if root >= 0 else Basis.IDENTITY}
		node.free()
	_clip_cache[id] = out
	return out

# ---------------------------------------------------------------- source <-> godot
static func src_to_godot(v: Vector3) -> Vector3:
	return Vector3(v.y, v.z, v.x) * U2M

static func src_yaw_to_godot(yaw_deg: float) -> float:
	# Source forward (cos, sin, 0) maps to Godot (sin, 0, cos); Godot nodes look down -Z.
	var f := Vector3(sin(deg_to_rad(yaw_deg)), 0, cos(deg_to_rad(yaw_deg)))
	return atan2(-f.x, -f.z)
