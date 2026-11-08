extends SceneTree
## Primeira metade do conversor do Bot Strike 360: le o Dust II que o Bot Strike exportou do CS2,
## escolhe um LOD por superficie ate caber no orcamento de triangulos e comprime as texturas em DXT.
## Grava um intermediario little-endian em --out; o pack.py monta o pacote big-endian do console.
##
##   Godot_v4.7.2-stable_win64_console.exe --headless --path C:/mlt/x360/tools/converter -s export_map.gd -- \
##       --glb <cs2cache>/maps/de_dust2.glb --out C:/mlt/x360/work --tris 600000 --tex 256 --near-error 1.0
##
## --tris e o orcamento do LOD de longe; --near-error escolhe o de perto (o console troca pela distancia).

var glb := ""
var out := ""
var budget := 600000
var tex_max := 256
var max_error := -1.0   # --error: fixa o limite de erro de longe em vez de procurar pelo orcamento
var near_error := 1.0   # --near-error: o LOD que aparece perto da camera
var t0 := 0

func _init() -> void:
	t0 = Time.get_ticks_msec()
	var args := OS.get_cmdline_user_args()
	for i in args.size():
		match args[i]:
			"--glb": glb = args[i + 1]
			"--out": out = args[i + 1]
			"--tris": budget = int(args[i + 1])
			"--tex": tex_max = int(args[i + 1])
			"--error": max_error = float(args[i + 1])
			"--near-error": near_error = float(args[i + 1])
	if glb == "" or out == "":
		_fail("uso: -- --glb <de_dust2.glb> --out <pasta> [--tris N] [--tex N]")
		return
	var code := _run()
	quit(code)

func _fail(msg: String) -> void:
	printerr("ERRO: " + msg)
	quit(1)

func _log(msg: String) -> void:
	print("[%5.1fs] %s" % [(Time.get_ticks_msec() - t0) / 1000.0, msg])

