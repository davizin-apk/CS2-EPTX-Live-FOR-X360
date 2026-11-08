class_name BotFighter
extends Fighter
## Bot brain: buys, walks the navmesh to a bomb site, spots/aims/shoots, plants, defuses, builds cover.
## In portal mode it also opens portals, takes portal shortcuts and sees and shoots through portals.

var skill: Dictionary
var goal := Vector3.ZERO
var goal_kind := "site"      # site, bomb, hold, follow
var site := "A"
var path := PackedVector3Array()
var path_i := 0
var target: Fighter
var seen_since := 0.0
var last_seen_pos := Vector3.ZERO
var next_think := 0.0
var next_repath := 0.0
var stuck_t := 0.0
var last_pos := Vector3.ZERO
var build_cd := 0.0
var aim_err := Vector2.ZERO
var hold_until := 0.0
var autopilot := false
var aim_height := 1.2
var stuck_count := 0
var unstick := Vector3.ZERO
var unstick_until := 0.0       # test mode drives the player's seat with this brain
var portal_done := false       # opened this round's portals already
var portal_delay := 5.0        # seconds into the round before opening them
var portal_goal: Portal        # walking into this portal as a shortcut
var target_portal: Portal      # the target is seen through this portal

func setup_bot(skill_id: String) -> void:
	skill = Defs.BOT_SKILLS[skill_id]

func round_start(world: World, plan_site: String) -> void:
	site = plan_site
	target = null
	path = PackedVector3Array()
	goal_kind = "site"
	hold_until = 0.0
	goal = world.random_point_in_site(site)
	next_repath = 0.0
	portal_goal = null
	target_portal = null
	portal_delay = randf_range(3.0, 9.0)

func buy_round() -> void:
	var rifle := "ak47" if team == "T" else "m4a1s"
	if money >= 4750 + 1000 and randf() < 0.18:
		buy("awp")
	elif money >= int(wd(rifle).price) + 650:
		buy(rifle)
	if match_node.mode.portals and money >= int(Defs.WEAPONS.portalgun.price) + 650 and randf() < 0.5:
		if buy("portalgun"):
			Game.log_line("portal: %s bought the Portal Gun" % display_name)
	if money >= 1000:
		buy("kevlar_helmet")
	elif money >= 650:
		buy("kevlar")
	if team == "CT" and money >= 400 and randf() < 0.6:
		buy("defuser")
	# use the best gun we have
	for s in [1, 2, 3]:
		if weapons.has(s):
			switch_to(s)
			break

func _physics_process(dt: float) -> void:
	super._physics_process(dt)
	if not alive or match_node == null:
		return
	var t := now()
	if t >= next_think:
		next_think = t + 0.12
		_think()
	var wish := Vector3.ZERO
	var busy: bool = match_node.is_busy(self)
	if not frozen and not busy:
		wish = _steer(dt)
	# look
	var look_target := Vector3.ZERO
	var has_look := false
	if target and target.alive and t - seen_since > 0.0:
		look_target = _aim_point() + Vector3(aim_err.x, aim_err.y, 0)
		has_look = true
	elif wish.length() > 0.1:
		look_target = global_position + Vector3(0, EYE, 0) + wish.normalized() * 5.0
		has_look = true
	if has_look:
		var d := (look_target - eye_pos())
		var want_yaw := atan2(-d.x, -d.z)
		var want_pitch := atan2(d.y, Vector2(d.x, d.z).length())
		var turn := 10.0 * dt if target else 6.0 * dt
		yaw = lerp_angle(yaw, want_yaw, clampf(turn, 0, 1))
		pitch = lerpf(pitch, want_pitch, clampf(turn, 0, 1))
	rotation.y = yaw
	# move
	var speed := max_speed()
	if target and target.alive:
		speed *= 0.0 if current_def().id != "knife" else 1.0   # stop to shoot, like a disciplined player
	var hv := Vector3(velocity.x, 0, velocity.z)
	var want := wish * speed
	hv = hv.move_toward(want, (40.0 if want.length() > 0 else 30.0) * dt)
	velocity.x = hv.x
	velocity.z = hv.z
	apply_gravity(dt)
	move_and_slide()
	play_anim("run" if hv.length() > 1.0 else "idle")
	# shoot once the reaction time has passed and the aim has settled a bit
	if target and target.alive and t - seen_since > float(skill.reaction_s) and not frozen:
		var d2 := _aim_point() - global_position
		if current_def().id == "knife" and (target_portal != null or d2.length() > 2.0):
			pass
		else:
			aim_err = aim_err.lerp(Vector2.ZERO, clampf(dt / float(skill.aim_settle_s), 0, 1))
			if absf(angle_difference(yaw, atan2(-d2.x, -d2.z))) < deg_to_rad(8):
				fire()

