"""Generates the per plate type 3D bed textures (SVG, nanosvg friendly: paths, gradients, opacity and clip-path only, no
filters, masks, patterns or text) for the printers listed in PRINTERS, next to the vendor profiles.

    python tools/zoukei/gen_bed_textures.py            # writes resources/profiles/<vendor>/*_bed_*.svg
    python tools/zoukei/gen_bed_textures.py --out DIR  # writes into DIR instead (for review renders)

Every printer keeps its own logo / marks: the existing *_texture.svg of the model is embedded on top of the plate
(recoloured for the light plates). Output is deterministic (fixed seeds)."""
import argparse
import math
import os
import random
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PROFILES = os.path.join(ROOT, 'resources', 'profiles')

TEAL = '#00a99d'  # Orca accent, only used for the corner ticks

# vendor dir, file prefix, bed size (mm), source logo svg (relative to the vendor dir), clip for the label, light recolors
PRINTERS = [
    dict(vendor='Creality', prefix='creality_k2plus', size=350, logo='creality_k2plus_buildplate_texture.svg'),
    dict(vendor='BBL', prefix='bbl_a1', size=256, logo='bbl-3dp-logo.svg', tick_left=False),
    dict(vendor='BBL', prefix='bbl_a1mini', size=180, logo='bbl-3dp-logo.svg', tick_left=False),
    dict(vendor='Flashforge', prefix='flashforge_ad5x', size=220, logo='flashforge_ad5x_buildplate_texture.svg',
         # the original carries a "Smooth PEI Plate" label (outlines); only the smooth plate keeps it
         clip_label=(110, 900), keep_label_on='smooth_pei', tick_left=False),
    dict(vendor='Snapmaker', prefix='snapmaker_u1', size=270, logo='Snapmaker U1_texture.svg'),
]

KINDS = ['smooth_pei', 'textured_pei', 'holographic', 'cool', 'engineering']
LIGHT = {'cool', 'engineering'}


