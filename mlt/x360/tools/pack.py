"""Segunda metade do conversor do Bot Strike 360: monta o dust2.pak big-endian que o console le.

Entrada: o intermediario do export_map.gd (work/map.json, geo.bin, tex/) e, do cache do Bot Strike, a colisao e as
entidades do Dust II. Saida: um unico arquivo, descrito em src/pack_format.h.

    python C:/mlt/x360/tools/pack.py --work C:/mlt/x360/work --out C:/mlt/x360/out/dust2.pak

Nada aqui sai da maquina: o pacote e feito da copia do CS2 de quem roda e so vai para o console dessa pessoa.
"""
import argparse
import json
import math
import os
import re
import struct
import sys

import numpy as np

VERSION = 1
U2M = 0.0254
TEX_DXT1, TEX_DXT5 = 1, 2
MAT_ALPHA_TEST = 1
DRAW_ALPHA_TEST, DRAW_NEAR, DRAW_FAR = 1, 2, 4
COL_CLIP, COL_SKY = 1, 2
MAX_DRAW = 65535
GRID_CELL = 2.0

# A mesma luz do world.gd, fixa nos vertices: o 360 nao paga sombra nem normal map por pixel.
AMBIENT = 0.42
SKY_LIGHT = 0.18
SUN_LIGHT = 0.72
FOG_START, FOG_END = 90.0, 260.0
SKY_RGB = (0.45, 0.62, 0.85)
FOG_RGB = (0.74, 0.78, 0.84)
# Ainda nao ha transparencia no console: estes ficam de fora.
SKIP_SHADERS = {'csgo_effects.vfx'}


def src_to_godot(v):
    return np.array([v[1], v[2], v[0]], dtype=np.float64) * U2M


def src_yaw_to_godot(yaw_deg):
    f = (math.sin(math.radians(yaw_deg)), 0.0, math.cos(math.radians(yaw_deg)))
    return math.atan2(-f[0], -f[2])


def read_glb(path):
    with open(path, 'rb') as f:
        magic, _, _ = struct.unpack('<4sII', f.read(12))
        if magic != b'glTF':
            sys.exit(f'nao e glb: {path}')
        jlen, _ = struct.unpack('<II', f.read(8))
        doc = json.loads(f.read(jlen))
        blen, _ = struct.unpack('<II', f.read(8))
        blob = f.read(blen)
    return doc, blob


def accessor(doc, blob, i):
    a = doc['accessors'][i]
    bv = doc['bufferViews'][a['bufferView']]
    comps = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    dt = {5126: '<f4', 5125: '<u4', 5123: '<u2', 5121: 'u1'}[a['componentType']]
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    n = a['count']
    itemsize = np.dtype(dt).itemsize * comps
    stride = bv.get('byteStride', itemsize)
    if stride == itemsize:
        arr = np.frombuffer(blob, dt, n * comps, off)
    else:
        raw = np.frombuffer(blob, 'u1', stride * (n - 1) + itemsize, off)
        arr = np.lib.stride_tricks.as_strided(raw, (n, itemsize), (stride, 1)).copy().view(dt)
    return arr.reshape(n, comps) if comps > 1 else arr