func _run() -> int:
	DirAccess.make_dir_recursive_absolute(out.path_join("tex"))
	var doc := GLTFDocument.new()
	var st := GLTFState.new()
	_log("lendo " + glb)
	if doc.append_from_file(glb, st) != OK:
		printerr("ERRO: nao consegui ler " + glb)
		return 1
	var j: Dictionary = st.json
	var mats_json: Array = j.get("materials", [])
	var mats: Array = st.get_materials()
	var mat_index := {}
	for i in mats.size():
		mat_index[mats[i]] = i
	var nodes: Array = st.get_nodes()
	var meshes: Array = st.get_meshes()
	_log("%d nos, %d malhas, %d materiais" % [nodes.size(), meshes.size(), mats.size()])

	# --- superficies que o console desenha, com as mesmas regras do world.gd/prep.gd
	var surfs := []   # {mesh: ImporterMesh, s, mat, xf, cands: [[tris, err]]}
	var skipped := 0
	var full_tris := 0
	for ni in nodes.size():
		var n: GLTFNode = nodes[ni]
		if n.mesh < 0:
			continue
		var name := String(n.resource_name).to_lower()
		if name.contains("blocklight"):
			skipped += 1
			continue
		var im: ImporterMesh = (meshes[n.mesh] as GLTFMesh).mesh
		if im == null:
			continue
		var xf := _global_xform(nodes, ni)
		var keep := []
		var tris := 0
		for s in im.get_surface_count():
			var m: Material = im.get_surface_material(s)
			var mi: int = mat_index.get(m, -1)
			var mn := String(mats_json[mi].get("name", "")).to_lower() if mi >= 0 else ""
			if mn.begins_with("tools") or mn.contains("black_simple") or mn.contains("nodraw") or mn.contains("transition_overlay"):
				skipped += 1
				continue
			if im.get_surface_primitive_type(s) != Mesh.PRIMITIVE_TRIANGLES:
				skipped += 1
				continue
			var idx = im.get_surface_arrays(s)[Mesh.ARRAY_INDEX]
			if idx == null or idx.size() < 3:
				continue
			keep.append([s, mi, idx.size() / 3])
			tris += idx.size() / 3
		if keep.is_empty():
			continue
		full_tris += tris
		if tris >= 300:
			im.generate_lods(25.0, 60.0, [])
		for k in keep:
			var s: int = k[0]
			var cands := [[k[2], 0.0]]
			if tris >= 300:
				for l in im.get_surface_lod_count(s):
					cands.append([im.get_surface_lod_indices(s, l).size() / 3, im.get_surface_lod_size(s, l)])
			surfs.append({"mesh": im, "s": s, "mat": k[1], "xf": xf, "cands": cands})
	_log("%d superficies (%d puladas), %d triangulos no original" % [surfs.size(), skipped, full_tris])

	# --- um limite de erro so para o mapa todo: o maior que ainda cabe no orcamento
	var lo := 0.0
	var hi := 1.0
	while _tris_at(surfs, hi) > budget and hi < 1.0e6:
		hi *= 2.0
	for _i in 40:
		var mid := (lo + hi) * 0.5
		if _tris_at(surfs, mid) > budget:
			lo = mid
		else:
			hi = mid
	var limit := hi
	var curve := []
	for e in [0.0, 0.001, 0.003, 0.01, 0.03, 0.1, 0.3, 1.0, 3.0, 10.0, 1.0e6]:
		curve.append("%.3f:%d" % [e, _tris_at(surfs, e)])
	_log("triangulos por limite de erro: " + ", ".join(curve))
	if max_error >= 0.0:
		limit = max_error
	_log("limite de erro %.4f -> %d triangulos" % [limit, _tris_at(surfs, limit)])

	_log("perto (erro ate %.3f): %d triangulos" % [near_error, _tris_at(surfs, near_error)])

	# --- geometria: cada superficie com o LOD de perto e o de longe (um so quando sao o mesmo),
	# so os vertices que cada LOD usa, ja no espaco do mundo
	var geo := FileAccess.open(out.path_join("geo.bin"), FileAccess.WRITE)
	var draws := []
	var used_mats := {}
	for sf in surfs:
		var near := 0
		var far := 0
		for c in sf.cands.size():
			if sf.cands[c][1] <= near_error:
				near = c
			if sf.cands[c][1] <= limit:
				far = c
		if near == far:
			_emit(geo, sf, far, "any", draws)
		else:
			_emit(geo, sf, near, "near", draws)
			_emit(geo, sf, far, "far", draws)
		used_mats[sf.mat] = true
	geo.close()
	_log("%d draws gravados" % draws.size())

	# --- materiais e texturas (so as usadas), DXT1 opaco ou DXT5 com recorte por alfa
	var base_dir := glb.get_base_dir()
	var tex_of_image := {}
	var textures := []
	var materials := []
	for mi in mats_json.size():
		var mj: Dictionary = mats_json[mi]
		var pbr: Dictionary = mj.get("pbrMetallicRoughness", {})
		var color: Array = pbr.get("baseColorFactor", [1, 1, 1, 1])
		var alpha_test := String(mj.get("alphaMode", "OPAQUE")) != "OPAQUE"
		var tex := -1
		if used_mats.has(mi) and pbr.has("baseColorTexture"):
			var ti: int = pbr.baseColorTexture.index
			var src = j.textures[ti].get("source", -1)
			if src != null and int(src) >= 0:
				var uri := String(j.images[int(src)].get("uri", ""))
				var key := uri + ("|a" if alpha_test else "|o")
				if tex_of_image.has(key):
					tex = tex_of_image[key]
				elif uri != "":
					tex = _texture(base_dir.path_join(uri.uri_decode()), alpha_test, textures)
					tex_of_image[key] = tex
		materials.append({"name": mj.get("name", ""), "color": color, "alpha_test": alpha_test, "tex": tex})
	_log("%d texturas comprimidas" % textures.size())

	var meta := {"draws": draws, "materials": materials, "textures": textures, "budget": budget, "tex_max": tex_max,
		"error_limit": limit, "near_error": near_error, "source_tris": full_tris}
	var mf := FileAccess.open(out.path_join("map.json"), FileAccess.WRITE)
	mf.store_string(JSON.stringify(meta))
	mf.close()
	_log("pronto: " + out)
	return 0

