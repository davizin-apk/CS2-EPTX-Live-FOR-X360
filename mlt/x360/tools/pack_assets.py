"""Monta o assets.pak do Bot Strike 360 (modelos, animacoes, texturas, sons, HUD, navmesh) e gera src/defs.h.

Entrada: o que o x360_bake.gd (dentro do projeto do Bot Strike) gravou em work/assets, as fontes do CS2 e as
planilhas de design do jogo original. Tudo sai big-endian; o formato esta em src/assets.h.

    python C:/mlt/x360/tools/pack_assets.py --work C:/mlt/x360/work/assets --out C:/mlt/x360/out/assets.pak
"""
import argparse
import json
import os
import struct
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DESIGN = 'C:/mlt/proj/design'

T_MODEL, T_ANIM, T_TEXTURE, T_SOUND, T_IMAGE, T_FONT, T_NAV, T_SITES, T_RECTS = range(1, 10)
TEX_DXT1, TEX_DXT5 = 1, 2
ATLAS = 1024


def be_f32(a):
    return np.asarray(a, np.float32).astype('>f4').tobytes()


def level_bytes(fmt, w, h, levels):
    bs = 8 if fmt == TEX_DXT1 else 16
    return [max(1, (max(1, w >> l) + 3) // 4) * max(1, (max(1, h >> l) + 3) // 4) * bs for l in range(levels)]


def texture_blob(work, t):
    fmt = TEX_DXT1 if t['format'] == 'dxt1' else TEX_DXT5
    w, h, offs = t['width'], t['height'], t['level_offsets']
    with open(os.path.join(work, t['file']), 'rb') as f:
        data = f.read()
    parts = []
    for lv, sz in enumerate(level_bytes(fmt, w, h, len(offs))):
        parts.append(data[offs[lv]:offs[lv] + sz])
    raw = np.frombuffer(b''.join(parts), '<u2').astype('>u2').tobytes()
    return b'TEX1' + struct.pack('>IIII', fmt, w, h, len(offs)) + raw


def sound_blob(path, music):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 1:
        a = (np.frombuffer(raw, np.uint8).astype(np.int16) - 128) * 256
    elif width == 2:
        a = np.frombuffer(raw, '<i2').astype(np.int32)
    else:
        sys.exit(f'{path}: {width * 8} bits por amostra')
    a = a.reshape(-1, ch).astype(np.float32)
    mono = a.mean(axis=1)
    # efeitos e musica em mono; a musica e o vento a 22 kHz, que ocupam muito no 360
    if music and rate >= 44000:
        mono = mono[: len(mono) // 2 * 2].reshape(-1, 2).mean(axis=1)
        rate //= 2
    pcm = np.clip(mono, -32768, 32767).astype('>i2')
    return b'SND1' + struct.pack('>III', rate, 1, len(pcm)) + pcm.tobytes()


# ---------------------------------------------------------------- HUD: atlas com fontes e icones

class Packer:
    def __init__(self, size):
        self.img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
        self.x = self.y = 0
        self.row = 0
        self.size = size

    def add(self, im):
        w, h = im.size
        if self.x + w + 2 > self.size:
            self.x = 0
            self.y += self.row + 2
            self.row = 0
        if self.y + h > self.size:
            sys.exit('atlas da HUD cheio')
        self.img.paste(im, (self.x, self.y))
        r = (self.x, self.y, w, h)
        self.x += w + 2
        self.row = max(self.row, h)
        return r


def font_blob(packer, ttf, px):
    font = ImageFont.truetype(ttf, px)
    ascent, descent = font.getmetrics()
    glyphs = []
    for c in range(32, 256):
        ch = chr(c)
        adv = font.getlength(ch)
        box = font.getbbox(ch)
        if box[2] <= box[0] or box[3] <= box[1]:
            glyphs.append((c, 0, 0, 0, 0, 0, 0, adv))
            continue
        w, h = box[2] - box[0] + 2, box[3] - box[1] + 2
        im = Image.new('RGBA', (w, h), (255, 255, 255, 0))
        mask = Image.new('L', (w, h), 0)
        ImageDraw.Draw(mask).text((1 - box[0], 1 - box[1]), ch, font=font, fill=255)
        im.putalpha(mask)
        x, y, w, h = packer.add(im)
        glyphs.append((c, x, y, w, h, box[0] - 1, box[1] - 1, adv))
    out = b'FNT1' + struct.pack('>Iff', len(glyphs), float(ascent + descent), float(ascent))
    for c, x, y, w, h, ox, oy, adv in glyphs:
        out += struct.pack('>IHHHH', c, x, y, w, h) + be_f32([ox, oy, adv])
    return out


def hud_atlas(work, idx, cs2):
    pk = Packer(ATLAS)
    rects = {}
    white = Image.new('RGBA', (8, 8), (255, 255, 255, 255))
    x, y, w, h = pk.add(white)
    rects['white'] = (x + 2, y + 2, 4, 4)
    fonts = {}
    for name, file, px in (('bold', 'notosans-bold.ttf', 44), ('regular', 'notosans-regular.ttf', 30)):
        path = os.path.join(cs2, 'game/csgo/panorama/fonts', file)
        if not os.path.exists(path):
            sys.exit(f'fonte do CS2 nao encontrada: {path}')
        fonts[name] = font_blob(pk, path, px)
    for icon in sorted(idx.get('icons', {})):
        im = Image.open(os.path.join(work, 'icons', icon + '.png')).convert('RGBA')
        rects[icon] = pk.add(im)
    # o redemoinho do portal e o brilho do cano: desenhados aqui, sem nada do CS2
    sz = 64
    yy, xx = np.mgrid[0:sz, 0:sz].astype(np.float32) - (sz - 1) / 2
    d = np.sqrt(xx * xx + yy * yy) / (sz / 2)
    star = np.abs(np.sin(np.arctan2(yy, xx) * 3.0)) * 0.35
    a = np.clip(1.0 - d / (0.65 + star), 0, 1) ** 2
    flash = np.dstack([np.full_like(a, 255), np.full_like(a, 255), np.full_like(a, 255), a * 255]).astype(np.uint8)
    rects['flash'] = pk.add(Image.fromarray(flash, 'RGBA'))
    ang = np.arctan2(yy, xx)
    s = np.sin(ang * 3.0 + d * 9.0) * 0.5 + 0.5
    inside = d <= 1.0
    lum = np.where(inside, 0.18 + 0.72 * s * (1.0 - d * 0.5), 0.0)
    rim = np.clip((d - 0.8) / 0.17, 0, 1) * inside
    lum = lum * (1 - rim) + rim * 1.0
    swirl = np.dstack([lum * 255, lum * 255, lum * 255, inside * 255]).astype(np.uint8)
    rects['swirl'] = pk.add(Image.fromarray(swirl, 'RGBA'))
    circ = (d <= 1.0).astype(np.float32)
    rects['disc'] = pk.add(Image.fromarray(np.dstack([circ * 255] * 3 + [circ * 255]).astype(np.uint8), 'RGBA'))
    rgba = np.asarray(pk.img, np.uint8)
    img = b'IMG1' + struct.pack('>II', ATLAS, ATLAS) + rgba.tobytes()
    rb = b'RCT1' + struct.pack('>I', len(rects))
    for k, (x, y, w, h) in rects.items():
        nm = k.encode()[:31].ljust(32, b'\0')
        rb += nm + struct.pack('>HHHH', x, y, w, h)
    return img, rb, fonts


# ---------------------------------------------------------------- planilhas -> defs.h

def c_str(s):
    return '"' + str(s).replace('\\', '\\\\').replace('"', '\\"') + '"'


def gen_defs(out_h):
    sheets = ['weapons', 'equipment', 'modes', 'teams', 'bot_skills', 'build_pieces', 'build_materials']
    lines = ['// Gerado pelo tools/pack_assets.py a partir das planilhas do Bot Strike (C:/mlt/proj/design).',
             '// Nao edite: mude a planilha e gere de novo.', '#ifndef BS_DEFS_H', '#define BS_DEFS_H', '',
             '#include <stdbool.h>', '']
    for sh in sheets:
        with open(os.path.join(DESIGN, sh + '.json'), encoding='utf-8') as f:
            d = json.load(f)
        rows = d['rows']
        cols = d['columns']
        tname = ''.join(p.capitalize() for p in sh.split('_')) + 'Def'
        fields = []
        for c in cols:
            vals = [r[c] for r in rows]
            if all(isinstance(v, bool) for v in vals):
                fields.append((c, 'bool'))
            elif all(isinstance(v, int) and not isinstance(v, bool) for v in vals):
                fields.append((c, 'int'))
            elif all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in vals):
                fields.append((c, 'float'))
            elif all(isinstance(v, list) for v in vals):
                fields.append((c, 'list'))
            else:
                fields.append((c, 'str'))
        lines.append('typedef struct {')
        for c, t in fields:
            ct = {'bool': 'bool', 'int': 'int', 'float': 'float', 'str': 'const char *', 'list': 'const char *'}[t]
            cn = c + '_' if c in ('auto', 'default', 'int', 'float', 'char', 'case', 'register') else c
            lines.append(f'	{ct} {cn}{"[5]" if t == "list" else ""};')
        lines.append(f'}} {tname};')
        up = sh.upper()
        lines.append(f'enum {{')
        for i, r in enumerate(rows):
            lines.append(f'\t{up}_{str(r["id"]).upper()} = {i},')
        lines.append(f'\t{up}_COUNT = {len(rows)},')
        lines.append('};')
        lines.append(f'static const {tname} {up}[{up}_COUNT] = {{')
        for r in rows:
            vals = []
            for c, t in fields:
                v = r[c]
                if t == 'bool':
                    vals.append('true' if v else 'false')
                elif t == 'int':
                    vals.append(str(v))
                elif t == 'float':
                    vals.append(repr(float(v)) + 'f')
                elif t == 'list':
                    vals.append('{ ' + ', '.join(c_str(x) for x in v[:5]) + ' }')
                else:
                    vals.append(c_str(v))
            lines.append('\t{ ' + ', '.join(vals) + ' },')
        lines.append('};')
        lines.append('')
    lines.append('#endif')
    with open(out_h, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{out_h}: {len(sheets)} planilhas')


# ---------------------------------------------------------------- montagem

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--cs2', default='D:/SteamLibrary/steamapps/common/Counter-Strike Global Offensive')
    args = ap.parse_args()
    gen_defs(os.path.join(ROOT, 'src', 'defs.h'))
    with open(os.path.join(args.work, 'index.json'), encoding='utf-8') as f:
        idx = json.load(f)
    entries = []
    for i, t in enumerate(idx['textures']):
        entries.append((f'tex/{i}', T_TEXTURE, texture_blob(args.work, t)))
    for name in sorted(idx['models']):
        with open(os.path.join(args.work, 'models', name + '.mdl'), 'rb') as f:
            entries.append((f'mdl/{name}', T_MODEL, f.read()))
    for name in sorted(idx['anims']):
        with open(os.path.join(args.work, 'anims', name + '.anm'), 'rb') as f:
            entries.append((f'anm/{name}', T_ANIM, f.read()))
    for sid in sorted(idx.get('sounds', {})):
        p = os.path.join(args.work, 'sounds', sid + '.wav')
        if os.path.exists(p):
            entries.append((f'snd/{sid}', T_SOUND, sound_blob(p, sid.startswith('mus_') or sid.startswith('amb_'))))
    img, rects, fonts = hud_atlas(args.work, idx, args.cs2)
    entries.append(('hud/atlas', T_IMAGE, img))
    entries.append(('hud/rects', T_RECTS, rects))
    for k, v in fonts.items():
        entries.append((f'font/{k}', T_FONT, v))
    navp = os.path.join(args.work, 'nav.bin')
    if os.path.exists(navp):
        with open(navp, 'rb') as f:
            entries.append(('nav/dust2', T_NAV, f.read()))
    sites = idx.get('sites', {})
    if 'A' in sites and 'B' in sites:
        entries.append(('map/sites', T_SITES, b'SIT1' + be_f32(sites['A'] + sites['B'])))
    bt = idx.get('build_textures', {})
    entries.append(('map/build_textures', T_RECTS, b'BTX1' + struct.pack('>iii', bt.get('wood', -1), bt.get('brick', -1),
                                                                          bt.get('metal', -1))))

    head = 16 + 60 * len(entries)
    offset = (head + 15) // 16 * 16
    table, body = b'', b''
    for name, typ, data in entries:
        nm = name.encode()[:47].ljust(48, b'\0')
        table += nm + struct.pack('>III', typ, offset, len(data))
        pad = (-len(data)) % 16
        body += data + b'\0' * pad
        offset += len(data) + pad
    blob = b'BSA1' + struct.pack('>III', 1, len(entries), offset) + table
    blob += b'\0' * ((-len(blob)) % 16) + body
    assert len(blob) == offset
    with open(args.out, 'wb') as f:
        f.write(blob)
    by = {}
    for name, typ, data in entries:
        k = name.split('/')[0]
        by[k] = by.get(k, 0) + len(data)
    print(f'{args.out}: {len(blob) / 1e6:.1f} MB, {len(entries)} itens; ' +
          ', '.join(f'{k} {v / 1e6:.1f} MB' for k, v in sorted(by.items())))


if __name__ == '__main__':
    main()