def node_matrix(n):
    if 'matrix' in n:
        return np.array(n['matrix'], dtype=np.float64).reshape(4, 4).T
    m = np.eye(4)
    t = n.get('translation', [0, 0, 0])
    r = n.get('rotation', [0, 0, 0, 1])
    s = n.get('scale', [1, 1, 1])
    x, y, z, w = r
    rot = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                    [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                    [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m[:3, :3] = rot * np.array(s)
    m[:3, 3] = t
    return m


# ---------------------------------------------------------------- colisao

def load_collision(path):
    doc, blob = read_glb(path)
    parent = {}
    for i, n in enumerate(doc['nodes']):
        for c in n.get('children', []):
            parent[c] = i
    tris, flags = [], []
    kept, skipped = 0, 0
    for i, n in enumerate(doc['nodes']):
        if 'mesh' not in n:
            continue
        name = n.get('name', '').lower()
        p = parent.get(i)
        full = (doc['nodes'][p].get('name', '').lower() + ' ' if p is not None else '') + name
        if 'grenadeclip' in full:
            skipped += 1
            continue
        flag = 0
        if 'playerclip' in full or 'passbullets' in full or 'physics_sky' in full:
            flag |= COL_CLIP
        if 'sky' in full:
            flag |= COL_SKY
        m = node_matrix(n)
        while p is not None:
            m = node_matrix(doc['nodes'][p]) @ m
            p = parent.get(p)
        for prim in doc['meshes'][n['mesh']]['primitives']:
            if prim.get('mode', 4) != 4:
                continue
            pos = accessor(doc, blob, prim['attributes']['POSITION']).astype(np.float64)
            pos = pos @ m[:3, :3].T + m[:3, 3]
            idx = accessor(doc, blob, prim['indices']).astype(np.int64) if 'indices' in prim else np.arange(len(pos))
            t = pos[idx.reshape(-1, 3)]
            tris.append(t)
            flags.append(np.full(len(t), flag, np.uint8))
            kept += 1
    tris = np.concatenate(tris).astype(np.float32)
    flags = np.concatenate(flags)
    # triangulos degenerados nao seguram ninguem e so atrapalham a normal
    area = np.linalg.norm(np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]), axis=1)
    ok = area > 1e-6
    print(f'colisao: {kept} pecas, {skipped} puladas, {ok.sum()} triangulos ({(~ok).sum()} degenerados fora)')
    return tris[ok], flags[ok]


def build_grid(tris):
    lo = tris.reshape(-1, 3).min(0) - 0.5
    hi = tris.reshape(-1, 3).max(0) + 0.5
    dims = np.ceil((hi - lo) / GRID_CELL).astype(np.int64)
    nx, ny, nz = (int(d) for d in dims)
    tmin = np.floor((tris.min(1) - lo) / GRID_CELL).astype(np.int64)
    tmax = np.floor((tris.max(1) - lo) / GRID_CELL).astype(np.int64)
    tmin = np.clip(tmin, 0, dims - 1)
    tmax = np.clip(tmax, 0, dims - 1)
    span = tmax - tmin + 1
    count = span[:, 0] * span[:, 1] * span[:, 2]
    tri_id = np.repeat(np.arange(len(tris), dtype=np.int64), count)
    k = np.arange(count.sum(), dtype=np.int64) - np.repeat(np.cumsum(count) - count, count)
    sx = span[tri_id, 0]
    sy = span[tri_id, 1]
    ix = tmin[tri_id, 0] + k % sx
    iy = tmin[tri_id, 1] + (k // sx) % sy
    iz = tmin[tri_id, 2] + k // (sx * sy)
    cell = (iz * ny + iy) * nx + ix
    order = np.argsort(cell, kind='stable')
    refs = tri_id[order].astype(np.uint32)
    starts = np.zeros(nx * ny * nz + 1, np.uint32)
    starts[1:] = np.cumsum(np.bincount(cell, minlength=nx * ny * nz)).astype(np.uint32)
    print(f'grade: {nx}x{ny}x{nz} celulas de {GRID_CELL} m, {len(refs)} referencias, '
          f'maior celula {int(np.diff(starts).max())} triangulos')
    return lo, (nx, ny, nz), starts, refs


# ---------------------------------------------------------------- entidades

def parse_entities(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        txt = f.read()
    ents = []
    for block in txt.split('===='):
        d = {}
        for line in block.split('\n'):
            line = line.strip()
            sp = line.find(' ')
            if sp > 0:
                d[line[:sp]] = line[sp:].strip().strip('"')
        if 'classname' in d:
            ents.append(d)
    return ents


def vec(s):
    p = re.sub(r'[\[\],]', ' ', s).split()
    return [float(x) for x in p[:3]] if len(p) >= 3 else [0.0, 0.0, 0.0]


def spawns_and_sun(ents):
    spawns = []
    for team, cls in ((0, 'info_player_counterterrorist'), (1, 'info_player_terrorist')):
        lst = [e for e in ents if e['classname'] == cls]
        lst.sort(key=lambda e: -int(float(e.get('priority', '0'))))
        for e in lst:
            o = src_to_godot(vec(e.get('origin', '0 0 0'))) + np.array([0, 0.05, 0])
            yaw = src_yaw_to_godot(vec(e.get('angles', '0 0 0'))[1])
            spawns.append((team, o, yaw))
    sun = np.array([-0.35, -0.85, 0.4])
    for e in ents:
        if e['classname'] == 'light_environment':
            a = vec(e.get('angles', '0 0 0'))
            p, y = math.radians(a[0]), math.radians(a[1])
            d = src_to_godot([math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p)])
            d /= np.linalg.norm(d)
            if d[1] <= -0.2:
                sun = d
    sun = sun / np.linalg.norm(sun)
    ct = sum(1 for s in spawns if s[0] == 0)
    print(f'spawns: CT {ct}, T {len(spawns) - ct}; sol {np.round(sun, 3)}')
    return spawns, sun


# ---------------------------------------------------------------- render

def level_bytes(fmt, w, h, levels):
    bs = 8 if fmt == TEX_DXT1 else 16
    out = []
    for lv in range(levels):
        lw, lh = max(1, w >> lv), max(1, h >> lv)
        out.append(max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bs)
    return out


def load_textures(work, meta):
    recs, blobs, offset = [], [], 0
    for t in meta['textures']:
        fmt = TEX_DXT1 if t['format'] == 'dxt1' else TEX_DXT5
        w, h = t['width'], t['height']
        offs = t['level_offsets']
        with open(os.path.join(work, t['file']), 'rb') as f:
            data = f.read()
        sizes = level_bytes(fmt, w, h, len(offs))
        parts = []
        for lv, (o, sz) in enumerate(zip(offs, sizes)):
            nxt = offs[lv + 1] if lv + 1 < len(offs) else len(data)
            if nxt - o < sz:
                sys.exit(f'textura {t["file"]}: nivel {lv} tem {nxt - o} bytes, esperava {sz}')
            parts.append(data[o:o + sz])
        raw = np.frombuffer(b''.join(parts), '<u2')
        be = raw.astype('>u2').tobytes()   # cada palavra de 16 bits do .dds em big-endian
        recs.append((fmt, w, h, len(offs), offset, len(be)))
        blobs.append(be)
        offset += len(be)
        pad = (-offset) % 16
        if pad:
            blobs.append(b'\0' * pad)
            offset += pad
    return recs, b''.join(blobs)


def shade(normals, sun):
    n = normals / np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-6)
    lam = np.maximum(0.0, n @ (-sun))
    sky = n[:, 1] * 0.5 + 0.5
    return (AMBIENT + SKY_LIGHT * sky + SUN_LIGHT * lam).astype(np.float32)


