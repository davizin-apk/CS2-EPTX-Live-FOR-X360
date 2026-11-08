extends Control
## Boot screen: finds the player's CS2, exports what the assets sheet lists with the bundled
## Source2Viewer-CLI into the cache (first launch / after a CS2 update), bakes the map, then opens the menu.

const Defs = preload("res://scripts/gen/defs.gd")
const UI = preload("res://scripts/ui.gd")

var _status: Label
var _detail: Label
var _bar: ProgressBar
var _thread: Thread
var _progress := 0.0
var _msg := ""
var _err := ""
var _done := false

func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var bg := ColorRect.new()
	bg.color = Color(0.04, 0.05, 0.07)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(center)
	var box := VBoxContainer.new()
	box.custom_minimum_size = Vector2(760, 0)
	box.add_theme_constant_override("separation", 14)
	center.add_child(box)
	var title := UI.label("BOT STRIKE", 64, Color.WHITE, true)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(title)
	_status = UI.label("Looking for Counter-Strike 2...", 24, Color(0.85, 0.87, 0.9))
	_status.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(_status)
	_bar = ProgressBar.new()
	_bar.custom_minimum_size = Vector2(760, 10)
	_bar.show_percentage = false
	var fill := StyleBoxFlat.new()
	fill.bg_color = Color(0.37, 0.6, 0.95)
	var back := StyleBoxFlat.new()
	back.bg_color = Color(1, 1, 1, 0.08)
	_bar.add_theme_stylebox_override("fill", fill)
	_bar.add_theme_stylebox_override("background", back)
	box.add_child(_bar)
	_detail = UI.label("", 16, Color(0.6, 0.63, 0.68))
	_detail.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_detail)

	if Game.cs2_root == "":
		_fail("Counter-Strike 2 wasn't found. Bot Strike reads its maps, agents and guns from your own CS2 install - install CS2 from Steam, then start Bot Strike again.")
		return
	if Game.vrf == "":
		_fail("The asset reader (Source2Viewer-CLI) is missing next to BotStrike.exe. Reinstall Bot Strike from Melty.")
		return
	_thread = Thread.new()
	_thread.start(_work)

func _fail(msg: String) -> void:
	_status.text = "Can't start"
	_detail.text = msg
	Game.log_line("boot failed: " + msg)
	Game.melty_report("prep-done", false, msg)

func _process(_dt: float) -> void:
	if _thread == null:
		return
	_bar.value = _progress * 100.0
	_status.text = _msg
	if _done:
		_thread.wait_to_finish()
		_thread = null
		if _err != "":
			_fail(_err)
			return
		Game.melty_report("prep-done", true)
		Game.melty_report("map-loaded", Game.map_scene != null, "" if Game.map_scene else "map scene failed to load")
		get_tree().change_scene_to_file("res://scenes/menu.tscn")

# ---------------------------------------------------------------- worker thread
func _work() -> void:
	var stamp_path := Game.cache.path_join("stamp.json")
	var stamp = JSON.parse_string(FileAccess.get_file_as_string(stamp_path)) if FileAccess.file_exists(stamp_path) else null
	var fresh: bool = stamp is Dictionary and stamp.get("prep", 0) == Game.PREP_VERSION and stamp.get("patch", "") == Game.patch
	if fresh:
		for id in Defs.ASSETS:
			if Defs.ASSETS[id].kind in ["map", "phys", "ents", "trig", "model"] and not FileAccess.file_exists(Game.asset_path(id)):
				fresh = false
				break
	if not fresh:
		DirAccess.make_dir_recursive_absolute(Game.cache)
		var groups := _groups()
		var i := 0
		for g in groups:
			_msg = g.label
			_progress = 0.05 + 0.55 * float(i) / groups.size()
			var out: Array = []
			var code := OS.execute(Game.vrf, g.args, out, true)
			Game.log_line("vrf %s -> %d\n%s" % [g.label, code, String("\n".join(out)).right(1500)])
			i += 1
		for id in Defs.ASSETS:
			if Defs.ASSETS[id].kind != "file" and not FileAccess.file_exists(Game.asset_path(id)):
				if not Defs.ASSETS[id].kind in ["map", "phys", "ents", "trig", "model"]:
					Game.log_line("optional asset missing, playing without it: %s (%s)" % [id, Defs.ASSETS[id].path])
					continue
				_err = "Couldn't read %s from your CS2 install (%s). Verify CS2's files in Steam, then start Bot Strike again." % [id, Defs.ASSETS[id].path]
				_done = true
				return
		# a fresh export invalidates the baked map
		DirAccess.remove_absolute(_baked_path())
		var f := FileAccess.open(stamp_path, FileAccess.WRITE)
		f.store_string(JSON.stringify({"prep": Game.PREP_VERSION, "patch": Game.patch}))
		f.close()
	if not FileAccess.file_exists(_baked_path()):
		_msg = "Building Dust II from your CS2 files (first launch only)..."
		_progress = 0.62
		_bake_map()
	_msg = "Loading Dust II..."
	_progress = 0.9
	Game.map_scene = ResourceLoader.load(_baked_path(), "", ResourceLoader.CACHE_MODE_REUSE)
	# warm up agents and guns so the first round doesn't hitch
	for id in Defs.ASSETS:
		if Defs.ASSETS[id].kind == "model":
			Game.scene(id)
	_progress = 1.0
	_done = true

func _baked_path() -> String:
	return Game.cache.path_join("de_dust2_baked_v3.scn")

