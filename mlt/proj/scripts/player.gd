class_name PlayerFighter
extends Fighter
## The human player: first-person camera, Source-style movement, weapon slots, buy menu, plant/defuse,
## Fortnite building (Q/X/C/V or F1-F4, turbo build, F to edit, right mouse to switch material) and the
## Portal Gun (4: left mouse blue portal, right mouse the map for the orange one).

# Movement constants come from the match's ruleset row (design/rulesets.json): CS2 or Classic 1.6.
const TURBO := 0.08          # turbo build: holding left mouse places a piece this often
const EDIT_RANGE := 7.0

var camera: Camera3D
var viewmodel_root: Node3D
var viewmodel: Node3D
var flash_light: OmniLight3D
var build_piece := ""       # "" = not building
var ghost: MeshInstance3D
var scoped := false
var using := false          # holding E
var spectate_target: Fighter
var _bob := 0.0
var _kick := 0.0
var editing: BuildPiece     # the piece being edited, or null
var edit_sel: Array = []    # selected tiles, in the order they were picked
var _edit_tiles: Array = []
var _edit_paint := true     # dragging selects (true) or clears (false)
var _next_build := 0.0
var _jump_queued := false     # 1.6: jump on each press of Space or wheel down (no hold-to-hop)
var _space_was := false
var jump_stamina := 0.0       # 1.6: ms; lowers the next jump and slows the landing
var bhops := 0                # 1.6: jumps taken within 0.25 s of landing (for the Melty check)
var _landed_at := -10.0
var _was_floor := true

func _ready() -> void:
	super._ready()
	is_bot = false
	camera = Camera3D.new()
	camera.position.y = EYE
	camera.fov = Game.settings.fov
	camera.near = 0.03
	camera.far = 600.0
	add_child(camera)
	viewmodel_root = Node3D.new()
	camera.add_child(viewmodel_root)
	flash_light = OmniLight3D.new()
	flash_light.light_color = Color(1, 0.8, 0.5)
	flash_light.omni_range = 4.0
	flash_light.light_energy = 0.0
	flash_light.position = Vector3(0.15, -0.1, -0.7)
	camera.add_child(flash_light)
	ghost = MeshInstance3D.new()
	ghost.material_override = BuildPiece.holo_material(Color(0.3, 0.6, 1.0, 0.3))
	ghost.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	ghost.top_level = true
	ghost.visible = false
	add_child(ghost)

