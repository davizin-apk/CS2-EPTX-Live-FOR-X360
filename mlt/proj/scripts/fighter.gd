class_name Fighter
extends CharacterBody3D
## Shared body for the player and the bots: health, armor, money, weapons, shooting, hits, agent model.

const Defs = preload("res://scripts/gen/defs.gd")
const U2M := 0.0254
const GRAVITY := 800.0 * U2M
const JUMP_SPEED := 301.993 * U2M
const EYE := 1.64
const EYE_CROUCH := 1.18

signal died(victim: Fighter, killer: Fighter, weapon_id: String, headshot: bool)

var match_node: Node   # match.gd
var team := "CT"
var display_name := "Player"
var is_bot := true
var hp := 100
var armor := 0
var helmet := false
var has_kit := false
var has_bomb := false
var money := 800
var mats := {"wood": 0, "brick": 0, "metal": 0}   # building materials
var build_mat := "wood"
var portal_cd := 0.0          # no portal trips until this time
var kills := 0
var deaths := 0
var alive := true
var frozen := false
var crouching := false
var weapons := {}            # slot -> {id, mag, reserve}
var slot := 3
var last_slot := 2
var next_fire := 0.0
var reload_until := 0.0
var recoil := 0.0            # accumulated spray, decays
var punch := Vector2.ZERO     # 1.6 view punch (pitch, yaw) in degrees; bullets follow view + punch
var spray_shots := 0          # 1.6: consecutive shots, drives spread and kick
var _last_shot := -10.0
var _kick_left := false
var yaw := 0.0
var pitch := 0.0
var model: Node3D
var anim: AnimationPlayer
var hand_attach: BoneAttachment3D
var held_model: Node3D
var _cur_anim := ""
var _audio: AudioStreamPlayer3D

func _ready() -> void:
	collision_layer = Game.LAYER_FIGHTER
	collision_mask = Game.LAYER_WORLD | Game.LAYER_CLIP | Game.LAYER_BUILD
	floor_snap_length = 0.35
	floor_max_angle = deg_to_rad(46)
	safe_margin = 0.02
	var shape := CollisionShape3D.new()
	var cap := CapsuleShape3D.new()
	cap.radius = 0.38
	cap.height = 1.83
	shape.shape = cap
	shape.position.y = 0.915
	shape.name = "Hull"
	add_child(shape)
	_audio = AudioStreamPlayer3D.new()
	_audio.position.y = 1.5
	_audio.unit_size = 12.0
	_audio.max_db = 3.0
	add_child(_audio)

# ---------------------------------------------------------------- agent model (third person)
func build_model() -> void:
	if model:
		model.queue_free()
	model = Game.instance(Defs.TEAMS[team].agent_asset) if OS.get_environment("BOTSTRIKE_NOBOTMODEL") != "1" else null
	if model == null:
		var mi := MeshInstance3D.new()
		var cm := CapsuleMesh.new()
		cm.radius = 0.35
		cm.height = 1.8
		mi.mesh = cm
		mi.position.y = 0.9
		model = Node3D.new()
		model.add_child(mi)
	add_child(model)
	# Source models face +X, which lands on Godot +Z; fighters look down -Z.
	model.rotation.y = PI
	for n in model.find_children("*", "Node3D", true, false):
		var nn := String(n.name).to_lower()
		if nn.contains("defusekit") or nn.contains("firstperson"):
			n.visible = false
	anim = Game.animate_agent(model)
	if anim:
		for a in anim.get_animation_list():
			if a.contains("idle") or a.contains("run"):
				anim.get_animation(a).loop_mode = Animation.LOOP_LINEAR
	var skels := model.find_children("*", "Skeleton3D", true, false)
	if skels.size() > 0:
		var sk: Skeleton3D = skels[0]
		var bone := Game.hand_bone(sk)
		if bone >= 0:
			hand_attach = BoneAttachment3D.new()
			hand_attach.bone_idx = bone
			sk.add_child(hand_attach)
	play_anim("idle")
	refresh_held()

func play_anim(kind: String) -> void:
	if anim == null or kind == _cur_anim:
		return
	var name := ""
	for a in anim.get_animation_list():
		var s := String(a)
		if s == kind or (kind == "idle" and s.contains("idle")) or (kind == "run" and s.contains("run_fast")) or (kind == "fire" and s.contains("firing")) or (kind == "death" and s.contains("death")) or (kind == "reload" and s.contains("reload")):
			name = s
			break
	if name == "":
		return
	_cur_anim = kind
	anim.play(name, 0.15)

