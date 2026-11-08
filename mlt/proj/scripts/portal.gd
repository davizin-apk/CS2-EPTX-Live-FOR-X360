class_name Portal
extends Node3D
## A Portal Gun portal: an oval on a wall, floor or ceiling, or standing on the ground when dropped from the
## tactical map. Its twin is the other portal of the same owner; whatever goes into the front of one comes
## out of the front of the other, turned around: fighters, bot routes, bot sight lines and bullets.
## The oval shows the view out of its twin, scaled by the graphics sheet's portal_view (0 = swirl only).

const Defs = preload("res://scripts/gen/defs.gd")
const RX := 0.62
const RY := 1.05
const COLORS := {"blue": Color(0.15, 0.55, 1.0), "orange": Color(1.0, 0.5, 0.08)}
const LAYER_PORTAL := 4      # render layer 3: portal ovals never show up inside portal views
const LAYER_VIEWMODEL := 2

static var _shader: Shader

var owner_f: Fighter
var color_id := "blue"
var twin: Portal
var standing := false         # dropped from the map: free-standing on the ground
var surface: Object           # what it sits on (a build piece can break under it)
var _mat: ShaderMaterial
var _vp: SubViewport
var _cam: Camera3D

static func color_of(id: String) -> Color:
	return COLORS.get(id, Color.WHITE)

func setup(f: Fighter, id: String, xf: Transform3D, free_standing: bool, on: Object) -> void:
	owner_f = f
	color_id = id
	standing = free_standing
	surface = on
	global_transform = xf
	var c := color_of(id)
	if _shader == null:
		_shader = Shader.new()
		_shader.code = """shader_type spatial;
render_mode unshaded, cull_disabled;
uniform vec4 col : source_color = vec4(0.15, 0.55, 1.0, 1.0);
uniform sampler2D view : source_color, filter_linear;
uniform float use_view = 0.0;
void fragment() {
	vec2 p = (UV - 0.5) * 2.0;
	float r = length(p);
	if (r > 1.0) discard;
	float a = atan(p.y, p.x);
	float s = sin(a * 3.0 + r * 9.0 - TIME * 4.0) * 0.5 + 0.5;
	vec3 swirl = mix(col.rgb * 0.18, col.rgb * 0.9, s * (1.0 - r * 0.5));
	vec3 base = mix(swirl, texture(view, SCREEN_UV).rgb, use_view);
	float rim = smoothstep(0.8, 0.97, r);
	ALBEDO = mix(base, col.rgb * 1.8, rim);
}
"""
	_mat = ShaderMaterial.new()
	_mat.shader = _shader
	_mat.set_shader_parameter("col", c)
	var disc := MeshInstance3D.new()
	var q := QuadMesh.new()
	q.size = Vector2(RX * 2.0, RY * 2.0)
	disc.mesh = q
	disc.material_override = _mat
	disc.layers = LAYER_PORTAL
	disc.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(disc)
	var light := OmniLight3D.new()
	light.light_color = c
	light.light_energy = 0.7
	light.omni_range = 2.6
	light.position = Vector3(0, 0, 0.4)
	add_child(light)

func normal() -> Vector3:
	return global_basis.z

## Maps a point or direction going into this portal to where it leaves the twin.
func through() -> Transform3D:
	return twin.global_transform * Transform3D(Basis(Vector3.UP, PI), Vector3.ZERO) * global_transform.affine_inverse()

func inside_oval(world_p: Vector3, slack := 1.0) -> bool:
	var l := global_transform.affine_inverse() * world_p
	return (l.x / RX) ** 2 + (l.y / RY) ** 2 <= slack

## Where along the segment a..b it enters the front of this portal (0..1), or -1.
func segment_t(a: Vector3, b: Vector3) -> float:
	var n := normal()
	var d := b - a
	var den := d.dot(n)
	if den >= -0.0001:
		return -1.0
	var t := (global_position - a).dot(n) / den
	if t < 0.0 or t > 1.0:
		return -1.0
	return t if inside_oval(a + d * t) else -1.0

## A point in front of the portal where someone stands to walk into it (or lands coming out).
func front(dist := 0.7) -> Vector3:
	var n := normal()
	if absf(n.y) > 0.7:
		return global_position
	return global_position + Vector3(n.x, 0, n.z).normalized() * dist

# ---------------------------------------------------------------- placement helpers
## A portal lying on a surface hit at point with normal n, shot along view_dir. Wall portals sit with
## their bottom on the floor when shot low (like Portal 2 nudging them to fit); floor and ceiling
## portals point their top away from the shooter.
static func surface_xf(space: PhysicsDirectSpaceState3D, point: Vector3, n: Vector3, view_dir: Vector3) -> Transform3D:
	var up := Vector3.UP
	if absf(n.y) > 0.7:
		up = Vector3(view_dir.x, 0, view_dir.z)
		up = up.normalized() if up.length() > 0.05 else Vector3.FORWARD
	else:
		var probe := point + n * 0.35
		var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(probe, probe + Vector3.DOWN * 3.0, Game.LAYER_WORLD | Game.LAYER_BUILD))
		if not hit.is_empty() and point.y - RY < hit.position.y + 0.05:
			point.y = hit.position.y + RY + 0.05
	var x := up.cross(n).normalized()
	var y := n.cross(x).normalized()
	return Transform3D(Basis(x, y, n), point + n * 0.03)

## A free-standing portal on the ground at feet, facing along yaw (where you come out looking).
static func standing_xf(feet: Vector3, yaw: float) -> Transform3D:
	return Transform3D(Basis(Vector3.UP, yaw + PI), feet + Vector3(0, RY + 0.08, 0))

