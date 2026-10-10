#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""CHR-ROM (8 KiB) of the ReplayNES Test Cartridge, drawn from scratch for this project.

Pattern table 0 ($0000) = background, pattern table 1 ($1000) = sprites. Text uses the font in
font.txt at the tile number of its ASCII code (0x20-0x7E) in both tables. Every other tile is
generated here; its number is exported as an assembler symbol (T_* background, S_* sprite).
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def load_font():
    glyphs, cur, name = {}, [], None
    with open(os.path.join(HERE, 'font.txt'), encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('//') or not line:
                continue
            if line.startswith('= '):
                if name is not None:
                    glyphs[name] = cur
                key = line[2:]
                name, cur = (' ' if key == 'space' else key), []
            else:
                if len(line) != 8:
                    raise ValueError('font row must be 8 wide: %r (%r)' % (line, name))
                cur.append([1 if c == '#' else 0 for c in line])
    glyphs[name] = cur
    for k, rows in glyphs.items():
        if len(rows) != 8:
            raise ValueError('glyph %r has %d rows' % (k, len(rows)))
    return glyphs


def blank():
    return [[0] * 8 for _ in range(8)]


def solid(c, rows=range(8)):
    t = blank()
    for y in rows:
        t[y] = [c] * 8
    return t


def encode(tile):
    lo = bytearray(8)
    hi = bytearray(8)
    for y in range(8):
        for x in range(8):
            p = tile[y][x]
            if p & 1:
                lo[y] |= 0x80 >> x
            if p & 2:
                hi[y] |= 0x80 >> x
    return bytes(lo + hi)


def glyph(g, ink=1, paper=0):
    return [[ink if v else paper for v in row] for row in g]


def from_art(rows, keymap):
    return [[keymap[c] for c in row] for row in rows]


def big_letter(g):
    """2x scaled glyph in a 16x16 cell: top half colour 1, bottom half colour 2, shadow colour 3."""
    cell = [[0] * 16 for _ in range(16)]
    for y in range(7):
        for x in range(7):
            if g[y][x]:
                for dy in range(2):
                    for dx in range(2):
                        sy, sx = 2 * y + dy + 1, 2 * x + dx + 1
                        if sy < 16 and sx < 16 and cell[sy][sx] == 0:
                            cell[sy][sx] = 3
    for y in range(7):
        for x in range(7):
            if g[y][x]:
                for dy in range(2):
                    for dx in range(2):
                        sy, sx = 2 * y + dy, 2 * x + dx
                        cell[sy][sx] = 1 if sy < 7 else 2
    return [[row[x:x + 8] for row in cell[y:y + 8]] for y in (0, 8) for x in (0, 8)]


def circle_cell(r_out, colour, ring=None, ring_colour=None, highlight=None):
    """16x16 filled circle split into 4 tiles (TL, TR, BL, BR)."""
    cell = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            dx, dy = x - 7.5, y - 7.5
            d2 = dx * dx + dy * dy
            if d2 <= r_out * r_out:
                cell[y][x] = colour
                if ring is not None and d2 >= ring * ring:
                    cell[y][x] = ring_colour
                if highlight is not None and (dx + 2.5) ** 2 + (dy + 2.5) ** 2 <= highlight:
                    cell[y][x] = 3
    return [[row[x:x + 8] for row in cell[y:y + 8]] for y in (0, 8) for x in (0, 8)]


def build():
    font = load_font()
    bg = [blank() for _ in range(256)]
    spr = [blank() for _ in range(256)]
    syms = {}

    for code in range(0x20, 0x7F):
        g = font[chr(code)]
        bg[code] = glyph(g)
        # Sprite text: white glyph on an opaque dark box (readable over any background).
        spr[code] = glyph(g, ink=1, paper=3)

    def put(name, tile, table=bg, at=None):
        idx = at
        if idx is None:
            idx = put.next_bg if table is bg else put.next_spr
        table[idx] = tile
        syms[name] = idx
        if table is bg and at is None:
            put.next_bg += 1
        if table is spr and at is None:
            put.next_spr += 1
        return idx

    # ---------------------------------------------------------------- background, 0x00-0x1F
    put('T_BLANK', blank(), at=0x00)
    put('T_SOLID1', solid(1), at=0x01)
    put('T_SOLID2', solid(2), at=0x02)
    put('T_SOLID3', solid(3), at=0x03)
    t = blank()
    t[6][6] = 2
    put('T_SYNCDOT', t, at=0x04)                 # palette chart: sprite-0 hit target (x=6, y=6)
    put('T_SWATCH', solid(3, range(7)), at=0x05)  # palette chart swatch: 7 rows of colour 3
    put('T_HUDLINE', solid(1, (6, 7)), at=0x06)   # scroll HUD bottom border (sprite-0 target)
    put('T_RULE', solid(2, (3,)), at=0x07)        # thin horizontal rule
    for n in range(9):                            # 0..8 px of a horizontal bar, rows 1-6
        t = blank()
        for y in range(1, 7):
            for x in range(n):
                t[y][x] = 3
        put('T_BAR%d' % n, t, at=0x08 + n)
    # 0x11-0x14 oscilloscope trace (high line row 1, low line row 6, colour 1)
    def wave(prev, cur):
        t = blank()
        row = 1 if cur else 6
        t[row] = [1] * 8
        if prev != cur:
            for y in range(1, 7):
                t[y][0] = 1
        return t
    put('T_WAVE_LL', wave(0, 0), at=0x11)
    put('T_WAVE_LH', wave(0, 1), at=0x12)
    put('T_WAVE_HL', wave(1, 0), at=0x13)
    put('T_WAVE_HH', wave(1, 1), at=0x14)
    t = blank()
    for y in range(0, 8, 2):
        t[y][3] = 2
    put('T_WAVE_CURSOR', t, at=0x15)
    # 0x16-0x19 sharpness patterns (colour 1 on 0)
    put('T_VSTRIPE1', [[1 if x % 2 == 0 else 0 for x in range(8)] for _ in range(8)], at=0x16)
    put('T_VSTRIPE2', [[1 if (x // 2) % 2 == 0 else 0 for x in range(8)] for _ in range(8)], at=0x17)
    put('T_CHECK1', [[1 if (x + y) % 2 == 0 else 0 for x in range(8)] for y in range(8)], at=0x18)
    put('T_HSTRIPE1', [[1 if y % 2 == 0 else 0 for x in range(8)] for y in range(8)], at=0x19)
    # 0x1A-0x1F box drawing (colour 2 single line, 1 px from the cell edge to look centred)
    def boxtile(top, bottom, left, right):
        t = blank()
        for x in range(8):
            if top:
                t[3][x] = 2
            if bottom:
                t[4][x] = 2
        for y in range(8):
            if left:
                t[y][3] = 2
            if right:
                t[y][4] = 2
        return t
    def corner(kind):
        t = blank()
        xs = range(3, 8) if kind in ('tl', 'bl') else range(0, 4)
        ys = range(3, 8) if kind in ('tl', 'tr') else range(0, 4)
        for x in xs:
            t[3][x] = 2
        for y in ys:
            t[y][3] = 2
        return t
    put('T_BOX_H', boxtile(True, False, False, False), at=0x1A)
    put('T_BOX_V', boxtile(False, False, True, False), at=0x1B)
    put('T_BOX_TL', corner('tl'), at=0x1C)
    put('T_BOX_TR', corner('tr'), at=0x1D)
    put('T_BOX_BL', corner('bl'), at=0x1E)
    put('T_BOX_BR', corner('br'), at=0x1F)
    # glyph 0x7F: menu cursor (background version, colour 3)
    put('T_ARROW', from_art(['........', '.##.....', '.###....', '.####...', '.####...', '.###....',
                             '.##.....', '........'], {'.': 0, '#': 3}), at=0x7F)

    # ---------------------------------------------------------------- big letters, 0x80-0xB3
    put.next_bg = 0x80
    for ch in 'REPLAYNSTCIDG':
        tl, tr, bl, br = big_letter(font[ch])
        put('T_BIG_%s' % ch, tl)
        put('T_BIG_%s_TR' % ch, tr)
        put('T_BIG_%s_BL' % ch, bl)
        put('T_BIG_%s_BR' % ch, br)

    # ---------------------------------------------------------------- controller, from 0xB4
    put.next_bg = 0xB4
    arm_up = ['..####..', '.######.', '########', '########', '########', '########', '########', '########']
    def dpad_arm(direction, c):
        t = from_art(arm_up, {'.': 0, '#': c})
        for x, y in [(3, 2), (4, 2), (2, 3), (3, 3), (4, 3), (5, 3)]:  # arrow head, pointing out
            t[y][x] = 1 if c == 2 else 2
        if direction == 'down':
            t = t[::-1]
        elif direction == 'left':
            t = [list(r) for r in zip(*t)]
        elif direction == 'right':
            t = [list(r)[::-1] for r in zip(*t)]
        return t
    for d in ('up', 'down', 'left', 'right'):
        put('T_DPAD_%s' % d.upper(), dpad_arm(d, 2))
        put('T_DPAD_%s_ON' % d.upper(), dpad_arm(d, 3))
    put('T_DPAD_C', solid(2))
    put('T_DPAD_C_ON', solid(2))
    pill_l = ['........', '........', '..######', '.#######', '.#######', '..######', '........', '........']
    put('T_PILL_L', from_art(pill_l, {'.': 0, '#': 2}))
    put('T_PILL_R', [row[::-1] for row in from_art(pill_l, {'.': 0, '#': 2})])
    put('T_PILL_L_ON', from_art(pill_l, {'.': 0, '#': 3}))
    put('T_PILL_R_ON', [row[::-1] for row in from_art(pill_l, {'.': 0, '#': 3})])
    for i, t in enumerate(circle_cell(6.6, 2)):
        put('T_ROUND%d' % i, t)
    for i, t in enumerate(circle_cell(6.6, 3)):
        put('T_ROUND%d_ON' % i, t)
    # controller body frame (colour 1, rounded corners)
    def body(kind):
        t = blank()
        if kind == 'h_top':
            t[2] = [1] * 8
        if kind == 'h_bot':
            t[5] = [1] * 8
        if kind == 'v_l':
            for y in range(8): t[y][2] = 1
        if kind == 'v_r':
            for y in range(8): t[y][5] = 1
        if kind == 'tl':
            t = from_art(['........', '........', '....####', '...#....', '..#.....', '..#.....', '..#.....', '..#.....'], {'.': 0, '#': 1})
        if kind == 'tr':
            t = [row[::-1] for row in body('tl')]
        if kind == 'bl':
            t = body('tl')[::-1]
        if kind == 'br':
            t = [row[::-1] for row in body('bl')]
        return t
    for k in ('tl', 'h_top', 'tr', 'v_l', 'v_r', 'bl', 'h_bot', 'br'):
        put('T_PAD_%s' % k.upper(), body(k))
    if put.next_bg > 0xE0:
        raise ValueError('controller tiles overflow')

    # ---------------------------------------------------------------- scroll world, from 0xE0
    put.next_bg = 0xE0
    for ch in 'ABCDEFGH12345':
        put('T_W_%s' % ch, glyph(font[ch], ink=1, paper=2))
    for ch in 'ABCDEFGH12345':
        put('T_W2_%s' % ch, glyph(font[ch], ink=1, paper=3))
    # world frame line for the split-scroll HUD: colour 1 on colour 3
    if put.next_bg > 0x100:
        raise ValueError('world tiles overflow')

    # ---------------------------------------------------------------- sprites
    put('S_BLANK', blank(), table=spr, at=0x00)
    t = blank()
    t[0][0] = 1
    put('S_DOT', t, table=spr, at=0x01)           # sprite-0 sync pixel (top-left)
    ball = from_art(['..1111..', '.111111.', '11311112', '11111112', '11111112', '11111122',
                     '.111122.', '..2222..'], {'.': 0, '1': 1, '2': 2, '3': 3})
    put('S_BALL', ball, table=spr, at=0x02)
    put('S_CROSS', from_art(['...1....', '...1....', '...1....', '111.111.', '...1....', '...1....',
                             '...1....', '........'], {'.': 0, '1': 1}), table=spr, at=0x03)
    put('S_ARROW', from_art(['........', '.11.....', '.111....', '.1111...', '.1111...', '.111....',
                             '.11.....', '........'], {'.': 0, '1': 1}), table=spr, at=0x04)
    put('S_SOLID', solid(1), table=spr, at=0x05)
    put('S_BOX', solid(3), table=spr, at=0x06)

    data = b''.join(encode(t) for t in bg) + b''.join(encode(t) for t in spr)
    assert len(data) == 8192
    return data, syms


if __name__ == '__main__':
    d, s = build()
    for k in sorted(s, key=lambda k: s[k]):
        print('%-20s $%02X' % (k, s[k]))