func _think() -> void:
	var world: World = match_node.world
	# spotting
	var best: Fighter = null
	var best_d := INF
	for f in match_node.fighters:
		if f.team == team or not f.alive:
			continue
		var to: Vector3 = f.global_position - global_position
		var dist := to.length()
		if dist > float(skill.see_range_m):
			continue
		var fwd := Vector3(-sin(yaw), 0, -cos(yaw))
		var ang := rad_to_deg(fwd.angle_to(Vector3(to.x, 0, to.z)))
		if ang > float(skill.fov_deg) * 0.5 and f != target and dist > 4.0:
			continue
		if _can_see(f) and dist < best_d:
			best = f
			best_d = dist
	var via: Portal = null
	if best == null and match_node.mode.portals:
		var seen := _spot_through_portals()
		if not seen.is_empty():
			best = seen.f
			best_d = seen.d
			via = seen.p
	target_portal = via
	if best != target:
		if best:
			seen_since = now()
			aim_height = 1.55 if randf() < float(skill.build_chance) * 0.4 else randf_range(1.0, 1.3)
			var e := deg_to_rad(float(skill.aim_error_deg)) * best_d
			aim_err = Vector2(randf_range(-e, e), randf_range(-e * 0.6, e * 0.6))
		target = best
	if target:
		last_seen_pos = target.global_position
	# reload when safe
	var w := current()
	if not target and not w.is_empty() and int(current_def().mag) > 0 and w.mag < int(current_def().mag) * 0.4:
		start_reload()
	# objectives
	match_node.bot_objective(self)
	if build_cd > 0:
		build_cd -= 0.12

func _can_see(f: Fighter) -> bool:
	var space := get_world_3d().direct_space_state
	for h in [1.45, 1.0]:
		var to: Vector3 = f.global_position + Vector3(0, h if not f.crouching else h * 0.7, 0)
		var q := PhysicsRayQueryParameters3D.create(eye_pos(), to, Game.LAYER_WORLD | Game.LAYER_BUILD | Game.LAYER_FIGHTER, [get_rid()])
		var hit := space.intersect_ray(q)
		if hit.is_empty() or hit.collider == f:
			return true
	return false

## Where to aim at the target: its body, or where its body shows up inside the portal it is seen through.
func _aim_point() -> Vector3:
	var p := target.global_position + Vector3(0, (aim_height if not target.crouching else aim_height * 0.7), 0)
	if target_portal and is_instance_valid(target_portal) and target_portal.twin and is_instance_valid(target_portal.twin):
		p = target_portal.twin.through() * p
	return p

## An enemy visible through a portal pair that this bot can see into: {f, p, d} or {}.
func _spot_through_portals() -> Dictionary:
	var space := get_world_3d().direct_space_state
	var eye := eye_pos()
	var fwd := Vector3(-sin(yaw), 0, -cos(yaw))
	var best := {}
	for p in match_node.linked_portals():
		var to_p: Vector3 = p.global_position - eye
		var dp := to_p.length()
		if dp > float(skill.see_range_m) or (eye - p.global_position).dot(p.normal()) < 0.2:
			continue
		if rad_to_deg(fwd.angle_to(Vector3(to_p.x, 0, to_p.z))) > float(skill.fov_deg) * 0.5 and dp > 3.0:
			continue
		var into := space.intersect_ray(PhysicsRayQueryParameters3D.create(eye, p.global_position + p.normal() * 0.05, Game.LAYER_WORLD | Game.LAYER_BUILD | Game.LAYER_FIGHTER, [get_rid()]))
		if not into.is_empty():
			continue
		var q: Portal = p.twin
		var from: Vector3 = q.global_position + q.normal() * 0.1
		for f in match_node.fighters:
			if f.team == team or not f.alive:
				continue
			var chest: Vector3 = f.global_position + Vector3(0, 1.2 if not f.crouching else 0.85, 0)
			var d := dp + from.distance_to(chest)
			if d > float(skill.see_range_m) or (chest - q.global_position).dot(q.normal()) < 0.1:
				continue
			if not best.is_empty() and d >= best.d:
				continue
			var out := space.intersect_ray(PhysicsRayQueryParameters3D.create(from, chest, Game.LAYER_WORLD | Game.LAYER_BUILD | Game.LAYER_FIGHTER))
			if out.is_empty() or out.collider == f:
				best = {"f": f, "p": p, "d": d}
	return best

