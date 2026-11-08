class_name TacMap
extends Control
## The Portal Gun's map (right mouse): a radar of Dust II drawn from the bot navmesh (north up, like CS2's
## radar), your side, the portals, where the enemies are heading (predicted routes) and recommended spots
## for the orange portal. Click a star or anywhere on the walkable map to open the second portal there;
## the mouse wheel turns which way you will come out.

const UI = preload("res://scripts/ui.gd")
const ENEMY := Color(1.0, 0.3, 0.25)
const REC := Color(1.0, 0.82, 0.25)

static var _polys := []       # [{pts: PackedVector2Array (map plane), y: float}] lowest first
static var _poly_patch := ""
static var _y_range := Vector2(0, 1)

var match_node: Node
var viewer: Fighter
var _recs := []
var _routes := []
var _facing := 0.0
var _turned := false
var _hover := Vector2(-1, -1)
var _center := Vector2.ZERO   # map plane point shown at the middle
var _scale := 1.0
var _rect := Rect2()

## Map plane: right = Source +x (Godot +z), up = Source +y (Godot +x).
static func plane(p: Vector3) -> Vector2:
	return Vector2(p.z, -p.x)

func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	size = get_viewport_rect().size
	mouse_filter = Control.MOUSE_FILTER_STOP
	_load_polys()
	_routes = enemy_routes(match_node, viewer.team)
	_recs = recommend(match_node, viewer.team, _routes)
	_facing = viewer.yaw
	resized.connect(_fit)
	_fit()
	Game.melty_report("map-opened", true)

func _load_polys() -> void:
	var world: World = match_node.world
	if _poly_patch == Game.patch and not _polys.is_empty():
		return
	_polys = []
	if world.nav_region == null or world.nav_region.navigation_mesh == null:
		return
	var nm := world.nav_region.navigation_mesh
	var verts := nm.get_vertices()
	# only the part of the navmesh you can walk to from the spawns: roofs are walkable too, but nobody
	# gets up there, and from above they would cover the streets
	var by_vert := {}
	for i in nm.get_polygon_count():
		for k in nm.get_polygon(i):
			if not by_vert.has(k):
				by_vert[k] = []
			by_vert[k].append(i)
	var seeds := []
	for team in ["CT", "T"]:
		for sp in world.spawns[team]:
			var c := world.closest_nav_point((sp as Transform3D).origin)
			var best := -1
			var best_d := INF
			for i in nm.get_polygon_count():
				for k in nm.get_polygon(i):
					var d := verts[k].distance_to(c)
					if d < best_d:
						best_d = d
						best = i
			if best >= 0:
				seeds.append(best)
			break
	var keep := {}
	var todo := seeds.duplicate()
	for sd in seeds:
		keep[sd] = true
	while not todo.is_empty():
		var i: int = todo.pop_back()
		for k in nm.get_polygon(i):
			for j in by_vert[k]:
				if not keep.has(j):
					keep[j] = true
					todo.append(j)
	var lo := INF
	var hi := -INF
	for i in nm.get_polygon_count():
		if not keep.is_empty() and not keep.has(i):
			continue
		var idx := nm.get_polygon(i)
		var pts := PackedVector2Array()
		var y := 0.0
		for k in idx:
			pts.append(plane(verts[k]))
			y += verts[k].y
		y /= maxf(idx.size(), 1)
		lo = minf(lo, y)
		hi = maxf(hi, y)
		_polys.append({"pts": pts, "y": y})
	_polys.sort_custom(func(a, b): return a.y < b.y)
	_y_range = Vector2(lo, maxf(hi, lo + 1.0))
	_poly_patch = Game.patch

