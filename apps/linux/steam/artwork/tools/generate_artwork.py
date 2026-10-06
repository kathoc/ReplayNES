#!/usr/bin/env python3
"""Generates the Steam library artwork SVG sources (apps/linux/steam/artwork/src/*.svg).

    python3 apps/linux/steam/artwork/tools/generate_artwork.py --font /path/to/NotoSans-Black.ttf

The wordmark "ReplayNES" is converted to outlines from Noto Sans Black (SIL OFL 1.1; on SteamOS
/usr/share/fonts/noto/NotoSans-Black.ttf), so the SVGs need no font at render time. The emblem is
apps/macos/Resources/IconSource/replaynes.svg, used as the app icon does: full bleed on a white
rounded square (appicon-1024.svg). Everything else is plain vector shapes. Standard library only
(a minimal TrueType 'glyf' reader). Render the PNGs with render.sh (rsvg-convert).
"""
import argparse
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, '..', 'src'))

# ---------------------------------------------------------------- palette
RED = '#ff0000'          # emblem red (replaynes.svg)
INK = '#040000'          # emblem black
WHITE = '#f5f5f3'        # wordmark / tile
BG_TOP = '#202228'
BG_BOT = '#121317'
LINE = '#3a3d46'         # timeline track

# ---------------------------------------------------------------- emblem (replaynes.svg, 400x400)
EMBLEM_SVG = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..', 'macos', 'Resources', 'IconSource', 'replaynes.svg'))


def load_emblem():
    """The shapes of replaynes.svg with its two classes resolved to fills."""
    out = []
    for line in open(EMBLEM_SVG):
        line = line.strip()
        if line.startswith(('<circle', '<polygon', '<path')):
            out.append(line.replace('class="cls-1"', f'fill="{INK}"').replace('class="cls-2"', f'fill="{RED}"'))
    assert len(out) == 6, 'replaynes.svg changed shape'
    return '\n'.join(out)


EMBLEM = load_emblem()


def tile(x, y, size, uid, shadow=True):
    """The app icon tile: white rounded square, emblem full bleed, clipped (appicon-1024.svg)."""
    r = size * 185.0 / 824.0
    s = size / 400.0
    out = []
    if shadow:
        out.append(f'<rect x="{x:.2f}" y="{y + size * 0.03:.2f}" width="{size:.2f}" height="{size:.2f}" '
                   f'rx="{r:.2f}" fill="#000" opacity="0.35" filter="url(#{uid}-blur)"/>')
    out.append(f'<clipPath id="{uid}-clip"><rect x="{x:.2f}" y="{y:.2f}" width="{size:.2f}" height="{size:.2f}" rx="{r:.2f}"/></clipPath>')
    out.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{size:.2f}" height="{size:.2f}" rx="{r:.2f}" fill="#ffffff"/>')
    out.append(f'<g clip-path="url(#{uid}-clip)"><g transform="translate({x:.3f},{y:.3f}) scale({s:.6f})">{EMBLEM}</g></g>')
    return '\n'.join(out)


def blur_def(uid, std):
    return (f'<filter id="{uid}-blur" x="-20%" y="-20%" width="140%" height="140%">'
            f'<feGaussianBlur stdDeviation="{std:.1f}"/></filter>')


