extends Node
## Conversor do Bot Strike 360 (modelos, animacoes, icones, sons, navmesh, bombsites).
## Roda dentro do projeto do Bot Strike, com o autoload Game, e monta os modelos do mesmo jeito que o jogo:
## agentes pelo fighter.gd + Game.animate_agent, viewmodels pelo ViewModel.show_weapon. Assim as manhas do CS2
## (dois esqueletos por clipe, WPN_TILT, faca, pelve) ficam iguais as do PC.
##
## Grava arquivos big-endian em --out; o x360/tools/pack_assets.py junta tudo no assets.pak.
##
##   Godot_v4.7.2-stable_win64_console.exe --headless --path C:/mlt/proj -s res://tools/x360_bake.gd -- \
##       --out C:/mlt/x360/work/assets --tex 256 --vm-tex 512

const Defs = preload("res://scripts/gen/defs.gd")
const FPS := 30.0
const MAX_CLIP := 4.0
const VM_CLIPS := ["draw", "idle", "shoot", "reload"]

var out := ""
var tex_max := 256
var vm_tex_max := 512
var skip_audio := false
var textures := []          # [{file, ...}]
var tex_of := {}            # Image instance id + size -> texture id
var index := {"models": {}, "anims": {}, "textures": [], "icons": {}, "sounds": {}}
var t0 := 0

func _ready() -> void:
	t0 = Time.get_ticks_msec()
	var args := OS.get_cmdline_user_args()
	for i in args.size():
		match args[i]:
			"--out": out = args[i + 1]
			"--tex": tex_max = int(args[i + 1])
			"--vm-tex": vm_tex_max = int(args[i + 1])
			"--no-audio": skip_audio = true
	if out == "":
		printerr("uso: -- --out <pasta>")
		get_tree().quit(1)
		return
	for d in ["models", "anims", "tex", "icons", "sounds"]:
		DirAccess.make_dir_recursive_absolute(out.path_join(d))
	await get_tree().process_frame
	if Game.cache == "":
		printerr("o autoload Game nao achou o cache do Bot Strike")
		get_tree().quit(1)
		return
	await _run()
	var f := FileAccess.open(out.path_join("index.json"), FileAccess.WRITE)
	f.store_string(JSON.stringify(index, "\t"))
	f.close()
	_log("pronto")
	get_tree().quit(0)

func _log(s: String) -> void:
	print("[%5.1fs] %s" % [(Time.get_ticks_msec() - t0) / 1000.0, s])

func _run() -> void:
	# agentes em terceira pessoa
	for team in ["CT", "T"]:
		await _bake_agent(team)
	# armas na mao dos bots (terceira pessoa), a C4 no chao, capsulas
	for id in Defs.WEAPONS:
		var asset: String = Defs.WEAPONS[id].model_asset
		if Defs.ASSETS.has(asset) and not index.models.has("w_" + id):
			await _bake_rigid(asset, "w_" + id, func(n: String) -> bool: return not n.contains("_hd"), tex_max)
	await _bake_rigid("mdl_c4", "w_c4", func(n: String) -> bool: return not n.contains("_hd"), tex_max)
	for s in ["shell_9mm", "shell_762", "shell_50cal"]:
		await _bake_rigid(s, s, func(_n: String) -> bool: return true, 64)
	var pg: Node3D = Portal.gun_model(false)
	get_tree().root.add_child(pg)
	await _bake_node(pg, "w_portalgun", func(_n: String) -> bool: return true, 64, [])
	pg.free()
	# viewmodels: cada conjunto de animacao com a arma dele, nos bracos de cada time
	var sets := {}
	for id in Defs.WEAPONS:
		var w: Dictionary = Defs.WEAPONS[id]
		if w.anim_set != "none":
			sets[w.anim_set] = w.model_asset
	sets["c4"] = "mdl_c4"
	for team in ["CT", "T"]:
		for s in sets:
			await _bake_viewmodel(team, s, sets[s])
	_icons()
	_nav_and_sites()
	_build_textures()
	if not skip_audio:
		await _sounds()
	index.textures = textures

# ---------------------------------------------------------------- modelos

