#!/usr/bin/env python3
"""Render the touch button artwork (original art, "scrap metal" frames) into one texture atlas.

Writes platform/touch_atlas.h (zlib-compressed RGBA, built into libmain.so) and design/touch_atlas.png
(for review). Needs cairosvg and Pillow (development only; the generated header is committed).

    .venv/bin/python tools/make_touch_atlas.py
"""
import io, os, zlib
import cairosvg
from PIL import Image, ImageFilter

CELL = 256          # pixels per button in the atlas
COLS = 4
FRAME_R = 0.94      # hexagon radius as a fraction of half a cell (the rest is room for the outline)

DEFS = '''
<linearGradient id="gM" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f6f8ff"/><stop offset=".45" stop-color="#aeb6c8"/><stop offset="1" stop-color="#555d70"/></linearGradient>
<linearGradient id="gO" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe08a"/><stop offset=".5" stop-color="#ff8a1e"/><stop offset="1" stop-color="#a84300"/></linearGradient>
<linearGradient id="gG" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#d4ffbd"/><stop offset=".5" stop-color="#46d45e"/><stop offset="1" stop-color="#0f6e26"/></linearGradient>
<linearGradient id="gR" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffc0b5"/><stop offset=".5" stop-color="#ff4a3a"/><stop offset="1" stop-color="#7d1006"/></linearGradient>
<linearGradient id="gY" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff6c2"/><stop offset=".5" stop-color="#ffcc2a"/><stop offset="1" stop-color="#9a5f00"/></linearGradient>
<linearGradient id="gK" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4a4f5e"/><stop offset="1" stop-color="#15171f"/></linearGradient>
<linearGradient id="steel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5d636e" stop-opacity=".85"/>
  <stop offset=".5" stop-color="#30343c" stop-opacity=".8"/><stop offset="1" stop-color="#16181d" stop-opacity=".85"/></linearGradient>
'''
K = 'stroke="#0b0d1a" stroke-width="4.5" stroke-linejoin="round"'

# 5x7 pixel letters, in the spirit of the HUD's pixel font (own design)
PIX = {
    'C': ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'],
    'R': ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'],
    'A': ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
    'P': ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'],
    'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
}

def pixtext(s, cx, top, h, fill):
    p = h / 7.0
    w = (len(s) * 6 - 1) * p
    x0 = cx - w / 2
    out = []
    for i, ch in enumerate(s):
        for r, row in enumerate(PIX[ch]):
            for c, v in enumerate(row):
                if v == '#':
                    out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"/>'
                               % (x0 + (i * 6 + c) * p, top + r * p, p + 0.05, p + 0.05, fill))
    return ''.join(out)

def tag(t):
    return ('<rect x="62" y="64" width="32" height="30" rx="5" fill="url(#gO)" %s/>' % K) + pixtext(t, 78, 70, 18, '#1a0b00')