# ---------------------------------------------------------------- minimal TrueType reader
class Font:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        n = struct.unpack('>H', self.d[4:6])[0]
        self.t = {}
        for i in range(n):
            tag, _, off, ln = struct.unpack('>4sIII', self.d[12 + 16 * i:28 + 16 * i])
            self.t[tag.decode()] = (off, ln)
        h = self.t['head'][0]
        self.upem = struct.unpack('>H', self.d[h + 18:h + 20])[0]
        self.loc_long = struct.unpack('>h', self.d[h + 50:h + 52])[0] == 1
        self.nglyphs = struct.unpack('>H', self.d[self.t['maxp'][0] + 4:self.t['maxp'][0] + 6])[0]
        hh = self.t['hhea'][0]
        self.nhm = struct.unpack('>H', self.d[hh + 34:hh + 36])[0]
        self.cmap = self._cmap()

    def _cmap(self):
        c = self.t['cmap'][0]
        n = struct.unpack('>H', self.d[c + 2:c + 4])[0]
        for i in range(n):
            pid, eid, off = struct.unpack('>HHI', self.d[c + 4 + 8 * i:c + 12 + 8 * i])
            st = c + off
            fmt = struct.unpack('>H', self.d[st:st + 2])[0]
            if pid == 3 and eid == 1 and fmt == 4:
                segx2 = struct.unpack('>H', self.d[st + 6:st + 8])[0]
                seg = segx2 // 2
                ends = struct.unpack(f'>{seg}H', self.d[st + 14:st + 14 + segx2])
                starts = struct.unpack(f'>{seg}H', self.d[st + 16 + segx2:st + 16 + 2 * segx2])
                deltas = struct.unpack(f'>{seg}h', self.d[st + 16 + 2 * segx2:st + 16 + 3 * segx2])
                ro_off = st + 16 + 3 * segx2
                ros = struct.unpack(f'>{seg}H', self.d[ro_off:ro_off + segx2])
                m = {}
                for k in range(seg):
                    for ch in range(starts[k], ends[k] + 1):
                        if ch == 0xFFFF:
                            continue
                        if ros[k] == 0:
                            g = (ch + deltas[k]) & 0xFFFF
                        else:
                            a = ro_off + 2 * k + ros[k] + 2 * (ch - starts[k])
                            g = struct.unpack('>H', self.d[a:a + 2])[0]
                            if g:
                                g = (g + deltas[k]) & 0xFFFF
                        m[ch] = g
                return m
        raise SystemExit('no (3,1,4) cmap')

    def advance(self, g):
        h = self.t['hmtx'][0]
        i = min(g, self.nhm - 1)
        return struct.unpack('>H', self.d[h + 4 * i:h + 4 * i + 2])[0]

    def _loca(self, g):
        l = self.t['loca'][0]
        if self.loc_long:
            a, b = struct.unpack('>II', self.d[l + 4 * g:l + 4 * g + 8])
        else:
            a, b = struct.unpack('>HH', self.d[l + 2 * g:l + 2 * g + 4])
            a, b = a * 2, b * 2
        return a, b

    def contours(self, g):
        """List of contours; each a list of (x, y, on_curve)."""
        a, b = self._loca(g)
        if a == b:
            return []
        p = self.t['glyf'][0] + a
        nc = struct.unpack('>h', self.d[p:p + 2])[0]
        if nc < 0:
            return self._composite(p)
        ends = struct.unpack(f'>{nc}H', self.d[p + 10:p + 10 + 2 * nc])
        npts = ends[-1] + 1
        q = p + 10 + 2 * nc
        il = struct.unpack('>H', self.d[q:q + 2])[0]
        q += 2 + il
        flags = []
        while len(flags) < npts:
            f = self.d[q]
            q += 1
            flags.append(f)
            if f & 8:
                r = self.d[q]
                q += 1
                flags.extend([f] * r)
        xs, ys = [], []
        for arr, short, same in ((xs, 2, 16), (ys, 4, 32)):
            v = 0
            for f in flags:
                if f & short:
                    dv = self.d[q]
                    q += 1
                    v += dv if f & same else -dv
                elif not f & same:
                    v += struct.unpack('>h', self.d[q:q + 2])[0]
                    q += 2
                arr.append(v)
        out, s = [], 0
        for e in ends:
            out.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)])
            s = e + 1
        return out

    def _composite(self, p):
        q = p + 10
        out = []
        while True:
            fl, gi = struct.unpack('>HH', self.d[q:q + 4])
            q += 4
            if fl & 1:
                dx, dy = struct.unpack('>hh', self.d[q:q + 4])
                q += 4
            else:
                dx, dy = struct.unpack('>bb', self.d[q:q + 2])
                q += 2
            if fl & 8:
                q += 2
            elif fl & 0x40:
                q += 4
            elif fl & 0x80:
                q += 8
            for c in self.contours(gi):
                out.append([(x + dx, y + dy, on) for x, y, on in c])
            if not fl & 0x20:
                return out