func _groups() -> Array:
	var gameinfo := Game.csgo.path_join("gameinfo.gi")
	var by_vpk := {}
	for id in Defs.ASSETS:
		var r: Dictionary = Defs.ASSETS[id]
		if r.kind == "file" or r.kind == "trig":
			continue  # trig comes out of the map export itself
		var group := "|plain"
		if r.kind == "model":
			group = "|anim"
		elif r.kind == "anim":
			group = "|clip"
		elif r.kind in ["map", "phys"]:
			group = "|gltf"
		var key: String = r.vpk + group
		if not by_vpk.has(key):
			by_vpk[key] = []
		by_vpk[key].append(r)
	var labels := {"|gltf": "Reading Dust II from your CS2...", "|anim": "Reading agents and weapons...", "|clip": "Reading agent animations...", "|plain": "Reading sounds, icons and map entities..."}
	var groups := []
	for key in by_vpk:
		var rows: Array = by_vpk[key]
		var vpk_name: String = key.split("|")[0]
		var vpk := Game.csgo.path_join("pak01_dir.vpk") if vpk_name == "pak" else Game.csgo.path_join("maps/%s.vpk" % vpk_name.split(":")[1])
		var paths := []
		var anims := {}
		for r in rows:
			paths.append_array(String(r.path).split("|"))
			for a in String(r.anims).split(",", false):
				anims[a] = true
		var args := ["-i", vpk, "-f", ",".join(paths), "-o", Game.cache, "-d", "--game", gameinfo, "--threads", "4"]
		var kind: String = key.split("|")[1]
		if kind != "plain":
			args += ["--gltf_export_format", "glb", "--gltf_export_materials", "--gltf_textures_adapt"]
		if kind == "anim" and anims.size() > 0:
			args += ["--gltf_export_animations", "--gltf_animation_list", ",".join(anims.keys())]
		if kind == "clip":
			args += ["--gltf_export_animations"]
		groups.append({"label": labels.get("|" + kind, "Reading files..."), "args": args})
	return groups

func _bake_map() -> void:
	var doc := GLTFDocument.new()
	var st := GLTFState.new()
	if doc.append_from_file(Game.asset_path("map_dust2"), st) != OK:
		_err = "Dust II couldn't be read from the export."
		return
	var root := doc.generate_scene(st)
	_progress = 0.75
	_msg = "Optimizing textures (first launch only)..."
	var seen := {}
	var drop := []
	for mi in root.find_children("*", "MeshInstance3D", true, false):
		var m: MeshInstance3D = mi
		if m.mesh == null:
			continue
		var tools := 0
		for s in m.mesh.get_surface_count():
			var mat := m.mesh.surface_get_material(s)
			if mat is BaseMaterial3D:
				var bm: BaseMaterial3D = mat
				if String(bm.resource_name).to_lower().begins_with("tools"):
					tools += 1
				_shrink_material(bm, seen)
		if String(m.name).contains("blocklight") or tools == m.mesh.get_surface_count():
			drop.append(m)
	for m in drop:
		if is_instance_valid(m) and m.get_parent():
			m.get_parent().remove_child(m)
			m.queue_free()
	_msg = "Building lower-detail map pieces for distance (first launch only)..."
	_progress = 0.82
	Game.log_line("map lods: %d meshes" % Game.add_lods(root, 800))
	_set_owner(root, root)
	var ps := PackedScene.new()
	ps.pack(root)
	ResourceSaver.save(ps, _baked_path(), ResourceSaver.FLAG_COMPRESS)
	Game.log_line("baked map textures: %s" % _tex_stats)
	root.queue_free()

func _set_owner(n: Node, owner_node: Node) -> void:
	for c in n.get_children():
		c.owner = owner_node
		_set_owner(c, owner_node)

var _tex_stats := {"compressed": 0, "raw": 0, "dropped": 0}

func _shrink_material(bm: BaseMaterial3D, seen: Dictionary) -> void:
	# Keep VRAM small: only color and normal maps survive, color at 1024 (GPU-compressed) or 512 (raw),
	# normal maps only when they compress. Everything else becomes plain material values.
	for prop in ["orm_texture", "roughness_texture", "metallic_texture", "emission_texture", "ao_texture"]:
		if prop in bm and bm.get(prop) != null:
			bm.set(prop, null)
			_tex_stats.dropped += 1
	bm.roughness = 0.85
	bm.metallic = 0.0
	bm.ao_enabled = false
	bm.emission_enabled = false
	for prop in ["albedo_texture", "normal_texture"]:
		var t = bm.get(prop)
		if not (t is ImageTexture):
			continue
		if seen.has(t):
			if seen[t] == null:
				bm.set(prop, null)
			continue
		var img: Image = t.get_image()
		if img == null or img.is_empty():
			seen[t] = null
			bm.set(prop, null)
			continue
		if img.is_compressed():
			img.decompress()
		_fit(img, 1024 if prop == "albedo_texture" else 512)
		img.generate_mipmaps()
		var ok := img.compress(Image.COMPRESS_S3TC, Image.COMPRESS_SOURCE_NORMAL if prop == "normal_texture" else Image.COMPRESS_SOURCE_GENERIC) == OK and img.is_compressed()
		if ok:
			_tex_stats.compressed += 1
		elif prop == "normal_texture":
			seen[t] = null
			bm.set(prop, null)
			bm.normal_enabled = false
			_tex_stats.dropped += 1
			continue
		else:
			img.clear_mipmaps()
			_fit(img, 512)
			img.generate_mipmaps()
			_tex_stats.raw += 1
		t.set_image(img)
		seen[t] = t

func _fit(img: Image, mx: int) -> void:
	if img.get_width() > mx or img.get_height() > mx:
		var k := float(mx) / maxf(img.get_width(), img.get_height())
		img.resize(maxi(4, int(img.get_width() * k)), maxi(4, int(img.get_height() * k)), Image.INTERPOLATE_BILINEAR)
