"""Generates the fairyfly .ico files from the logo geometry (assets/logo/*.svg).

    python assets/generate_icons.py [--preview out.png]

Needs Pillow (pip install pillow). Writes assets/icons/fairyfly.ico (executable icon: mark on the light tile) and
assets/icons/tray_<state>_<theme>.ico (tray icons: whole mark recoloured per server state, dark/light taskbar).
The polygons are the ones of assets/logo/fairyfly_mark.svg; keep both in sync when the logo changes.
"""

import argparse
import colorsys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
ICONS = ROOT / "icons"

# Mark geometry (SVG units, origin at the spine). Order: wing planes, then spine and head on top.
LEFT_MAIN = [(-6, -60), (-150, -110), (-115, 25)]
LEFT_LOWER = [(-6, -60), (-115, 25), (-55, 80)]
LEFT_BASE = [(-6, 5), (-55, 80), (-20, 95)]
RIGHT_MAIN = [(6, -60), (150, -110), (115, 25)]
RIGHT_LOWER = [(6, -60), (115, 25), (55, 80)]
RIGHT_BASE = [(6, 5), (55, 80), (20, 95)]
SPINE = [(0, -85), (-7, -15), (0, 75), (7, -15)]
HEAD = ((0, -98), 6)
BBOX = (-150, -110, 150, 95)   # wings + head

# Brand palettes of the two logo variants (fairyfly_logo_dark.svg / fairyfly_logo_light.svg).
DARK = {"lm": "#25B493", "ll": "#1A7C64", "lb": "#1C3144", "rm": "#3CD6B2", "rl": "#20A884", "rb": "#274259",
        "spine": "#FFFFFF"}
LIGHT = {"lm": "#20A884", "ll": "#17745B", "lb": "#1E2E3D", "rm": "#26BA93", "rl": "#1C8B6D", "rb": "#293E52",
         "spine": "#0F172A"}
# On the tray the dark taskbar needs lighter base folds than the logo tile, or they disappear.
TRAY_BASE = {"dark": {"lb": "#3A5670", "rb": "#4A6984"}, "light": {"lb": "#1E2E3D", "rb": "#293E52"}}

# Server state -> (hue in degrees or None for grey, saturation factor, lightness shift).
STATES = {
    "running": None,                # brand teal as designed
    "warning": (40, 1.15, 0.06),    # amber
    "stopped": (None, 0.0, 0.04),   # grey
    "error": (356, 1.05, 0.04),     # red
}

TRAY_SIZES = [16, 20, 24, 32, 40, 48, 64]
APP_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SUPERSAMPLE = 8


def hex_rgb(value):
    value = value.lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def recolour(hex_value, state):
    spec = STATES[state]
    if spec is None:
        return hex_value
    hue, sat_factor, light_shift = spec
    r, g, b = (c / 255 for c in hex_rgb(hex_value))
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    if hue is not None:
        h = hue / 360
    s = min(1.0, s * sat_factor)
    l = min(0.92, l + light_shift)
    r, g, b = colorsys.hls_to_rgb(h, l, s)
    return "#%02X%02X%02X" % (round(r * 255), round(g * 255), round(b * 255))


def palette_for(theme, state):
    base = dict(DARK if theme == "dark" else LIGHT)
    base.update(TRAY_BASE[theme])
    return {k: (v if k in ("spine", "lb", "rb") else recolour(v, state)) for k, v in base.items()}


def draw_mark(draw, palette, cx, cy, scale, spine_boost=1.0):
    def pts(poly):
        return [(cx + x * scale, cy + y * scale) for x, y in poly]

    for key, poly in (("lm", LEFT_MAIN), ("ll", LEFT_LOWER), ("lb", LEFT_BASE),
                      ("rm", RIGHT_MAIN), ("rl", RIGHT_LOWER), ("rb", RIGHT_BASE)):
        draw.polygon(pts(poly), fill=hex_rgb(palette[key]))
    # At tray sizes the needle spine is under a pixel wide: widen it so it stays visible.
    spine = [(x * spine_boost, y) for x, y in SPINE]
    draw.polygon(pts(spine), fill=hex_rgb(palette["spine"]))
    (hx, hy), r = HEAD
    r *= spine_boost
    draw.ellipse([cx + (hx - r) * scale, cy + (hy - r) * scale, cx + (hx + r) * scale, cy + (hy + r) * scale],
                 fill=hex_rgb(palette["spine"]))