def contour_path(c, ox, scale, base):
    """Quadratic TrueType contour -> SVG path (y flipped, scaled, translated)."""
    def P(pt):
        return (ox + pt[0] * scale, base - pt[1] * scale)
    n = len(c)
    # start on an on-curve point (or the midpoint of two off-curve points)
    start = next((i for i in range(n) if c[i][2]), None)
    if start is None:
        a, b = c[0], c[1]
        c = [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, True)] + c[1:] + [c[0]]
        start = 0
    pts = c[start:] + c[:start]
    d = ['M%.2f %.2f' % P(pts[0])]
    i = 1
    prev_off = None
    for pt in pts[1:] + [pts[0]]:
        if pt[2]:
            if prev_off is None:
                d.append('L%.2f %.2f' % P(pt))
            else:
                d.append('Q%.2f %.2f %.2f %.2f' % (P(prev_off) + P(pt)))
                prev_off = None
        else:
            if prev_off is not None:
                mid = ((prev_off[0] + pt[0]) / 2, (prev_off[1] + pt[1]) / 2, True)
                d.append('Q%.2f %.2f %.2f %.2f' % (P(prev_off) + P(mid)))
            prev_off = pt
        i += 1
    if prev_off is not None:
        d.append('Q%.2f %.2f %.2f %.2f' % (P(prev_off) + P(pts[0])))
    d.append('Z')
    return ''.join(d)


class Wordmark:
    """'Replay' + 'NES' outlines. Width/cap height known so layouts can fit them."""
    TEXT = 'ReplayNES'
    # Optical kerning in font units (Noto's GPOS kerning is not read): tighten the black weight.
    TRACK = -18
    KERN = {('R', 'e'): -10, ('a', 'y'): -14, ('y', 'N'): 6, ('l', 'a'): 0}

    def __init__(self, font):
        self.f = font
        self.glyphs = []
        x = 0
        prev = None
        for ch in self.TEXT:
            g = font.cmap[ord(ch)]
            if prev:
                x += self.KERN.get((prev, ch), 0) + self.TRACK
            self.glyphs.append((ch, g, x))
            x += font.advance(g)
            prev = ch
        # tight horizontal bounds from outlines
        xs = []
        ys = []
        for ch, g, gx in self.glyphs:
            for c in font.contours(g):
                for px, py, _ in c:
                    xs.append(px + gx)
                    ys.append(py)
        self.xmin, self.xmax = min(xs), max(xs)
        self.ymin, self.ymax = min(ys), max(ys)   # descender of 'p'/'y' .. ascender of 'l'
        capg = font.cmap[ord('N')]
        self.cap = max(p[1] for c in font.contours(capg) for p in c)

    def width(self, scale):
        return (self.xmax - self.xmin) * scale

    def svg(self, x, baseline, width, red_nes=True, white=WHITE):
        s = width / (self.xmax - self.xmin)
        a, b = [], []
        for ch, g, gx in self.glyphs:
            ox = x + (gx - self.xmin) * s
            d = ''.join(contour_path(c, ox, s, baseline) for c in self.f.contours(g))
            (b if (red_nes and ch in 'NES') else a).append(d)
        out = f'<path fill="{white}" d="{"".join(a)}"/>'
        if b:
            out += f'\n<path fill="{RED}" d="{"".join(b)}"/>'
        return out, s


# ---------------------------------------------------------------- motifs
def scanlines(w, h, uid, period=4, opacity=0.06):
    """CRT scanlines: a dark 1 px line every `period` px (averages out when Steam scales down)."""
    return (f'<pattern id="{uid}-scan" width="{period}" height="{period}" patternUnits="userSpaceOnUse">'
            f'<rect width="{period}" height="1" fill="#000" opacity="{opacity}"/></pattern>'), \
           f'<rect width="{w}" height="{h}" fill="url(#{uid}-scan)"/>'


def grain(w, h, uid, opacity=0.05):
    """Fixed-seed fine noise: dithers the dark gradients (no 8-bit banding)."""
    return (f'<filter id="{uid}-grain" x="0" y="0" width="100%" height="100%">'
            '<feTurbulence type="fractalNoise" baseFrequency="0.85" numOctaves="2" seed="7" result="n"/>'
            '<feColorMatrix type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 1.4 -0.7"/>'
            '</filter>'), \
           f'<rect width="{w}" height="{h}" filter="url(#{uid}-grain)" opacity="{opacity}"/>'


def rewind(cx, cy, h, fill, opacity=1.0):
    """Two left-pointing triangles (the rewind glyph), height h, centred on (cx, cy)."""
    w = h * 0.62
    x0 = cx - w
    p1 = f'{x0:.1f},{cy:.1f} {x0 + w:.1f},{cy - h / 2:.1f} {x0 + w:.1f},{cy + h / 2:.1f}'
    p2 = f'{x0 + w:.1f},{cy:.1f} {x0 + 2 * w:.1f},{cy - h / 2:.1f} {x0 + 2 * w:.1f},{cy + h / 2:.1f}'
    return (f'<g fill="{fill}" opacity="{opacity}" stroke="{fill}" stroke-linejoin="round" stroke-width="{h * 0.06:.1f}">'
            f'<polygon points="{p1}"/><polygon points="{p2}"/></g>')