func build_model() -> void:
	super.build_model()
	# the player sees their own body only through its shadow
	for mi in model.find_children("*", "MeshInstance3D", true, false):
		(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY

func refresh_held() -> void:
	super.refresh_held()
	if held_model:
		for mi in held_model.find_children("*", "MeshInstance3D", true, false):
			(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	_refresh_viewmodel()

var vm: ViewModel
var _vm_key := ""

func _refresh_viewmodel() -> void:
	if vm == null:
		vm = ViewModel.new()
		viewmodel_root.add_child(vm)
	var key := ""
	var set_name := ""
	var id := "mdl_c4"
	if alive and build_piece == "" and editing == null:
		if slot == 5:
			set_name = "c4"
		else:
			var w := current()
			if not w.is_empty():
				id = Defs.WEAPONS[w.id].model_asset
				set_name = Defs.WEAPONS[w.id].anim_set
		if set_name == "none":
			key = "portalgun"
		elif set_name != "":
			key = "%s|%s|%s" % [set_name, id, team]
	if key == _vm_key:
		return
	_vm_key = key
	vm.clear()
	if viewmodel:
		viewmodel.queue_free()
		viewmodel = null
	if key == "":
		return
	if key == "portalgun":
		viewmodel = Portal.gun_model(true)
		viewmodel_root.add_child(viewmodel)
		viewmodel.position = Vector3(0.2, -0.21, -0.6)
		viewmodel.scale = Vector3.ONE * 0.85
		viewmodel.rotation = Vector3(0.04, 0.06, 0)
		return
	if vm.show_weapon(set_name, id, Defs.TEAMS[team].agent_asset):
		return
	# no viewmodel clips in this CS2 build: hold the plain gun model in front of the camera
	viewmodel = Game.instance(id)
	if viewmodel == null:
		return
	viewmodel_root.add_child(viewmodel)
	for mi in viewmodel.find_children("*", "MeshInstance3D", true, false):
		(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		(mi as MeshInstance3D).layers = 2
	# CS2 gun models point down +Z here; the camera looks down -Z
	viewmodel.rotation = Vector3(0, PI, 0)
	viewmodel.position = Vector3(0.16, -0.2, -0.36) if slot != 3 else Vector3(0.2, -0.2, -0.32)

func is_scoped() -> bool:
	return scoped

func _unhandled_input(e: InputEvent) -> void:
	if match_node == null or match_node.ui_blocking():
		return
	if e is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		var s: float = 0.022 * Game.settings.sensitivity * (0.3 if scoped else 1.0)
		yaw -= deg_to_rad(e.relative.x * s)
		pitch = clampf(pitch - deg_to_rad(e.relative.y * s), deg_to_rad(-89), deg_to_rad(89))
	if not alive:
		if e is InputEventMouseButton and e.pressed:
			match_node.next_spectate()
		return
	if e is InputEventKey and e.pressed and not e.echo:
		var piece := _build_key(e.keycode)
		if piece != "" and match_node.building_enabled():
			_build_select(piece)
			return
		match e.keycode:
			KEY_1: _select(1)
			KEY_2: _select(2)
			KEY_3: _select(3)
			KEY_4: _select(4 if weapons.has(4) else 5)
			KEY_5: _select(5)
			KEY_R: start_reload()
			KEY_Q: _select(last_slot)
			KEY_F:
				if match_node.building_enabled():
					_toggle_edit()
			KEY_G:
				if has_bomb and slot == 5:
					match_node.drop_bomb(self)
	if e is InputEventMouseButton and e.pressed:
		if e.button_index == MOUSE_BUTTON_RIGHT:
			if editing:
				_reset_edit()
			elif build_piece != "":
				_cycle_material()
			elif current_def().id == "portalgun" and slot == 4:
				match_node.open_tacmap(self)
			elif current_def().id == "awp":
				scoped = not scoped
		elif e.button_index == MOUSE_BUTTON_LEFT:
			if editing:
				_edit_press()
			elif build_piece != "":
				_place()
				_next_build = now() + TURBO * 2.0
		elif e.button_index == MOUSE_BUTTON_WHEEL_UP:
			_select(_cycle(-1))
		elif e.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			if rules().jump_on_press:
				_jump_queued = true   # 1.6: wheel down jumps (bunny hopping)
			else:
				_select(_cycle(1))

## Which build piece a key picks (the sheet's key or its F1-F4 alternative), or "".
func _build_key(code: Key) -> String:
	for id in Defs.BUILD_PIECES:
		var row: Dictionary = Defs.BUILD_PIECES[id]
		if OS.find_keycode_from_string(row.key) == code or OS.find_keycode_from_string(row.alt_key) == code:
			return id
	return ""

func _cycle(d: int) -> int:
	var order := [1, 2, 3, 4, 5]
	var i := order.find(slot)
	for k in range(1, 6):
		var s: int = order[(i + d * k + 10) % 5]
		if weapons.has(s) or (s == 5 and has_bomb):
			return s
	return slot

func _select(s: int) -> void:
	build_piece = ""
	ghost.visible = false
	scoped = false
	_cancel_edit()
	switch_to(s)
	_refresh_viewmodel()

func _build_select(id: String) -> void:
	_cancel_edit()
	build_piece = id
	scoped = false
	ghost.mesh = BuildPiece.make_mesh(id, [], 0)
	_refresh_viewmodel()

func _cycle_material() -> void:
	var ids := Defs.BUILD_MATERIALS.keys()
	build_mat = ids[(ids.find(build_mat) + 1) % ids.size()]
	Game.play_ui("snd_knife", -20)

func _place() -> void:
	match_node.try_build(self, build_piece, match_node.placement_for(self, build_piece))

# ---------------------------------------------------------------- editing (F)
func _toggle_edit() -> void:
	if editing:
		_confirm_edit()
		return
	var from := eye_pos()
	var hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(from, from + aim_dir() * EDIT_RANGE, Game.LAYER_BUILD))
	if hit.is_empty() or not (hit.collider is BuildPiece) or (hit.collider as BuildPiece).team != team:
		return
	build_piece = ""
	ghost.visible = false
	scoped = false
	editing = hit.collider
	edit_sel = []
	for i in editing.edits.size():
		if editing.edits[i]:
			edit_sel.append(i)
	var n := BuildPiece.grid_of(editing.piece_id)
	var sz := editing.tile_size() * 0.94
	for i in n * n:
		var mi := MeshInstance3D.new()
		var pm := PlaneMesh.new()
		pm.size = sz
		mi.mesh = pm
		mi.material_override = BuildPiece.holo_material(Color(0.3, 0.6, 1.0, 0.3))
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		mi.top_level = true
		add_child(mi)
		_edit_tiles.append(mi)
	_refresh_viewmodel()

func _edit_press() -> void:
	var t := editing.tile_at(eye_pos(), aim_dir())
	if t < 0:
		return
	_edit_paint = not edit_sel.has(t)
	_paint(t)

func _paint(t: int) -> void:
	if t < 0:
		return
	if _edit_paint and not edit_sel.has(t):
		edit_sel.append(t)
	elif not _edit_paint and edit_sel.has(t):
		edit_sel.erase(t)

func _confirm_edit() -> void:
	if is_instance_valid(editing):
		match_node.edit_piece(self, editing, edit_sel)
	_cancel_edit()

func _reset_edit() -> void:
	if is_instance_valid(editing) and editing.team == team:
		editing.reset_edit()
	_cancel_edit()

func _cancel_edit() -> void:
	for mi in _edit_tiles:
		mi.queue_free()
	_edit_tiles.clear()
	edit_sel = []
	if editing != null:
		editing = null
		_refresh_viewmodel()

func _update_edit() -> void:
	if editing == null:
		return
	if not is_instance_valid(editing) or editing.is_queued_for_deletion() or editing.bounds.get_center().distance_to(eye_pos()) > EDIT_RANGE + 4.0:
		_cancel_edit()
		return
	var hover := editing.tile_at(eye_pos(), aim_dir())
	for i in _edit_tiles.size():
		var mi: MeshInstance3D = _edit_tiles[i]
		mi.global_transform = editing.tile_xf(i, eye_pos())
		var c := Color(0.3, 0.6, 1.0, 0.3)
		if edit_sel.has(i):
			c = Color(1.0, 0.6, 0.25, 0.1)
		if i == hover:
			c = Color(0.55, 0.85, 1.0, 0.55) if not edit_sel.has(i) else Color(1.0, 0.7, 0.4, 0.35)
		(mi.material_override as ShaderMaterial).set_shader_parameter("tint", c)

func start_reload() -> void:
	var before := reload_until
	super.start_reload()
	if reload_until > before and vm:
		vm.play("reload")

func on_fired(d: Dictionary) -> void:
	if vm:
		vm.play("shoot")
		if d.id != "knife":
			match_node.muzzle_flash(vm.muzzle_world())
			flash_light.global_position = vm.muzzle_world()
			if d.shell_asset != "none":
				match_node.eject_shell(d.shell_asset, vm.eject_world(), camera.global_basis)
	_kick = 0.02 + float(d.recoil) * 0.012
	flash_light.light_energy = 1.6 if d.id != "knife" else 0.0
	pitch = clampf(pitch + deg_to_rad(float(d.recoil) * 0.35), deg_to_rad(-89), deg_to_rad(89))
	yaw += deg_to_rad(randf_range(-0.25, 0.25) * float(d.recoil))
	if d.id == "awp":
		scoped = false

func on_hurt(attacker: Fighter, dmg: int) -> void:
	match_node.hud.hurt_flash(attacker, dmg)

func _physics_process(dt: float) -> void:
	super._physics_process(dt)
	if not alive:
		return
	rotation.y = yaw
	camera.rotation.x = pitch + deg_to_rad(punch.x)
	camera.rotation.y = deg_to_rad(punch.y)
	var cmd_ok: bool = match_node != null and not match_node.ui_blocking() and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
	var wish := Vector3.ZERO
	if cmd_ok and not frozen and not match_node.is_busy(self):
		var iv := Vector2(
			float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A)),
			float(Input.is_physical_key_pressed(KEY_S)) - float(Input.is_physical_key_pressed(KEY_W)))
		wish = (Basis(Vector3.UP, yaw) * Vector3(iv.x, 0, iv.y))
		if wish.length() > 1.0:
			wish = wish.normalized()
	crouching = cmd_ok and Input.is_physical_key_pressed(KEY_CTRL)
	var r := rules()
	var walking: bool = cmd_ok and Input.is_physical_key_pressed(KEY_SHIFT)
	var speed := max_speed() * (float(r.duck_mult) if crouching else (float(r.walk_mult) if walking else 1.0))
	if scoped:
		speed = minf(speed, 150.0 * U2M)
	var hull: CollisionShape3D = $Hull
	var target_h := 1.3 if crouching else 1.83
	var cap: CapsuleShape3D = hull.shape
	cap.height = lerpf(cap.height, target_h, 0.4)
	hull.position.y = cap.height / 2
	camera.position.y = lerpf(camera.position.y, EYE_CROUCH if crouching else EYE, 0.25)
	var space: bool = cmd_ok and not frozen and Input.is_physical_key_pressed(KEY_SPACE)
	var want_jump: bool
	if r.jump_on_press:
		want_jump = (space and not _space_was) or _jump_queued
	else:
		want_jump = space
	_space_was = space
	_jump_queued = false
	if not cmd_ok or frozen:
		want_jump = false
	jump_stamina = maxf(0.0, jump_stamina - dt * 1000.0)
	var on_floor := is_on_floor()
	if on_floor and not _was_floor:
		_landed_at = now()
		if r.jump_stamina and jump_stamina > 0.0:
			var f := (100.0 - jump_stamina * 0.001 * 19.0) * 0.01
			velocity.x *= f
			velocity.z *= f
	_was_floor = on_floor
	if on_floor:
		_friction(dt, float(r.friction), float(r.stop_speed) * U2M)
		_accelerate(wish, speed, float(r.accel), dt)
		if want_jump:
			_jump(r)
	else:
		_accelerate(wish, minf(speed, float(r.air_cap) * U2M), float(r.air_accel), dt, speed)
		velocity.y -= float(r.gravity) * U2M * dt
	move_and_slide()
	# weapon bob and kick
	var hv := Vector2(velocity.x, velocity.z).length()
	_bob += dt * hv * 1.6
	_kick = lerpf(_kick, 0.0, dt * 12.0)
	viewmodel_root.position = Vector3(sin(_bob) * 0.006 * minf(hv, 6.0) / 6.0, absf(cos(_bob)) * 0.006 * minf(hv, 6.0) / 6.0, _kick)
	flash_light.light_energy = lerpf(flash_light.light_energy, 0.0, dt * 30.0)
	camera.fov = 25.0 if scoped else Game.settings.fov
	camera.far = float(Defs.GRAPHICS[Game.settings.quality].view_distance)

	viewmodel_root.visible = not scoped
	play_anim("run" if hv > 1.0 else "idle")
	var lmb: bool = cmd_ok and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	if lmb and build_piece != "" and now() >= _next_build:
		_place()   # turbo build
		_next_build = now() + TURBO
	if lmb and editing:
		_paint(editing.tile_at(eye_pos(), aim_dir()))
	if cmd_ok and build_piece == "" and editing == null and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
		if slot == 5:
			match_node.plant_tick(self, dt)
		elif current_def().auto or now() >= next_fire:
			if current_def().auto or not _held_last:
				fire()
	elif slot == 5:
		match_node.plant_cancel(self)
	_held_last = cmd_ok and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	using = cmd_ok and Input.is_physical_key_pressed(KEY_E)
	if using:
		match_node.defuse_tick(self, dt)
	else:
		match_node.defuse_cancel(self)
	_update_ghost()
	_update_edit()