func on_teleported() -> void:
	portal_goal = null
	path = PackedVector3Array()
	next_repath = 0.0
	stuck_t = 0.0
	last_pos = global_position

func set_goal(p: Vector3, kind: String) -> void:
	if goal.distance_to(p) > 1.0 or goal_kind != kind:
		goal = p
		goal_kind = kind
		next_repath = 0.0

func _steer(dt: float) -> Vector3:
	var world: World = match_node.world
	var t := now()
	if t >= next_repath:
		next_repath = t + 2.5
		portal_goal = null
		var cut: Dictionary = match_node.portal_shortcut(self, goal) if match_node.mode.portals else {}
		if not cut.is_empty():
			portal_goal = cut.entry
			path = world.path(global_position, cut.approach)
		else:
			path = world.path(global_position, goal)
		path_i = 0
	if portal_goal and not (is_instance_valid(portal_goal) and portal_goal.twin and is_instance_valid(portal_goal.twin)):
		portal_goal = null
		next_repath = 0.0
	if path.is_empty():
		return Vector3.ZERO
	while path_i < path.size() and Vector2(path[path_i].x - global_position.x, path[path_i].z - global_position.z).length() < 0.6:
		path_i += 1
	if path_i >= path.size():
		if portal_goal:
			# at the portal: walk straight into it
			var into := portal_goal.global_position - global_position
			into.y = 0
			return into.normalized() if into.length() > 0.05 else -Vector3(portal_goal.normal().x, 0, portal_goal.normal().z).normalized()
		return Vector3.ZERO
	var to := path[path_i] - global_position
	to.y = 0
	if t < unstick_until:
		return unstick
	# stuck: hop and repath, then sidestep if hopping didn't free us
	stuck_t += dt
	if stuck_t > 1.2:
		if Vector2(global_position.x - last_pos.x, global_position.z - last_pos.z).length() < 0.4:
			stuck_count += 1
			if is_on_floor():
				velocity.y = JUMP_SPEED
			next_repath = 0.0
			if stuck_count >= 2:
				var side := to.normalized().cross(Vector3.UP) * (1.0 if randf() < 0.5 else -1.0)
				unstick = (side - to.normalized() * 0.5).normalized()
				unstick_until = t + 0.9
		else:
			stuck_count = 0
		last_pos = global_position
		stuck_t = 0.0
	return to.normalized() if to.length() > 0.05 else Vector3.ZERO

func on_hurt(attacker: Fighter, _dmg: int) -> void:
	if attacker and attacker.alive and target == null:
		target = attacker
		seen_since = now() - float(skill.reaction_s) * 0.5
		var e := deg_to_rad(float(skill.aim_error_deg)) * global_position.distance_to(attacker.global_position)
		aim_err = Vector2(randf_range(-e, e), randf_range(-e * 0.6, e * 0.6))
		yaw = lerp_angle(yaw, atan2(-(attacker.global_position.x - global_position.x), -(attacker.global_position.z - global_position.z)), 0.5)
	# build-fight reflex: throw up a wall toward whoever is shooting
	if match_node.building_enabled() and build_cd <= 0 and attacker and randf() < float(skill.build_chance):
		build_cd = 3.5
		# the material it has most of, like a player saving the good stuff
		for m in mats:
			if mats[m] > mats[build_mat]:
				build_mat = m
		var d := attacker.global_position - global_position
		var y := atan2(-d.x, -d.z)
		match_node.try_build(self, "wall", match_node.placement_for(self, "wall", y, 0.0))
		# the better bots also throw a ramp up behind the wall to take height
		if randf() < float(skill.build_chance) * 0.5:
			match_node.try_build(self, "ramp", match_node.placement_for(self, "ramp", y, 0.0))