func refresh_held() -> void:
	if held_model:
		held_model.queue_free()
		held_model = null
	if model == null or not alive or slot == 5:
		return
	var w := current()
	if w.is_empty():
		return
	held_model = Portal.gun_model(false) if w.id == "portalgun" else Game.instance(Defs.WEAPONS[w.id].model_asset)
	if held_model:
		model.add_child(held_model)
		held_model.position = Game.grip_offset(slot)
		if w.id == "portalgun":
			held_model.rotation.y = PI   # built pointing -Z; agents hold guns pointing +Z

# ---------------------------------------------------------------- loadout
func give(id: String) -> void:
	var d: Dictionary = wd(id)
	weapons[int(d.slot)] = {"id": id, "mag": int(d.mag), "reserve": int(d.reserve)}

func reset_loadout() -> void:
	weapons.clear()
	give("knife")
	give(Defs.TEAMS[team].default_pistol)
	slot = 2
	armor = 0
	helmet = false
	has_kit = false

func current() -> Dictionary:
	return weapons.get(slot, {})

func current_def() -> Dictionary:
	var w := current()
	return wd(w.id) if not w.is_empty() else wd("knife")

## The match's ruleset row (rulesets sheet): "cs2" outside a match.
func rules() -> Dictionary:
	if match_node != null and match_node.get("mode") is Dictionary and not match_node.mode.is_empty():
		return Defs.RULESETS[match_node.mode.ruleset]
	return Defs.RULESETS.cs2

static var _wd_cache := {}

## Weapon definition for the current ruleset: the weapons row, with the weapons16 row's
## columns laid over it under the 1.6 rules.
func wd(id: String) -> Dictionary:
	var r := rules()
	if r.weapon_sheet != "weapons16":
		return Defs.WEAPONS[id]
	if not _wd_cache.has(id):
		var d: Dictionary = Defs.WEAPONS[id].duplicate()
		for row in Defs.WEAPONS16.values():
			if row.weapon == id:
				for k in row:
					if k != "id" and k != "weapon":
						d[k] = row[k]
		_wd_cache[id] = d
	return _wd_cache[id]

func switch_to(s: int) -> void:
	if s == 5 and has_bomb:
		if slot != 5:
			last_slot = slot
		slot = 5
		refresh_held()
		return
	if not weapons.has(s) or s == slot:
		return
	last_slot = slot
	slot = s
	next_fire = maxf(next_fire, now() + 0.25)
	reload_until = 0.0
	refresh_held()

func max_speed() -> float:
	if slot == 5:
		return 250.0 * U2M
	return float(current_def().move_speed) * U2M

func now() -> float:
	return Time.get_ticks_msec() / 1000.0

func buy(id: String) -> bool:
	var price := 0
	if Defs.WEAPONS.has(id):
		price = int(wd(id).price)
		var wt: String = Defs.WEAPONS[id].team
		if wt != "any" and wt != team:
			return false
		var om: String = Defs.WEAPONS[id].only_mode
		if om != "any" and (match_node == null or match_node.mode.id != om):
			return false
		if weapons.has(int(Defs.WEAPONS[id].slot)) and weapons[int(Defs.WEAPONS[id].slot)].id == id:
			return false
	elif Defs.EQUIPMENT.has(id):
		price = int(Defs.EQUIPMENT[id].price)
		var et: String = Defs.EQUIPMENT[id].team
		if et != "any" and et != team:
			return false
		if id == "defuser" and has_kit:
			return false
		if id.begins_with("kevlar") and armor >= 100 and (helmet or id == "kevlar"):
			return false
	if money < price:
		return false
	money -= price
	if Defs.WEAPONS.has(id):
		give(id)
		switch_to(int(Defs.WEAPONS[id].slot))
	elif id == "kevlar":
		armor = 100
	elif id == "kevlar_helmet":
		armor = 100
		helmet = true
	elif id == "defuser":
		has_kit = true
	return true

# ---------------------------------------------------------------- shooting
func eye_pos() -> Vector3:
	return global_position + Vector3(0, EYE_CROUCH if crouching else EYE, 0)

func aim_dir() -> Vector3:
	return Basis.from_euler(Vector3(pitch + deg_to_rad(punch.x), yaw + deg_to_rad(punch.y), 0)) * Vector3.FORWARD

func can_fire() -> bool:
	if not alive or frozen or slot == 5:
		return false
	var w := current()
	if w.is_empty() or now() < next_fire or now() < reload_until:
		return false
	if int(current_def().mag) > 0 and w.mag <= 0:
		start_reload()
		return false
	return true

