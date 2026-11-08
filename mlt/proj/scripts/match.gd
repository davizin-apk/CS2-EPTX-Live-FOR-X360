extends Node3D
## A 5v5 match vs bots on Dust II: rounds, buy time, bomb plant/defuse/explosion, economy, halftime,
## Fortnite-style building and Portal Gun portals.

const Defs = preload("res://scripts/gen/defs.gd")
const HudScript = preload("res://scripts/hud.gd")

enum Phase { FREEZE, LIVE, PLANTED, OVER, MATCH_OVER }

var mode: Dictionary
var world: World
var hud: Node
var fighters: Array[Fighter] = []
var player: PlayerFighter
var phase := Phase.FREEZE
var phase_end := 0.0
var round_num := 1
var score := {"CT": 0, "T": 0}
var loss_streak := {"CT": 0, "T": 0}
var player_team := "CT"
var t_plan_site := "A"
var bomb_carrier: Fighter
var bomb_dropped_at := Vector3.INF
var bomb_node: Node3D
var bomb_pos := Vector3.ZERO
var bomb_site := ""
var bomb_explode_at := 0.0
var next_beep := 0.0
var planter: Fighter
var plant_progress := 0.0
var defuser: Fighter
var defuse_progress := 0.0
var spectate_i := 0
var paused := false
var _pieces: Node3D
var _music: AudioStreamPlayer
var _round_reported := false
var _built_reported := false
var _round_start_time := 0.0
var _portal_root: Node3D
var _portals := {}          # owner instance id -> {"blue": Portal, "orange": Portal}
var _portal_reported := false
var tacmap: TacMap

func _ready() -> void:
	var cfg: Dictionary = Game.match_cfg
	mode = Defs.MODES[cfg.get("mode", "classic")]
	player_team = cfg.get("side", "CT")
	if player_team == "random":
		player_team = "CT" if randf() < 0.5 else "T"
	world = World.new()
	add_child(world)
	world.setup(mode.map, true)
	_pieces = Node3D.new()
	add_child(_pieces)
	_portal_root = Node3D.new()
	add_child(_portal_root)
	var amb := AudioStreamPlayer.new()
	amb.stream = Game.sound("amb_wind")
	if amb.stream is AudioStreamWAV:
		(amb.stream as AudioStreamWAV).loop_mode = AudioStreamWAV.LOOP_FORWARD
		(amb.stream as AudioStreamWAV).loop_end = (amb.stream as AudioStreamWAV).get_length() * (amb.stream as AudioStreamWAV).mix_rate
	amb.volume_db = -14
	add_child(amb)
	amb.play()
	_spawn_fighters(cfg.get("difficulty", "normal"))
	hud = HudScript.new()
	hud.match_node = self
	add_child(hud)
	_start_round()
	if not Game.test_mode:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

func _spawn_fighters(difficulty: String) -> void:
	for team in ["CT", "T"]:
		var names: Array = Defs.TEAMS[team].bot_names.duplicate()
		for i in int(mode.team_size):
			var f: Fighter
			if team == player_team and i == 0 and (not Game.test_mode or OS.get_environment("BOTSTRIKE_FP") == "1"):
				player = PlayerFighter.new()
				f = player
				f.display_name = "You"
			else:
				var b := BotFighter.new()
				b.setup_bot(difficulty)
				f = b
				f.display_name = names[i % names.size()]
				if team == player_team and i == 0:
					b.autopilot = true
					b.display_name = "You (autopilot)"
			f.team = team
			f.match_node = self
			f.money = int(mode.start_money)
			add_child(f)
			f.died.connect(_on_died)
			fighters.append(f)

# ---------------------------------------------------------------- rounds
func _start_round() -> void:
	phase = Phase.FREEZE
	phase_end = now() + float(mode.freeze_time)
	_round_start_time = now()
	for p in get_tree().get_nodes_in_group("build_pieces"):
		p.remove_from_group("build_pieces")
		p.queue_free()
	clear_portals()
	close_tacmap()
	if bomb_node:
		bomb_node.queue_free()
		bomb_node = null
	bomb_site = ""
	bomb_dropped_at = Vector3.INF
	planter = null
	defuser = null
	plant_progress = 0.0
	defuse_progress = 0.0
	t_plan_site = "A" if randf() < 0.5 else "B"
	var idx := {"CT": 0, "T": 0}
	for f in fighters:
		var sp: Array = world.spawns[f.team]
		var tr: Transform3D = sp[idx[f.team] % sp.size()] if sp.size() > 0 else Transform3D.IDENTITY
		idx[f.team] += 1
		if not f.alive:
			f.weapons.clear()
		f.has_bomb = false
		f.respawn(tr)
		f.frozen = true
		for m in Defs.BUILD_MATERIALS:
			f.mats[m] = int(Defs.BUILD_MATERIALS[m].start_amount) if mode.building else 0
		f.build_mat = "wood"
		if f is BotFighter:
			(f as BotFighter).portal_done = false
		if f is BotFighter:
			(f as BotFighter).buy_round()
			(f as BotFighter).round_start(world, _bot_site(f))
	var ts := fighters.filter(func(f): return f.team == "T")
	bomb_carrier = ts[randi() % ts.size()]
	bomb_carrier.has_bomb = true
	if player:
		player.camera.current = true
	_play_music("mus_start", -8)
	if hud:
		hud.round_started()

