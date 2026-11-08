class_name ViewModel
extends Node3D
## First-person arms and gun from the player's CS2. A viewmodel clip carries two skeletons: the arms
## (driving the agent's "firstperson" meshes) and the weapon (slide, magazine, hammer, bolt...). Both
## the arm meshes and the gun model are re-skinned onto them by bone name, so CS2's own draw, idle,
## shoot and reload clips move the hands and the gun's parts together.

const Defs = preload("res://scripts/gen/defs.gd")
const CLIPS := ["draw", "idle", "shoot", "reload"]
const ORIENT := Basis(Vector3.UP, PI)          # clips reach down +Z; the camera looks down -Z
const OFFSET := Vector3(0.05, -0.03, 0.02)    # nudge toward the lower right, like CS2

static var WPN_MODE := 1
static var KNIFE_FIX := Basis(Vector3.RIGHT, PI)  # knives come blade-down off the wpn bone
static var WPN_TILT := Basis(Vector3.UP, PI / 2) * Basis(Vector3.BACK, PI / 2)  # weapon skeleton frame -> wpn bone frame
var _root: Node3D
var _ap: AnimationPlayer
var _set := ""
var _arm_sk: Skeleton3D
var _wpn_sk: Skeleton3D

## Shows a weapon set holding the given model. Returns false when this CS2 build has no
## viewmodel clips for it, so the caller can fall back to a plain gun model.
func show_weapon(set_name: String, model_asset: String, agent_asset: String) -> bool:
	clear()
	_root = Game.instance("vm_%s_idle" % set_name)
	if _root == null:
		return false
	_set = set_name
	add_child(_root)
	_root.basis = ORIENT
	_root.position = offset()
	for old in _root.find_children("*", "AnimationPlayer", true, false):
		old.queue_free()
	for sk in _root.find_children("*", "Skeleton3D", true, false):
		if (sk as Skeleton3D).find_bone("wpnHand_R") >= 0 or (sk as Skeleton3D).find_bone("hand_R") >= 0:
			_arm_sk = sk
		elif (sk as Skeleton3D).find_bone("weapon") >= 0:
			_wpn_sk = sk
	if _arm_sk == null:
		return false
	# the weapon skeleton hangs from the arms' wpn bone, which the clips animate
	if _wpn_sk and WPN_MODE == 1 and set_name != "knife":  # knives sit on the wpn bone as they are
		var wb := _arm_sk.find_bone("wpn")
		if wb >= 0:
			# follow the bone remotely: the clips address the weapon skeleton by its node path,
			# so it has to stay where it is for the slide, magazine and hammer tracks to play
			var att := BoneAttachment3D.new()
			att.bone_idx = wb
			_arm_sk.add_child(att)
			var rt := RemoteTransform3D.new()
			rt.transform = Transform3D(WPN_TILT, Vector3.ZERO)
			att.add_child(rt)
			rt.remote_path = rt.get_path_to(_wpn_sk)
	# the clips' placeholder meshes are not part of the picture
	for mi in _root.find_children("*empty_mesh_reference*", "MeshInstance3D", true, false):
		mi.queue_free()
	_ap = AnimationPlayer.new()
	_root.add_child(_ap)
	_ap.root_node = NodePath("..")
	var lib := AnimationLibrary.new()
	for c in CLIPS:
		var clip: Dictionary = Game._clip_animation("vm_%s_%s" % [set_name, c])
		if clip.is_empty():
			continue
		var a: Animation = clip.anim.duplicate()
		a.loop_mode = Animation.LOOP_LINEAR if c == "idle" else Animation.LOOP_NONE
		lib.add_animation(c, a)
	_ap.add_animation_library("", lib)
	_borrow_meshes(agent_asset, _arm_sk, "firstperson")
	if model_asset != "":
		if _wpn_sk and set_name != "knife":
			_borrow_meshes(model_asset, _wpn_sk, "")
		else:
			_attach_plain(model_asset)
	for mi in _root.find_children("*", "MeshInstance3D", true, false):
		var m: MeshInstance3D = mi
		m.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		m.layers = 2
		# re-skinned meshes keep their old bind-pose bounds; give them room so culling never drops them
		m.custom_aabb = AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4))
	play("draw")
	return true