func _emit(geo: FileAccess, sf: Dictionary, pick: int, lod: String, draws: Array) -> void:
	var im: ImporterMesh = sf.mesh
	var s: int = sf.s
	var arrays := im.get_surface_arrays(s)
	var src_idx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX] if pick == 0 else im.get_surface_lod_indices(s, pick - 1)
	var pos: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var nrm = arrays[Mesh.ARRAY_NORMAL]
	var uv = arrays[Mesh.ARRAY_TEX_UV]
	var remap := PackedInt32Array()
	remap.resize(pos.size())
	remap.fill(-1)
	var new_idx := PackedInt32Array()
	new_idx.resize(src_idx.size())
	var order := PackedInt32Array()
	for i in src_idx.size():
		var v := src_idx[i]
		if remap[v] < 0:
			remap[v] = order.size()
			order.append(v)
		new_idx[i] = remap[v]
	var xf: Transform3D = sf.xf
	var nb := xf.basis.inverse().transposed()
	var vb := PackedFloat32Array()
	vb.resize(order.size() * 8)
	for i in order.size():
		var v := order[i]
		var p := xf * pos[v]
		var nn: Vector3 = (nb * (nrm[v] as Vector3)).normalized() if nrm != null and nrm.size() > v else Vector3.UP
		var t: Vector2 = (uv[v] as Vector2) if uv != null and uv.size() > v else Vector2.ZERO
		var o := i * 8
		vb[o] = p.x; vb[o + 1] = p.y; vb[o + 2] = p.z
		vb[o + 3] = nn.x; vb[o + 4] = nn.y; vb[o + 5] = nn.z
		vb[o + 6] = t.x; vb[o + 7] = t.y
	var voff := geo.get_position()
	geo.store_buffer(vb.to_byte_array())
	var ioff := geo.get_position()
	geo.store_buffer(new_idx.to_byte_array())
	draws.append({"mat": sf.mat, "vcount": order.size(), "icount": new_idx.size(), "voff": voff, "ioff": ioff, "lod": lod})

func _tris_at(surfs: Array, limit: float) -> int:
	var total := 0
	for sf in surfs:
		var t: int = sf.cands[0][0]
		for c in sf.cands:
			if c[1] <= limit:
				t = c[0]
		total += t
	return total

func _global_xform(nodes: Array, i: int) -> Transform3D:
	var t: Transform3D = (nodes[i] as GLTFNode).xform
	var p: int = (nodes[i] as GLTFNode).parent
	while p >= 0:
		t = (nodes[p] as GLTFNode).xform * t
		p = (nodes[p] as GLTFNode).parent
	return t

func _po2_floor(v: int) -> int:
	var p := 1
	while p * 2 <= v:
		p *= 2
	return p

func _texture(path: String, alpha_test: bool, textures: Array) -> int:
	var img := Image.load_from_file(path)
	if img == null or img.is_empty():
		_log("textura faltando: " + path)
		return -1
	if img.is_compressed():
		img.decompress()
	img.convert(Image.FORMAT_RGBA8 if alpha_test else Image.FORMAT_RGB8)
	var w := mini(tex_max, _po2_floor(img.get_width()))
	var h := mini(tex_max, _po2_floor(img.get_height()))
	w = maxi(w, 4)
	h = maxi(h, 4)
	if w != img.get_width() or h != img.get_height():
		img.resize(w, h, Image.INTERPOLATE_LANCZOS)
	img.generate_mipmaps()
	if img.compress(Image.COMPRESS_S3TC, Image.COMPRESS_SOURCE_GENERIC) != OK or not img.is_compressed():
		_log("nao comprimiu: " + path)
		return -1
	var fmt := ""
	match img.get_format():
		Image.FORMAT_DXT1: fmt = "dxt1"
		Image.FORMAT_DXT5: fmt = "dxt5"
		_:
			_log("formato inesperado %d: %s" % [img.get_format(), path])
			return -1
	var data := img.get_data()
	var levels := []
	for l in img.get_mipmap_count() + 1:
		levels.append(img.get_mipmap_offset(l))
	var id := textures.size()
	var f := FileAccess.open(out.path_join("tex/%04d.bin" % id), FileAccess.WRITE)
	f.store_buffer(data)
	f.close()
	textures.append({"file": "tex/%04d.bin" % id, "format": fmt, "width": w, "height": h, "level_offsets": levels,
		"bytes": data.size(), "source": path.get_file()})
	return id