func _bot_site(f: Fighter) -> String:
	if f.team == "T":
		return t_plan_site
	var cts := fighters.filter(func(x): return x.team == "CT")
	return "A" if cts.find(f) % 2 == 0 else "B"

func _process(_dt: float) -> void:
	var t := now()
	match phase:
		Phase.FREEZE:
			if t >= phase_end:
				phase = Phase.LIVE
				phase_end = t + float(mode.round_time)
				for f in fighters:
					f.frozen = false
		Phase.LIVE:
			if t >= phase_end:
				_end_round("CT", "Target saved")
		Phase.PLANTED:
			if t >= next_beep:
				var left := bomb_explode_at - t
				next_beep = t + clampf(left / 40.0, 0.12, 1.0)
				_sound_at("snd_c4_beep", bomb_pos, -4)
			if t >= bomb_explode_at:
				_explode()
		Phase.OVER:
			if t >= phase_end:
				_next_round()
		Phase.MATCH_OVER:
			if t >= phase_end and not Game.test_mode:
				back_to_menu()
	_check_bomb_pickup()

func now() -> float:
	return Time.get_ticks_msec() / 1000.0

func time_left() -> float:
	if phase == Phase.PLANTED:
		return maxf(0, bomb_explode_at - now())
	return maxf(0, phase_end - now())

func buy_allowed(f: Fighter) -> bool:
	if not f.alive:
		return false
	var in_time := phase == Phase.FREEZE or (phase == Phase.LIVE and now() - _round_start_time < float(mode.freeze_time) + float(mode.buy_time))
	var sp: Array = world.spawns[f.team]
	var near := sp.is_empty() or f.global_position.distance_to((sp[0] as Transform3D).origin) < 18.0
	return in_time and near

func _on_died(victim: Fighter, killer: Fighter, weapon_id: String, headshot: bool) -> void:
	if killer and killer != victim:
		if killer.team != victim.team:
			killer.kills += 1
			killer.money = mini(int(mode.max_money), killer.money + int(mode.kill_reward))
			if mode.building:
				for m in Defs.BUILD_MATERIALS:
					killer.mats[m] = mini(999, killer.mats[m] + int(Defs.BUILD_MATERIALS[m].per_kill))
		else:
			killer.money = maxi(0, killer.money - 300)
	if victim.has_bomb:
		victim.has_bomb = false
		bomb_dropped_at = victim.global_position
		_make_bomb_prop(bomb_dropped_at, false)
	if victim == planter:
		plant_cancel(victim)
	if victim == defuser:
		defuse_cancel(victim)
	hud.kill_feed(killer, victim, weapon_id, headshot)
	if victim == player:
		spectate_i = 0
		next_spectate()
	_check_elimination()

func _alive(team: String) -> int:
	return fighters.filter(func(f): return f.team == team and f.alive).size()

func _check_elimination() -> void:
	if phase == Phase.OVER or phase == Phase.MATCH_OVER:
		return
	if _alive("CT") == 0:
		_end_round("T", "Counter-Terrorists eliminated")
	elif _alive("T") == 0 and phase != Phase.PLANTED:
		_end_round("CT", "Terrorists eliminated")

func _end_round(winner: String, why: String) -> void:
	if phase == Phase.OVER or phase == Phase.MATCH_OVER:
		return
	score[winner] += 1
	var loser := "T" if winner == "CT" else "CT"
	loss_streak[winner] = 0
	loss_streak[loser] = mini(loss_streak[loser] + 1, 5)
	for f in fighters:
		if f.team == winner:
			f.money = mini(int(mode.max_money), f.money + int(mode.win_bonus))
		else:
			f.money = mini(int(mode.max_money), f.money + int(mode.loss_bonus) + 500 * (loss_streak[loser] - 1))
	if player:
		_play_music("mus_won" if winner == player.team else "mus_lost", -6)
	hud.round_over(winner, why)
	Game.log_line("round %d: %s win (%s) score CT %d - T %d" % [round_num, winner, why, score.CT, score.T])
	if not _round_reported:
		_round_reported = true
		Game.melty_report("round-played", true)
	planter = null
	defuser = null
	if score[winner] >= int(mode.rounds_to_win) or round_num >= int(mode.max_rounds):
		phase = Phase.MATCH_OVER
		phase_end = now() + 10.0
		hud.match_over(winner if score[winner] != score[loser] else "")
		if Game.test_mode:
			# keep playing for the whole Melty test run
			get_tree().create_timer(9.0).timeout.connect(_restart_match)
	else:
		phase = Phase.OVER
		phase_end = now() + 6.0