func _fit() -> void:
	if _polys.is_empty():
		return
	var box := Rect2(_polys[0].pts[0], Vector2.ZERO)
	for p in _polys:
		for v in p.pts:
			box = box.expand(v)
	var side := minf(size.x, size.y) * 0.86
	_rect = Rect2((size - Vector2(side, side)) / 2 + Vector2(0, 20), Vector2(side, side))
	_scale = side / maxf(box.size.x, box.size.y)
	_center = box.get_center()
	queue_redraw()

func to_screen(p: Vector3) -> Vector2:
	return _rect.get_center() + (plane(p) - _center) * _scale

func _to_plane(s: Vector2) -> Vector2:
	return (s - _rect.get_center()) / _scale + _center

## The walkable point under a map position: the highest navmesh polygon there (what the radar shows).
func ground_at(s: Vector2) -> Vector3:
	var q := _to_plane(s)
	var best_y := NAN
	for p in _polys:
		if Geometry2D.is_point_in_polygon(q, p.pts):
			best_y = p.y
	if is_nan(best_y):
		return Vector3.INF
	return match_node.world.closest_nav_point(Vector3(-q.y, best_y + 0.3, q.x))

# ---------------------------------------------------------------- predictions
## Where each living enemy is heading: what is left of a bot's route, or a straight guess for a player.
static func enemy_routes(m: Node, team: String) -> Array:
	var out := []
	for f in m.fighters:
		if f.team == team or not f.alive:
			continue
		var r := PackedVector3Array([f.global_position])
		if f is BotFighter:
			var b: BotFighter = f
			for i in range(b.path_i, b.path.size()):
				r.append(b.path[i])
			if r.size() == 1:
				r.append(b.goal)
		else:
			r.append(f.global_position + Vector3(f.velocity.x, 0, f.velocity.z) * 4.0)
		out.append(r)
	return out

static func _route_len(r: PackedVector3Array) -> float:
	var l := 0.0
	for i in range(1, r.size()):
		l += r[i].distance_to(r[i - 1])
	return l

static func _along(r: PackedVector3Array, dist: float) -> Array:
	# point and heading at a distance along a route
	for i in range(1, r.size()):
		var seg := r[i].distance_to(r[i - 1])
		if dist <= seg and seg > 0.01:
			return [r[i - 1].lerp(r[i], dist / seg), (r[i] - r[i - 1]).normalized()]
		dist -= seg
	var last := r[r.size() - 1]
	var dir := (last - r[r.size() - 2]).normalized() if r.size() > 1 else Vector3.FORWARD
	return [last, dir]

## Up to three good exits for the second portal: off to the side of where the enemies will pass, with a
## clear look at their route, not right on top of them. Each {pos, yaw, label, look}.
static func recommend(m: Node, team: String, routes: Array) -> Array:
	var world: World = m.world
	var space: PhysicsDirectSpaceState3D = world.get_world_3d().direct_space_state
	var cands := []
	for r in routes:
		if r.size() < 2:
			continue
		var total := _route_len(r)
		if total < 3.0:
			continue
		for frac in [0.45, 0.65, 0.85, 1.0]:
			var a := _along(r, total * frac)
			var p: Vector3 = a[0]
			var dir: Vector3 = a[1]
			var side := dir.cross(Vector3.UP).normalized()
			for s in [-1.0, 1.0]:
				var c := world.closest_nav_point(p + side * s * 5.0 + dir * 2.0)
				if c.distance_to(p) < 2.5 or absf(c.y - p.y) > 2.5:
					continue
				var label := "AMBUSH"
				if frac >= 0.99:
					var st := world.site_at(p)
					label = ("SITE " + st) if st != "" else "WHERE THEY GO"
				elif frac < 0.5:
					label = "CUT OFF"
				cands.append({"pos": c, "look": p, "label": label})
	for c in cands:
		var score := 0.0
		for r in routes:
			var dmin := INF
			for v in r:
				dmin = minf(dmin, (v as Vector3).distance_to(c.pos))
			score += exp(-(dmin * dmin) / 144.0)
			if (r[0] as Vector3).distance_to(c.pos) < 10.0:
				score -= 1.5   # don't drop in right next to them
		var eye: Vector3 = c.pos + Vector3(0, 1.4, 0)
		var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(eye, c.look + Vector3(0, 1.2, 0), Game.LAYER_WORLD | Game.LAYER_BUILD))
		score += 0.8 if hit.is_empty() else -0.4
		c["score"] = score
		var d: Vector3 = c.look - c.pos
		c["yaw"] = atan2(-d.x, -d.z)
	cands.sort_custom(func(a, b): return a.score > b.score)
	var out := []
	for c in cands:
		var ok := true
		for o in out:
			if (o.pos as Vector3).distance_to(c.pos) < 10.0:
				ok = false
		if ok:
			out.append(c)
		if out.size() >= 3:
			break
	return out

