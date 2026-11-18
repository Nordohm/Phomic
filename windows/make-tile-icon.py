"""Makes Phomic's taskbar icon: the glyph from Phomic/Assets/Phomic.svg in white on a blue squircle,
like the iPhone app's icon. Writes Phomic/Assets/PhomicTile.svg (the large version) and PhomicTile.ico.

Small sizes (up to 48 px: taskbar, Start, title bars) get a larger glyph so it stays readable; larger
ones keep the iPhone icon's proportions. Renders with Microsoft Edge (headless); needs Pillow.

    python windows/make-tile-icon.py
"""
import math
import pathlib
import re
import subprocess
import tempfile

from PIL import Image

ASSETS = pathlib.Path(__file__).parent / "Phomic" / "Assets"
EDGE = pathlib.Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")
SIZES = [16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 96, 128, 256]
# The squircle fills 23 of the icon's 24 units, like other apps' taskbar icons (a smaller one looks lost
# there). The glyph's scale is on the same 24-unit grid.
TILE_HALF = 11.5
SMALL_GLYPH, LARGE_GLYPH = 0.68, 0.61
SMALL_UP_TO = 48


def squircle(center=12.0, half=TILE_HALF, n=5.0, points=240):
    """A superellipse (|x|^n + |y|^n = 1), the continuous-corner shape of app icons."""
    out = []
    for i in range(points):
        t = 2 * math.pi * i / points
        c, s = math.cos(t), math.sin(t)
        x = center + half * math.copysign(abs(c) ** (2 / n), c)
        y = center + half * math.copysign(abs(s) ** (2 / n), s)
        out.append(f"{x:.3f} {y:.3f}")
    return "M" + " L".join(out) + "Z"


def tile_svg(glyph_scale):
    source = (ASSETS / "Phomic.svg").read_text(encoding="utf-8")
    # The glyph's shapes, drawn white instead of with the gradient.
    shapes = re.search(r"</defs>(.*)</svg>", source, re.S).group(1).replace('"url(#g)"', '"#fff"')
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 24 24">'
        f'<defs><linearGradient id="bg" gradientUnits="userSpaceOnUse" x1="0" y1="{12 - TILE_HALF}" x2="0" y2="{12 + TILE_HALF}">'
        '<stop offset="0" stop-color="#3A97F2"/><stop offset="1" stop-color="#1467D6"/></linearGradient></defs>'
        f'<path d="{squircle()}" fill="url(#bg)"/>'
        f'<g transform="translate(12 12) scale({glyph_scale}) translate(-12 -12.2)">{shapes}</g></svg>'
    )


def render(svg, folder):
    svg_path = folder / "icon.svg"
    png_path = folder / "icon.png"
    svg_path.write_text(svg, encoding="utf-8")
    subprocess.run([str(EDGE), "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    "--default-background-color=00000000", "--window-size=1024,1024",
                    f"--screenshot={png_path}", svg_path.as_uri()],
                   check=True, capture_output=True, timeout=120)
    return Image.open(png_path).convert("RGBA")


def main():
    large_svg = tile_svg(LARGE_GLYPH)
    (ASSETS / "PhomicTile.svg").write_text(large_svg + "\n", encoding="utf-8")
    with tempfile.TemporaryDirectory() as temp:
        folder = pathlib.Path(temp)
        small = render(tile_svg(SMALL_GLYPH), folder)
        large = render(large_svg, folder)
    images = [(small if size <= SMALL_UP_TO else large).resize((size, size), Image.LANCZOS) for size in SIZES]
    images[-1].save(ASSETS / "PhomicTile.ico", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print("wrote", ASSETS / "PhomicTile.svg", "and", ASSETS / "PhomicTile.ico")


if __name__ == "__main__":
    main()