def split_draw(v, idx):
    """Corta uma superficie em pedacos de no maximo 65535 indices e 65535 vertices, cada um compactado."""
    tris = idx.reshape(-1, 3)
    per = MAX_DRAW // 3
    for s in range(0, len(tris), per):
        t = tris[s:s + per]
        used, inv = np.unique(t.reshape(-1), return_inverse=True)
        yield v[used], inv.astype(np.uint16)


def max_edge(vb, ib):
    t = vb[ib.reshape(-1, 3), 0:3]
    e = np.maximum(np.maximum(np.linalg.norm(t[:, 0] - t[:, 1], axis=1), np.linalg.norm(t[:, 1] - t[:, 2], axis=1)),
                   np.linalg.norm(t[:, 2] - t[:, 0], axis=1))
    return float(e.max()) if len(e) else 0.0


def effect_materials(map_glb):
    """Materiais de efeito do CS2 (poeira no ar, vapor): translucidos, que aqui sairiam como placas opacas."""
    with open(map_glb, 'rb') as f:
        f.read(12)
        jlen, _ = struct.unpack('<II', f.read(8))
        doc = json.loads(f.read(jlen))
    skip = set()
    for i, m in enumerate(doc.get('materials', [])):
        shader = m.get('extras', {}).get('vmat', {}).get('ShaderName', '')
        if shader in SKIP_SHADERS:
            skip.add(i)
    return skip


