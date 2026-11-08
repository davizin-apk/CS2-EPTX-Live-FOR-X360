class_name BuildPiece
extends StaticBody3D
## A Fortnite-style build piece on a 5.12 m x 3.84 m grid: wall, floor, stairs or cone, in wood, brick or
## metal. Grows from 10% to full health over its build time, can be edited (3x3 tiles on walls, 2x2 on the
## rest) and collapses when nothing connects it to the ground any more (match.gd checks support).

const Defs = preload("res://scripts/gen/defs.gd")
const TILE := 5.12
const H := 3.84
const THICK := 0.22
const CONE_H := H / 2
## piece-local -Z for each cardinal: north (-Z), east (+X), south (+Z), west (-X)
const CARD := [Vector3(0, 0, -1), Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(-1, 0, 0)]
const CARD_YAW := [0.0, -PI / 2, PI, PI / 2]

static var _mats := {}
static var _holo: Shader

var piece_id := "wall"
var mat_id := "wood"
var team := ""
var key := ""
var cell := Vector3i.ZERO   # x, level, z
var card := 0
var oy := 0.0               # height of level 0 of the grid this structure sits on
var grounded := false
var max_hp := 150.0
var hp := 15.0
var build_time := 1.0
var edits: Array = []       # removed tiles (true = removed), edit_grid x edit_grid
var ramp_card := 0          # stairs: which way they climb, changed by an edit
var bounds := AABB()        # world space, for support checks
var _built := 0.0
var _mesh: MeshInstance3D
var _bmat: StandardMaterial3D

# ---------------------------------------------------------------- materials
static func material(id: String) -> StandardMaterial3D:
	if _mats.has(id):
		return _mats[id]
	var row: Dictionary = Defs.BUILD_MATERIALS[id]
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(String(row.color))
	m.roughness = 0.45 if id == "metal" else 0.85
	m.metallic = 0.55 if id == "metal" else 0.0
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	# a matching texture from the player's own Dust II export, mapped in world space
	var dir := DirAccess.open(Game.cache.path_join("maps"))
	if dir:
		for f in dir.get_files():
			var lf := f.to_lower()
			if lf.contains(String(row.texture_hint)) and lf.contains("color") and lf.ends_with(".png"):
				var img := Image.load_from_file(Game.cache.path_join("maps").path_join(f))
				if img:
					img.generate_mipmaps()
					m.albedo_texture = ImageTexture.create_from_image(img)
					m.albedo_color = Color(1, 1, 1).lerp(Color(String(row.color)), 0.25)
					m.uv1_triplanar = true
					m.uv1_world_triplanar = true
					m.uv1_scale = Vector3.ONE * 0.4
					break
	_mats[id] = m
	return m

## The blue build-preview look: see-through, with grid lines every 64 cm like Fortnite's holograms.
static func holo_material(tint: Color) -> ShaderMaterial:
	if _holo == null:
		_holo = Shader.new()
		_holo.code = """shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform vec4 tint : source_color = vec4(0.3, 0.6, 1.0, 0.35);
varying vec3 wpos;
varying vec3 wnrm;
void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	wnrm = (MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz;
}
void fragment() {
	vec3 n = abs(wnrm);
	vec3 f = abs(fract(wpos / 0.64) - 0.5) * 2.0;
	float l = n.x > n.y && n.x > n.z ? max(f.y, f.z) : (n.y > n.z ? max(f.x, f.z) : max(f.x, f.y));
	float line = smoothstep(0.95, 0.985, l) * clamp(8.0 / length(VERTEX), 0.25, 1.0);   // thin lines up close
	ALBEDO = tint.rgb * (0.7 + line * 0.8);
	ALPHA = clamp(tint.a + line * 0.35, 0.0, 1.0);
}
"""
	var m := ShaderMaterial.new()
	m.shader = _holo
	m.set_shader_parameter("tint", tint)
	return m

# ---------------------------------------------------------------- grid placement
static func cardinal(yaw: float) -> int:
	var fwd := Vector3(-sin(yaw), 0, -cos(yaw))
	if absf(fwd.x) > absf(fwd.z):
		return 1 if fwd.x > 0 else 3
	return 2 if fwd.z > 0 else 0

static func grid_cell(p: Vector3) -> Vector2i:
	return Vector2i(floori(p.x / TILE), floori(p.z / TILE))