# ---------------------------------------------------------------- the view through the twin
static func view_scale() -> float:
	return float(Defs.GRAPHICS[Game.settings.quality].portal_view)

func _process(_dt: float) -> void:
	if surface != null and not is_instance_valid(surface):
		var mt := get_tree().current_scene
		if mt and mt.has_method("remove_portal"):
			mt.remove_portal(self)
		return
	var sc := view_scale()
	var main_cam := get_viewport().get_camera_3d()
	var on_view := twin != null and is_instance_valid(twin) and sc > 0.0 and main_cam != null and main_cam != _cam
	if on_view:
		var to_cam := main_cam.global_position - global_position
		on_view = to_cam.dot(normal()) > 0.0 and to_cam.length() < 60.0 and main_cam.is_position_in_frustum(global_position)
	if not on_view:
		_mat.set_shader_parameter("use_view", 0.0)
		if _vp:
			_vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
		return
	if _vp == null:
		_vp = SubViewport.new()
		_vp.handle_input_locally = false
		_vp.audio_listener_enable_3d = false
		add_child(_vp)
		_cam = Camera3D.new()
		_cam.cull_mask = 0xFFFFF & ~(LAYER_PORTAL | LAYER_VIEWMODEL)
		_vp.add_child(_cam)
		_mat.set_shader_parameter("view", _vp.get_texture())
	var full := Vector2(get_viewport().get_visible_rect().size)
	var want := Vector2i(maxi(64, int(full.x * sc)), maxi(36, int(full.y * sc)))
	if _vp.size != want:
		_vp.size = want
	_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var xf := through() * main_cam.global_transform
	_cam.global_transform = xf
	_cam.fov = main_cam.fov
	_cam.far = main_cam.far
	# skip whatever is behind the twin (the wall it sits on): start drawing at the nearest point of its
	# rim, so nothing in front of the oval gets cut (cutting at its centre blacked out the ground)
	var fwd := -xf.basis.z
	var near := INF
	for c in [Vector3(RX, 0, 0), Vector3(-RX, 0, 0), Vector3(0, RY, 0), Vector3(0, -RY, 0)]:
		near = minf(near, (twin.global_transform * c - xf.origin).dot(fwd))
	_cam.near = clampf(near - 0.1, 0.05, 50.0)
	_mat.set_shader_parameter("use_view", 1.0)

# ---------------------------------------------------------------- the gun
## A Portal Gun built from shapes (CS2 has no such model): white body, black barrel, glowing core and
## three prongs at the front. It points down -Z.
static func gun_model(first_person: bool, core := Color(0.15, 0.55, 1.0)) -> Node3D:
	var root := Node3D.new()
	var white := StandardMaterial3D.new()
	white.albedo_color = Color(0.93, 0.93, 0.95)
	white.roughness = 0.35
	var black := StandardMaterial3D.new()
	black.albedo_color = Color(0.08, 0.08, 0.09)
	black.roughness = 0.6
	var glow := StandardMaterial3D.new()
	glow.albedo_color = core
	glow.emission_enabled = true
	glow.emission = core
	glow.emission_energy_multiplier = 2.5
	glow.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	var add := func(mesh: Mesh, mat: Material, xf: Transform3D, nm := "") -> void:
		var mi := MeshInstance3D.new()
		mi.mesh = mesh
		mi.material_override = mat
		mi.transform = xf
		if nm != "":
			mi.name = nm
		root.add_child(mi)
	var along := Basis(Vector3.RIGHT, PI / 2)   # cylinders stand on Y; lay them along Z
	var body := CylinderMesh.new()
	body.top_radius = 0.05
	body.bottom_radius = 0.065
	body.height = 0.26
	add.call(body, white, Transform3D(along, Vector3(0, 0, 0.02)))
	var back := SphereMesh.new()
	back.radius = 0.068
	back.height = 0.12
	add.call(back, white, Transform3D(Basis.IDENTITY, Vector3(0, 0, 0.15)))
	var barrel := CylinderMesh.new()
	barrel.top_radius = 0.034
	barrel.bottom_radius = 0.042
	barrel.height = 0.1
	add.call(barrel, black, Transform3D(along, Vector3(0, 0, -0.15)))
	var corem := CylinderMesh.new()
	corem.top_radius = 0.022
	corem.bottom_radius = 0.022
	corem.height = 0.06
	add.call(corem, glow, Transform3D(along, Vector3(0, 0, -0.21)), "Core")
	var handle := BoxMesh.new()
	handle.size = Vector3(0.035, 0.11, 0.05)
	add.call(handle, black, Transform3D(Basis(Vector3.RIGHT, -0.3), Vector3(0, -0.08, 0.06)))
	for k in 3:
		var prong := BoxMesh.new()
		prong.size = Vector3(0.012, 0.012, 0.09)
		var a := TAU * k / 3.0
		var b := Basis(Vector3.BACK, a) * Basis(Vector3.RIGHT, 0.35)
		add.call(prong, white, Transform3D(b, Basis(Vector3.BACK, a) * Vector3(0, 0.045, -0.24)))
	for mi in root.get_children():
		(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF if first_person else GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		if first_person:
			(mi as MeshInstance3D).layers = LAYER_VIEWMODEL
	return root

static func set_gun_core(gun: Node3D, c: Color) -> void:
	if gun == null:
		return
	var core := gun.get_node_or_null("Core") as MeshInstance3D
	if core:
		var m: StandardMaterial3D = core.material_override
		m.albedo_color = c
		m.emission = c
