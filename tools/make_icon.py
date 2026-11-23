#!/usr/bin/env python3
"""Turn the game's .ico (from the user's install) into Android launcher mipmaps and a BMP for the desktop window.
Usage: make_icon.py game.ico android_res_dir out_icon.bmp"""
import sys, os
from PIL import Image

ico, resdir, bmp = sys.argv[1:4]
im = Image.open(ico)
sizes = sorted(im.info.get('sizes', [im.size]))
im.size = sizes[-1]
icon = im.convert('RGBA')
for name, size in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    d = os.path.join(resdir, 'mipmap-' + name)
    os.makedirs(d, exist_ok=True)
    icon.resize((size, size), Image.LANCZOS).save(os.path.join(d, 'ic_launcher.png'))
    # adaptive icon foreground: the icon on a 108dp canvas, black behind it
    canvas = size * 108 // 48
    fg = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 255))
    s = int(canvas * 0.72)
    fg.alpha_composite(icon.resize((s, s), Image.LANCZOS), ((canvas - s) // 2, (canvas - s) // 2))
    fg.save(os.path.join(d, 'ic_launcher_fg.png'))
bg = Image.new('RGB', icon.size, (0, 0, 0)); bg.paste(icon, mask=icon.split()[3])
bg.resize((64, 64), Image.LANCZOS).save(bmp)
os.makedirs(os.path.join(resdir, 'mipmap-anydpi-v26'), exist_ok=True)
with open(os.path.join(resdir, 'mipmap-anydpi-v26', 'ic_launcher.xml'), 'w') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@color/ic_launcher_bg"/>\n    <foreground android:drawable="@mipmap/ic_launcher_fg"/>\n</adaptive-icon>\n')
os.makedirs(os.path.join(resdir, 'values'), exist_ok=True)
with open(os.path.join(resdir, 'values', 'ic_launcher_bg.xml'), 'w') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_bg">#000000</color>\n</resources>\n')
print('icon %dx%d -> %s, %s' % (icon.size[0], icon.size[1], resdir, bmp))
