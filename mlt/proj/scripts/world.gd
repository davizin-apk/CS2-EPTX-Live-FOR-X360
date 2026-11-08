class_name World
extends Node3D
## Dust II from the player's CS2: render mesh, world collision, sun and sky, spawns, bomb sites, bot navmesh.

const Defs = preload("res://scripts/gen/defs.gd")

var map_row: Dictionary
var spawns := {"CT": [], "T": []}   # Array of Transform3D
var sites := {}                     # "A"/"B" -> AABB
var nav_map: RID
var nav_region: NavigationRegion3D
var nav_ready := false
var sun_dir := Vector3(-0.4, -0.8, -0.3)
var sun: DirectionalLight3D
var _props := []   # [MeshInstance3D, size] for distance culling
var _mats := {}    # map material -> had a normal map
var env: Environment

func setup(map_id: String, with_nav := true) -> void:
	map_row = Defs.MAPS[map_id]
	var t0 := Time.get_ticks_msec()
	if Game.map_scene and OS.get_environment("BOTSTRIKE_NOMAP") != "1":
		var m := Game.map_scene.instantiate()
		_clean_map(m)
		add_child(m)
	var t1 := Time.get_ticks_msec()
	_load_collision()
	var t2 := Time.get_ticks_msec()
	_parse_entities()
	_sites()
	var t3 := Time.get_ticks_msec()
	if OS.get_environment("BOTSTRIKE_NOENV") != "1":
		_environment()
	Game.log_line("world timings ms: map %d collision %d ents+sites %d env %d" % [t1 - t0, t2 - t1, t3 - t2, Time.get_ticks_msec() - t3])
	if with_nav:
		_navigation()
	Game.world_ref = self
	Game.apply_graphics()

func apply_quality(g: Dictionary) -> void:
	if sun:
		sun.shadow_enabled = bool(g.shadows)
		sun.directional_shadow_max_distance = maxf(float(g.shadow_distance), 1.0)
		sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS if float(g.shadow_distance) > 60 else DirectionalLight3D.SHADOW_PARALLEL_2_SPLITS
	if env:
		env.ssao_enabled = bool(g.ssao)
		env.glow_enabled = bool(g.glow)
		# the procedural sky is drawn per pixel; on Low a flat sky color with fixed ambient is far cheaper
		if not bool(g.shadows):
			env.background_mode = Environment.BG_COLOR
			env.background_color = Color(0.45, 0.62, 0.85)
			env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
			env.ambient_light_color = Color(0.62, 0.64, 0.68)
			env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
		else:
			env.background_mode = Environment.BG_SKY
			env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
			env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
		var far := float(g.view_distance)
		env.fog_enabled = far < 300.0
		env.fog_mode = Environment.FOG_MODE_DEPTH
		env.fog_light_color = Color(0.74, 0.78, 0.84)
		env.fog_depth_begin = far * 0.55
		env.fog_depth_end = far
		env.fog_density = 1.0
	# Low lights the map per vertex without normal maps: far less work per pixel on old GPUs
	var cheap := not bool(g.shadows)
	for mat in _mats:
		var bm: BaseMaterial3D = mat
		bm.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX if cheap else BaseMaterial3D.SHADING_MODE_PER_PIXEL
		bm.normal_enabled = _mats[mat] and not cheap
		bm.specular_mode = BaseMaterial3D.SPECULAR_DISABLED if cheap else BaseMaterial3D.SPECULAR_SCHLICK_GGX
	# small props stop drawing past a distance that depends on the preset
	var pd := float(g.prop_distance)
	for p in _props:
		if is_instance_valid(p[0]):
			var gi: GeometryInstance3D = p[0]
			gi.visibility_range_end = pd if p[1] < 1.5 else (pd * 2.0 if p[1] < 6.0 else 0.0)
			gi.visibility_range_end_margin = 2.0

static var _inv: StandardMaterial3D
static func _invisible() -> StandardMaterial3D:
	if _inv == null:
		_inv = StandardMaterial3D.new()
		_inv.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		_inv.albedo_color = Color(0, 0, 0, 0)
		_inv.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	return _inv