# icon artwork in a 100x100 box (same as design/touch-concepts.html)
ICONS = [
    ('left', '<path d="M76 14 L16 50 L76 86 L64 50 Z" fill="url(#gM)" %s/><path d="M70 22 L26 50" stroke="#fff" stroke-opacity=".7" stroke-width="3" fill="none"/>' % K),
    ('right', '<path d="M24 14 L84 50 L24 86 L36 50 Z" fill="url(#gM)" %s/><path d="M30 22 L74 50" stroke="#fff" stroke-opacity=".7" stroke-width="3" fill="none"/>' % K),
    ('gas', '''<rect x="42" y="74" width="16" height="18" fill="url(#gM)" {K}/>
     <path d="M30 10 H70 Q76 10 76 16 V72 Q76 78 70 78 H30 Q24 78 24 72 V16 Q24 10 30 10 Z" fill="url(#gG)" {K}/>
     <path d="M32 22 H68 M32 34 H68 M32 46 H68 M32 58 H68" stroke="#0b3d14" stroke-width="4" stroke-linecap="round"/>
     <path d="M30 15 H52" stroke="#fff" stroke-opacity=".75" stroke-width="3" stroke-linecap="round"/>'''),
    ('brake', '''<rect x="42" y="70" width="16" height="22" fill="url(#gM)" {K}/>
     <path d="M20 26 H80 Q86 26 86 32 V66 Q86 72 80 72 H20 Q14 72 14 66 V32 Q14 26 20 26 Z" fill="url(#gR)" {K}/>
     <path d="M24 38 H76 M24 49 H76 M24 60 H76" stroke="#4a0802" stroke-width="4" stroke-linecap="round"/>
     <path d="M21 31 H50" stroke="#fff" stroke-opacity=".75" stroke-width="3" stroke-linecap="round"/>'''),
    ('hand', '''<circle cx="28" cy="78" r="12" fill="url(#gK)" {K}/>
     <path d="M24 72 L66 22 L78 32 L36 82 Z" fill="url(#gM)" {K}/>
     <path d="M52 39 L66 22 L78 32 L64 49 Z" fill="url(#gK)" {K}/>
     <path d="M58 40 L70 28 M62 44 L74 31" stroke="#6d7385" stroke-width="3"/>
     <circle cx="73" cy="25" r="8" fill="url(#gO)" {K}/>'''),
    ('repair', '''<path d="M70 10 a20 20 0 0 0-21 27 L14 72 a9 9 0 0 0 13 13 l35-35 a20 20 0 0 0 27-21 l-12 12 -11-2 -2-11 Z" fill="url(#gM)" {K}/>
     <path d="M20 76 L50 46" stroke="#fff" stroke-opacity=".6" stroke-width="3"/>
     <path d="M80 62 l10 -4 M76 72 l9 6 M68 76 l2 10" stroke="#ffd21f" stroke-width="5" stroke-linecap="round"/>'''),
    ('recover', '''<path d="M14 46 a36 36 0 0 1 64-20" stroke="url(#gO)" stroke-width="10" fill="none" stroke-linecap="round"/>
     <path d="M86 12 L84 38 L62 30 Z" fill="url(#gO)" {K}/>
     <g transform="rotate(180 50 66)"><path d="M18 64 L28 50 H66 L80 62 L84 74 H16 Z" fill="url(#gM)" {K}/>
     <circle cx="32" cy="76" r="8" fill="url(#gK)" {K}/><circle cx="68" cy="76" r="8" fill="url(#gK)" {K}/></g>'''),
    ('use', '''<path d="M50 4 L59 30 L84 18 L72 42 L96 52 L70 60 L80 86 L55 72 L44 96 L38 70 L12 80 L26 58 L4 46 L30 40 L20 14 L42 28 Z" fill="url(#gO)" {K}/>
     <path d="M54 22 L36 54 H50 L44 80 L66 44 H52 Z" fill="#fff8d6" stroke="#5a2600" stroke-width="3" stroke-linejoin="round"/>'''),
    ('next', '''<path d="M80 50 a30 30 0 1 1 -9 -21" stroke="url(#gO)" stroke-width="11" fill="none"/>
     <path d="M88 12 L86 42 L60 32 Z" fill="url(#gO)" {K}/>
     <path d="M40 34 L64 50 L40 66 Z" fill="url(#gM)" {K}/>'''),
    ('coin', '''<circle cx="50" cy="50" r="40" fill="url(#gY)" {K}/>
     <circle cx="50" cy="50" r="30" fill="none" stroke="#9a5f00" stroke-width="4" stroke-dasharray="5 4"/>
     {CR}
     <path d="M26 30 Q36 18 52 16" stroke="#fff" stroke-opacity=".8" stroke-width="4" fill="none" stroke-linecap="round"/>'''),
    ('armour', '''<path d="M50 6 L86 18 V48 C86 72 70 86 50 94 C30 86 14 72 14 48 V18 Z" fill="url(#gM)" {K}/>
     <path d="M50 16 L76 25 V48 C76 66 65 77 50 84 Z" fill="#000" opacity=".18"/>
     <circle cx="26" cy="26" r="4" fill="#3c4252"/><circle cx="74" cy="26" r="4" fill="#3c4252"/>
     <circle cx="26" cy="56" r="4" fill="#3c4252"/><circle cx="74" cy="56" r="4" fill="#3c4252"/>{TAG_A}'''),
    ('power', '''<path d="M10 68 A40 40 0 0 1 90 68 Z" fill="url(#gK)" {K}/>
     <path d="M20 66 A30 30 0 0 1 32 42" stroke="#46d45e" stroke-width="7" fill="none"/>
     <path d="M34 40 A30 30 0 0 1 66 40" stroke="#ffcc2a" stroke-width="7" fill="none"/>
     <path d="M68 42 A30 30 0 0 1 80 66" stroke="#ff4a3a" stroke-width="7" fill="none"/>
     <path d="M50 66 L78 46" stroke="url(#gO)" stroke-width="7" stroke-linecap="round"/>
     <circle cx="50" cy="66" r="8" fill="url(#gM)" {K}/>{TAG_P}'''),
    ('offensive', '''<rect x="12" y="40" width="70" height="22" rx="4" fill="url(#gM)" {K}/>
     <path d="M18 40 L26 14 L34 40 M38 40 L46 10 L54 40 M58 40 L66 14 L74 40" fill="url(#gR)" {K}/>
     <path d="M18 62 L26 84 L34 62" fill="url(#gR)" {K}/>
     <path d="M16 45 H76" stroke="#fff" stroke-opacity=".6" stroke-width="3"/>{TAG_O}'''),
    ('pause', '<rect x="22" y="16" width="20" height="68" rx="3" fill="url(#gM)" {K}/><rect x="58" y="16" width="20" height="68" rx="3" fill="url(#gM)" {K}/>'),
]
SUBST = {'K': K, 'CR': pixtext('CR', 50, 40, 21, '#5a3600'), 'TAG_A': tag('A'), 'TAG_P': tag('P'), 'TAG_O': tag('O')}
ICONS = [(n, a.format(**SUBST) if '{' in a else a) for n, a in ICONS]