def build_render(work, meta, sun, skip_mats):
    geo = np.memmap(os.path.join(work, 'geo.bin'), 'u1', 'r')
    mats = meta['materials']
    draws = []
    src = [d for d in meta['draws'] if d['mat'] not in skip_mats]
    print(f'{len(meta["draws"]) - len(src)} draws de efeito translucido fora')
    lods = []
    for d in src:
        vb = np.frombuffer(geo, '<f4', d['vcount'] * 8, d['voff']).reshape(-1, 8)
        ib = np.frombuffer(geo, '<i4', d['icount'], d['ioff']).astype(np.int64)
        lods.append([d, vb, ib, d.get('lod', 'any')])
    # O CS2 junta pecas soltas (fios, toldos) numa malha so, e o LOD mais grosso as vezes liga pecas distantes com
    # triangulos enormes. Se o de longe tem aresta bem maior que o de perto, a superficie fica com o de perto.
    rejected = 0
    for k in range(len(lods) - 1):
        a, b = lods[k], lods[k + 1]
        if a[3] == 'near' and b[3] == 'far' and a[0]['mat'] == b[0]['mat'] and len(a[2]) >= 3 and len(b[2]) >= 3:
            if max_edge(b[1], b[2]) > max_edge(a[1], a[2]) * 1.3 + 0.25:
                a[3] = 'any'
                b[3] = 'drop'
                rejected += 1
    print(f'LOD de longe trocado pelo de perto em {rejected} superficies (triangulos esticados)')
    for d, vb, ib, lod_name in lods:
        if lod_name == 'drop':
            continue
        d = dict(d, lod=lod_name)
        if len(ib) < 3 or ib.max() >= len(vb) or ib.min() < 0:
            continue
        v = np.empty((len(vb), 6), np.float32)
        v[:, 0:3] = vb[:, 0:3]
        v[:, 3:5] = vb[:, 6:8]
        v[:, 5] = shade(vb[:, 3:6].astype(np.float64), sun)
        if not np.isfinite(v).all():
            continue
        lod = {'near': DRAW_NEAR, 'far': DRAW_FAR}.get(d.get('lod', 'any'), 0)
        for sv, si in split_draw(v, ib):
            draws.append((d['mat'], sv, si, lod))
    # opacos primeiro, depois os recortados por alfa; dentro de cada grupo, por material
    draws.sort(key=lambda x: (mats[x[0]]['alpha_test'], x[0]))
    verts, inds, recs = [], [], []
    vfirst = ifirst = 0
    for mat, v, i, lod in draws:
        flags = (DRAW_ALPHA_TEST if mats[mat]['alpha_test'] else 0) | lod
        lo, hi = v[:, 0:3].min(0), v[:, 0:3].max(0)
        recs.append((mat, vfirst, len(v), ifirst, len(i), flags, lo, hi))
        verts.append(v)
        inds.append(i)
        vfirst += len(v)
        ifirst += len(i)
        if ifirst % 2:   # cada lista de indices comeca em 4 bytes
            inds.append(np.zeros(1, np.uint16))
            ifirst += 1
    return recs, np.concatenate(verts), np.concatenate(inds)


# ---------------------------------------------------------------- gravacao