def render(size, palette, *, fill=0.96, tile=None, spine_boost=1.0):
    n = size * SUPERSAMPLE
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    if tile:
        fill_rgb, border_rgb = tile
        radius = n * 100 / 512
        width = max(SUPERSAMPLE, round(n * 2 / 512))
        draw.rounded_rectangle([0, 0, n - 1, n - 1], radius=radius, fill=hex_rgb(fill_rgb),
                               outline=hex_rgb(border_rgb), width=width)
    x0, y0, x1, y1 = BBOX
    scale = n * fill / max(x1 - x0, y1 - y0)
    cx = n / 2 - (x0 + x1) / 2 * scale
    cy = n / 2 - (y0 + y1) / 2 * scale
    draw_mark(draw, palette, cx, cy, scale, spine_boost)
    return img.resize((size, size), Image.LANCZOS)


def spine_boost_for(size):
    return 2.2 if size <= 20 else 1.8 if size <= 32 else 1.4 if size <= 48 else 1.0


def save_ico(path, images):
    images = sorted(images, key=lambda im: im.width, reverse=True)
    images[0].save(path, format="ICO", sizes=[im.size for im in images], append_images=images[1:])


def tray_images(theme, state):
    pal = palette_for(theme, state)
    return [render(s, pal, fill=1.0, spine_boost=spine_boost_for(s)) for s in TRAY_SIZES]


def app_images():
    pal = dict(LIGHT)
    return [render(s, pal, fill=0.70 if s >= 48 else 0.84, tile=("#F8FAFC", "#E2E8F0"), spine_boost=spine_boost_for(s))
            for s in APP_SIZES]


def preview(out):
    """Contact sheet: tray icons at 1x and 4x on Windows dark/light taskbar colours, plus the app icon."""
    rows = []
    for theme, bg in (("dark", (32, 32, 32)), ("light", (238, 238, 238))):
        for state in STATES:
            rows.append((bg, tray_images(theme, state)))
    rows.append(((255, 255, 255), app_images()[::-1]))
    rows.append(((32, 32, 32), app_images()[::-1]))
    width = 1600
    height = sum(max(im.height for im in ims) * 1 + 16 for _, ims in rows) + sum(16 * 4 + 16 for _ in rows)
    sheet = Image.new("RGB", (width, height), (128, 128, 128))
    y = 0
    for bg, ims in rows:
        row_h = max(max(im.height for im in ims), 64) + 16 + 16 * 4
        Image.Image.paste(sheet, Image.new("RGB", (width, row_h), bg), (0, y))
        x = 8
        for im in ims:
            sheet.paste(im, (x, y + 8), im)
            x += im.width + 8
        x += 24
        for s in (16, 20, 24, 32):
            src = next((i for i in ims if i.width == s), None)
            if src is None:
                continue
            up = src.resize((s * 4, s * 4), Image.NEAREST)
            sheet.paste(up, (x, y + 8), up)
            x += s * 4 + 12
        y += row_h
    sheet.crop((0, 0, width, y)).save(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", type=Path, help="also write a PNG contact sheet of all icons")
    args = parser.parse_args()
    ICONS.mkdir(exist_ok=True)
    for theme in ("dark", "light"):
        for state in STATES:
            save_ico(ICONS / f"tray_{state}_{theme}.ico", tray_images(theme, state))
    save_ico(ICONS / "fairyfly.ico", app_images())
    if args.preview:
        preview(args.preview)


if __name__ == "__main__":
    main()