# ---------------------------------------------------------------- input
func _rec_under(s: Vector2) -> Dictionary:
	for r in _recs:
		if to_screen(r.pos).distance_to(s) < 22.0:
			return r
	return {}

func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion:
		_hover = e.position
		queue_redraw()
	elif e is InputEventMouseButton and e.pressed:
		match e.button_index:
			MOUSE_BUTTON_WHEEL_UP:
				_facing += deg_to_rad(15)
				_turned = true
				queue_redraw()
			MOUSE_BUTTON_WHEEL_DOWN:
				_facing -= deg_to_rad(15)
				_turned = true
				queue_redraw()
			MOUSE_BUTTON_RIGHT:
				match_node.close_tacmap()
			MOUSE_BUTTON_LEFT:
				var r := _rec_under(e.position)
				if not r.is_empty():
					match_node.place_orange(viewer, r.pos, _facing if _turned else float(r.yaw))
					return
				var g := ground_at(e.position)
				if g == Vector3.INF:
					match_node.hud.notice("Pick a walkable spot on the map", 1.2)
					return
				match_node.place_orange(viewer, g, _facing if _turned else _auto_yaw(g))
	accept_event()

func _auto_yaw(g: Vector3) -> float:
	# come out facing the closest predicted enemy route point
	var best := Vector3.INF
	for r in _routes:
		for v in r:
			if best == Vector3.INF or (v as Vector3).distance_to(g) < best.distance_to(g):
				best = v
	if best == Vector3.INF or best.distance_to(g) < 0.5:
		return viewer.yaw
	var d := best - g
	return atan2(-d.x, -d.z)