func fire() -> void:
	if not can_fire():
		return
	var d := current_def()
	var w := current()
	next_fire = now() + float(d.fire_interval)
	if int(d.mag) > 0:
		w.mag -= 1
	if d.id == "portalgun":
		if match_node:
			match_node.fire_portal(self, eye_pos(), aim_dir())
		play_shot_sound(d)
		on_fired(d)
		return
	if rules().recoil_model == "punch":
		_fire_16(d)
		return
	var spread := float(d.spread) + recoil * 0.004
	var hv := Vector2(velocity.x, velocity.z).length()
	if hv > 1.5:
		spread += hv * 0.006
	if not is_on_floor():
		spread += 0.06
	if crouching:
		spread *= 0.75
	if d.id == "awp" and not is_scoped():
		spread += 0.06
	var dir := aim_dir()
	dir = (dir + Vector3(randfn(0, spread), randfn(0, spread), randfn(0, spread))).normalized()
	recoil = minf(recoil + float(d.recoil), 14.0)
	_trace(eye_pos(), dir, float(d.range_m), d)
	play_shot_sound(d)
	play_anim("fire")
	on_fired(d)

## 1.6 shot: spread grows with consecutive shots (and moving / air), then the view punch
## kicks up and drifts sideways, flipping side at random like the classic KickBack.
func _fire_16(d: Dictionary) -> void:
	if now() - _last_shot > 0.35:
		spray_shots = 0
	_last_shot = now()
	var spread := minf(float(d.spread) + spray_shots * float(d.spread_per_shot), float(d.spread_max))
	var hv := Vector2(velocity.x, velocity.z).length() / U2M
	if not is_on_floor():
		spread += 0.12 if d.id != "awp" else 0.25
	elif hv > 140.0:
		spread += 0.04 if d.id != "awp" else 0.1
	elif hv > 10.0:
		spread += 0.012
	if crouching:
		spread *= 0.8
	if d.id == "awp" and not is_scoped():
		spread += 0.08
	var dir := aim_dir()
	var b := Basis.from_euler(Vector3(pitch + deg_to_rad(punch.x), yaw + deg_to_rad(punch.y), 0))
	var ox := randf_range(-0.5, 0.5) + randf_range(-0.5, 0.5)
	var oy := randf_range(-0.5, 0.5) + randf_range(-0.5, 0.5)
	dir = (dir + b.x * ox * spread + b.y * oy * spread).normalized()
	spray_shots += 1
	if spray_shots >= 3 and punch.x > 1.0:
		Game.melty_report("recoil-1-6", true, "a spray climbed with 1.6 view punch")
	var k: Array = d.kick
	var up := minf(float(k[0]) + (spray_shots - 1) * float(k[1]), float(k[2]))
	var side := minf(float(k[3]) + (spray_shots - 1) * float(k[4]), float(k[5]))
	punch.x = minf(punch.x + up, float(k[2]) * 1.6)
	punch.y = clampf(punch.y + (side if _kick_left else -side), -float(k[5]), float(k[5]))
	if randi() % 8 == 0:
		_kick_left = not _kick_left
	_trace(eye_pos(), dir, float(d.range_m), d)
	play_shot_sound(d)
	play_anim("fire")
	on_fired(d)

func is_scoped() -> bool:
	return false

func on_fired(_d: Dictionary) -> void:
	pass

func play_shot_sound(d: Dictionary) -> void:
	var s := Game.sound(d.sound_asset)
	if s:
		_audio.stream = s
		_audio.pitch_scale = randf_range(0.97, 1.03)
		_audio.play()

static var _portal_shots := 0

func _trace(from: Vector3, dir: Vector3, range_m: float, d: Dictionary, depth := 0) -> void:
	var space := get_world_3d().direct_space_state
	var q := PhysicsRayQueryParameters3D.create(from, from + dir * range_m, Game.LAYER_WORLD | Game.LAYER_FIGHTER | Game.LAYER_BUILD, [get_rid()] if depth == 0 else [])
	var hit := space.intersect_ray(q)
	var end := from + dir * range_m
	if not hit.is_empty():
		end = hit.position
	var tracer_from := from + dir * 0.6 + Vector3(0, -0.08, 0) if depth == 0 else from
	# a portal in the way carries the bullet on out of its twin
	if match_node and depth < 3 and range_m > 0.5:
		var ph: Dictionary = match_node.portal_on_segment(from, end)
		if not ph.is_empty():
			if d.id != "knife":
				match_node.effect_tracer(tracer_from, ph.point)
			var m: Transform3D = ph.through
			var nd := (m.basis * dir).normalized()
			if _portal_shots < 12:
				_portal_shots += 1
				Game.log_line("portal: %s's %s shot went through %s" % [display_name, d.id, ph.portal.color_id])
			_trace(m * ph.point + nd * 0.02, nd, range_m - from.distance_to(ph.point), d, depth + 1)
			return
	if not hit.is_empty():
		var c = hit.collider
		if c is Fighter and c.alive:
			var local_y: float = hit.position.y - c.global_position.y
			var head_y := 1.42 if not c.crouching else 0.98
			var part := "chest"
			if local_y > head_y:
				part = "head"
			elif local_y < 0.75:
				part = "legs"
			elif local_y < 1.05:
				part = "stomach"
			c.take_hit(self, d, part)
			if match_node:
				match_node.effect_blood(hit.position)
		elif c and c.has_method("damage"):
			c.damage(float(d.damage), self)
			if match_node:
				match_node.effect_impact(hit.position, hit.normal, true)
		elif match_node:
			match_node.effect_impact(hit.position, hit.normal, false)
	if match_node and d.id != "knife":
		match_node.effect_tracer(tracer_from, end)