def rings(cx, cy, radii, base=0.06, step=0.01, width=2):
    return '\n'.join(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="none" stroke="#ffffff" '
                     f'stroke-opacity="{max(base - i * step, 0.01):.3f}" stroke-width="{width}"/>'
                     for i, r in enumerate(radii))


def timeline(x0, x1, y, head, scale=1.0, ticks=True, track=LINE, icon=True):
    """Recorded timeline: a small rewind glyph, the track, the red played part up to `head`
    (0..1), frame ticks and the playhead."""
    out = []
    if icon:
        ih = 22 * scale
        out.append(rewind(x0 + ih * 0.62, y, ih, WHITE, 0.9))
        x0 += ih * 1.24 + 22 * scale
    t = 6 * scale
    hx = x0 + (x1 - x0) * head
    out += [f'<rect x="{x0:.1f}" y="{y - t / 2:.1f}" width="{x1 - x0:.1f}" height="{t:.1f}" rx="{t / 2:.1f}" fill="{track}"/>',
            f'<rect x="{x0:.1f}" y="{y - t / 2:.1f}" width="{hx - x0:.1f}" height="{t:.1f}" rx="{t / 2:.1f}" fill="{RED}"/>']
    if ticks:
        n = 24
        for i in range(n + 1):
            tx = x0 + (x1 - x0) * i / n
            th = (12 if i % 6 == 0 else 6) * scale
            out.append(f'<rect x="{tx - 1 * scale:.1f}" y="{y + t / 2 + 7 * scale:.1f}" width="{2 * scale:.1f}" '
                       f'height="{th:.1f}" fill="{track}"/>')
    r = 11 * scale
    out.append(f'<circle cx="{hx:.1f}" cy="{y:.1f}" r="{r + 5 * scale:.1f}" fill="{RED}" opacity="0.3"/>')
    out.append(f'<circle cx="{hx:.1f}" cy="{y:.1f}" r="{r:.1f}" fill="{WHITE}"/>')
    return '\n'.join(out)


def doc(w, h, defs, body):
    head = f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
    gen = '<!-- Generated by apps/linux/steam/artwork/tools/generate_artwork.py - edit the script, not this file. -->'
    return f'{head}\n{gen}\n<defs>\n{defs}\n</defs>\n{body}\n</svg>\n'


def bg_defs(uid, vertical=True):
    if vertical:
        return (f'<linearGradient id="{uid}-bg" x1="0" y1="0" x2="0" y2="1">'
                f'<stop offset="0" stop-color="{BG_TOP}"/><stop offset="1" stop-color="{BG_BOT}"/></linearGradient>')
    return (f'<linearGradient id="{uid}-bg" x1="0" y1="0" x2="1" y2="0">'
            f'<stop offset="0" stop-color="{BG_BOT}"/><stop offset="1" stop-color="{BG_TOP}"/></linearGradient>')


def glow(uid, strength=0.22, color=RED):
    return (f'<radialGradient id="{uid}-glow"><stop offset="0" stop-color="{color}" stop-opacity="{strength}"/>'
            f'<stop offset="1" stop-color="{color}" stop-opacity="0"/></radialGradient>')


# ---------------------------------------------------------------- assets
def capsule(wm):
    W, H = 600, 900
    sd, sr = scanlines(W, H, 'cap')
    gd, gr = grain(W, H, 'cap')
    defs = '\n'.join([bg_defs('cap'), glow('cap', 0.26), blur_def('cap', 12), sd, gd])
    t = 300
    tx, ty = (W - t) / 2, 165
    cy = ty + t / 2
    _, s = wm.svg(70, 0, W - 140)
    base = ty + t + 92 + wm.cap * s
    word, s = wm.svg(70, base, W - 140)
    body = '\n'.join([
        f'<rect width="{W}" height="{H}" fill="url(#cap-bg)"/>',
        f'<circle cx="{W / 2}" cy="{cy}" r="340" fill="url(#cap-glow)"/>',
        rings(W / 2, cy, (205, 260, 315, 370), 0.07, 0.015),
        tile(tx, ty, t, 'cap'),
        word,
        timeline(70, W - 70, 742, 0.58, 1.0),
        sr, gr,
    ])
    return doc(W, H, defs, body)