def be_f32(a):
    return np.asarray(a, np.float32).astype('>f4').tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--cache', default=os.path.join(os.environ.get('APPDATA', ''), 'BotStrike', 'cs2cache'))
    args = ap.parse_args()

    with open(os.path.join(args.work, 'map.json'), encoding='utf-8') as f:
        meta = json.load(f)
    phys = None
    for rel in ('maps/de_dust2/world_physics_physics.glb', 'maps/de_dust2/world_physics.glb'):
        p = os.path.join(args.cache, rel)
        if os.path.exists(p) and os.path.getsize(p) > 1024:
            phys = p
            break
    if phys is None:
        sys.exit('colisao do Dust II nao encontrada no cache do Bot Strike; abra o Bot Strike uma vez antes')
    ents = parse_entities(os.path.join(args.cache, 'maps/de_dust2/entities/default_ents.vents'))
    spawns, sun = spawns_and_sun(ents)
    if not spawns:
        sys.exit('nenhum spawn nas entidades do Dust II')

    draws, verts, inds = build_render(args.work, meta, sun,
                                      effect_materials(os.path.join(args.cache, 'maps/de_dust2.glb')))
    tex_recs, tex_data = load_textures(args.work, meta)
    tris, cflags = load_collision(phys)
    g_origin, g_dims, g_starts, g_refs = build_grid(tris)

    lo, hi = verts[:, 0:3].min(0), verts[:, 0:3].max(0)
    sections = []

    info = be_f32(list(sun) + [AMBIENT, SUN_LIGHT, FOG_START, FOG_END] + list(SKY_RGB) + list(FOG_RGB) +
                  list(lo) + list(hi) + [0.0])
    assert len(info) == 80
    sections.append((b'INFO', info, 1))

    sp = b''.join(struct.pack('>I', t) + be_f32(list(o) + [y]) for t, o, y in spawns)
    sections.append((b'SPWN', sp, len(spawns)))

    tr = b''.join(struct.pack('>IHHHHIII', f, w, h, lv, 0, o, n, 0) for f, w, h, lv, o, n in tex_recs)
    sections.append((b'TEXR', tr, len(tex_recs)))
    sections.append((b'TEXD', tex_data, len(tex_data)))

    ml = b''
    for m in meta['materials']:
        c = (list(m['color']) + [1, 1, 1, 1])[:4]
        ml += struct.pack('>i', m['tex'] if m['tex'] is not None else -1) + be_f32(c) + \
            struct.pack('>I', MAT_ALPHA_TEST if m['alpha_test'] else 0)
    sections.append((b'MATL', ml, len(meta['materials'])))

    sections.append((b'VERT', verts.astype('>f4').tobytes(), len(verts)))
    sections.append((b'INDX', inds.astype('>u2').tobytes(), len(inds)))

    dr = b''.join(struct.pack('>IIIIII', m, vf, vc, i_f, ic, fl) + be_f32(list(a) + list(b))
                  for m, vf, vc, i_f, ic, fl, a, b in draws)
    sections.append((b'DRAW', dr, len(draws)))

    sections.append((b'CTRI', tris.astype('>f4').tobytes(), len(tris)))
    sections.append((b'CFLG', cflags.tobytes(), len(cflags)))
    grid = be_f32(list(g_origin) + [GRID_CELL]) + struct.pack('>IIII', *g_dims, len(g_refs)) + \
        g_starts.astype('>u4').tobytes() + g_refs.astype('>u4').tobytes()
    sections.append((b'CGRD', grid, 1))

    head_size = 16 + 16 * len(sections)
    offset = (head_size + 15) // 16 * 16
    table, body = b'', b''
    for tag, data, count in sections:
        table += tag + struct.pack('>III', offset, len(data), count)
        pad = (-len(data)) % 16
        body += data + b'\0' * pad
        offset += len(data) + pad
    header = b'BS36' + struct.pack('>III', VERSION, len(sections), offset)
    blob = header + table + b'\0' * ((-(len(header) + len(table))) % 16) + body
    assert len(blob) == offset
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(blob)

    gpu = len(verts) * 24 + len(inds) * 2 + len(tex_data)
    cpu = len(tris) * 37 + len(g_starts) * 4 + len(g_refs) * 4 + len(draws) * 48
    near_tris = sum(r[4] for r in draws if not r[5] & DRAW_FAR) // 3
    far_tris = sum(r[4] for r in draws if not r[5] & DRAW_NEAR) // 3
    report = (f'pacote: {args.out} ({len(blob) / 1e6:.1f} MB)\n'
              f'mapa inteiro com o LOD de perto: {near_tris} triangulos; com o de longe: {far_tris}\n'
              f'desenho: {len(draws)} draws, {len(verts)} vertices, {len(inds) // 3} triangulos, '
              f'{len(tex_recs)} texturas ({len(tex_data) / 1e6:.1f} MB)\n'
              f'memoria no console: ~{gpu / 1e6:.1f} MB de GPU, ~{cpu / 1e6:.1f} MB de CPU\n'
              f'limites do mapa: {np.round(lo, 1)} a {np.round(hi, 1)}\n')
    print(report)
    with open(os.path.splitext(args.out)[0] + '.txt', 'w', encoding='utf-8') as f:
        f.write(report)


if __name__ == '__main__':
    main()