func _clean_map(m: Node) -> void:
	# The export carries CS2's own lights in CS2's units (far too bright here) and the black
	# occluder/skybox-blocker meshes CS2 never draws; our sun and sky light the map instead.
	var lights := 0
	var hidden := 0
	for l in m.find_children("*", "Light3D", true, false):
		l.get_parent().remove_child(l)
		l.free()
		lights += 1
	for mi in m.find_children("*", "MeshInstance3D", true, false):
		var mesh: Mesh = (mi as MeshInstance3D).mesh
		if mesh == null:
			continue
		for s in mesh.get_surface_count():
			var mat := mesh.surface_get_material(s)
			# CS2 keeps layer-blend weights in vertex colors; as a tint they paint surfaces black
			if mat is BaseMaterial3D:
				(mat as BaseMaterial3D).vertex_color_use_as_albedo = false
			var mn := String(mat.resource_name).to_lower() if mat else ""
			# blend overlays fade by vertex alpha in CS2's shader and would draw as dark slabs here
			if mn.contains("black_simple") or mn.begins_with("tools") or mn.contains("nodraw") or mn.contains("transition_overlay"):
				(mi as MeshInstance3D).set_surface_override_material(s, _invisible())
				hidden += 1
	Game.log_line("map cleanup: removed %d CS2 lights, hid %d blocker meshes" % [lights, hidden])
	for mi in m.find_children("*", "MeshInstance3D", true, false):
		var mm: Mesh = (mi as MeshInstance3D).mesh
		if mm:
			for s in mm.get_surface_count():
				var mat := mm.surface_get_material(s)
				if mat is BaseMaterial3D and not _mats.has(mat):
					_mats[mat] = (mat as BaseMaterial3D).normal_enabled
		var size := (mi as MeshInstance3D).get_aabb().get_longest_axis_size()
		if size < 6.0:
			_props.append([mi, size])
	if OS.get_environment("BOTSTRIKE_SHOTS") != "":
		var seen := {}
		for mi in m.find_children("*", "MeshInstance3D", true, false):
			var mesh: Mesh = (mi as MeshInstance3D).mesh
			if mesh == null:
				continue
			for s in mesh.get_surface_count():
				var mat := mesh.surface_get_material(s)
				if mat is BaseMaterial3D and (mat as BaseMaterial3D).albedo_texture == null and not seen.has(mat.resource_name):
					seen[mat.resource_name] = true
					Game.log_line("untextured: '%s' color %s on %s" % [mat.resource_name, (mat as BaseMaterial3D).albedo_color, mi.name])

func _load_collision() -> void:
	var doc := GLTFDocument.new()
	var st := GLTFState.new()
	if doc.append_from_file(Game.asset_path(map_row.phys_asset), st) != OK:
		Game.log_line("world collision missing")
		return
	var root := doc.generate_scene(st)
	add_child(root)
	var bodies := {}
	for mi in root.find_children("*", "MeshInstance3D", true, false):
		var m: MeshInstance3D = mi
		var n := String(m.name).to_lower()
		var src_name := String(m.get_parent().name).to_lower() + " " + n
		if src_name.contains("grenadeclip"):
			continue
		var layer := Game.LAYER_WORLD
		if src_name.contains("playerclip") or src_name.contains("passbullets") or src_name.contains("physics_sky"):
			layer = Game.LAYER_CLIP
		if not bodies.has(layer):
			var b := StaticBody3D.new()
			b.collision_layer = layer
			b.collision_mask = 0
			b.name = "Collision%d" % layer
			add_child(b)
			bodies[layer] = b
		# bake the node's (scaled) transform into the triangles: physics shapes must stay unscaled
		var xf := m.global_transform
		var faces := m.mesh.get_faces()
		for i in faces.size():
			faces[i] = xf * faces[i]
		var cps := ConcavePolygonShape3D.new()
		cps.set_faces(faces)
		cps.backface_collision = true
		var shape := CollisionShape3D.new()
		shape.shape = cps
		shape.set_meta("sky", src_name.contains("sky"))
		bodies[layer].add_child(shape)
		if layer == Game.LAYER_WORLD:
			_occluder_faces(faces)
	root.queue_free()
	_build_occluder()

# big walls and floors from the world collision become an occluder, so whatever is hidden behind
# Dust II's buildings is never drawn
var _occ_verts := PackedVector3Array()

func _occluder_faces(faces: PackedVector3Array) -> void:
	for i in range(0, faces.size(), 3):
		var a := faces[i]
		var b := faces[i + 1]
		var c := faces[i + 2]
		if (b - a).cross(c - a).length() * 0.5 > 3.0:
			_occ_verts.append(a)
			_occ_verts.append(b)
			_occ_verts.append(c)