## Os "slots" de um modelo: cada osso (ou no) cuja transformacao o console precisa a cada quadro.
class Rig:
	var keys := []          # [node, bone] por slot
	var names := []
	var binds := []         # [slot, Transform3D]
	func slot(node: Node3D, bone: int, name: String) -> int:
		for i in keys.size():
			if keys[i][0] == node and keys[i][1] == bone:
				return i
		keys.append([node, bone])
		names.append(name)
		return keys.size() - 1

func _xf_of(root: Node3D, key: Array) -> Transform3D:
	var node: Node3D = key[0]
	var base := root.global_transform.affine_inverse() * node.global_transform
	if key[1] >= 0:
		return base * (node as Skeleton3D).get_bone_global_pose(key[1])
	return base

func _store_xf(f: FileAccess, t: Transform3D) -> void:
	for r in 3:
		f.store_float(t.basis.x[r])
		f.store_float(t.basis.y[r])
		f.store_float(t.basis.z[r])
		f.store_float(t.origin[r])

func _store_name(f: FileAccess, s: String) -> void:
	var b := s.to_utf8_buffer()
	b.resize(32)
	b[31] = 0
	f.store_buffer(b)

## Grava o modelo: malhas visiveis sob root (filtradas por nome), em espaco de bind, com ate 4 ossos por vertice.
## Devolve o Rig, para gravar as animacoes nos mesmos slots.
func _write_model(root: Node3D, name: String, keep: Callable, tmax: int, attach: Array, ratio := 1.0) -> Rig:
	var rig := Rig.new()
	var pos := PackedFloat32Array()
	var nrm := PackedFloat32Array()
	var uvs := PackedFloat32Array()
	var bidx := PackedByteArray()
	var bw := PackedByteArray()
	var idx := PackedInt32Array()
	var subs := []
	for mi in root.find_children("*", "MeshInstance3D", true, false):
		var m: MeshInstance3D = mi
		var nm := String(m.name).to_lower()
		if not m.is_visible_in_tree() or m.mesh == null or not keep.call(nm):
			continue
		var sk: Skeleton3D = m.get_node_or_null(m.skeleton) as Skeleton3D if m.skin else null
		var bind_base := rig.binds.size()
		if sk and m.skin:
			for i in m.skin.get_bind_count():
				var b := m.skin.get_bind_bone(i)
				if b < 0:
					b = sk.find_bone(m.skin.get_bind_name(i))
				if b < 0:
					b = 0
				rig.binds.append([rig.slot(sk, b, sk.get_bone_name(b)), m.skin.get_bind_pose(i)])
		else:
			rig.binds.append([rig.slot(m, -1, String(m.name)), Transform3D.IDENTITY])
		for s in m.mesh.get_surface_count():
			if m.mesh is ArrayMesh and (m.mesh as ArrayMesh).surface_get_primitive_type(s) != Mesh.PRIMITIVE_TRIANGLES:
				continue
			var a := m.mesh.surface_get_arrays(s)
			var v: PackedVector3Array = a[Mesh.ARRAY_VERTEX]
			var ii = a[Mesh.ARRAY_INDEX]
			if v.is_empty():
				continue
			if ii == null or (ii as PackedInt32Array).is_empty():
				ii = PackedInt32Array(range(v.size()))
			# menos triangulos (o LOD do proprio Godot), e so os vertices que sobram
			if ratio < 0.99 and (ii as PackedInt32Array).size() > 900:
				var im := ImporterMesh.new()
				im.add_surface(Mesh.PRIMITIVE_TRIANGLES, a)
				im.generate_lods(25.0, 60.0, [])
				var want := int((ii as PackedInt32Array).size() * ratio)
				for l in im.get_surface_lod_count(0):
					var li := im.get_surface_lod_indices(0, l)
					if li.size() >= 3:
						ii = li
					if li.size() <= want:
						break
			var used := PackedInt32Array()
			var remap := {}
			var ii2 := PackedInt32Array()
			for k in (ii as PackedInt32Array):
				if not remap.has(k):
					remap[k] = used.size()
					used.append(k)
				ii2.append(remap[k])
			ii = ii2
			var n = a[Mesh.ARRAY_NORMAL]
			var t = a[Mesh.ARRAY_TEX_UV]
			var bones = a[Mesh.ARRAY_BONES]
			var weights = a[Mesh.ARRAY_WEIGHTS]
			var per := 4
			if bones != null and v.size() > 0 and (bones as PackedInt32Array).size() >= v.size() * 8:
				per = 8
			var base := pos.size() / 3
			for k in used:
				pos.append_array([v[k].x, v[k].y, v[k].z])
				var nn: Vector3 = n[k] if n != null and n.size() > k else Vector3.UP
				nrm.append_array([nn.x, nn.y, nn.z])
				var tt: Vector2 = t[k] if t != null and t.size() > k else Vector2.ZERO
				uvs.append_array([tt.x, tt.y])
				# os 4 maiores pesos, normalizados para 255
				var pairs := []
				if bones != null and weights != null and sk and m.skin and bones.size() >= (k + 1) * per:
					for j in per:
						pairs.append([float(weights[k * per + j]), int(bones[k * per + j])])
					pairs.sort_custom(func(x, y): return x[0] > y[0])
				else:
					pairs = [[1.0, 0]]
				pairs.resize(4)
				var tot := 0.0
				for j in 4:
					if pairs[j] == null:
						pairs[j] = [0.0, 0]
					tot += pairs[j][0]
				if tot <= 0.0:
					pairs[0] = [1.0, pairs[0][1]]
					tot = 1.0
				var left := 255
				for j in 4:
					var q := int(round(pairs[j][0] / tot * 255.0)) if j < 3 else left
					q = clampi(q, 0, left)
					left -= q
					var bi: int = bind_base + clampi(pairs[j][1], 0, rig.binds.size() - bind_base - 1)
					bidx.append(bi)
					bw.append(q)
			var first := idx.size()
			for k in (ii as PackedInt32Array).size():
				idx.append(base + ii[k])
			var mat = m.get_active_material(s)
			var color := Color.WHITE
			var tex := -1
			var flags := 0
			if mat is BaseMaterial3D:
				var bm: BaseMaterial3D = mat
				color = bm.albedo_color
				if bm.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED:
					flags |= 1
				if bm.albedo_texture:
					tex = _texture(bm.albedo_texture.get_image(), tmax, flags & 1 != 0)
			subs.append([first, idx.size() - first, tex, color, flags])
	for a in attach:
		# [nome no arquivo, no, osso]
		rig.slot(a[1], a[2], a[0])
	if rig.binds.size() > 255:
		_log("AVISO: %s tem %d binds (o formato guarda ate 255)" % [name, rig.binds.size()])
	var vc := pos.size() / 3
	if vc > 65535:
		_log("AVISO: %s tem %d vertices (indices de 16 bits)" % [name, vc])
	var f := FileAccess.open(out.path_join("models/%s.mdl" % name), FileAccess.WRITE)
	f.big_endian = true
	f.store_buffer("MDL1".to_utf8_buffer())
	f.store_32(vc)
	f.store_32(idx.size())
	f.store_32(subs.size())
	f.store_32(rig.binds.size())
	f.store_32(rig.keys.size())
	f.store_32(attach.size())
	for s in subs:
		f.store_32(s[0])
		f.store_32(s[1])
		f.store_32(s[2] & 0xFFFFFFFF)
		var c: Color = s[3]
		for x in [c.r, c.g, c.b, c.a]:
			f.store_float(x)
		f.store_32(s[4])
	for i in rig.keys.size():
		_store_name(f, rig.names[i])
	for b in rig.binds:
		f.store_32(b[0])
		_store_xf(f, b[1])
	for a in attach:
		_store_name(f, a[0])
		f.store_32(rig.slot(a[1], a[2], a[0]))
	for k in vc:
		for j in 3:
			f.store_float(pos[k * 3 + j])
		for j in 3:
			f.store_float(nrm[k * 3 + j])
		f.store_float(uvs[k * 2])
		f.store_float(uvs[k * 2 + 1])
		for j in 4:
			f.store_8(bidx[k * 4 + j])
		for j in 4:
			f.store_8(bw[k * 4 + j])
	for k in idx.size():
		f.store_16(idx[k])
	f.close()
	index.models[name] = {"vertices": vc, "triangles": idx.size() / 3, "binds": rig.binds.size(), "slots": rig.keys.size(), "submeshes": subs.size()}
	_log("modelo %s: %d vertices, %d triangulos, %d binds, %d slots" % [name, vc, idx.size() / 3, rig.binds.size(), rig.keys.size()])
	return rig

