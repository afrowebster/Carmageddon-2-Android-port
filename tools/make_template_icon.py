#!/usr/bin/env python3
"""Draw the template APK's generic launcher icon (a stylised tyre). The APK builder replaces it with the icon
from the user's own copy of the game. Usage: make_template_icon.py android/app/src/main/res"""
import os, sys
from PIL import Image, ImageDraw

def tyre(size, canvas=None, bg=(24, 24, 24, 255)):
    canvas = canvas or size
    ss = 4                                    # supersampling
    S, C = size * ss, canvas * ss
    im = Image.new('RGBA', (C, C), bg)
    d = ImageDraw.Draw(im)
    o = (C - S) // 2
    c = C / 2
    def circle(r, **kw): d.ellipse([c - r, c - r, c + r, c + r], **kw)
    circle(S * 0.46, fill=(10, 10, 10, 255))                       # tyre
    for i in range(24):                                            # tread blocks
        import math
        a = i * math.pi / 12
        for r0, r1, w in ((0.40, 0.47, 0.035),):
            x0, y0 = c + math.cos(a) * S * r0, c + math.sin(a) * S * r0
            x1, y1 = c + math.cos(a + 0.12) * S * r1, c + math.sin(a + 0.12) * S * r1
            d.line([x0, y0, x1, y1], fill=(60, 60, 60, 255), width=int(S * w))
    circle(S * 0.36, fill=(30, 30, 30, 255))
    circle(S * 0.27, fill=(200, 30, 20, 255))                     # rim
    for i in range(5):                                            # spokes
        a = i * 2 * math.pi / 5 - math.pi / 2
        d.line([c, c, c + math.cos(a) * S * 0.26, c + math.sin(a) * S * 0.26], fill=(120, 12, 8, 255), width=int(S * 0.07))
    circle(S * 0.08, fill=(230, 230, 230, 255))                   # hub
    return im.resize((canvas, canvas), Image.LANCZOS)

resdir = sys.argv[1]
for name, size in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    d = os.path.join(resdir, 'mipmap-' + name)
    os.makedirs(d, exist_ok=True)
    tyre(size).save(os.path.join(d, 'ic_launcher.png'))
    canvas = size * 108 // 48
    fg = Image.new('RGBA', (canvas, canvas), (24, 24, 24, 255))
    t = tyre(int(canvas * 0.72), bg=(0, 0, 0, 0))
    fg.alpha_composite(t, ((canvas - t.width) // 2, (canvas - t.height) // 2))
    fg.save(os.path.join(d, 'ic_launcher_fg.png'))
os.makedirs(os.path.join(resdir, 'mipmap-anydpi-v26'), exist_ok=True)
open(os.path.join(resdir, 'mipmap-anydpi-v26', 'ic_launcher.xml'), 'w').write(
    '<?xml version="1.0" encoding="utf-8"?>\n<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
    '    <background android:drawable="@color/ic_launcher_bg"/>\n    <foreground android:drawable="@mipmap/ic_launcher_fg"/>\n</adaptive-icon>\n')
os.makedirs(os.path.join(resdir, 'values'), exist_ok=True)
open(os.path.join(resdir, 'values', 'ic_launcher_bg.xml'), 'w').write(
    '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_bg">#181818</color>\n</resources>\n')