func _restart_match() -> void:
	score = {"CT": 0, "T": 0}
	round_num = 0
	for f in fighters:
		f.money = int(mode.start_money)
		f.weapons.clear()
		f.alive = false
	_next_round()

func _next_round() -> void:
	round_num += 1
	if round_num == int(mode.max_rounds) / 2 + 1:
		# halftime: swap sides
		var s: int = score.CT
		score.CT = score.T
		score.T = s
		for f in fighters:
			f.team = "T" if f.team == "CT" else "CT"
			f.money = int(mode.start_money)
			f.weapons.clear()
			f.alive = false
			f.build_model()
		player_team = "T" if player_team == "CT" else "CT"
		hud.notice("HALFTIME - SWITCHING SIDES")
	_start_round()

# ---------------------------------------------------------------- bomb
func is_busy(f: Fighter) -> bool:
	return f == planter or f == defuser

func plant_tick(f: Fighter, dt: float) -> void:
	if phase != Phase.LIVE or not f.has_bomb or not f.is_on_floor():
		return
	var s := world.site_at(f.global_position)
	if s == "":
		if f == player:
			hud.notice("You must be in a bomb site to plant", 1.0)
		return
	if planter != f:
		planter = f
		plant_progress = 0.0
	plant_progress += dt
	if plant_progress >= float(mode.plant_time):
		_planted(f, s)

func plant_cancel(f: Fighter) -> void:
	if planter == f:
		planter = null
		plant_progress = 0.0

func _planted(f: Fighter, s: String) -> void:
	planter = null
	plant_progress = 0.0
	f.has_bomb = false
	if f.slot == 5:
		f.switch_to(1 if f.weapons.has(1) else 2)
	bomb_pos = f.global_position
	bomb_site = s
	_make_bomb_prop(bomb_pos, true)
	phase = Phase.PLANTED
	bomb_explode_at = now() + float(mode.bomb_time)
	next_beep = now()
	f.money += 300
	_play_music("mus_planted", -6)
	hud.notice("THE BOMB HAS BEEN PLANTED", 3.0)
	for b in fighters:
		if b is BotFighter:
			(b as BotFighter).next_repath = 0.0

func _make_bomb_prop(p: Vector3, planted: bool) -> void:
	if bomb_node:
		bomb_node.queue_free()
	bomb_node = Game.instance("mdl_c4")
	if bomb_node == null:
		bomb_node = MeshInstance3D.new()
		(bomb_node as MeshInstance3D).mesh = BoxMesh.new()
		bomb_node.scale = Vector3(0.3, 0.1, 0.2)
	add_child(bomb_node)
	bomb_node.global_position = p + Vector3(0, 0.05, 0)
	var light := OmniLight3D.new()
	light.light_color = Color(1, 0.1, 0.1)
	light.omni_range = 1.5
	light.light_energy = 1.2 if planted else 0.0
	bomb_node.add_child(light)

func drop_bomb(f: Fighter) -> void:
	if not f.has_bomb:
		return
	f.has_bomb = false
	f.switch_to(1 if f.weapons.has(1) else 2)
	bomb_dropped_at = f.global_position + f.aim_dir() * Vector3(1.5, 0, 1.5)
	bomb_dropped_at = world.closest_nav_point(bomb_dropped_at)
	_make_bomb_prop(bomb_dropped_at, false)

func _check_bomb_pickup() -> void:
	if bomb_dropped_at == Vector3.INF or phase == Phase.PLANTED:
		return
	for f in fighters:
		if f.alive and f.team == "T" and f.global_position.distance_to(bomb_dropped_at) < 1.2:
			f.has_bomb = true
			bomb_carrier = f
			bomb_dropped_at = Vector3.INF
			if bomb_node:
				bomb_node.queue_free()
				bomb_node = null
			break

func defuse_tick(f: Fighter, dt: float) -> void:
	if phase != Phase.PLANTED or f.team != "CT" or f.global_position.distance_to(bomb_pos) > 1.8:
		return
	if defuser != f:
		if defuser != null:
			return
		defuser = f
		defuse_progress = 0.0
	defuse_progress += dt
	var need := float(mode.defuse_kit_time) if f.has_kit else float(mode.defuse_time)
	if defuse_progress >= need:
		defuser = null
		Game.play_ui("snd_c4_disarm", -4)
		f.money += 300
		_end_round("CT", "Bomb defused")

func defuse_cancel(f: Fighter) -> void:
	if defuser == f:
		defuser = null
		defuse_progress = 0.0