var _held_last := false

func _update_ghost() -> void:
	if build_piece == "":
		ghost.visible = false
		return
	var p: Dictionary = match_node.placement_for(self, build_piece)
	ghost.global_transform = p.xf
	ghost.visible = true
	var ok: bool = match_node.can_build(self, build_piece, p)
	(ghost.material_override as ShaderMaterial).set_shader_parameter("tint", Color(0.3, 0.6, 1.0, 0.3) if ok else Color(1, 0.3, 0.25, 0.3))

func _jump(r: Dictionary) -> void:
	var cap := float(r.bhop_cap)
	if cap > 0.0:
		# 1.6 bunny-hop limiter: above 1.2 x maxspeed you are slowed to 0.8 of that cap
		var hs := Vector2(velocity.x, velocity.z).length()
		var lim := cap * max_speed()
		if hs > lim:
			var k := (lim / hs) * float(r.bhop_factor)
			velocity.x *= k
			velocity.z *= k
	var js := float(r.jump_speed) * U2M
	if r.jump_stamina and jump_stamina > 0.0:
		js *= (100.0 - jump_stamina * 0.001 * 19.0) * 0.01
	velocity.y = js
	if r.jump_stamina:
		jump_stamina = 1315.789429
	if now() - _landed_at < 0.25 and Vector2(velocity.x, velocity.z).length() > 200.0 * U2M:
		bhops += 1
		if bhops >= 3 and r.id == "cs16":
			Game.melty_report("bhop-1.6", true, "bunny hops chained under 1.6 rules")