func _build_occluder() -> void:
	if _occ_verts.is_empty() or OS.get_environment("BOTSTRIKE_NOOCC") == "1":
		return
	var occ := ArrayOccluder3D.new()
	var idx := PackedInt32Array()
	idx.resize(_occ_verts.size())
	for i in idx.size():
		idx[i] = i
	occ.set_arrays(_occ_verts, idx)
	var oi := OccluderInstance3D.new()
	oi.occluder = occ
	add_child(oi)
	Game.log_line("occluder: %d triangles" % (_occ_verts.size() / 3))
	_occ_verts = PackedVector3Array()

func _xform_of(n: Node3D, stop: Node) -> Transform3D:
	var t := n.transform
	var p := n.get_parent()
	while p and p != stop.get_parent():
		if p is Node3D:
			t = (p as Node3D).transform * t
		p = p.get_parent()
	return t

func _parse_entities() -> void:
	var txt := FileAccess.get_file_as_string(Game.asset_path(map_row.ents_asset))
	var ents := []
	for block in txt.split("===="):
		var d := {}
		for line in block.split("\n"):
			line = line.strip_edges()
			var sp := line.find(" ")
			if sp > 0:
				d[line.substr(0, sp)] = line.substr(sp).strip_edges().trim_prefix("\"").trim_suffix("\"")
		if d.has("classname"):
			ents.append(d)
	for team in ["CT", "T"]:
		var cls: String = Defs.TEAMS[team].spawn_class
		var list := []
		for e in ents:
			if e.classname == cls:
				list.append(e)
		list.sort_custom(func(a, b): return int(a.get("priority", "0")) > int(b.get("priority", "0")))
		for e in list:
			var o := _vec(e.get("origin", "0 0 0"))
			var ang := _vec(e.get("angles", "0 0 0"))
			var t := Transform3D(Basis(Vector3.UP, Game.src_yaw_to_godot(ang.y)), Game.src_to_godot(o) + Vector3(0, 0.05, 0))
			spawns[team].append(t)
	for e in ents:
		if e.classname == "light_environment":
			var ang := _vec(e.get("angles", "0 0 0"))
			# pitch/yaw of the light in Source angles
			var pitch := deg_to_rad(ang.x)
			var yaw := deg_to_rad(ang.y)
			var src_dir := Vector3(cos(pitch) * cos(yaw), cos(pitch) * sin(yaw), -sin(pitch))
			sun_dir = Game.src_to_godot(src_dir).normalized()
	Game.log_line("spawns CT=%d T=%d" % [spawns.CT.size(), spawns.T.size()])

func _vec(s: String) -> Vector3:
	# entity files write vectors as "x y z" or, since late-2026 CS2 builds, as [ x, y, z ]
	var p := s.replace("[", " ").replace("]", " ").replace(",", " ").split(" ", false)
	if p.size() < 3:
		return Vector3.ZERO
	return Vector3(float(p[0]), float(p[1]), float(p[2]))

func _sites() -> void:
	# the map's own func_bomb_target volumes (Source units); the sheet's boxes are the fallback
	var doc := GLTFDocument.new()
	var st := GLTFState.new()
	if doc.append_from_file(Game.asset_path(map_row.trig_asset), st) == OK:
		var root := doc.generate_scene(st)
		var boxes := []
		for mi in root.find_children("*", "MeshInstance3D", true, false):
			if String(mi.name).contains("bomb_target") or String(mi.get_parent().name).contains("bomb_target"):
				var faces: PackedVector3Array = (mi as MeshInstance3D).mesh.get_faces()
				if faces.is_empty():
					continue
				var box := AABB(Game.src_to_godot(faces[0]), Vector3.ZERO)
				for f in faces:
					box = box.expand(Game.src_to_godot(f))
				boxes.append(box)
		root.free()
		if boxes.size() == 2:
			# A is the site further along Source +x (Godot +z)
			boxes.sort_custom(func(a, b): return a.get_center().z > b.get_center().z)
			for i in 2:
				var box: AABB = boxes[i].grow(0.3)
				box.size.y += 2.0
				sites["AB"[i]] = box
			Game.log_line("bomb sites from map: A %s  B %s" % [sites.A, sites.B])
			return
	for k in ["a", "b"]:
		var mn: Array = map_row["site_%s_min" % k]
		var mx: Array = map_row["site_%s_max" % k]
		var a := Game.src_to_godot(Vector3(mn[0], mn[1], mn[2]))
		var b := Game.src_to_godot(Vector3(mx[0], mx[1], mx[2]))
		var box := AABB(a, Vector3.ZERO).expand(b)
		box = box.grow(0.3)
		box.size.y += 2.0
		sites[k.to_upper()] = box