func _explode() -> void:
	phase = Phase.LIVE  # so _end_round accepts it
	_sound_at("snd_c4_explode", bomb_pos, 6)
	var flash := OmniLight3D.new()
	flash.light_color = Color(1, 0.7, 0.3)
	flash.omni_range = 30.0
	flash.light_energy = 12.0
	add_child(flash)
	flash.global_position = bomb_pos + Vector3(0, 2, 0)
	create_tween().tween_property(flash, "light_energy", 0.0, 1.2).finished.connect(flash.queue_free)
	for f in fighters:
		if f.alive:
			var d := f.global_position.distance_to(bomb_pos)
			if d < 20.0:
				f.hp = 0
				f.die(null, "c4", false)
	if bomb_node:
		bomb_node.queue_free()
		bomb_node = null
	_end_round("T", "Target bombed")

# ---------------------------------------------------------------- bot objectives (called from bot._think)
func bot_objective(b: BotFighter) -> void:
	if phase == Phase.FREEZE:
		return
	if b.team == "T":
		if b.has_bomb:
			var s := world.site_at(b.global_position)
			if s != "" and phase == Phase.LIVE and b.target == null:
				b.set_goal(b.global_position, "plant")
				plant_tick(b, 0.12)
			else:
				plant_cancel(b)
				b.set_goal(world.random_point_in_site(t_plan_site) if b.goal_kind != "site" else b.goal, "site")
		elif bomb_dropped_at != Vector3.INF and phase == Phase.LIVE:
			var nearest = fighters.filter(func(f): return f.team == "T" and f.alive).reduce(func(a, c): return c if c.global_position.distance_to(bomb_dropped_at) < a.global_position.distance_to(bomb_dropped_at) else a)
			if nearest == b:
				b.set_goal(bomb_dropped_at, "bomb")
		elif phase == Phase.PLANTED:
			if b.goal_kind != "guard":
				b.set_goal(world.closest_nav_point(bomb_pos + Vector3(randf_range(-6, 6), 0, randf_range(-6, 6))), "guard")
				if mode.building and randf() < float(b.skill.build_chance):
					try_build(b, "wall", placement_for(b, "wall", b.yaw, 0.0))
	else:
		if phase == Phase.PLANTED:
			var d := b.global_position.distance_to(bomb_pos)
			var closest = fighters.filter(func(f): return f.team == "CT" and f.alive).reduce(func(a, c): return c if c.global_position.distance_to(bomb_pos) < a.global_position.distance_to(bomb_pos) else a)
			if closest == b or _alive("T") == 0:
				b.set_goal(bomb_pos, "defuse")
				if d < 1.6 and (b.target == null or _alive("T") == 0):
					defuse_tick(b, 0.12)
				else:
					defuse_cancel(b)
			elif b.goal_kind != "retake":
				b.set_goal(world.closest_nav_point(bomb_pos + Vector3(randf_range(-5, 5), 0, randf_range(-5, 5))), "retake")
		elif b.goal_kind == "site" and b.global_position.distance_to(b.goal) < 1.5 and b.now() > b.hold_until:
			b.hold_until = b.now() + randf_range(4, 9)
			b.set_goal(world.random_point_in_site(b.site), "site")
	# test mode: the player's seat builds as soon as it can, to show the build mode works
	if b.autopilot and mode.building and phase == Phase.LIVE and not _built_reported:
		try_build(b, "wall", placement_for(b, "wall", b.yaw, 0.0))
	# portal mode: a bot carrying the Portal Gun opens a shortcut toward its goal once per round
	if mode.portals and b.weapons.has(4) and not b.portal_done and phase == Phase.LIVE and now() - _round_start_time > float(mode.freeze_time) + b.portal_delay and b.target == null:
		_bot_portals(b)

# ---------------------------------------------------------------- building (Fortnite rules)
func building_enabled() -> bool:
	return bool(mode.building) and phase != Phase.MATCH_OVER

func _piece_with_key(k: String) -> Node:
	for p in get_tree().get_nodes_in_group("build_pieces"):
		if p.key == k and not p.is_queued_for_deletion():
			return p
	return null

## Level 0 of the grid near someone: the structure next to them if there is one (so pieces line up),
## otherwise the ground under their feet.
func grid_origin(feet: Vector3) -> float:
	var best := INF
	var oy := feet.y - 0.05
	for p in get_tree().get_nodes_in_group("build_pieces"):
		var bp: BuildPiece = p
		var d := Vector2(bp.global_position.x - feet.x, bp.global_position.z - feet.z).length()
		var lv := (feet.y - bp.oy) / BuildPiece.H
		if d < BuildPiece.TILE * 1.6 and lv > -1.2 and lv < 2.5 and d < best:
			best = d
			oy = bp.oy
	return oy

func placement_for(f: Fighter, id: String, yaw := NAN, pitch := NAN) -> Dictionary:
	return BuildPiece.placement(id, f.global_position, f.yaw if is_nan(yaw) else yaw, f.pitch if is_nan(pitch) else pitch, grid_origin(f.global_position))

static var _shape_aabb := {}

