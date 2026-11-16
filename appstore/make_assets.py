"""App Store assets for Phomic, made from real iPhone screenshots.

    python appstore/make_assets.py <folder with the screenshots>

Expects the screenshots (1179 x 2556, iPhone with Dynamic Island) named in SOURCES below.
Writes to appstore/out/:
  screenshots/6.1/*.png   1179 x 2556  (iPhone with Dynamic Island, medium; required)
  screenshots/6.9/*.png   1320 x 2868  (iPhone with Dynamic Island, large)
  header-universal.png    5244 x 2950  (product page header + search results)
Before composing, the screenshots get a clean status bar (9:41) and a generic PC name.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
FONTS = r"C:\Windows\Fonts"
BLUE_TOP = (58, 151, 242)     # app icon gradient, #3A97F2
BLUE_BOTTOM = (20, 103, 214)  # #1467D6
PC_NAME = "Desktop PC"

SOURCES = {
    "wifi": "IMG_3977.PNG",
    "bluetooth": "IMG_3978.PNG",
    "settings": "IMG_3979.PNG",
    "lock": "IMG_3980.PNG",  # Lock Screen with the Live Activity (only its pieces are used)
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


def clean(im, kind):
    """Status bar 9:41 without the silent-mode bell; the PC's real name replaced."""
    im = im.convert("RGB").copy()
    d = ImageDraw.Draw(im)
    top_bg = im.getpixel((40, 90))
    # Time "07.15" (x 128-246, digits 69-106) and the bell (x 261-299).
    d.rectangle((110, 55, 315, 120), fill=top_bg)
    draw_on_baseline(d, "9:41", font_for_cap_height("semibold", 38), 107, (255, 255, 255), center_x=187)
    if kind in ("wifi", "bluetooth"):
        # "Live on <name>": capitals 846-879, centered on the card.
        d.rectangle((70, 836, 1110, 894), fill=im.getpixel((200, 860)))
        draw_on_baseline(d, f"Live on {PC_NAME}", font_for_cap_height("semibold", 34), 880, (255, 255, 255),
                         center_x=590)
    if kind == "settings":
        # The saved pairing code's row: capitals 950-983, from x 227.
        d.rectangle((220, 940, 1080, 996), fill=im.getpixel((700, 970)))
        draw_on_baseline(d, PC_NAME, font_for_cap_height("regular", 34), 984, (255, 255, 255), left_x=227)
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


def paste_glyph(dst, src, box, color=None, floor=60):
    """Copies light glyphs from src (box) onto dst without their background: alpha from brightness."""
    crop = src.crop(box)
    alpha = crop.convert("L").point(lambda v: max(0, min(255, (v - floor) * 255 // (255 - floor))))
    fill = Image.new("RGB", crop.size, color) if color else crop
    dst.paste(fill, box[:2], alpha)


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
        draw_on_baseline(d, PC_NAME, font_for_cap_height("regular", 31), 2102, name_color, left_x=344)

    # Flashlight and camera, and the home indicator.
    for cx, glyph in ((225, None), (955, "")):
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


def sound_rings(canvas, center, first, step, count, width, alpha):
    """Faint concentric rings: sound spreading from the phone."""
    layer = Image.new("L", canvas.size, 0)
    d = ImageDraw.Draw(layer)
    for i in range(count):
        r = first + i * step
        a = int(255 * alpha * (1 - i / count))
        d.ellipse((center[0] - r, center[1] - r, center[0] + r, center[1] + r), outline=a, width=width)
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
    ("bluetooth", ["Wi-Fi or", "Bluetooth"], "No cables. Switch while you talk."),
    ("settings", ["Private", "by design"], "Optional pairing code. No tracking."),
]


def screenshot(screen, headline, subline, size, whole_phone=False, callout=None):
    """whole_phone: show the bottom of the screen too (the Live Activity sits there).
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
    width = round(w * 0.84)
    if whole_phone:
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


def header(screen):
    """Universal asset (5244 x 2950, PNG): key art inside the safe area 1402 x 962 at (1921, 660)."""
    w, h = 5244, 2950
    sx, sy, sw, sh = 1921, 660, 1402, 962
    canvas = gradient(w, h, (52, 140, 236), (16, 92, 200))
    frame, pad = phone(screen, 436)  # the whole phone, 940 px tall: fits the safe area's height
    body_h = frame.height - 2 * pad
    px = sx + sw - 30 - (frame.width - pad)
    py = sy + (sh - body_h) // 2 - pad
    mic = (px + frame.width // 2, py + pad + round(body_h * 0.385))  # the big microphone button
    sound_rings(canvas, mic, 420, 210, 14, 10, 0.16)
    glow(canvas, mic, 700, 0.20)
    canvas.paste(frame, (px, py), frame)

    d = ImageDraw.Draw(canvas)
    left = sx + 40
    room = px + pad - 70 - left  # up to just before the phone
    size = 150
    while d.textlength("Your PC's mic.", font=font("bold", size)) > room:
        size -= 2
    head, sub = font("bold", size), font("semibold", round(size * 0.46))
    lines = [(t, head, (255, 255, 255), 36) for t in ("Your iPhone.", "Your PC's mic.")]
    lines.append(("Over Wi-Fi or Bluetooth.", sub, (225, 236, 252), 0))
    heights = [d.textbbox((0, 0), t, font=f)[3] - d.textbbox((0, 0), t, font=f)[1] for t, f, _, _ in lines]
    total = sum(heights) + 36 + 50  # gap between the headline lines, then before the subline
    top = sy + (sh - total) // 2
    for (text, fnt, fill, gap), height in zip(lines, heights):
        if fnt is sub:
            top += 50 - 36
        x0, y0, _, _ = d.textbbox((0, 0), text, font=fnt)
        d.text((left - x0, top - y0), text, font=fnt, fill=fill)
        top += height + 36
    return canvas


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads")
    cleaned = {k: clean(Image.open(os.path.join(src, v)), k) for k, v in SOURCES.items() if k != "lock"}
    lock_src = Image.open(os.path.join(src, SOURCES["lock"])).convert("RGB")
    cleaned["lock"] = lock_screen(lock_src, live_activity=False)  # shown enlarged on top instead
    lock_with_activity = lock_screen(lock_src)
    for folder, size in (("6.1", (1179, 2556)), ("6.9", (1320, 2868))):
        folder_path = os.path.join(OUT, "screenshots", folder)
        os.makedirs(folder_path, exist_ok=True)
        for old in os.listdir(folder_path):
            os.remove(os.path.join(folder_path, old))
        for i, (kind, headline, subline) in enumerate(SLIDES, 1):
            path = os.path.join(OUT, "screenshots", folder, f"{i}-{kind}.png")
            lock = kind == "lock"
            # The Lock Screen buttons span y 2232-2408 and leave 148 px to the screen's bottom edge
            # (2556): the enlarged Live Activity ends the same 148 px above them.
            callout = (lock_with_activity, (40, 1934, 1139, 2171), 70, 2232 - (2556 - 2408))
            screenshot(cleaned[kind], headline, subline, size, whole_phone=lock,
                       callout=callout if lock else None).save(path)
    header(cleaned["wifi"]).save(os.path.join(OUT, "header-universal.png"))
    print("wrote", OUT)