def wide(wm):
    W, H = 920, 430
    sd, sr = scanlines(W, H, 'wide')
    gd, gr = grain(W, H, 'wide')
    defs = '\n'.join([bg_defs('wide', vertical=False), glow('wide', 0.26), blur_def('wide', 10), sd, gd])
    t = 230
    tx, ty = 78, (H - t) / 2
    x0 = tx + t + 58
    ww = W - x0 - 72
    _, s = wm.svg(x0, 0, ww)
    base = H / 2 + wm.cap * s / 2 - 18
    word, s = wm.svg(x0, base, ww)
    body = '\n'.join([
        f'<rect width="{W}" height="{H}" fill="url(#wide-bg)"/>',
        f'<circle cx="{tx + t / 2}" cy="{H / 2}" r="300" fill="url(#wide-glow)"/>',
        rings(tx + t / 2, H / 2, (160, 205, 250, 295), 0.07, 0.015),
        tile(tx, ty, t, 'wide'),
        word,
        timeline(x0, x0 + ww, base + 60, 0.58, 0.8),
        sr, gr,
    ])
    return doc(W, H, defs, body)


def hero():
    """Background only: Steam draws the logo over it (pinned bottom left by the <id>.json we
    write), so the left half stays calm; the motif sits right of centre."""
    W, H = 1920, 620
    sd, sr = scanlines(W, H, 'hero', period=4, opacity=0.08)
    gd, gr = grain(W, H, 'hero', 0.07)
    defs = '\n'.join([bg_defs('hero', vertical=False), sd, gd,
                      '<radialGradient id="hero-glow" cx="0.5" cy="0.5" r="0.5">'
                      f'<stop offset="0" stop-color="{RED}" stop-opacity="0.30"/>'
                      f'<stop offset="1" stop-color="{RED}" stop-opacity="0"/></radialGradient>',
                      '<linearGradient id="hero-fade" x1="0" y1="0" x2="1" y2="0">'
                      f'<stop offset="0" stop-color="{BG_BOT}" stop-opacity="1"/>'
                      f'<stop offset="0.42" stop-color="{BG_BOT}" stop-opacity="0"/></linearGradient>'])
    cx, cy = 1300, 280
    body = '\n'.join([
        f'<rect width="{W}" height="{H}" fill="url(#hero-bg)"/>',
        f'<ellipse cx="{cx}" cy="{cy}" rx="640" ry="430" fill="url(#hero-glow)"/>',
        rings(cx, cy, (160, 240, 320, 400, 480, 560), 0.06, 0.009),
        # motion trail behind the rewind glyph (it moves left, the trail fades to the right)
        rewind(cx + 300, cy, 210, '#ffffff', 0.08),
        rewind(cx + 500, cy, 140, '#ffffff', 0.045),
        rewind(cx, cy, 300, RED, 0.95),
        timeline(1000, 1800, 548, 0.42, 1.1, icon=False),
        f'<rect width="{W}" height="{H}" fill="url(#hero-fade)"/>',
        sr, gr,
    ])
    return doc(W, H, defs, body)


def logo(wm):
    """Library logo: tile + wordmark lockup on transparency, tightly cropped to 1280 px wide."""
    W = 1280
    t = 300
    gap = 64
    pad = 20
    ww = W - 2 * pad - t - gap
    _, s = wm.svg(0, 0, ww)
    H = t + 2 * pad + 20
    base = pad + t / 2 + wm.cap * s / 2
    word, s = wm.svg(pad + t + gap, base, ww)
    defs = blur_def('logo', 8)
    body = '\n'.join([tile(pad, pad, t, 'logo'), word])
    return doc(W, int(H), defs, body)


def icon():
    """Square icon (Steam _icon, hicolor app icon): the tile with a small margin."""
    W = 512
    m = 16
    return doc(W, W, '', tile(m, m, W - 2 * m, 'icon', shadow=False))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--font', required=True, help='NotoSans-Black.ttf')
    a = ap.parse_args()
    wm = Wordmark(Font(a.font))
    os.makedirs(SRC, exist_ok=True)
    for name, text in (('capsule.svg', capsule(wm)), ('wide.svg', wide(wm)), ('hero.svg', hero()),
                       ('logo.svg', logo(wm)), ('icon.svg', icon())):
        with open(os.path.join(SRC, name), 'w') as f:
            f.write(text)
        print('wrote', os.path.join(SRC, name))


if __name__ == '__main__':
    main()