## Fortnite only lets a piece go where it touches the ground or another piece.
func _supported(p: Dictionary) -> bool:
	if not _shape_aabb.has(p.id):
		_shape_aabb[p.id] = BuildPiece.make_mesh(p.id, [], 0).get_aabb()
	var b: AABB = (p.xf as Transform3D) * (_shape_aabb[p.id] as AABB)
	for q in get_tree().get_nodes_in_group("build_pieces"):
		if b.grow(0.15).intersects((q as BuildPiece).bounds):
			return true
	var qp := PhysicsShapeQueryParameters3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(maxf(b.size.x - 0.3, 0.1), 0.5, maxf(b.size.z - 0.3, 0.1))
	qp.shape = box
	qp.transform = Transform3D(Basis.IDENTITY, Vector3(b.get_center().x, b.position.y + 0.1, b.get_center().z))
	qp.collision_mask = Game.LAYER_WORLD | Game.LAYER_CLIP
	return not get_world_3d().direct_space_state.intersect_shape(qp, 1).is_empty()

func can_build(f: Fighter, id: String, p: Dictionary) -> bool:
	if p.is_empty() or not building_enabled() or not f.alive or phase == Phase.FREEZE:
		return false
	if int(f.mats.get(f.build_mat, 0)) < int(Defs.BUILD_PIECES[id].cost):
		return false
	return _piece_with_key(p.key) == null and _supported(p)

func try_build(f: Fighter, id: String, p: Dictionary) -> bool:
	if not f.is_inside_tree() or not can_build(f, id, p):
		return false
	f.mats[f.build_mat] -= int(Defs.BUILD_PIECES[id].cost)
	var piece := BuildPiece.new()
	_pieces.add_child(piece)
	piece.setup(id, f.build_mat, f.team, p)
	_sound_at("snd_knife", p.xf.origin, -6)
	if not _built_reported:
		_built_reported = true
		Game.melty_report("piece-built", true)
	return true

func edit_piece(f: Fighter, piece: BuildPiece, selected: Array) -> bool:
	if not is_instance_valid(piece) or piece.team != f.team:
		return false
	var ok := piece.apply_edit(selected)
	if ok:
		_sound_at("snd_knife", piece.bounds.get_center(), -10)
	return ok

## After a piece breaks, everything no longer connected to the ground through other pieces falls.
func check_support() -> void:
	var pieces: Array = get_tree().get_nodes_in_group("build_pieces").filter(func(x): return not x.is_queued_for_deletion())
	var held := {}
	var todo := []
	for p in pieces:
		if p.grounded:
			held[p] = true
			todo.append(p)
	while not todo.is_empty():
		var a: BuildPiece = todo.pop_back()
		for b in pieces:
			if not held.has(b) and a.touches(b):
				held[b] = true
				todo.append(b)
	var i := 0
	for p in pieces:
		if not held.has(p):
			p.remove_from_group("build_pieces")
			i += 1
			get_tree().create_timer(0.04 * i).timeout.connect(func():
				if is_instance_valid(p):
					effect_break(p.bounds.get_center())
					p.queue_free())

# ---------------------------------------------------------------- portals
func all_portals() -> Array:
	var out := []
	for k in _portals:
		for c in ["blue", "orange"]:
			var p = _portals[k].get(c)
			if p and is_instance_valid(p):
				out.append(p)
	return out

func linked_portals() -> Array:
	return all_portals().filter(func(p): return p.twin != null and is_instance_valid(p.twin))

func clear_portals() -> void:
	for p in all_portals():
		p.queue_free()
	_portals.clear()

func remove_portal(p: Portal) -> void:
	for k in _portals:
		for c in ["blue", "orange"]:
			if _portals[k].get(c) == p:
				_portals[k].erase(c)
	if p.twin and is_instance_valid(p.twin):
		p.twin.twin = null
	p.queue_free()

func _place_portal(f: Fighter, color: String, xf: Transform3D, standing: bool, on: Object) -> Portal:
	var pair: Dictionary = _portals.get(f.get_instance_id(), {})
	_portals[f.get_instance_id()] = pair
	var old = pair.get(color)
	if old and is_instance_valid(old):
		old.queue_free()
	var p := Portal.new()
	_portal_root.add_child(p)
	p.setup(f, color, xf, standing, on)
	pair[color] = p
	var other = pair.get("orange" if color == "blue" else "blue")
	if other and is_instance_valid(other):
		p.twin = other
		other.twin = p
	_sound_at("snd_usp", xf.origin, -4)
	Game.log_line("portal: %s opened %s at %s" % [f.display_name, color, xf.origin])
	for b in fighters:
		if b is BotFighter:
			(b as BotFighter).next_repath = 0.0
	if f is PlayerFighter:
		Portal.set_gun_core((f as PlayerFighter).viewmodel, Portal.color_of("orange" if color == "blue" else "blue"))
	return p