## Where a piece goes for someone standing at feet looking along yaw/pitch, on the grid whose level 0
## is at oy, the way Fortnite picks it: walls on the edge of your tile you face, floors ahead (under you
## when looking down, over you when looking up), stairs ahead climbing away, cones over your head.
## Returns {id, xf, key, cell, card, oy}.
static func placement(id: String, feet: Vector3, yaw: float, pitch: float, origin_y: float) -> Dictionary:
	var c := cardinal(yaw)
	var fwd: Vector3 = CARD[c]
	var rel := (feet.y - origin_y) / H
	var lvl_stand := floori(rel + 0.15)      # the level you are on
	var lvl_climb := floori(rel + 0.45)      # past the middle of a stair counts as the next level
	var here := grid_cell(feet)
	var ahead := grid_cell(feet + Vector3(-sin(yaw), 0, -cos(yaw)) * TILE * 0.55)
	if ahead == here:
		ahead = here + Vector2i(roundi(fwd.x), roundi(fwd.z))
	var cxz := here
	var lvl := lvl_stand
	var face := c
	match id:
		"wall":
			if pitch > 0.6:
				lvl += 1
			elif pitch < -0.9:
				lvl -= 1
		"floor":
			face = 0
			if pitch > 0.55:
				lvl += 1
			elif pitch > -0.75:
				cxz = ahead
		"ramp":
			lvl = lvl_climb
			if pitch < -0.75:
				lvl = lvl_stand
			else:
				cxz = grid_cell(feet + Vector3(-sin(yaw), 0, -cos(yaw)) * TILE * 0.55)
				if pitch > 0.7:
					lvl += 1
		"cone":
			face = 0
			if pitch > -0.3:
				lvl += 1
			else:
				cxz = ahead
	var base_y := origin_y + lvl * H
	var center := Vector3((cxz.x + 0.5) * TILE, base_y, (cxz.y + 0.5) * TILE)
	var hy := roundi(base_y * 2.0)
	var k := ""
	match id:
		"wall":
			# an edge is shared by two tiles: name it the same from both sides
			match c:
				0: k = "wz:%d:%d:%d" % [cxz.x, hy, cxz.y]
				2: k = "wz:%d:%d:%d" % [cxz.x, hy, cxz.y + 1]
				1: k = "wx:%d:%d:%d" % [cxz.x + 1, hy, cxz.y]
				3: k = "wx:%d:%d:%d" % [cxz.x, hy, cxz.y]
		_:
			k = "%s:%d:%d:%d" % [id.substr(0, 1), cxz.x, hy, cxz.y]
	return {"id": id, "xf": Transform3D(Basis(Vector3.UP, CARD_YAW[face]), center), "key": k,
		"cell": Vector3i(cxz.x, lvl, cxz.y), "card": face, "oy": origin_y}

# ---------------------------------------------------------------- shapes
static func grid_of(id: String) -> int:
	return int(Defs.BUILD_PIECES[id].edit_grid)

## The pieces of a shape in its own frame: boxes [{xf, size}] and loose triangles (cones).
static func parts(id: String, removed: Array, turn: int) -> Dictionary:
	var boxes := []
	var tris := PackedVector3Array()
	var n := grid_of(id)
	var gone := func(i: int, j: int) -> bool: return removed.size() == n * n and bool(removed[j * n + i])
	match id:
		"wall":
			var tw := TILE / n
			var th := H / n
			for j in n:
				for i in n:
					if not gone.call(i, j):
						boxes.append({"xf": Transform3D(Basis.IDENTITY, Vector3(-TILE / 2 + (i + 0.5) * tw, (j + 0.5) * th, -TILE / 2)), "size": Vector3(tw, th, THICK)})
		"floor":
			var tw := TILE / n
			for j in n:
				for i in n:
					if not gone.call(i, j):
						boxes.append({"xf": Transform3D(Basis.IDENTITY, Vector3(-TILE / 2 + (i + 0.5) * tw, 0, -TILE / 2 + (j + 0.5) * tw)), "size": Vector3(tw, THICK, tw)})
		"ramp":
			var ln := sqrt(TILE * TILE + H * H)
			var b := Basis(Vector3.UP, CARD_YAW[turn]) * Basis(Vector3.RIGHT, atan2(H, TILE))
			boxes.append({"xf": Transform3D(b, Vector3(0, H / 2, 0)), "size": Vector3(TILE, THICK, ln)})
		"cone":
			for j in 2:
				for i in 2:
					if gone.call(i, j):
						continue
					var sx := -1.0 if i == 0 else 1.0
					var sz := -1.0 if j == 0 else 1.0
					var corner := Vector3(sx * TILE / 2, 0, sz * TILE / 2)
					var mx := Vector3(sx * TILE / 2, 0, 0)
					var mz := Vector3(0, 0, sz * TILE / 2)
					var apex := Vector3(0, CONE_H, 0)
					tris.append_array([corner, mx, apex, corner, apex, mz])
	return {"boxes": boxes, "tris": tris}