def hexa(cx, cy, r):
    import math
    return ' '.join('%.2f,%.2f' % (cx + r * math.cos(math.pi / 3 * i), cy + r * math.sin(math.pi / 3 * i) * 0.92) for i in range(6))

def frame_svg():
    import math
    c, r = CELL / 2, CELL / 2 * FRAME_R
    rivets = ''.join('<circle cx="%.2f" cy="%.2f" r="%.2f" fill="#9aa1ad" stroke="#111" stroke-width="%.2f"/>'
                     % (c + r * .8 * math.cos(math.pi / 3 * i + math.pi / 6), c + r * .74 * math.sin(math.pi / 3 * i + math.pi / 6),
                        r * .05, r * .015) for i in range(6))
    return ('<polygon points="%s" fill="url(#steel)" stroke="#a4abb6" stroke-width="%.2f"/>'
            '<polygon points="%s" fill="none" stroke="rgba(0,0,0,.6)" stroke-width="%.2f"/>%s'
            % (hexa(c, c, r), r * .05, hexa(c, c, r * .93), r * .04, rivets))

def render(body):
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>%s</defs>%s</svg>'
           % (CELL, CELL, CELL, CELL, DEFS, body))
    return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode()))).convert('RGBA')

def cell(art):
    r = CELL / 2 * FRAME_R
    s = r * 0.80 / 50                      # icon (100 units) fills 80% of the hexagon
    off = CELL / 2 - 50 * s
    icon = render('<g transform="translate(%.2f,%.2f) scale(%.4f)">%s</g>' % (off, off, s, art))
    shadow = Image.new('RGBA', icon.size, (0, 0, 0, 0))
    shadow.putalpha(icon.getchannel('A').point(lambda a: a * 0.7))
    shadow = shadow.filter(ImageFilter.GaussianBlur(3))
    out = render(frame_svg())
    out.alpha_composite(shadow, (0, 4))
    out.alpha_composite(icon)
    return out

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rows = (len(ICONS) + COLS - 1) // COLS
    atlas = Image.new('RGBA', (COLS * CELL, rows * CELL), (0, 0, 0, 0))
    for i, (name, art) in enumerate(ICONS):
        atlas.paste(cell(art), ((i % COLS) * CELL, (i // COLS) * CELL))
    os.makedirs(os.path.join(root, 'design'), exist_ok=True)
    atlas.save(os.path.join(root, 'design', 'touch_atlas.png'))
    raw = zlib.compress(atlas.tobytes(), 9)
    w, h = atlas.size
    with open(os.path.join(root, 'platform', 'touch_atlas.h'), 'w') as f:
        f.write('/* generated by tools/make_touch_atlas.py - touch button artwork (original, GPL-3.0) */\n')
        f.write('enum { %s, TA_COUNT };\n' % ', '.join('TA_' + n.upper() for n, _ in ICONS))
        f.write('enum { TA_W = %d, TA_H = %d, TA_CELL = %d, TA_COLS = %d };\n' % (w, h, CELL, COLS))
        f.write('#define TA_FRAME_R %.3ff  /* hexagon radius / half cell */\n' % FRAME_R)
        f.write('static const unsigned char ta_zlib[%d] = {\n' % len(raw))
        for i in range(0, len(raw), 24):
            f.write(','.join(str(b) for b in raw[i:i + 24]) + ',\n')
        f.write('};\n')
    print('atlas %dx%d, %d buttons, %d KB compressed' % (w, h, len(ICONS), len(raw) // 1024))

if __name__ == '__main__':
    main()