## Uma animacao: a transformacao de cada slot (relativa a root) a cada 1/30 s.
func _write_anim(root: Node3D, rig: Rig, name: String, ap: AnimationPlayer, clip: String, loop: bool) -> void:
	var frames := []
	if ap == null or clip == "" or not ap.has_animation(clip):
		frames.append(_sample(root, rig))
	else:
		var a := ap.get_animation(clip)
		var length := minf(a.length, MAX_CLIP)
		var n := maxi(1, int(ceil(length * FPS)))
		ap.play(clip, 0.0)
		for i in n:
			ap.seek(minf(i / FPS, a.length), true)
			await get_tree().process_frame
			frames.append(_sample(root, rig))
	var f := FileAccess.open(out.path_join("anims/%s.anm" % name), FileAccess.WRITE)
	f.big_endian = true
	f.store_buffer("ANM1".to_utf8_buffer())
	f.store_32(rig.keys.size())
	f.store_32(frames.size())
	f.store_float(FPS)
	f.store_32(1 if loop else 0)
	for fr in frames:
		for t in fr:
			_store_xf(f, t)
	f.close()
	index.anims[name] = {"frames": frames.size(), "slots": rig.keys.size(), "loop": loop}

func _sample(root: Node3D, rig: Rig) -> Array:
	var out_xf := []
	for k in rig.keys:
		out_xf.append(_xf_of(root, k))
	return out_xf