## Moves the matching meshes of an asset onto a clip skeleton, re-skinned by bone name.
func _borrow_meshes(asset: String, target: Skeleton3D, name_filter: String) -> void:
	var src := Game.instance(asset)
	if src == null:
		return
	var sks := src.find_children("*", "Skeleton3D", true, false)
	if sks.is_empty():
		src.free()
		return
	var from_sk: Skeleton3D = sks[0]
	for mi in src.find_children("*", "MeshInstance3D", true, false):
		var m: MeshInstance3D = mi
		var n := String(m.name).to_lower()
		if name_filter != "" and not n.contains(name_filter):
			continue
		if n.contains("legacy"):
			continue  # guns carry a legacy and an HD body; keep the HD one
		var skin := _rebind(m.skin, from_sk, target)
		m.get_parent().remove_child(m)
		m.owner = null
		target.add_child(m)
		m.transform = Transform3D.IDENTITY
		m.skin = skin
		m.skeleton = NodePath("..")
	src.free()

## Fallback for clips without a weapon skeleton: hang the gun on the arms' wpn bone.
func _attach_plain(asset: String) -> void:
	var wpn := _arm_sk.find_bone("wpn")
	var gun := Game.instance(asset)
	if wpn < 0 or gun == null:
		return
	var att := BoneAttachment3D.new()
	att.bone_idx = wpn
	_arm_sk.add_child(att)
	att.add_child(gun)
	gun.basis = KNIFE_FIX

## The source skin binds bones by index; rebuild it by bone name for the target skeleton.
## A bone the target lacks (twist bones, clavicles, attachment bones) follows its nearest ancestor
## that it has, keeping its rest offset from that ancestor so the skin doesn't tear.
func _rebind(src: Skin, from_sk: Skeleton3D, to_sk: Skeleton3D) -> Skin:
	var out := Skin.new()
	if src == null:
		return out
	var root_name := to_sk.get_bone_name(0)
	for i in src.get_bind_count():
		var b := src.get_bind_bone(i)
		var nm := String(src.get_bind_name(i))
		if b < 0 and nm != "":
			b = from_sk.find_bone(nm)
		if nm == "" and b >= 0:
			nm = from_sk.get_bone_name(b)
		var pose := src.get_bind_pose(i)
		if to_sk.find_bone(nm) < 0 and b >= 0:
			var anc := from_sk.get_bone_parent(b)
			while anc >= 0 and to_sk.find_bone(from_sk.get_bone_name(anc)) < 0:
				anc = from_sk.get_bone_parent(anc)
			if anc >= 0:
				var offset := from_sk.get_bone_global_rest(anc).affine_inverse() * from_sk.get_bone_global_rest(b)
				pose = offset * pose
				nm = from_sk.get_bone_name(anc)
			else:
				nm = root_name
		out.add_named_bind(nm, pose)
	return out

## Viewmodel position from settings (like CS2's viewmodel_offset_x/y/z): right, up, back.
static func offset() -> Vector3:
	return Vector3(float(Game.settings.vm_x), float(Game.settings.vm_y), float(Game.settings.vm_z))

func _process(_dt: float) -> void:
	if _root:
		_root.position = offset()

func play(kind: String) -> void:
	if _ap == null or not _ap.has_animation(kind):
		return
	_ap.stop()
	_ap.play(kind, 0.04)
	if kind != "idle" and _ap.has_animation("idle"):
		_ap.queue("idle")

## World position of a weapon bone (muzzle, slide...), or a camera-relative fallback.
func bone_world(names: Array) -> Vector3:
	for sk in [_wpn_sk, _arm_sk]:
		if sk == null:
			continue
		for n in names:
			var b: int = sk.find_bone(n)
			if b >= 0:
				return sk.global_transform * sk.get_bone_global_pose(b).origin
	return global_transform * Vector3(0.1, -0.1, -0.5)

func muzzle_world() -> Vector3:
	return bone_world(["muzzle", "silencer", "wpnTip"])

func eject_world() -> Vector3:
	return bone_world(["shell_eject", "eject", "slide", "bolt", "weapon"])

func clear() -> void:
	if _root:
		_root.queue_free()
	_root = null
	_ap = null
	_arm_sk = null
	_wpn_sk = null
	_set = ""
