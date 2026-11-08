extends Node
var t := 0.0
func _process(dt):
	t += dt
	if int(t) != int(t - dt):
		print("blank fps=%d process=%.1fms" % [Engine.get_frames_per_second(), Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0])
	if t > 8: get_tree().quit()