func _bake_node(node: Node3D, name: String, keep: Callable, tmax: int, attach: Array, ratio := 1.0) -> void:
	if node.get_parent() == null:
		get_tree().root.add_child(node)
	await get_tree().process_frame
	var rig := _write_model(node, name, keep, tmax, attach, ratio)
	await _write_anim(node, rig, name + ".rest", null, "", false)
	node.get_parent().remove_child(node)

func _bake_rigid(asset: String, name: String, keep: Callable, tmax: int) -> void:
	var n: Node3D = Game.instance(asset)
	if n == null:
		_log("faltando: " + asset)
		return
	# se o filtro deixa quase nada (a C4 so tem pecas pequenas fora do HD), fica com tudo
	var kept := 0
	for mi in n.find_children("*", "MeshInstance3D", true, false):
		var m := mi as MeshInstance3D
		if keep.call(String(m.name).to_lower()) and m.mesh:
			for s in m.mesh.get_surface_count():
				var ia = m.mesh.surface_get_arrays(s)[Mesh.ARRAY_INDEX]
				kept += (ia as PackedInt32Array).size() / 3 if ia != null else 0
	var tris_max := 2500 if name.begins_with("w_") else 100000
	await _bake_node(n, name, keep if kept > 300 else func(_x: String) -> bool: return not _x.contains("legacy"), tmax, [], _ratio_for(n, tris_max))
	n.free()

## A fracao de triangulos que deixa o modelo com ate tris_max.
func _ratio_for(n: Node3D, tris_max: int) -> float:
	var total := 0
	for mi in n.find_children("*", "MeshInstance3D", true, false):
		var m := mi as MeshInstance3D
		if m.mesh:
			for s in m.mesh.get_surface_count():
				var ia = m.mesh.surface_get_arrays(s)[Mesh.ARRAY_INDEX]
				total += (ia as PackedInt32Array).size() / 3 if ia != null else 0
	return clampf(float(tris_max) / maxf(total, 1.0), 0.05, 1.0)

func _bake_agent(team: String) -> void:
	var model: Node3D = Game.instance(Defs.TEAMS[team].agent_asset)
	if model == null:
		_log("faltando agente " + team)
		return
	get_tree().root.add_child(model)
	for n in model.find_children("*", "Node3D", true, false):
		var nn := String(n.name).to_lower()
		if nn.contains("defusekit") or nn.contains("firstperson"):
			n.visible = false
	var ap: AnimationPlayer = Game.animate_agent(model)
	if ap:
		ap.callback_mode_process = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
	await get_tree().process_frame
	var name := "agent_" + team.to_lower()
	# a pele dos bots e calculada na CPU do 360: menos vertices
	var rig := _write_model(model, name, func(_n: String) -> bool: return true, tex_max, [], 0.4)
	for c in ["idle", "run", "fire"]:
		await _write_anim(model, rig, "%s.%s" % [name, c], ap, c if ap and ap.has_animation(c) else "", c != "fire")
	model.free()