## The Portal Gun's trigger: the blue portal goes on the surface you hit.
func fire_portal(f: Fighter, from: Vector3, dir: Vector3) -> void:
	var space := get_world_3d().direct_space_state
	var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(from, from + dir * float(Defs.WEAPONS.portalgun.range_m), Game.LAYER_WORLD | Game.LAYER_BUILD | Game.LAYER_FIGHTER, [f.get_rid()]))
	if hit.is_empty() or hit.collider is Fighter:
		if f == player:
			hud.notice("No surface for a portal there", 1.0)
		return
	effect_tracer(from + dir * 0.6 + Vector3(0, -0.08, 0), hit.position)
	_place_portal(f, "blue", Portal.surface_xf(space, hit.position, hit.normal, dir), false, hit.collider)

## The orange portal, dropped from the tactical map: standing on the ground at feet, exit facing yaw.
func place_orange(f: Fighter, feet: Vector3, yaw: float) -> void:
	_place_portal(f, "orange", Portal.standing_xf(feet, yaw), true, null)
	close_tacmap()

func open_tacmap(f: Fighter) -> void:
	if tacmap or f != player:
		return
	tacmap = TacMap.new()
	tacmap.match_node = self
	tacmap.viewer = f
	hud.add_child(tacmap)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

func close_tacmap() -> void:
	if tacmap == null:
		return
	tacmap.queue_free()
	tacmap = null
	if player and not Game.test_mode:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

## The first portal a segment enters: {point, through, portal} or {}.
func portal_on_segment(a: Vector3, b: Vector3) -> Dictionary:
	if _portals.is_empty():
		return {}
	var best_t := INF
	var best: Portal = null
	for p in linked_portals():
		var t: float = p.segment_t(a, b)
		if t >= 0.0 and t < best_t:
			best_t = t
			best = p
	if best == null:
		return {}
	return {"point": a.lerp(b, best_t), "through": best.through(), "portal": best}

func _physics_process(_dt: float) -> void:
	if _portals.is_empty():
		return
	var linked := linked_portals()
	if linked.is_empty():
		return
	for f in fighters:
		if f.alive and f.now() >= f.portal_cd:
			for p in linked:
				if _enters(f, p):
					_send_through(f, p)
					break

func _enters(f: Fighter, p: Portal) -> bool:
	var n := p.normal()
	if f.velocity.dot(n) > 0.2:
		return false   # moving away (or walking through the back of a standing portal)
	var inv := p.global_transform.affine_inverse()
	for h in [0.15, 0.9, 1.55]:
		var l := inv * (f.global_position + Vector3(0, h, 0))
		if l.z > -0.05 and l.z < 0.45 and (l.x / Portal.RX) ** 2 + (l.y / Portal.RY) ** 2 <= 1.0:
			return true
	return false

func _send_through(f: Fighter, p: Portal) -> void:
	var q: Portal = p.twin
	var m := p.through()
	var center := m * (f.global_position + Vector3(0, 0.9, 0))
	var nq := q.normal()
	var l := q.global_transform.affine_inverse() * center
	l.x = clampf(l.x, -0.3, 0.3)
	l.y = clampf(l.y, -0.6, 0.6)
	l.z = maxf(l.z, 1.05 if absf(nq.y) > 0.5 else 0.75)
	center = q.global_transform * l
	if absf(nq.y) <= 0.5:
		# don't come out with the feet in the floor
		var hit := get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(center, center + Vector3.DOWN * 0.95, Game.LAYER_WORLD | Game.LAYER_BUILD))
		if not hit.is_empty():
			center.y = hit.position.y + 0.92
	var v := m.basis * f.velocity
	var out := v.dot(nq)
	if out < 3.0:
		v += nq * (3.0 - out)   # speedy thing goes in, speedy thing comes out
	f.teleport(center, v, m.basis * f.aim_dir())
	_sound_at("snd_usp", center, -8)
	if not _portal_reported:
		_portal_reported = true
		Game.melty_report("portal-used", true)
	Game.log_line("portal: %s went %s -> %s" % [f.display_name, p.color_id, q.color_id])

## For bots: a portal pair that makes the way to goal clearly shorter, as {entry, exit, approach}, or {}.
func portal_shortcut(b: Fighter, goal: Vector3) -> Dictionary:
	var linked := linked_portals()
	if linked.is_empty():
		return {}
	var direct := _path_len(b.global_position, goal)
	var best := {}
	var best_len := direct * 0.75 - 5.0
	for p in linked:
		var front: Vector3 = p.front()
		var nav := world.closest_nav_point(front)
		if Vector2(nav.x - front.x, nav.z - front.z).length() > 1.2:
			continue
		var rise: float = p.global_position.y - nav.y
		if absf(p.normal().y) > 0.7:
			if absf(rise) > 0.4 or p.normal().y < 0:
				continue   # floor portals only, and only ones you can step on
		elif rise > 1.8 or rise < 0.4:
			continue       # too high (or low) on its wall to walk into
		var out := world.closest_nav_point(p.twin.front(1.0))
		var l := _path_len(b.global_position, nav) + _path_len(out, goal) + 2.0
		if l < best_len:
			best_len = l
			best = {"entry": p, "exit": p.twin, "approach": nav}
	return best

