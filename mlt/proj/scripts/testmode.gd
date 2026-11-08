extends Node
## Melty test mode watchdog: while a test runs, logs what the match is doing every 20 s and
## reports a failed check if a round or a build never happens, so the run says why.

var _t := 0.0

func _process(dt: float) -> void:
	var m := get_tree().current_scene
	if m == null or not m.has_method("time_left"):
		_t = 0.0  # count only while a match runs, not during first-launch export
		return
	_t += dt
	if int(_t) % 20 == 0 and int(_t - dt) % 20 != 0:
		if true:
			Game.log_line("test: round %d phase %d score CT %d - T %d alive CT %d T %d" % [m.round_num, m.phase, m.score.CT, m.score.T, m._alive("CT"), m._alive("T")])
			var c = m.bomb_carrier
			if c and c.alive and c.has_bomb and c is BotFighter:
				Game.log_line("test: bomb carrier %s at %s goal %s (%s) path %d/%d site_here '%s' target %s" % [c.display_name, c.global_position, c.goal, c.goal_kind, c.path_i, c.path.size(), m.world.site_at(c.global_position), c.target != null])
			elif m.bomb_dropped_at != Vector3.INF:
				Game.log_line("test: bomb dropped at %s" % m.bomb_dropped_at)
	if _t > 480.0 and not Game._reported.has("round-played"):
		Game.melty_report("round-played", false, "no round finished in 8 minutes of play")
	if _t > 480.0 and m.mode.building and not Game._reported.has("piece-built"):
		Game.melty_report("piece-built", false, "nobody built anything in 8 minutes of play")
	if _t > 300.0 and m.mode.ruleset == "cs16" and not Game._reported.has("recoil-1-6"):
		Game.melty_report("recoil-1-6", false, "no spray climbed with 1.6 view punch in 5 minutes")