## A mesh for the parts (used for the piece itself and for the blue preview).
static func make_mesh(id: String, removed: Array, turn: int) -> ArrayMesh:
	var p := parts(id, removed, turn)
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for b in p.boxes:
		_add_box(st, b.xf, b.size)
	var t: PackedVector3Array = p.tris
	for i in range(0, t.size(), 3):
		var nrm := (t[i + 1] - t[i]).cross(t[i + 2] - t[i]).normalized()
		if nrm.y < 0:
			nrm = -nrm
		for k in 3:
			st.set_normal(nrm)
			st.add_vertex(t[i + k])
	return st.commit()

static func _add_box(st: SurfaceTool, xf: Transform3D, size: Vector3) -> void:
	var h := size / 2
	for axis in 3:
		for s in [-1.0, 1.0]:
			var nrm := Vector3.ZERO
			nrm[axis] = s
			var u := Vector3.ZERO
			var v := Vector3.ZERO
			u[(axis + 1) % 3] = h[(axis + 1) % 3]
			v[(axis + 2) % 3] = h[(axis + 2) % 3]
			var c := nrm * h[axis]
			var q := [c - u - v, c + u - v, c + u + v, c - u + v]
			var wn := (xf.basis * nrm).normalized()
			for idx in [0, 1, 2, 0, 2, 3]:
				st.set_normal(wn)
				st.add_vertex(xf * (q[idx] as Vector3))

# ---------------------------------------------------------------- the piece
func setup(id: String, material_id: String, owner_team: String, p: Dictionary) -> void:
	piece_id = id
	mat_id = material_id
	team = owner_team
	key = p.key
	cell = p.cell
	card = p.card
	oy = p.oy
	var row: Dictionary = Defs.BUILD_PIECES[id]
	var mrow: Dictionary = Defs.BUILD_MATERIALS[material_id]
	max_hp = float(row.max_hp) * float(mrow.hp_mult)
	build_time = float(row.build_time) * float(mrow.time_mult)
	hp = max_hp * float(mrow.start_hp)
	collision_layer = Game.LAYER_BUILD
	collision_mask = 0
	global_transform = p.xf
	var n := grid_of(id)
	edits.resize(n * n)
	edits.fill(false)
	_bmat = material(material_id).duplicate()
	_bmat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_rebuild()
	add_to_group("build_pieces")
	grounded = _touches_world()

func _rebuild() -> void:
	for c in get_children():
		c.free()
	var p := parts(piece_id, edits, ramp_card)
	_mesh = MeshInstance3D.new()
	_mesh.mesh = make_mesh(piece_id, edits, ramp_card)
	_mesh.material_override = _bmat
	add_child(_mesh)
	for b in p.boxes:
		var cs := CollisionShape3D.new()
		var box := BoxShape3D.new()
		box.size = b.size
		cs.shape = box
		cs.transform = b.xf
		add_child(cs)
	if not (p.tris as PackedVector3Array).is_empty():
		var cs := CollisionShape3D.new()
		var cps := ConcavePolygonShape3D.new()
		cps.set_faces(p.tris)
		cps.backface_collision = true
		cs.shape = cps
		add_child(cs)
	bounds = global_transform * _mesh.mesh.get_aabb()

## True when the piece rests on (or sits in) the map itself.
func _touches_world() -> bool:
	var space := get_world_3d().direct_space_state
	var q := PhysicsShapeQueryParameters3D.new()
	var box := BoxShape3D.new()
	var b := bounds
	# the bottom slice of the piece, a little below it
	box.size = Vector3(maxf(b.size.x - 0.3, 0.1), 0.5, maxf(b.size.z - 0.3, 0.1))
	q.shape = box
	q.transform = Transform3D(Basis.IDENTITY, Vector3(b.get_center().x, b.position.y + 0.1, b.get_center().z))
	q.collision_mask = Game.LAYER_WORLD | Game.LAYER_CLIP
	return not space.intersect_shape(q, 1).is_empty()

func touches(other: BuildPiece) -> bool:
	return bounds.grow(0.15).intersects(other.bounds)

func _process(dt: float) -> void:
	if _built < build_time:
		_built += dt
		hp = minf(max_hp, hp + max_hp * 0.9 * dt / build_time)
		_bmat.albedo_color.a = lerpf(0.4, 1.0, clampf(_built / build_time, 0, 1))
		if _built >= build_time:
			_bmat.transparency = BaseMaterial3D.TRANSPARENCY_DISABLED

func damage(amount: float, _by) -> void:
	hp -= amount
	if hp <= 0:
		destroy()

func destroy() -> void:
	if is_queued_for_deletion():
		return
	var mt := get_tree().current_scene
	remove_from_group("build_pieces")
	if mt and mt.has_method("effect_break"):
		mt.effect_break(bounds.get_center())
	queue_free()
	if mt and mt.has_method("check_support"):
		mt.check_support()