func take_hit(attacker: Fighter, d: Dictionary, part: String) -> void:
	if not alive:
		return
	var dmg := float(d.damage)
	var mult: float = {"head": float(d.head_mult), "chest": 1.0, "stomach": 1.25, "legs": 0.75}[part]
	dmg *= mult
	if d.id == "knife":
		dmg = float(d.damage)
	var armored := armor > 0 and (part != "head" or helmet) and part != "legs"
	if armored:
		var pen := float(d.armor_pen)
		var absorbed := dmg * (1.0 - pen)
		armor = maxi(0, armor - int(absorbed * 0.5))
		dmg *= pen
	hp -= int(round(dmg))
	on_hurt(attacker, int(round(dmg)))
	if hp <= 0:
		hp = 0
		die(attacker, d.id, part == "head")

func on_hurt(_attacker: Fighter, _dmg: int) -> void:
	pass

func die(killer: Fighter, weapon_id: String, headshot: bool) -> void:
	if not alive:
		return
	alive = false
	deaths += 1
	velocity = Vector3.ZERO
	collision_layer = 0
	$Hull.disabled = true
	if held_model:
		held_model.queue_free()
		held_model = null
	play_anim("death")
	if _cur_anim != "death" and model:
		# no death clip in this CS2 build: topple the agent over
		var tw := create_tween()
		tw.tween_property(model, "rotation:x", -PI / 2, 0.45).set_trans(Tween.TRANS_QUAD).set_ease(Tween.EASE_IN)
		tw.parallel().tween_property(model, "position:y", 0.15, 0.45)
		if anim:
			anim.pause()
	died.emit(self, killer, weapon_id, headshot)

func start_reload() -> void:
	var w := current()
	var d := current_def()
	if w.is_empty() or int(d.mag) == 0 or w.reserve <= 0 or w.mag >= int(d.mag) or now() < reload_until:
		return
	reload_until = now() + float(d.reload_time)
	play_anim("reload")
	var t := get_tree().create_timer(float(d.reload_time))
	t.timeout.connect(func():
		if not alive or current() != w:
			return
		var need := int(d.mag) - int(w.mag)
		var take := mini(need, int(w.reserve))
		w.mag += take
		w.reserve -= take)

func respawn(at: Transform3D) -> void:
	alive = true
	hp = 100
	global_transform = Transform3D(Basis.IDENTITY, at.origin)
	yaw = at.basis.get_euler().y
	pitch = 0.0
	velocity = Vector3.ZERO
	collision_layer = Game.LAYER_FIGHTER
	$Hull.disabled = false
	recoil = 0.0
	punch = Vector2.ZERO
	spray_shots = 0
	reload_until = 0.0
	crouching = false
	_cur_anim = ""
	if weapons.is_empty():
		reset_loadout()
	if not weapons.has(slot):
		slot = 2
	for k in weapons:
		var w: Dictionary = weapons[k]
		var d: Dictionary = wd(w.id)
		w.mag = int(d.mag)
		w.reserve = int(d.reserve)
	if model == null:
		build_model()
	else:
		model.rotation = Vector3(0, PI, 0)
		model.position = Vector3.ZERO
		refresh_held()
	play_anim("idle")
	rotation.y = yaw

## Comes out of a portal: center of the body at center, moving at vel, looking along look.
func teleport(center: Vector3, vel: Vector3, look: Vector3) -> void:
	global_position = center - Vector3(0, 0.9, 0)
	velocity = vel
	if Vector2(look.x, look.z).length() > 0.2:
		yaw = atan2(-look.x, -look.z)
		rotation.y = yaw
	pitch = clampf(asin(clampf(look.y, -1, 1)), deg_to_rad(-89), deg_to_rad(89))
	portal_cd = now() + 0.4
	on_teleported()

func on_teleported() -> void:
	pass

func apply_gravity(dt: float) -> void:
	if not is_on_floor():
		velocity.y -= GRAVITY * dt

func _physics_process(dt: float) -> void:
	recoil = maxf(0.0, recoil - dt * 18.0)
	var pd := float(rules().punch_decay)
	if pd > 0.0 and punch != Vector2.ZERO:
		var l := punch.length()
		punch *= maxf(l - (pd + l * 0.5) * dt, 0.0) / l