def f(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return s if s not in ('', '-0') else '0'


def f1(v):
    """One decimal, no leading zero (compact path data)."""
    s = '%.1f' % v
    if s.endswith('.0'):
        s = s[:-2]
    if s.startswith('0.'):
        s = s[1:]
    elif s.startswith('-0.'):
        s = '-' + s[2:]
    return s or '0'


def stipple(n, density, r, seed, margin):
    """Evenly jittered diamonds of radius r, one path (compact relative data)."""
    rnd = random.Random(seed)
    step = 1.0 / math.sqrt(density)
    rr = f1(r)
    dia = 'l%s %s-%s %s-%s-%sz' % (rr, rr, rr, rr, rr, rr)
    out = []
    y = margin
    while y < n - margin:
        x = margin
        while x < n - margin:
            px = x + rnd.uniform(0, step)
            py = y + rnd.uniform(0, step)
            if margin < px < n - margin and margin < py < n - margin:
                out.append('M%s %s%s' % (f1(px), f1(py - r), dia))
            x += step
        y += step
    return ''.join(out)


def hatch(n, spacing, margin, diagonal_sign):
    """Parallel 45 degree lines clipped to the square [margin, n - margin]."""
    out = []
    m, hi = margin, n - margin
    k = -n
    while k < n:
        # line: y = diagonal_sign * x + k  (diagonal_sign = +1 or -1)
        pts = []
        for x in (m, hi):
            y = diagonal_sign * x + k
            pts.append((x, y))
        # clip y to [m, hi]
        (x0, y0), (x1, y1) = pts
        if diagonal_sign == 1:
            lo_x = max(m, m - k)
            hi_x = min(hi, hi - k)
        else:
            lo_x = max(m, k - hi)
            hi_x = min(hi, k - m)
        if hi_x - lo_x > 0.5:
            out.append('M%s %sL%s %s' % (f(lo_x), f(diagonal_sign * lo_x + k), f(hi_x), f(diagonal_sign * hi_x + k)))
        k += spacing
    return ''.join(out)


def corner_ticks(n, inset, length, left=True):
    a, b = inset, n - inset
    parts = []
    for cx, cy, sx, sy in (((a, a, 1, 1),) if left else ()) + ((b, a, -1, 1),):  # top corners only, the bottom edge carries the logo
        parts.append('M%s %sh%sM%s %sv%s' % (f(cx), f(cy), f(sx * length), f(cx), f(cy), f(sy * length)))
    return ''.join(parts)


def sparkle(cx, cy, s):
    k = s * 0.16
    pts = [(0, -s), (k, -k), (s, 0), (k, k), (0, s), (-k, k), (-s, 0), (-k, -k)]
    return 'M' + 'L'.join('%s %s' % (f(cx + px), f(cy + py)) for px, py in pts) + 'Z'


def frame(n, dark):
    """Plate edge: outer rim and a thin inner line."""
    rim = '#16171a' if dark else '#8b929a'
    line = '#ffffff' if dark else '#ffffff'
    return ('<rect x="0.6" y="0.6" width="%s" height="%s" rx="2.2" fill="none" stroke="%s" stroke-width="1.2" stroke-opacity="0.85"/>'
            '<rect x="2.6" y="2.6" width="%s" height="%s" rx="1.4" fill="none" stroke="%s" stroke-width="0.35" stroke-opacity="0.16"/>'
            % (f(n - 1.2), f(n - 1.2), rim, f(n - 5.2), f(n - 5.2), line))


def plate_smooth_pei(n):
    d = []
    d.append('<defs>'
             '<linearGradient id="b" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="%s" y2="%s">'
             '<stop offset="0" stop-color="#34373d"/><stop offset="0.5" stop-color="#2a2c31"/><stop offset="1" stop-color="#3b3e45"/></linearGradient>'
             '<radialGradient id="g" gradientUnits="userSpaceOnUse" cx="%s" cy="%s" r="%s">'
             '<stop offset="0" stop-color="#ffffff" stop-opacity="0.10"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/></radialGradient>'
             '</defs>' % (f(n), f(n), f(n * 0.36), f(n * 0.34), f(n * 0.62)))
    d.append('<rect width="%s" height="%s" rx="2.4" fill="url(#b)"/>' % (f(n), f(n)))
    d.append('<rect width="%s" height="%s" rx="2.4" fill="url(#g)"/>' % (f(n), f(n)))
    # glossy streaks: two soft diagonal bands
    for off, w, op in ((0.18, 0.10, 0.055), (0.52, 0.045, 0.05), (0.78, 0.13, 0.04)):
        a, b = n * off, n * (off + w)
        d.append('<path d="M%s 0L%s 0L0 %sL0 %sZ" fill="#ffffff" fill-opacity="%s"/>' % (f(a), f(b), f(b), f(a), op))
    # (the bands above are anchored to the top left corner, mirrored ones below add a second sheen)
    d.append('<path d="M%s %sL%s %sL%s %sL%s %sZ" fill="#ffffff" fill-opacity="0.035"/>' %
             (f(n), f(n * 0.40), f(n), f(n * 0.58), f(n * 0.58), f(n), f(n * 0.40), f(n)))
    d.append(frame(n, True))
    return ''.join(d)


def plate_textured_pei(n):
    margin = 3.4
    dens = 0.026
    d = []
    d.append('<defs><linearGradient id="b" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="%s" y2="%s">'
             '<stop offset="0" stop-color="#34302c"/><stop offset="1" stop-color="#2a2724"/></linearGradient></defs>' % (f(n), f(n)))
    d.append('<rect width="%s" height="%s" rx="2.4" fill="url(#b)"/>' % (f(n), f(n)))
    d.append('<path d="%s" fill="#a39687" fill-opacity="0.34"/>' % stipple(n, dens, 0.6, 11, margin))
    d.append('<path d="%s" fill="#d6c9b8" fill-opacity="0.26"/>' % stipple(n, dens * 0.7, 0.5, 12, margin))
    d.append('<path d="%s" fill="#000000" fill-opacity="0.45"/>' % stipple(n, dens * 0.8, 0.5, 13, margin))
    d.append(frame(n, True))
    return ''.join(d)


def plate_holographic(n):
    margin = 3.0
    rnd = random.Random(7)
    stops = ''.join('<stop offset="%s" stop-color="%s"/>' % (f(o), c) for o, c in
                    ((0, '#ff4d6d'), (0.17, '#ffb347'), (0.33, '#f4ef5a'), (0.5, '#4dffa0'), (0.66, '#46d7ff'),
                     (0.83, '#7a6bff'), (1, '#ff4dd2')))
    d = []
    d.append('<defs>'
             '<clipPath id="c"><rect width="%s" height="%s" rx="2.4"/></clipPath>'
             '<linearGradient id="r" gradientUnits="userSpaceOnUse" x1="0" y1="%s" x2="%s" y2="%s">%s</linearGradient>'
             '<linearGradient id="r2" gradientUnits="userSpaceOnUse" x1="%s" y1="0" x2="0" y2="%s">%s</linearGradient>'
             '<radialGradient id="g" gradientUnits="userSpaceOnUse" cx="%s" cy="%s" r="%s">'
             '<stop offset="0" stop-color="#ffffff" stop-opacity="0.30"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/></radialGradient>'
             '</defs>' % (f(n), f(n), f(n * 0.15), f(n), f(n * 0.85), stops, f(n), f(n), stops,
                          f(n * 0.38), f(n * 0.32), f(n * 0.55)))
    d.append('<g clip-path="url(#c)">')
    d.append('<rect width="%s" height="%s" fill="#171321"/>' % (f(n), f(n)))
    d.append('<rect width="%s" height="%s" fill="url(#r)" fill-opacity="0.50"/>' % (f(n), f(n)))
    # diffraction bands: dark diagonal strips of changing width let the spectrum show through in thin bands
    pos = -n * 0.1
    while pos < n * 1.25:
        w = rnd.uniform(n * 0.008, n * 0.03)
        d.append('<path d="M%s 0L%s 0L%s %sL%s %sZ" fill="#0c0a14" fill-opacity="%s"/>' %
                 (f(pos), f(pos + w), f(pos + w - n * 0.55), f(n), f(pos - n * 0.55), f(n), f(rnd.uniform(0.30, 0.55))))
        pos += w + rnd.uniform(n * 0.012, n * 0.04)
    # crossing bands the other way, fainter: together they read as a diffraction lattice
    pos = -n * 0.3
    while pos < n * 1.1:
        w = rnd.uniform(n * 0.004, n * 0.014)
        d.append('<path d="M%s 0L%s 0L%s %sL%s %sZ" fill="#0c0a14" fill-opacity="%s"/>' %
                 (f(pos), f(pos + w), f(pos - n * 0.8 + w), f(n), f(pos - n * 0.8), f(n), f(rnd.uniform(0.12, 0.28))))
        pos += w + rnd.uniform(n * 0.02, n * 0.06)
    # second, perpendicular spectrum at low strength for the cross sheen
    d.append('<rect width="%s" height="%s" fill="url(#r2)" fill-opacity="0.16"/>' % (f(n), f(n)))
    # fine diffraction grating
    d.append('<path d="%s" fill="none" stroke="#ffffff" stroke-opacity="0.075" stroke-width="0.28"/>' % hatch(n, 1.35, 0, 1))
    d.append('<path d="%s" fill="none" stroke="#ffffff" stroke-opacity="0.035" stroke-width="0.25"/>' % hatch(n, 2.7, 0, -1))
    d.append('<rect width="%s" height="%s" fill="url(#g)"/>' % (f(n), f(n)))
    # sparkles
    big, small = [], []
    count = int(n * n / 900)
    for i in range(count):
        x = rnd.uniform(margin + 3, n - margin - 3)
        y = rnd.uniform(margin + 3, n - margin - 3)
        (big if i % 5 == 0 else small).append(sparkle(x, y, rnd.uniform(1.6, 3.0) if i % 5 == 0 else rnd.uniform(0.7, 1.4)))
    d.append('<path d="%s" fill="#ffffff" fill-opacity="0.78"/>' % ''.join(big))
    d.append('<path d="%s" fill="#ffffff" fill-opacity="0.5"/>' % ''.join(small))
    d.append('</g>')
    d.append(frame(n, True))
    return ''.join(d)


def plate_cool(n):
    d = []
    d.append('<defs>'
             '<linearGradient id="b" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="%s" y2="%s">'
             '<stop offset="0" stop-color="#d4dde6"/><stop offset="0.55" stop-color="#bfcbd6"/><stop offset="1" stop-color="#cdd7e0"/></linearGradient>'
             '<radialGradient id="g" gradientUnits="userSpaceOnUse" cx="%s" cy="%s" r="%s">'
             '<stop offset="0" stop-color="#ffffff" stop-opacity="0.55"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/></radialGradient>'
             '</defs>' % (f(n), f(n), f(n * 0.32), f(n * 0.3), f(n * 0.6)))
    d.append('<rect width="%s" height="%s" rx="2.4" fill="url(#b)"/>' % (f(n), f(n)))
    d.append('<rect width="%s" height="%s" rx="2.4" fill="url(#g)"/>' % (f(n), f(n)))
    for off, w, op in ((0.22, 0.07, 0.35), (0.62, 0.03, 0.3)):
        a, b = n * off, n * (off + w)
        d.append('<path d="M%s 0L%s 0L0 %sL0 %sZ" fill="#ffffff" fill-opacity="%s"/>' % (f(a), f(b), f(b), f(a), op))
    d.append(frame(n, False))
    return ''.join(d)


def plate_engineering(n):
    margin = 3.0
    d = []
    d.append('<defs><linearGradient id="b" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="%s" y2="%s">'
             '<stop offset="0" stop-color="#d9d4c9"/><stop offset="1" stop-color="#c7c1b4"/></linearGradient></defs>' % (f(n), f(n)))
    d.append('<rect width="%s" height="%s" rx="2.4" fill="url(#b)"/>' % (f(n), f(n)))
    d.append('<path d="%s" fill="none" stroke="#5a554b" stroke-opacity="0.20" stroke-width="0.3"/>' % hatch(n, 1.6, margin, 1))
    d.append('<path d="%s" fill="none" stroke="#5a554b" stroke-opacity="0.20" stroke-width="0.3"/>' % hatch(n, 1.6, margin, -1))
    d.append('<path d="%s" fill="#ffffff" fill-opacity="0.22"/>' % stipple(n, 0.012, 0.4, 21, margin))
    d.append(frame(n, False))
    return ''.join(d)


PLATES = dict(smooth_pei=plate_smooth_pei, textured_pei=plate_textured_pei, holographic=plate_holographic,
              cool=plate_cool, engineering=plate_engineering)


def load_logo(p):
    path = os.path.join(PROFILES, p['vendor'], p['logo'])
    s = open(path, encoding='utf-8').read()
    m = re.search(r'<svg[^>]*viewBox="\s*([-\d.]+)[ ,]+([-\d.]+)[ ,]+([-\d.]+)[ ,]+([-\d.]+)\s*"[^>]*>', s)
    vw, vh = float(m.group(3)), float(m.group(4))
    body = s[m.end():s.rfind('</svg>')]
    body = re.sub(r'<\?xml[^>]*\?>', '', body)
    return body, vw, vh


def recolor(body, kind):
    if kind in LIGHT:
        body = re.sub(r'#bcbec0', '#6a7078', body, flags=re.I)
        body = re.sub(r'#cfd2d3', '#78808a', body, flags=re.I)
        body = re.sub(r'#393939', '#eef0f2', body, flags=re.I)
    elif kind == 'holographic':
        body = re.sub(r'#bcbec0', '#f1f2f7', body, flags=re.I)
        body = re.sub(r'#cfd2d3', '#f1f2f7', body, flags=re.I)
        body = re.sub(r'#393939', '#1b1830', body, flags=re.I)
    return body


def build(p, kind):
    n = p['size']
    body, vw, vh = load_logo(p)
    body = recolor(body, kind)
    sx, sy = n / vw, n / vh
    parts = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s">' % (f(n), f(n))]
    parts.append(PLATES[kind](n))
    logo = '<g transform="scale(%s %s)">%s</g>' % (('%.6f' % sx).rstrip('0'), ('%.6f' % sy).rstrip('0'), body)
    if 'clip_label' in p and kind != p.get('keep_label_on'):
        # clip in the logo's own coordinates: everything except the label block in the top left corner
        x0, y0 = p['clip_label']
        logo = ('<g transform="scale(%s %s)">' % (('%.6f' % sx).rstrip('0'), ('%.6f' % sy).rstrip('0'))
                + '<defs><clipPath id="nl"><path d="M%d 0H%s V%s H0 V%d H%d Z"/></clipPath></defs>' % (x0, f(vw), f(vh), y0, x0)
                + '<g clip-path="url(#nl)">%s</g></g>' % body)
    parts.append(logo)
    ink = '#ffffff'
    parts.append('<path d="%s" fill="none" stroke="%s" stroke-opacity="0.8" stroke-width="0.9" stroke-linecap="round"/>' %
                 (corner_ticks(n, 7.5 if n > 200 else 6, 5 if n > 200 else 4, p.get('tick_left', True)), TEAL))
    parts.append('</svg>')
    return '\n'.join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    for p in PRINTERS:
        out_dir = a.out or os.path.join(PROFILES, p['vendor'])
        os.makedirs(out_dir, exist_ok=True)
        for kind in KINDS:
            svg = build(p, kind)
            name = '%s_bed_%s.svg' % (p['prefix'], kind)
            with open(os.path.join(out_dir, name), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(svg)
            print('%-44s %7d bytes' % (name, len(svg)))


if __name__ == '__main__':
    main()