func _bake_viewmodel(team: String, set_name: String, model_asset: String) -> void:
	var vm := ViewModel.new()
	get_tree().root.add_child(vm)
	await get_tree().process_frame
	if not vm.show_weapon(set_name, model_asset, Defs.TEAMS[team].agent_asset):
		_log("sem clipes de viewmodel para " + set_name)
		vm.free()
		return
	var ap: AnimationPlayer = vm._ap
	ap.callback_mode_process = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
	await get_tree().process_frame
	await get_tree().process_frame
	var attach := []
	for pair in [["muzzle", ["muzzle", "silencer", "wpnTip"]], ["eject", ["shell_eject", "eject", "slide", "bolt", "weapon"]]]:
		var found := false
		for sk in [vm._wpn_sk, vm._arm_sk]:
			if sk == null or found:
				continue
			for bn in pair[1]:
				var b: int = sk.find_bone(bn)
				if b >= 0:
					attach.append([pair[0], sk, b])
					found = true
					break
	var name := "vm_%s_%s" % [set_name, team.to_lower()]
	var rig := _write_model(vm, name, func(n: String) -> bool: return not n.contains("legacy") and not n.contains("empty_mesh_reference"), vm_tex_max, attach, 0.6)
	for c in VM_CLIPS:
		await _write_anim(vm, rig, "%s.%s" % [name, c], ap, c if ap.has_animation(c) else "", c == "idle")
	vm.free()

# ---------------------------------------------------------------- texturas

func _po2(v: int, mx: int) -> int:
	var p := 4
	while p * 2 <= v and p * 2 <= mx:
		p *= 2
	return p

func _texture(img: Image, tmax: int, alpha: bool) -> int:
	if img == null or img.is_empty():
		return -1
	var key := "%d:%d:%s" % [img.get_instance_id(), tmax, alpha]
	if tex_of.has(key):
		return tex_of[key]
	var im := img.duplicate() as Image
	if im.is_compressed():
		im.decompress()
	im.clear_mipmaps()
	im.convert(Image.FORMAT_RGBA8 if alpha else Image.FORMAT_RGB8)
	var w := _po2(im.get_width(), tmax)
	var h := _po2(im.get_height(), tmax)
	if w != im.get_width() or h != im.get_height():
		im.resize(w, h, Image.INTERPOLATE_LANCZOS)
	im.generate_mipmaps()
	if im.compress(Image.COMPRESS_S3TC, Image.COMPRESS_SOURCE_GENERIC) != OK:
		tex_of[key] = -1
		return -1
	var id := textures.size()
	var levels := []
	for l in im.get_mipmap_count() + 1:
		levels.append(im.get_mipmap_offset(l))
	var file := "tex/%04d.bin" % id
	var f := FileAccess.open(out.path_join(file), FileAccess.WRITE)
	f.store_buffer(im.get_data())
	f.close()
	textures.append({"file": file, "format": "dxt5" if im.get_format() == Image.FORMAT_DXT5 else "dxt1", "width": w, "height": h, "level_offsets": levels})
	tex_of[key] = id
	return id

func _build_textures() -> void:
	index["build_textures"] = {}
	var dir := DirAccess.open(Game.cache.path_join("maps"))
	for m in Defs.BUILD_MATERIALS:
		var hint: String = Defs.BUILD_MATERIALS[m].texture_hint
		for f in dir.get_files():
			var lf := f.to_lower()
			if lf.contains(hint) and lf.contains("color") and lf.ends_with(".png"):
				var img := Image.load_from_file(Game.cache.path_join("maps").path_join(f))
				if img:
					index.build_textures[m] = _texture(img, 256, false)
					break

# ---------------------------------------------------------------- icones, navmesh, bombsites

func _icons() -> void:
	for id in Defs.ASSETS:
		if Defs.ASSETS[id].kind != "svg":
			continue
		var path: String = Game.asset_path(id)
		if not FileAccess.file_exists(path):
			continue
		var img := Image.new()
		if img.load_svg_from_buffer(FileAccess.get_file_as_bytes(path), 64.0 / 32.0) == OK:
			img.save_png(out.path_join("icons/%s.png" % id))
			index.icons[id] = [img.get_width(), img.get_height()]

