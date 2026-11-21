"""Makes every icon file from one drawing: windows/Phomic/Assets/Phomic.svg, an iPhone as the capsule of a
microphone on a 24-unit grid, in two groups (`phone` and `stand`).

Writes:
  ios/Resources/AppIcon.icon/Assets/phone.svg, stand.svg   the iPhone icon's layers, white on a 1024 canvas
                                                            (icon.json beside them says how they're stacked)
  windows/Phomic/Assets/Phomic.ico                          the glyph alone (title bar, tray)
  windows/Phomic/Assets/PhomicTile.svg, PhomicTile.ico      white on a blue squircle (taskbar, Start, installer)
  --website FILE                                            also the squircle icon for the website, as SVG

Small Windows sizes (up to 48 px) get a larger glyph so it stays readable; larger ones keep the iPhone icon's
proportions. Renders with Microsoft Edge (headless); needs Pillow.

    python make-icons.py [--website FILE]
"""
import argparse
import math
import pathlib
import re
import subprocess
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).parent
ASSETS = ROOT / "windows" / "Phomic" / "Assets"
LAYERS = ROOT / "ios" / "Resources" / "AppIcon.icon" / "Assets"
EDGE = pathlib.Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")
TOP, BOTTOM = "#3A97F2", "#1467D6"
GLYPH_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
TILE_SIZES = [16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 96, 128, 256]
# The glyph's scale on the icon's 24-unit grid: the iPhone icon's (and the website's), and Windows' tile.
# The tile fills 23 of the 24 units, like other apps' taskbar icons (a smaller one looks lost there).
IPHONE_GLYPH = 0.64
TILE_HALF = 11.5
SMALL_GLYPH, LARGE_GLYPH = 0.68, 0.61
SMALL_UP_TO = 48


def squircle(half, center=12.0, n=5.0, points=240):
    """A superellipse (|x|^n + |y|^n = 1), the continuous-corner shape of app icons."""
    out = []
    for i in range(points):
        t = 2 * math.pi * i / points
        c, s = math.cos(t), math.sin(t)
        x = center + half * math.copysign(abs(c) ** (2 / n), c)
        y = center + half * math.copysign(abs(s) ** (2 / n), s)
        out.append(f"{x:.3f} {y:.3f}")
    return "M" + " L".join(out) + "Z"


def shapes(colour, group=None):
    """The drawing's shapes (one group, or both), painted in `colour` instead of the gradient."""
    source = (ASSETS / "Phomic.svg").read_text(encoding="utf-8")
    if group:
        body = re.search(rf'<g id="{group}">(.*?)</g>', source, re.S).group(1)
    else:
        body = re.search(r"</defs>(.*)</svg>", source, re.S).group(1)
    return body.replace('"url(#g)"', f'"{colour}"')


def glyph_transform(scale):
    """Scales the glyph about the grid's centre (its visual centre sits 0.2 units low)."""
    return f"translate(12 12) scale({scale}) translate(-12 -12.2)"


def tile_svg(glyph_scale, half=TILE_HALF, clip=False):
    background = (f'<clipPath id="c"><path d="{squircle(half)}"/></clipPath>' if clip else "")
    fill = (f'<g clip-path="url(#c)"><rect width="24" height="24" fill="url(#bg)"/>' if clip
            else f'<path d="{squircle(half)}" fill="url(#bg)"/>')
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 24 24">'
        f'<defs><linearGradient id="bg" gradientUnits="userSpaceOnUse" x1="0" y1="{12 - half}" x2="0" y2="{12 + half}">'
        f'<stop offset="0" stop-color="{TOP}"/><stop offset="1" stop-color="{BOTTOM}"/></linearGradient>{background}</defs>'
        f'{fill}<g transform="{glyph_transform(glyph_scale)}">{shapes("#fff")}</g>{"</g>" if clip else ""}</svg>'
    )


def layer_svg(group):
    """One of the iPhone icon's layers: the group in white, where it sits on Icon Composer's 1024 canvas."""
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">'
        f'<g transform="scale({1024 / 24:.6f}) {glyph_transform(IPHONE_GLYPH)}">{shapes("#FFFFFF", group)}</g></svg>\n'
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


def save_ico(path, sizes, small, large=None):
    images = [((small if large is None or size <= SMALL_UP_TO else large)).resize((size, size), Image.LANCZOS)
              for size in sizes]
    images[-1].save(path, sizes=[(s, s) for s in sizes], append_images=images[:-1])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--website", type=pathlib.Path, help="also write the website's squircle icon (SVG) here")
    args = parser.parse_args()

    LAYERS.mkdir(parents=True, exist_ok=True)
    for group in ("phone", "stand"):
        (LAYERS / f"{group}.svg").write_text(layer_svg(group), encoding="utf-8")

    large_tile = tile_svg(LARGE_GLYPH)
    (ASSETS / "PhomicTile.svg").write_text(large_tile + "\n", encoding="utf-8")
    with tempfile.TemporaryDirectory() as temp:
        folder = pathlib.Path(temp)
        save_ico(ASSETS / "Phomic.ico", GLYPH_SIZES, render((ASSETS / "Phomic.svg").read_text(encoding="utf-8"), folder))
        save_ico(ASSETS / "PhomicTile.ico", TILE_SIZES, render(tile_svg(SMALL_GLYPH), folder), render(large_tile, folder))

    if args.website:
        args.website.write_text(tile_svg(IPHONE_GLYPH, half=12, clip=True) + "\n", encoding="utf-8")
    print("wrote the iPhone icon's layers, Phomic.ico, PhomicTile.svg and PhomicTile.ico"
          + (f", and {args.website}" if args.website else ""))


if __name__ == "__main__":
    main()
