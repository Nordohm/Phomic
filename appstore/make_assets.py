"""App Store assets for Phomic, made from real iPhone screenshots.

    python appstore/make_assets.py <folder with the screenshots>

Expects the screenshots (1179 x 2556, iPhone with Dynamic Island, dark mode) named in SOURCES below.
Writes to appstore/out/:
  screenshots/6.1/*.png   1179 x 2556  (iPhone with Dynamic Island, medium; required)
  screenshots/6.9/*.png   1320 x 2868  (iPhone with Dynamic Island, large)
  header-universal.png    5244 x 2950  (product page header + search results)
Before composing, the screenshots get a clean status bar (9:41), and the PC's name becomes "DESKTOP":
cut out of the real name ("ATT_DESKTOP_OLD"), so it stays in the iPhone's own font.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
FONTS = r"C:\Windows\Fonts"
BLUE_TOP = (58, 151, 242)     # app icon gradient, #3A97F2
BLUE_BOTTOM = (20, 103, 214)  # #1467D6

SOURCES = {  # 1.3, 22 Nov 2026 (the Lock Screen from 1.2: without its file, the slide made then stays)
    "wifi": "IMG_4098.PNG",   # live over Wi-Fi
    "usb": "IMG_4096.PNG",    # live over USB (its status bar is the Wi-Fi one's: a Live Activity filled it)
    "muted": "IMG_4099.PNG",  # muted (Wi-Fi)
    "pick": "IMG_4100.PNG",   # the Wi-Fi tab's list of PCs
    "lock": "IMG_3995.PNG",   # Lock Screen with the Live Activity (only its pieces are used)
}

# Where things are in those screenshots (pixels, measured). The name "ATT_DESKTOP_OLD" sits between
# two underscores; "DESKTOP" is what's between them.
BUTTON_CENTER = (589.5, 1277.5)   # the mute button: the glow is a circle around it
CHIP = {                          # the "<PC> · 3 ms" capsule under the title
    # icon, "DESKTOP", " · 3 ms" (from the space after "OLD" to the end), as x ranges
    "wifi": ((305, 351), (478, 656), (754, 879)),
    "usb": ((305, 351), (478, 656), (754, 878)),
    "muted": ((305, 351), (478, 656), (754, 878)),
}
CHIP_Y = (847, 924)
CHIP_LETTERS_END = 899                # the underscore after "DESKTOP" starts here, under the P
STATUS_BAR_HEIGHT = 165               # above the Settings button
CHIP_PAD = (34, 32)                   # the capsule's edge to the icon, and the text to its edge
CHIP_ICON_GAP = 28                    # icon to text
CHIP_FILL = ((118, 118, 128), 0.245)  # iOS's tertiary fill over the glow (measured)
TABS = {                              # tab bar icons, white (not the selected tab)
    "wifi": ("usb", (290, 2346, 374, 2410)),
    "bluetooth": ("wifi", (566, 2344, 612, 2414)),
    "usb": ("wifi", (833, 2337, 864, 2422)),
}


def font(weight, size):
    files = {"regular": "segoeui.ttf", "semibold": "seguisb.ttf", "bold": "segoeuib.ttf", "black": "seguibl.ttf"}
    return ImageFont.truetype(os.path.join(FONTS, files[weight]), size)


def font_for_cap_height(weight, cap_px):
    """Segoe UI at the size whose capital H is cap_px tall (to match iOS's text)."""
    probe = font(weight, 200)
    top, bottom = probe.getbbox("H")[1], probe.getbbox("H")[3]
    return font(weight, round(200 * cap_px / (bottom - top)))


def draw_on_baseline(draw, text, fnt, baseline_y, fill, center_x=None, left_x=None):
    ascent = fnt.getmetrics()[0]
    x0, _, x1, _ = draw.textbbox((0, 0), text, font=fnt)
    if center_x is not None:
        x = center_x - (x1 - x0) / 2 - x0
    else:
        x = left_x - x0
    draw.text((x, baseline_y - ascent), text, font=fnt, fill=fill)


def paste_glyph(dst, src, box, color=None, floor=60, at=None, ceiling=255):
    """Copies light glyphs from src (box) onto dst without their background: alpha from brightness
    between floor (background) and ceiling (the glyph's own color). at: where to put them."""
    crop = src.crop(box)
    span = max(1, ceiling - floor)
    alpha = crop.convert("L").point(lambda v: max(0, min(255, (v - floor) * 255 // span)))
    fill = Image.new("RGB", crop.size, color) if color else crop
    dst.paste(fill, at or box[:2], alpha)


def brightness(rgb):
    return round(0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2])


# --------------------------------------------------------------------------- the app's screens

def radial_background(im):
    """The glow's color by distance from the mute button, read along an empty line to its left."""
    cx, cy = BUTTON_CENTER
    return [im.getpixel((max(0, int(cx - r)), int(cy))) for r in range(int(cx) + 1)]


def rename_chip(im, kind):
    """Redraws the "<PC> · 8 ms" capsule as "DESKTOP · 8 ms": the glow behind it from the circle
    it follows, a capsule as wide as the new text, and the pieces of the real text on it."""
    src = im.copy()
    icon, name, tail = CHIP[kind]
    x0, x1 = icon[0] - CHIP_PAD[0], tail[1] + CHIP_PAD[1]  # the real capsule
    y0, y1 = CHIP_Y
    profile = radial_background(src)
    cx, cy = BUTTON_CENTER
    px = im.load()
    for y in range(y0 - 2, y1 + 2):
        for x in range(x0 - 2, x1 + 2):
            r = min(len(profile) - 1, int(math.hypot(x - cx, y - cy)))
            px[x, y] = profile[r]

    width = CHIP_PAD[0] + (icon[1] - icon[0]) + CHIP_ICON_GAP + (name[1] - name[0]) + (tail[1] - tail[0]) + CHIP_PAD[1]
    left = round(cx - width / 2)
    k = 4  # the capsule's edge, antialiased
    mask = Image.new("L", (width * k, (y1 - y0) * k), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, width * k - 1, (y1 - y0) * k - 1), (y1 - y0) * k // 2, fill=255)
    mask = mask.resize((width, y1 - y0), Image.LANCZOS)
    tint, a = CHIP_FILL
    for y in range(y0, y1):
        for x in range(left, left + width):
            m = mask.getpixel((x - left, y - y0)) / 255 * a
            b = px[x, y]
            px[x, y] = tuple(round(b[i] * (1 - m) + tint[i] * m) for i in range(3))

    def capsule_fill(x, y):
        """The real capsule's color at (x, y), text aside: the fill over the glow."""
        b = profile[min(len(profile) - 1, int(math.hypot(x - cx, y - cy)))]
        return brightness(tuple(b[i] * (1 - a) + tint[i] * a for i in range(3)))

    # The text and icon, lifted off the real capsule pixel by pixel (its fill varies a little with
    # the glow behind it) and put on the new one.
    text = max((src.getpixel((x, y)) for x in range(name[0], name[1]) for y in range(y0, y1)), key=brightness)
    ink = brightness(text)
    spx = src.load()
    x = left + CHIP_PAD[0]
    for piece in (icon, None, name, tail):
        if piece is None:
            x += CHIP_ICON_GAP
            continue
        for sy in range(y0, CHIP_LETTERS_END if piece is name else y1):
            for sx in range(piece[0], piece[1]):
                floor = capsule_fill(sx, sy)
                k = max(0.0, min(1.0, (brightness(spx[sx, sy]) - floor - 1) / max(1, ink - floor)))
                if k > 0:
                    dx = x + sx - piece[0]
                    b = px[dx, sy]
                    px[dx, sy] = tuple(round(b[i] * (1 - k) + text[i] * k) for i in range(3))
        x += piece[1] - piece[0]
    return im


def clean(im, kind):
    """Status bar 9:41 without the Focus icon; the PC's name "DESKTOP"."""
    im = im.convert("RGB").copy()
    d = ImageDraw.Draw(im)
    # The time "05.44" and the Focus icon next to it: x 111-315, capitals 69-107.
    d.rectangle((100, 55, 330, 120), fill=im.getpixel((40, 90)))
    draw_on_baseline(d, "9:41", font_for_cap_height("semibold", 38), 107, (255, 255, 255), center_x=187)
    if kind in CHIP:
        rename_chip(im, kind)
    if kind == "pick":
        # The PC's row: "ATT_DESKTOP_OLD" from x 272, "DESKTOP" at 393-619, letters y 1314-1351 (the
        # underscore after it, under the P, from 1352).
        src = im.copy()
        d.rectangle((266, 1300, 750, 1370), fill=src.getpixel((800, 1335)))
        paste_glyph(im, src, (393, 1300, 619, 1352), (255, 255, 255), floor=40, at=(272, 1300))
    return im


# --------------------------------------------------------------------------- Lock Screen

def lock_wallpaper(w, h):
    """Soft blue shapes in the app's colors (the screenshot's own wallpaper is someone else's art)."""
    im = gradient(w, h, (6, 20, 60), (5, 34, 96))
    for (cx, cy, r, color, strength) in (
        (0.85, 0.16, 0.75, (44, 124, 240), 0.85),
        (0.08, 0.64, 0.62, (12, 150, 170), 0.55),
        (0.55, 1.02, 0.55, (58, 151, 242), 0.45),
    ):
        mask = Image.new("L", (w, h), 0)
        R = round(w * r)
        ImageDraw.Draw(mask).ellipse((w * cx - R, h * cy - R, w * cx + R, h * cy + R), fill=int(255 * strength))
        im.paste(Image.new("RGB", (w, h), color), (0, 0), mask.filter(ImageFilter.GaussianBlur(w * 0.18)))
    return im


def lock_screen(src, live_activity=True):
    """A clean Lock Screen (9:41, English date, plain wallpaper) with the real Live Activity from
    the screenshot, its PC renamed. live_activity=False leaves it out (shown enlarged instead)."""
    w, h = src.size
    im = lock_wallpaper(w, h)
    d = ImageDraw.Draw(im, "RGBA")

    paste_glyph(im, src, (830, 45, 1100, 145))                      # signal, Wi-Fi, battery
    for x in range(640, 668):                                       # the orange microphone dot
        for y in range(76, 103):
            p = src.getpixel((x, y))
            if p[0] > 150 and p[2] < 120:
                im.putpixel((x, y), p)

    draw_on_baseline(d, "Monday, November 16", font_for_cap_height("semibold", 40), 300, (235, 240, 255),
                     center_x=w / 2)
    draw_on_baseline(d, "9:41", font_for_cap_height("semibold", 250), 600, (235, 240, 255, 235), center_x=w / 2)

    # The Live Activity: black, with a faint rim, like iOS draws it.
    if live_activity:
        banner = (40, 1934, 1138, 2170)
        d.rounded_rectangle(banner, 70, fill=(0, 0, 0, 236), outline=(255, 255, 255, 42), width=2)
        circle = src.crop((86, 1976, 238, 2128))
        m = Image.new("L", circle.size, 0)
        ImageDraw.Draw(m).ellipse((4, 4, 147, 147), fill=255)
        im.paste(circle, (86, 1976), m)
        paste_glyph(im, src, (275, 1995, 380, 2048), (255, 255, 255))   # "Live"
        name_color = (128, 128, 134)
        paste_glyph(im, src, (284, 2063, 314, 2108), name_color, floor=40)  # Bluetooth rune
        # "ATT_DESKTOP_OLD" starts at x 344; "DESKTOP" is at 446-645.
        paste_glyph(im, src, (446, 2056, 645, 2114), name_color, floor=40, ceiling=brightness(name_color), at=(344, 2056))

    # Flashlight and camera, and the home indicator.
    for cx, glyph in ((225, None), (955, "\ue722")):
        d.ellipse((cx - 88, 2232, cx + 88, 2408), fill=(255, 255, 255, 38), outline=(255, 255, 255, 60), width=2)
        if glyph:
            icon = ImageFont.truetype(os.path.join(FONTS, "SegoeIcons.ttf"), 74)
            x0, y0, x1, y1 = d.textbbox((0, 0), glyph, font=icon)
            d.text((cx - (x1 - x0) / 2 - x0, 2320 - (y1 - y0) / 2 - y0), glyph, font=icon, fill=(255, 255, 255))
    paste_glyph(im, src, (195, 2262, 256, 2376), (255, 255, 255), floor=120)  # flashlight
    d.rounded_rectangle((380, 2517, 799, 2531), 7, fill=(255, 255, 255))
    return im


# --------------------------------------------------------------------------- composing

def gradient(w, h, top=BLUE_TOP, bottom=BLUE_BOTTOM):
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(h - 1, 1)
        g.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return g.resize((w, h))


def glow(canvas, center, radius, strength):
    """A soft white light behind something."""
    layer = Image.new("L", canvas.size, 0)
    ImageDraw.Draw(layer).ellipse((center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius),
                                  fill=int(255 * strength))
    layer = layer.filter(ImageFilter.GaussianBlur(radius * 0.45))
    canvas.paste(Image.new("RGB", canvas.size, (255, 255, 255)), (0, 0), layer)


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return m


def with_island(screen):
    """Screenshots leave out the Dynamic Island; draw it (iPhone 15/16 Pro proportions) and keep
    the orange microphone dot iOS shows inside it."""
    im = screen.convert("RGB").copy()
    w = im.width
    dot = [(x, y) for y in range(30, 120) for x in range(round(w * 0.45), round(w * 0.68))
           if (lambda p: p[0] > 200 and 90 < p[1] < 190 and p[2] < 90)(im.getpixel((x, y)))]
    saved = [(p, im.getpixel(p)) for p in dot]
    iw, ih, top = round(w * 0.3206), round(w * 0.0942), round(w * 0.028)
    ImageDraw.Draw(im).rounded_rectangle(((w - iw) // 2, top, (w + iw) // 2, top + ih), ih // 2, fill=(0, 0, 0))
    for p, c in saved:
        im.putpixel(p, c)
    return im


def phone(screen, width):
    """The screenshot in a simple dark iPhone-like frame, with a shadow. Returns RGBA."""
    screen = with_island(screen)
    bezel = round(width * 0.022)
    inner_w = width - 2 * bezel
    inner_h = round(screen.height * inner_w / screen.width)
    height = inner_h + 2 * bezel
    radius = round(width * 0.145)
    pad = round(width * 0.12)  # room for the shadow
    out = Image.new("RGBA", (width + 2 * pad, height + 2 * pad), (0, 0, 0, 0))

    shadow = Image.new("L", out.size, 0)
    ImageDraw.Draw(shadow).rounded_rectangle((pad, pad + width * 0.03, pad + width, pad + height + width * 0.03),
                                             radius, fill=110)
    shadow = shadow.filter(ImageFilter.GaussianBlur(width * 0.045))
    out.paste(Image.new("RGBA", out.size, (6, 30, 80, 255)), (0, 0), shadow)

    body = Image.new("RGBA", (width, height), (24, 24, 27, 255))
    ImageDraw.Draw(body).rounded_rectangle((0, 0, width - 1, height - 1), radius, outline=(92, 96, 108, 255),
                                           width=max(2, round(width * 0.004)))
    out.paste(body, (pad, pad), rounded_mask((width, height), radius))
    scr = screen.convert("RGB").resize((inner_w, inner_h), Image.LANCZOS)
    out.paste(scr, (pad + bezel, pad + bezel), rounded_mask((inner_w, inner_h), radius - bezel))
    return out, pad


def text_block(draw, lines, fnt, center_x, top, fill, spacing):
    y = top
    for line in lines:
        x0, y0, x1, y1 = draw.textbbox((0, 0), line, font=fnt)
        draw.text((center_x - (x1 - x0) / 2 - x0, y - y0), line, font=fnt, fill=fill)
        y += (y1 - y0) + spacing
    return y


SLIDES = [
    ("wifi", ["Your iPhone is", "your PC's mic"], "Talk into your phone. Your PC hears it."),
    ("lock", ["Mute from the", "Lock Screen"], "A Live Activity with its own mute button."),
    ("usb", ["Wi-Fi, Bluetooth", "or USB"], "Switch while you talk."),
    ("muted", ["Mute with", "one tap"], "Muting releases the microphone."),
    ("pick", ["Private", "by design"], "Straight to your PC. No account, no tracking."),
]


def screenshot(screen, headline, subline, size, callout=None):
    """The whole phone under the text, tab bar included.
    callout: (image, box, corner radius, bottom) in screen pixels: that part of image shown enlarged,
    centered on its own place sideways, with its bottom edge at screen y `bottom`."""
    w, h = size
    s = w / 1179
    canvas = gradient(w, h)
    glow(canvas, (w // 2, round(h * 0.62)), round(w * 0.55), 0.18)
    d = ImageDraw.Draw(canvas)
    y = text_block(d, headline, font("bold", round(104 * s)), w / 2, round(150 * s), (255, 255, 255),
                   round(26 * s))
    size = round(46 * s)
    while d.textlength(subline, font=font("semibold", size)) > w * 0.86:  # keep clear of the edges
        size -= 1
    text_block(d, [subline], font("semibold", size), w / 2, y + round(30 * s), (225, 236, 252), 0)
    top = round(h * 0.255)
    # As wide as fits between the text and a bottom margin.
    avail = h - top - round(h * 0.035)
    width = round(avail / (screen.height / screen.width * (1 - 2 * 0.022) + 2 * 0.022))
    frame, pad = phone(screen, width)
    fx = round((w - frame.width) / 2)
    canvas.paste(frame, (fx, top - pad), frame)
    if callout:
        source, (x0, y0, x1, y1), radius, bottom = callout
        bezel = round(width * 0.022)
        k = (width - 2 * bezel) / screen.width               # screen pixels -> canvas
        cx = fx + pad + bezel + (x0 + x1) / 2 * k
        piece = source.crop((x0, y0, x1, y1))
        target_w = round(w * 0.9)
        target_h = round(piece.height * target_w / piece.width)
        r = round(radius * target_w / piece.width)
        piece = piece.resize((target_w, target_h), Image.LANCZOS)
        left = round(cx - target_w / 2)
        upper = round(top + bezel + bottom * k - target_h)
        shadow = Image.new("L", canvas.size, 0)
        ImageDraw.Draw(shadow).rounded_rectangle((left, upper + 18 * s, left + target_w, upper + target_h + 18 * s),
                                                 r, fill=150)
        canvas.paste(Image.new("RGB", canvas.size, (4, 22, 60)), (0, 0), shadow.filter(ImageFilter.GaussianBlur(28 * s)))
        canvas.paste(piece, (left, upper), rounded_mask(piece.size, r))
    return canvas


def tab_icon(screens, kind, height):
    """The tab bar's own icon for a connection, white, as an alpha mask `height` tall."""
    source, box = TABS[kind]
    crop = screens[source].crop(box).convert("L")
    floor = max(crop.getpixel((0, 0)), crop.getpixel((crop.width - 1, crop.height - 1))) + 12
    alpha = crop.point(lambda v: max(0, min(255, (v - floor) * 255 // (255 - floor))))
    alpha = alpha.crop(alpha.getbbox())
    return alpha.resize((round(alpha.width * height / alpha.height), height), Image.LANCZOS)


def header(screens):
    """Universal asset (5244 x 2950, PNG). It shows small on an iPhone, so: the headline and the three
    connections, large, filling the safe area 1402 x 962 at (1921, 660) that every crop keeps."""
    w, h = 5244, 2950
    sx, sy, sw, sh = 1921, 660, 1402, 962
    canvas = gradient(w, h, (52, 140, 236), (16, 92, 200))
    glow(canvas, (sx + sw // 2, sy + sh // 2), 900, 0.14)
    d = ImageDraw.Draw(canvas)

    lines = ["Your iPhone.", "Your PC's mic."]
    size = 240
    while max(d.textlength(t, font=font("bold", size)) for t in lines) > sw - 40:
        size -= 2
    head = font("bold", size)
    label = font("semibold", round(size * 0.36))
    icon_h = round(size * 0.62)
    line_h = [d.textbbox((0, 0), t, font=head)[3] - d.textbbox((0, 0), t, font=head)[1] for t in lines]
    label_h = d.textbbox((0, 0), "Wi-Fi", font=label)[3] - d.textbbox((0, 0), "Wi-Fi", font=label)[1]
    line_gap, row_gap, label_gap = round(size * 0.22), round(size * 0.55), round(size * 0.16)
    total = sum(line_h) + line_gap + row_gap + icon_h + label_gap + label_h
    top = sy + (sh - total) // 2
    y = text_block(d, lines, head, sx + sw / 2, top, (255, 255, 255), line_gap) - line_gap + row_gap

    items = [("wifi", "Wi-Fi"), ("bluetooth", "Bluetooth"), ("usb", "USB")]
    slot = sw // 3
    for i, (kind, text) in enumerate(items):
        cx = sx + slot * i + slot // 2
        mask = tab_icon(screens, kind, icon_h)
        canvas.paste(Image.new("RGB", mask.size, (255, 255, 255)), (round(cx - mask.width / 2), y), mask)
        text_block(d, [text], label, cx, y + icon_h + label_gap, (230, 240, 253), 0)
    return canvas


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads")
    raw = {k: Image.open(os.path.join(src, v)).convert("RGB") for k, v in SOURCES.items()
           if os.path.exists(os.path.join(src, v))}
    raw["usb"].paste(raw["wifi"].crop((0, 0, raw["wifi"].width, STATUS_BAR_HEIGHT)), (0, 0))
    cleaned = {k: clean(im, k) for k, im in raw.items() if k != "lock"}
    if "lock" in raw:
        cleaned["lock"] = lock_screen(raw["lock"], live_activity=False)  # shown enlarged on top instead
        lock_with_activity = lock_screen(raw["lock"])
    for folder, size in (("6.1", (1179, 2556)), ("6.9", (1320, 2868))):
        folder_path = os.path.join(OUT, "screenshots", folder)
        os.makedirs(folder_path, exist_ok=True)
        for i, (kind, headline, subline) in enumerate(SLIDES, 1):
            path = os.path.join(folder_path, f"{i}-{kind}.png")
            if kind not in cleaned:
                print("kept", path)
                continue
            # The Lock Screen buttons span y 2232-2408 and leave 148 px to the screen's bottom edge
            # (2556): the enlarged Live Activity ends the same 148 px above them.
            callout = (lock_with_activity, (40, 1934, 1139, 2171), 70, 2232 - (2556 - 2408)) if kind == "lock" else None
            screenshot(cleaned[kind], headline, subline, size, callout=callout).save(path)
    header(raw).save(os.path.join(OUT, "header-universal.png"))
    print("wrote", OUT)