# ---------------------------------------------------------------- drawing
func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), Color(0.02, 0.03, 0.04, 0.82))
	var font := Game.font("font_ui")
	draw_string(font, Vector2(_rect.position.x, _rect.position.y - 34), "PORTAL MAP", HORIZONTAL_ALIGNMENT_LEFT, -1, 30, Color.WHITE)
	draw_string(font, Vector2(_rect.position.x, _rect.position.y - 10), "Click a ★ or any walkable spot to open the orange portal  ·  wheel: turn exit  ·  right mouse: close", HORIZONTAL_ALIGNMENT_LEFT, -1, 16, UI.DIM)
	if _polys.is_empty():
		draw_string(font, _rect.get_center() - Vector2(200, 0), "The map is still being prepared (navmesh)", HORIZONTAL_ALIGNMENT_LEFT, -1, 20, Color.WHITE)
		return
	draw_rect(_rect.grow(8), Color(0.07, 0.08, 0.1, 0.9))
	for p in _polys:
		var k: float = (p.y - _y_range.x) / (_y_range.y - _y_range.x)
		var c := Color(0.32, 0.31, 0.28).lerp(Color(0.78, 0.74, 0.62), k)
		var pts := PackedVector2Array()
		for v in p.pts:
			pts.append(_rect.get_center() + (v - _center) * _scale)
		if pts.size() >= 3:
			draw_colored_polygon(pts, c)
	var world: World = match_node.world
	for s in world.sites:
		var box: AABB = world.sites[s]
		var a := to_screen(box.position)
		var b := to_screen(box.end)
		var r := Rect2(a, Vector2.ZERO).expand(b)
		draw_rect(r, Color(1, 0.4, 0.3, 0.16))
		draw_rect(r, Color(1, 0.4, 0.3, 0.7), false, 2.0)
		draw_string(font, r.get_center() - Vector2(10, -12), s, HORIZONTAL_ALIGNMENT_LEFT, -1, 34, Color(1, 0.55, 0.45))
	for team in ["CT", "T"]:
		var sp: Array = world.spawns[team]
		if sp.size() > 0:
			draw_string(font, to_screen((sp[0] as Transform3D).origin) - Vector2(14, 0), team + " SPAWN", HORIZONTAL_ALIGNMENT_LEFT, -1, 14, UI.CT.lightened(0.4) if team == "CT" else UI.T)
	# where the enemies are heading
	for r in _routes:
		var pts := PackedVector2Array()
		for v in r:
			pts.append(to_screen(v))
		if pts.size() >= 2:
			draw_polyline(pts, Color(ENEMY, 0.55), 3.0, true)
			var end := pts[pts.size() - 1]
			var dir := (end - pts[pts.size() - 2]).normalized()
			draw_colored_polygon(PackedVector2Array([end + dir * 12, end + dir.orthogonal() * 7, end - dir.orthogonal() * 7]), ENEMY)
	# portals already open
	for p in match_node.all_portals():
		var c := Portal.color_of(p.color_id)
		draw_circle(to_screen(p.global_position), 7.0, c)
		draw_arc(to_screen(p.global_position), 10.0, 0, TAU, 20, Color.WHITE, 1.5)
	# your side
	for f in match_node.fighters:
		if f.team != viewer.team or not f.alive:
			continue
		var s := to_screen(f.global_position)
		var col: Color = UI.CT.lightened(0.3) if f.team == "CT" else UI.T
		if f == viewer:
			var fwd := plane(Vector3(-sin(f.yaw), 0, -cos(f.yaw))).normalized()
			draw_colored_polygon(PackedVector2Array([s + fwd * 14, s + fwd.orthogonal() * 7 - fwd * 6, s - fwd.orthogonal() * 7 - fwd * 6]), Color.WHITE)
		else:
			draw_circle(s, 5.0, col)
	# recommended exits
	for r in _recs:
		var s := to_screen(r.pos)
		var hot := s.distance_to(_hover) < 22.0
		_star(s, 13.0 if hot else 10.0, REC if not hot else Color.WHITE)
		var yaw: float = _facing if _turned else float(r.yaw)
		var fwd := plane(Vector3(-sin(yaw), 0, -cos(yaw))).normalized()
		draw_line(s, s + fwd * 24, REC, 2.5)
		draw_string(font, s + Vector2(16, -10), "★ " + String(r.label), HORIZONTAL_ALIGNMENT_LEFT, -1, 15, REC)
	# the spot under the mouse
	if _rect.has_point(_hover) and _rec_under(_hover).is_empty():
		var g := ground_at(_hover)
		var ok := g != Vector3.INF
		draw_arc(_hover, 12.0, 0, TAU, 24, Portal.color_of("orange") if ok else Color(1, 1, 1, 0.3), 2.0)
		if ok:
			var yaw := _facing if _turned else _auto_yaw(g)
			var fwd := plane(Vector3(-sin(yaw), 0, -cos(yaw))).normalized()
			draw_line(_hover, _hover + fwd * 22, Portal.color_of("orange"), 2.5)

func _star(c: Vector2, r: float, col: Color) -> void:
	var pts := PackedVector2Array()
	for i in 10:
		var a := -PI / 2 + i * PI / 5
		pts.append(c + Vector2(cos(a), sin(a)) * (r if i % 2 == 0 else r * 0.45))
	draw_colored_polygon(pts, col)