func site_at(p: Vector3) -> String:
	for k in sites:
		if sites[k].has_point(p + Vector3(0, 0.3, 0)):
			return k
	return ""

func _environment() -> void:
	sun = DirectionalLight3D.new()
	sun.light_color = Color(1.0, 0.93, 0.82)
	sun.light_energy = 1.6
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 90.0
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	add_child(sun)
	if sun_dir.y > -0.2:
		sun_dir = Vector3(-0.35, -0.85, 0.4).normalized()
	sun.look_at_from_position(Vector3.ZERO, sun_dir, Vector3.UP if absf(sun_dir.y) < 0.99 else Vector3.FORWARD)
	env = Environment.new()
	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.32, 0.52, 0.8)
	sky_mat.sky_horizon_color = Color(0.78, 0.8, 0.82)
	sky_mat.ground_horizon_color = Color(0.7, 0.62, 0.5)
	sky_mat.ground_bottom_color = Color(0.45, 0.38, 0.3)
	sky_mat.sun_angle_max = 20.0
	var sky := Sky.new()
	sky.sky_material = sky_mat
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_energy = 1.25
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.tonemap_exposure = 1.0
	env.ssao_enabled = true
	env.ssao_radius = 1.2
	env.ssao_intensity = 1.6
	env.glow_enabled = true
	env.glow_intensity = 0.15
	env.adjustment_enabled = true
	env.adjustment_saturation = 1.08
	env.adjustment_contrast = 1.05
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

# ---------------------------------------------------------------- bot navigation (baked once, cached)
func _navigation() -> void:
	nav_region = NavigationRegion3D.new()
	add_child(nav_region)
	nav_map = get_world_3d().navigation_map
	NavigationServer3D.map_set_cell_size(nav_map, 0.15)
	NavigationServer3D.map_set_cell_height(nav_map, 0.1)
	var cached := Game.cache.path_join("nav_%s_%s_v3.res" % [map_row.id, Game.patch])
	if FileAccess.file_exists(cached):
		var nm = ResourceLoader.load(cached)
		if nm is NavigationMesh:
			nav_region.navigation_mesh = nm
			nav_ready = true
			return
	var nm := NavigationMesh.new()
	nm.cell_size = 0.15
	nm.cell_height = 0.1
	nm.agent_radius = 0.4
	nm.agent_height = 1.8
	nm.agent_max_climb = 0.5
	nm.agent_max_slope = 46.0
	nm.region_min_size = 4.0
	nm.filter_baking_aabb = AABB(Vector3(-40, -12, -60), Vector3(120, 40, 120))
	var geo := NavigationMeshSourceGeometryData3D.new()
	for body in get_children():
		if body is StaticBody3D:
			for cs in body.get_children():
				if cs is CollisionShape3D and cs.shape is ConcavePolygonShape3D and not cs.get_meta("sky", false):
					geo.add_faces(cs.shape.get_faces(), cs.transform)
	NavigationServer3D.bake_from_source_geometry_data_async(nm, geo, func():
		nav_region.navigation_mesh = nm
		ResourceSaver.save(nm, cached)
		nav_ready = true
		Game.log_line("navmesh baked: %d polygons" % nm.get_polygon_count()))

func path(from: Vector3, to: Vector3) -> PackedVector3Array:
	if not nav_ready or NavigationServer3D.map_get_iteration_id(nav_map) == 0:
		return PackedVector3Array([to])
	return NavigationServer3D.map_get_path(nav_map, from, to, true)

func closest_nav_point(p: Vector3) -> Vector3:
	if not nav_ready or NavigationServer3D.map_get_iteration_id(nav_map) == 0:
		return p
	return NavigationServer3D.map_get_closest_point(nav_map, p)

func random_point_in_site(k: String) -> Vector3:
	var box: AABB = sites[k]
	var p := box.position + Vector3(randf() * box.size.x, box.size.y * 0.2, randf() * box.size.z)
	return closest_nav_point(p)