func _friction(dt: float, friction: float, stop_speed: float) -> void:
	var v := Vector3(velocity.x, 0, velocity.z)
	var sp := v.length()
	if sp < 0.01:
		velocity.x = 0
		velocity.z = 0
		return
	var drop := maxf(sp, stop_speed) * friction * dt
	var ns := maxf(sp - drop, 0.0) / sp
	velocity.x *= ns
	velocity.z *= ns

func _accelerate(wish: Vector3, wishspeed: float, accel: float, dt: float, accel_speed := -1.0) -> void:
	if wish.length() < 0.01:
		return
	var dir := wish.normalized()
	var cur := Vector3(velocity.x, 0, velocity.z).dot(dir)
	var add := wishspeed - cur
	if add <= 0:
		return
	var a := minf(accel * (accel_speed if accel_speed > 0 else wishspeed) * dt * wish.length(), add)
	velocity.x += a * dir.x
	velocity.z += a * dir.z

func die(killer: Fighter, weapon_id: String, headshot: bool) -> void:
	build_piece = ""
	_cancel_edit()
	match_node.close_tacmap()
	ghost.visible = false
	scoped = false
	super.die(killer, weapon_id, headshot)
	_refresh_viewmodel()
	for mi in model.find_children("*", "MeshInstance3D", true, false):
		(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON

func respawn(at: Transform3D) -> void:
	build_piece = ""
	_cancel_edit()
	super.respawn(at)
	camera.position.y = EYE
	camera.current = true
	if model:
		for mi in model.find_children("*", "MeshInstance3D", true, false):
			(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	_refresh_viewmodel()