# ---------------------------------------------------------------- editing
## The frame the edit tiles live in (stairs turn with their edit).
func shape_xf() -> Transform3D:
	return global_transform

## Tile under a ray, or -1. Walls use their face, floors and cones their footprint, stairs their slope.
func tile_at(from: Vector3, dir: Vector3) -> int:
	var inv := shape_xf().affine_inverse()
	var o := inv * from
	var d := (inv.basis * dir).normalized()
	var n := grid_of(piece_id)
	var pn := Vector3.UP
	var pp := Vector3.ZERO
	match piece_id:
		"wall":
			pn = Vector3.BACK
			pp = Vector3(0, 0, -TILE / 2)
		"ramp":
			pn = Basis(Vector3.UP, CARD_YAW[ramp_card]) * Basis(Vector3.RIGHT, atan2(H, TILE)) * Vector3.UP
			pp = Vector3(0, H / 2, 0)
		"cone":
			pp = Vector3(0, CONE_H / 2, 0)
	var den := d.dot(pn)
	if absf(den) < 0.0001:
		return -1
	var t := (pp - o).dot(pn) / den
	if t < 0 or t > 9.0:
		return -1
	var hit := o + d * t
	var i := floori((hit.x + TILE / 2) / (TILE / n))
	var j := floori((hit.y / (H / n)) if piece_id == "wall" else (hit.z + TILE / 2) / (TILE / n))
	if i < 0 or j < 0 or i >= n or j >= n:
		return -1
	return j * n + i

## Transform of an edit tile's hologram in world space, on the side of the viewer.
func tile_xf(idx: int, viewer: Vector3) -> Transform3D:
	var n := grid_of(piece_id)
	var i := idx % n
	var j := idx / n
	var local_view := shape_xf().affine_inverse() * viewer
	var x := -TILE / 2 + (i + 0.5) * TILE / n
	var xf := Transform3D.IDENTITY
	match piece_id:
		"wall":
			var side := 1.0 if local_view.z > -TILE / 2 else -1.0
			xf = Transform3D(Basis(Vector3.RIGHT, PI / 2), Vector3(x, (j + 0.5) * H / n, -TILE / 2 + side * (THICK / 2 + 0.03)))
		"floor":
			var up := 1.0 if local_view.y > 0 else -1.0
			xf = Transform3D(Basis.IDENTITY, Vector3(x, up * (THICK / 2 + 0.03), -TILE / 2 + (j + 0.5) * TILE / n))
		"cone":
			var above := local_view.y > CONE_H / 2
			xf = Transform3D(Basis.IDENTITY, Vector3(x, CONE_H + 0.05 if above else -0.05, -TILE / 2 + (j + 0.5) * TILE / n))
		"ramp":
			var z := -TILE / 2 + (j + 0.5) * TILE / n
			var b := Basis(Vector3.RIGHT, atan2(H, TILE))
			var rb := Basis(Vector3.UP, CARD_YAW[ramp_card])
			# footprint point in the turned frame, lifted onto the slope
			var lp := rb.inverse() * Vector3(x, 0, z)
			var y := H / 2 - lp.z * H / TILE + THICK / 2 + 0.04
			xf = Transform3D(rb * b, Vector3(x, y, z))
	return shape_xf() * xf

func tile_size() -> Vector2:
	var n := grid_of(piece_id)
	match piece_id:
		"wall":
			return Vector2(TILE / n, H / n)
		"ramp":
			return Vector2(TILE / n, sqrt(TILE * TILE + H * H) / n)
	return Vector2(TILE / n, TILE / n)

## Applies an edit: the selected tiles go away (walls, floors, cones) or, for stairs, the drag from the
## first to the last selected tile turns them to climb that way. Returns false for an edit Fortnite
## would refuse (removing every tile).
func apply_edit(selected: Array) -> bool:
	var n := grid_of(piece_id)
	if piece_id == "ramp":
		if selected.size() < 2:
			return false
		var a: int = selected[0]
		var b: int = selected[selected.size() - 1]
		var d := Vector3((b % n) - (a % n), 0, (b / n) - (a / n))
		if d.length() < 0.5:
			return false
		var best := 0
		for k in 4:
			if (CARD[k] as Vector3).dot(d) > (CARD[best] as Vector3).dot(d):
				best = k
		ramp_card = best
		_rebuild()
		return true
	var out := []
	out.resize(n * n)
	out.fill(false)
	for s in selected:
		out[s] = true
	if not out.has(false):
		return false
	edits = out
	_rebuild()
	return true

func reset_edit() -> void:
	edits.fill(false)
	ramp_card = 0
	_rebuild()
