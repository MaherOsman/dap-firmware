#!/usr/bin/env python3
"""
fontgen.py — rasterise a TTF into 1-bit proportional bitmap fonts as C arrays.

The STM32 has no font engine and no filesystem full of TTFs, so glyphs get
baked into flash at build time. This runs on a PC, once, and its output is
checked in.

Why 1-bit and not anti-aliased: a 240x240 panel over SPI has a limited pixel
budget per frame, and 1-bit glyphs let us draw text by setting pixels rather
than blending them. Anti-aliasing would mean reading back the destination
colour for every pixel, which is both slower and much more code.

Usage:
    python3 tools/fontgen.py > src/core/font_data.c
"""

import sys
from PIL import Image, ImageDraw, ImageFont

# (C identifier, human name, TTF path, pixel size)
FONTS = [
    ("font_sm", "Roboto 11",        "/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/Roboto-Regular.ttf", 11),
    ("font_md", "Roboto 14",        "/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/Roboto-Regular.ttf", 14),
    ("font_lg", "Roboto Medium 18", "/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/Roboto-Medium.ttf",  18),
]

FIRST, LAST = 32, 126          # printable ASCII
THRESHOLD = 128                # 8-bit coverage -> on/off


def render_glyph(font, ch):
    """Return (w, h, xoff, yoff, advance, rows_of_bytes)."""
    # Measure with a generous canvas so nothing clips.
    pad = 8
    size = font.getbbox("Mg")
    canvas_h = (size[3] - size[1]) + pad * 4
    canvas_w = font.getlength(ch) + pad * 4
    img = Image.new("L", (int(canvas_w) + 1, int(canvas_h) + 1), 0)
    d = ImageDraw.Draw(img)
    # Draw at a known origin; ascender-relative, we convert to baseline below.
    origin = (pad, pad)
    d.text(origin, ch, fill=255, font=font)

    bbox = img.getbbox()
    advance = int(round(font.getlength(ch)))

    if bbox is None:  # space and friends
        return 0, 0, 0, 0, advance, []

    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0

    ascent, _descent = font.getmetrics()
    # Offsets are relative to the pen position sitting on the baseline.
    xoff = x0 - origin[0]
    yoff = (y0 - origin[1]) - ascent

    stride = (w + 7) // 8
    rows = []
    px = img.load()
    for y in range(y0, y1):
        row = bytearray(stride)
        for x in range(x0, x1):
            if px[x, y] >= THRESHOLD:
                bit = x - x0
                row[bit >> 3] |= 0x80 >> (bit & 7)
        rows.append(row)

    return w, h, xoff, yoff, advance, rows


def emit(ident, human, path, px_size, out):
    font = ImageFont.truetype(path, px_size)
    ascent, descent = font.getmetrics()

    blob = bytearray()
    glyphs = []

    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        w, h, xo, yo, adv, rows = render_glyph(font, ch)
        off = len(blob)
        for r in rows:
            blob.extend(r)
        glyphs.append((w, h, xo, yo, adv, off, ch))

    out.write(f"\n/* ---- {human} : {len(glyphs)} glyphs, "
              f"{len(blob)} bytes of bitmap ---- */\n")

    out.write(f"static const uint8_t {ident}_bits[] = {{\n")
    for i in range(0, len(blob), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in blob[i:i + 16])
        out.write(f"    {chunk},\n")
    out.write("};\n\n")

    out.write(f"static const font_glyph_t {ident}_glyphs[] = {{\n")
    for (w, h, xo, yo, adv, off, ch) in glyphs:
        label = "space" if ch == " " else ch
        out.write(f"    {{ {w:3d}, {h:3d}, {xo:4d}, {yo:4d}, {adv:3d}, "
                  f"{off:5d} }},  /* '{label}' */\n")
    out.write("};\n\n")

    out.write(f"const font_t {ident} = {{\n")
    out.write(f"    .name    = \"{human}\",\n")
    out.write(f"    .height  = {ascent + descent},\n")
    out.write(f"    .ascent  = {ascent},\n")
    out.write(f"    .first   = {FIRST},\n")
    out.write(f"    .last    = {LAST},\n")
    out.write(f"    .glyphs  = {ident}_glyphs,\n")
    out.write(f"    .bitmap  = {ident}_bits,\n")
    out.write("};\n")

    return len(blob) + len(glyphs) * 8


def main():
    out = sys.stdout
    out.write("/* GENERATED FILE — do not edit by hand.\n")
    out.write(" * Regenerate with:  python3 tools/fontgen.py > src/core/font_data.c\n")
    out.write(" */\n")
    out.write('#include "font.h"\n')

    total = 0
    for ident, human, path, size in FONTS:
        total += emit(ident, human, path, size, out)

    out.write(f"\n/* Total flash cost of all fonts: ~{total} bytes */\n")
    print(f"fontgen: {total} bytes total", file=sys.stderr)


if __name__ == "__main__":
    main()