func _path_len(a: Vector3, c: Vector3) -> float:
	var pth := world.path(a, c)
	if pth.size() < 2:
		return a.distance_to(c) * 1.4
	var l := a.distance_to(pth[0])
	for i in range(1, pth.size()):
		l += pth[i].distance_to(pth[i - 1])
	return l + pth[pth.size() - 1].distance_to(c) * 3.0

## A bot with the Portal Gun: blue on a wall next to it, orange at the best recommended spot for its side
## (toward its goal when nothing better turns up).
func _bot_portals(b: BotFighter) -> void:
	var space := get_world_3d().direct_space_state
	var eye := b.global_position + Vector3(0, 1.15, 0)
	var best := {}
	var best_d := INF
	for i in 16:
		var a := TAU * i / 16.0
		var d := Vector3(-sin(a), 0, -cos(a))
		var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(eye, eye + d * 12.0, Game.LAYER_WORLD, [b.get_rid()]))
		if not hit.is_empty() and absf(hit.normal.y) < 0.3 and hit.position.distance_to(eye) > 1.2 and hit.position.distance_to(eye) < best_d:
			best_d = hit.position.distance_to(eye)
			best = hit
	if best.is_empty():
		b.portal_delay += 2.0   # no wall around here: try again a bit further on
		return
	var toward := b.goal
	if b.team == "T":
		toward = world.random_point_in_site(t_plan_site)
	var recs := TacMap.recommend(self, b.team, TacMap.enemy_routes(self, b.team))
	var good := recs.filter(func(r): return (r.pos as Vector3).distance_to(toward) < 25.0)
	var spot: Vector3
	var yaw := 0.0
	if not good.is_empty():
		spot = good[0].pos
		yaw = good[0].yaw
	else:
		spot = world.closest_nav_point(toward + Vector3(randf_range(-4, 4), 0, randf_range(-4, 4)))
		var d := toward - spot
		yaw = atan2(-d.x, -d.z) if d.length() > 0.5 else randf() * TAU
	if spot.distance_to(b.global_position) < 15.0:
		b.portal_done = true   # already there: not worth a portal
		return
	b.portal_done = true
	var dir: Vector3 = (best.position - eye).normalized()
	b.yaw = atan2(-dir.x, -dir.z)
	b.switch_to(4)
	b.play_shot_sound(Defs.WEAPONS.portalgun)
	effect_tracer(eye, best.position)
	_place_portal(b, "blue", Portal.surface_xf(space, best.position, best.normal, dir), false, best.collider)
	_place_portal(b, "orange", Portal.standing_xf(spot, yaw), true, null)
	get_tree().create_timer(0.6).timeout.connect(func():
		if is_instance_valid(b) and b.alive:
			b.switch_to(1 if b.weapons.has(1) else 2))

# ---------------------------------------------------------------- effects
# ---------------------------------------------------------------- shells and muzzle flash
var _shells: Array[Node3D] = []
static var _flash_mat: StandardMaterial3D

## CS2's casing models come out about 2 m long; these are the real lengths per caliber.
const SHELL_LENGTH := {"shell_9mm": 0.021, "shell_762": 0.039, "shell_50cal": 0.07}

