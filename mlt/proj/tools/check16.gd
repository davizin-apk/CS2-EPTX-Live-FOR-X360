extends SceneTree
## Headless check of the Classic 1.6 ruleset: weapon rows, bhop limiter, jump height, recoil climb.
## godot --headless --path . -s tools/check16.gd
const Defs = preload("res://scripts/gen/defs.gd")

func _initialize() -> void:
	var fails := [0]
	var gs := GDScript.new()
	gs.source_code = "extends Node\nvar mode := {}\n"
	gs.reload()
	var mn := Node.new()
	mn.set_script(gs)
	mn.set("mode", Defs.MODES.classic16)
	var p = load("res://scripts/player.gd").new()
	p.match_node = mn
	var ok := func(name: String, cond: bool, info := "") -> void:
		print(("  PASS  " if cond else "  FAIL  ") + name + ("   (" + info + ")" if info != "" else ""))
		if not cond: fails[0] += 1
	ok.call("ruleset is cs16", p.rules().id == "cs16")
	ok.call("AK costs 2500 under 1.6", int(p.wd("ak47").price) == 2500, str(p.wd("ak47").price))
	ok.call("M4 mag is 30 under 1.6", int(p.wd("m4a1s").mag) == 30)
	ok.call("knife runs 250", int(p.wd("knife").move_speed) == 250)
	ok.call("CS2 values untouched", int(Defs.WEAPONS.ak47.price) == 2700)
	var r: Dictionary = p.rules()
	var apex := pow(float(r.jump_speed), 2) / (2.0 * float(r.gravity))
	ok.call("jump apex 45 units", absf(apex - 45.0) < 0.1, "%.2f" % apex)
	# bhop limiter: 600 u/s with the knife (maxspeed 250) -> 1.2*250*0.8 = 240
	p.weapons = {3: {"id": "knife", "mag": 0, "reserve": 0}}
	p.slot = 3
	p.velocity = Vector3(600.0 * 0.0254, 0, 0)
	p._jump(r)
	var hs: float = Vector2(p.velocity.x, p.velocity.z).length() / 0.0254
	ok.call("bhop cap 600 -> 240 u/s", absf(hs - 240.0) < 0.5, "%.1f" % hs)
	p._jump(r)
	var js: float = p.velocity.y / 0.0254
	ok.call("stamina lowers a quick re-jump", js < float(r.jump_speed) - 1.0, "%.1f u/s" % js)
	# recoil: AK spray kicks up and keeps climbing
	var d: Dictionary = p.wd("ak47")
	var k: Array = d.kick
	var punch := 0.0
	for i in range(1, 11):
		punch = minf(punch + minf(float(k[0]) + (i - 1) * float(k[1]), float(k[2])), float(k[2]) * 1.6)
	ok.call("AK 10-shot climb reaches its cap", punch > 9.0, "%.1f deg" % punch)
	print("=== %s ===" % ("ALL PASS" if fails[0] == 0 else "%d FAILED" % fails[0]))
	p.free()
	mn.free()
	quit(1 if fails[0] else 0)