func _nav_and_sites() -> void:
	var nav_path: String = Game.cache.path_join("nav_de_dust2_%s_v3.res" % Game.patch)
	var nm = ResourceLoader.load(nav_path) if FileAccess.file_exists(nav_path) else null
	if nm is NavigationMesh:
		var f := FileAccess.open(out.path_join("nav.bin"), FileAccess.WRITE)
		f.big_endian = true
		var verts: PackedVector3Array = nm.get_vertices()
		f.store_buffer("NAV1".to_utf8_buffer())
		f.store_32(verts.size())
		f.store_32(nm.get_polygon_count())
		for v in verts:
			f.store_float(v.x)
			f.store_float(v.y)
			f.store_float(v.z)
		for i in nm.get_polygon_count():
			var p: PackedInt32Array = nm.get_polygon(i)
			f.store_32(p.size())
			for k in p:
				f.store_32(k)
		f.close()
		_log("navmesh: %d vertices, %d poligonos" % [verts.size(), nm.get_polygon_count()])
	else:
		_log("AVISO: navmesh nao encontrado em %s (abra uma partida no Bot Strike uma vez)" % nav_path)
	# bombsites, como o world.gd: os func_bomb_target do mapa, ou as caixas da planilha
	var sites := {}
	var doc := GLTFDocument.new()
	var st := GLTFState.new()
	var row: Dictionary = Defs.MAPS.de_dust2
	if doc.append_from_file(Game.asset_path(row.trig_asset), st) == OK:
		var r := doc.generate_scene(st)
		var boxes := []
		for mi in r.find_children("*", "MeshInstance3D", true, false):
			if String(mi.name).contains("bomb_target") or String(mi.get_parent().name).contains("bomb_target"):
				var faces: PackedVector3Array = (mi as MeshInstance3D).mesh.get_faces()
				if faces.is_empty():
					continue
				var box := AABB(Game.src_to_godot(faces[0]), Vector3.ZERO)
				for fc in faces:
					box = box.expand(Game.src_to_godot(fc))
				boxes.append(box)
		r.free()
		if boxes.size() == 2:
			boxes.sort_custom(func(a, b): return a.get_center().z > b.get_center().z)
			for i in 2:
				var box: AABB = boxes[i].grow(0.3)
				box.size.y += 2.0
				sites["AB"[i]] = [box.position.x, box.position.y, box.position.z, box.end.x, box.end.y, box.end.z]
	if sites.size() != 2:
		for k in ["a", "b"]:
			var mn: Array = row["site_%s_min" % k]
			var mx: Array = row["site_%s_max" % k]
			var box := AABB(Game.src_to_godot(Vector3(mn[0], mn[1], mn[2])), Vector3.ZERO).expand(Game.src_to_godot(Vector3(mx[0], mx[1], mx[2]))).grow(0.3)
			box.size.y += 2.0
			sites[k.to_upper()] = [box.position.x, box.position.y, box.position.z, box.end.x, box.end.y, box.end.z]
	index["sites"] = sites

# ---------------------------------------------------------------- sons

func _sounds() -> void:
	var rec := AudioEffectRecord.new()
	AudioServer.add_bus_effect(0, rec)
	for id in Defs.ASSETS:
		if Defs.ASSETS[id].kind != "sound":
			continue
		var path: String = Game.asset_path(id)
		if not FileAccess.file_exists(path):
			continue
		var w: AudioStreamWAV = null
		if path.ends_with(".wav"):
			w = AudioStreamWAV.load_from_file(path)
		elif path.ends_with(".mp3"):
			# sem decodificador de MP3 aqui: toca no barramento e grava (em tempo real)
			var mp3 := AudioStreamMP3.load_from_file(path)
			var cap := minf(mp3.get_length(), 90.0 if id == "mus_menu" else 60.0)
			var p := AudioStreamPlayer.new()
			p.stream = mp3
			get_tree().root.add_child(p)
			rec.set_recording_active(true)
			p.play()
			var start := Time.get_ticks_msec()
			while Time.get_ticks_msec() - start < int(cap * 1000.0) + 150 and p.playing:
				await get_tree().process_frame
			rec.set_recording_active(false)
			p.queue_free()
			w = rec.get_recording()
			_log("som %s gravado do MP3 (%.0f s)" % [id, cap])
		if w == null or w.data.is_empty():
			continue
		w.save_to_wav(out.path_join("sounds/%s.wav" % id))
		index.sounds[id] = {"rate": w.mix_rate, "stereo": w.stereo, "format": w.format}