func eject_shell(asset: String, at: Vector3, view: Basis) -> void:
	var body := RigidBody3D.new()
	body.collision_layer = 0
	body.collision_mask = Game.LAYER_WORLD | Game.LAYER_CLIP | Game.LAYER_BUILD
	body.mass = 0.01
	var cs := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(0.012, 0.012, 0.03)
	cs.shape = box
	body.add_child(cs)
	var mesh := Game.instance(asset)
	if mesh:
		body.add_child(mesh)
		var size := 0.0
		for mi in mesh.find_children("*", "MeshInstance3D", true, false):
			size = maxf(size, (mi as MeshInstance3D).get_aabb().get_longest_axis_size())
		if size > 0.0:
			mesh.scale = Vector3.ONE * (float(SHELL_LENGTH.get(asset, 0.03)) / size)
		for mi in mesh.find_children("*", "MeshInstance3D", true, false):
			(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(body)
	body.global_position = at
	body.global_basis = view * Basis(Vector3.UP, PI / 2)
	# out of the ejection port: to the right, up, a little back
	body.linear_velocity = view.x * randf_range(1.6, 2.4) + view.y * randf_range(1.0, 1.8) + view.z * randf_range(0.1, 0.5)
	body.angular_velocity = Vector3(randf_range(-20, 20), randf_range(-20, 20), randf_range(-20, 20))
	_shells.append(body)
	while _shells.size() > 24:
		var old: Node3D = _shells.pop_front()
		if is_instance_valid(old):
			old.queue_free()
	get_tree().create_timer(4.0).timeout.connect(func():
		if is_instance_valid(body):
			_shells.erase(body)
			body.queue_free())

func muzzle_flash(at: Vector3) -> void:
	if _flash_mat == null:
		_flash_mat = StandardMaterial3D.new()
		_flash_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_flash_mat.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
		_flash_mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
		_flash_mat.albedo_color = Color(1.0, 0.7, 0.3)
		_flash_mat.no_depth_test = true
		var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
		for y in 64:
			for x in 64:
				var d := Vector2(x - 31.5, y - 31.5).length() / 32.0
				var star := absf(sin(atan2(y - 31.5, x - 31.5) * 3.0)) * 0.35
				var a := clampf(1.0 - d / (0.65 + star), 0.0, 1.0)
				img.set_pixel(x, y, Color(1, 1, 1, a * a))
		_flash_mat.albedo_texture = ImageTexture.create_from_image(img)
		_flash_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	var q := MeshInstance3D.new()
	var qm := QuadMesh.new()
	qm.size = Vector2(0.14, 0.14) * randf_range(0.8, 1.2)
	q.mesh = qm
	q.material_override = _flash_mat
	q.layers = 2
	q.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(q)
	q.global_position = at
	q.rotate_z(randf() * TAU)
	get_tree().create_timer(0.045).timeout.connect(q.queue_free)

func effect_tracer(a: Vector3, b: Vector3) -> void:
	var mi := MeshInstance3D.new()
	var im := ImmediateMesh.new()
	mi.mesh = im
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.albedo_color = Color(1, 0.85, 0.5, 0.6)
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	im.surface_begin(Mesh.PRIMITIVE_LINES, m)
	im.surface_add_vertex(a)
	im.surface_add_vertex(b)
	im.surface_end()
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(mi)
	get_tree().create_timer(0.05).timeout.connect(mi.queue_free)

func effect_impact(p: Vector3, n: Vector3, wood := false) -> void:
	var mi := MeshInstance3D.new()
	var q := QuadMesh.new()
	q.size = Vector2(0.06, 0.06)
	mi.mesh = q
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.1, 0.08, 0.06) if not wood else Color(0.35, 0.22, 0.1)
	mi.material_override = m
	add_child(mi)
	mi.global_position = p + n * 0.01
	if absf(n.dot(Vector3.UP)) < 0.99:
		mi.look_at(p + n, Vector3.UP)
	else:
		mi.rotation.x = -PI / 2 * signf(n.y)
	mi.rotate_object_local(Vector3.UP, PI)
	get_tree().create_timer(12.0).timeout.connect(mi.queue_free)
	var puff := _particles(p, Color(0.75, 0.65, 0.5) if not wood else Color(0.6, 0.42, 0.22), 6)
	puff.global_position = p

func effect_blood(p: Vector3) -> void:
	_particles(p, Color(0.5, 0.05, 0.03), 10)

func effect_break(p: Vector3) -> void:
	_particles(p, Color(0.6, 0.42, 0.22), 24)

func _particles(p: Vector3, c: Color, n: int) -> CPUParticles3D:
	var cp := CPUParticles3D.new()
	cp.one_shot = true
	cp.emitting = true
	cp.amount = n
	cp.lifetime = 0.5
	cp.explosiveness = 1.0
	cp.direction = Vector3.UP
	cp.spread = 60
	cp.initial_velocity_min = 1.0
	cp.initial_velocity_max = 3.0
	cp.scale_amount_min = 0.03
	cp.scale_amount_max = 0.06
	var mesh := SphereMesh.new()
	mesh.radius = 0.5
	mesh.height = 1.0
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mesh.material = m
	cp.mesh = mesh
	add_child(cp)
	cp.global_position = p
	get_tree().create_timer(1.0).timeout.connect(cp.queue_free)
	return cp

func _sound_at(id: String, p: Vector3, vol := 0.0) -> void:
	var s := Game.sound(id)
	if s == null:
		return
	var a := AudioStreamPlayer3D.new()
	a.stream = s
	a.volume_db = vol
	a.unit_size = 15.0
	add_child(a)
	a.global_position = p
	a.play()
	a.finished.connect(a.queue_free)

func _play_music(id: String, vol: float) -> void:
	if _music:
		_music.queue_free()
	_music = Game.play_ui(id, vol + linear_to_db(maxf(Game.settings.music, 0.001)))

# ---------------------------------------------------------------- spectating / menus
func next_spectate() -> void:
	var mates := fighters.filter(func(f): return f.alive and f != player and (player == null or f.team == player.team))
	if mates.is_empty():
		mates = fighters.filter(func(f): return f.alive)
	if mates.is_empty():
		return
	spectate_i = (spectate_i + 1) % mates.size()
	hud.spectate(mates[spectate_i])

func ui_blocking() -> bool:
	return paused or tacmap != null or (hud != null and hud.blocking())

func back_to_menu() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	get_tree().change_scene_to_file("res://scenes/menu.tscn")
