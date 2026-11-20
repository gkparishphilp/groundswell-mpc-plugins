#!/usr/bin/env python3
"""Build Undertow's hand-made skin (plugins/Undertow/skins/) from the artwork in this folder.

    python3 plugins/Undertow/art/make_skin.py            # writes skins/ and build/skin-preview-*.png

Source art (Groundswell's own, made for Undertow):
    background-src.png      the panel, tube and title (any size with a 1280:628 aspect)
    knob_small.webp         knob with skirt, pointer at 12 o'clock, transparent background
    knob_large.webp         big scalloped knob, same
    lamp_on/off.webp        FEEDBACK SAFETY lamp
    pill_on/off.webp        button pill

Everything else (tick scales, headings, section boxes, value arcs, captions, filter curves) is drawn here.
The skin binds to parameter indices, so it must be regenerated if the parameter list changes.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
sys.path.insert(0, os.path.join(SDK, "tools"))
import gen_ui as G  # noqa: E402  (TUI.json building blocks)
from gen_ui import obj, Obj  # noqa: E402

OUT = os.path.join(PROJECT, "skins")
PREVIEW_DIR = os.path.join(PROJECT, "build")

W, H = 1280, 628
CREAM = (233, 223, 198)
CYAN = (63, 217, 227)
ORANGE = (255, 120, 40)
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match src/Undertow.cpp).
P = dict(output=0, sync=1, time=3, feedback=4, mix=5, pingpong=6, wobble=7, limit=8, filter=9, cutoff=10,
         reso=11, drive=12, fxtype=13, fxamount=14, fxrate=15, pmode=16, pitch=17, pmix=18, reverse=19, input=20,
         lfoshape=21, lfosync=22, lforate=23, spread=24, lfocut=25, lfopitch=26, lfofx=27, lfotime=28,
         envcut=29, envfx=30, envspeed=31, duck=32, freeze=33, space=34, sdecay=35, stone=36, drip=37, spos=38,
         character=39, age=40, head2=41, hratio=42, h2pan=43, tube=44, level=45, echo=46,
         noise=110, ntype=111, lfonoise=112, mains=113, glide=114, cross=115,
         swirl=116, swmode=117, swrate=118, swdir=119, swwidth=120, swdepth=121, orbit=122)
CHOICES = {1: ["Free", "Sync"], 6: ["Off", "On"], 9: ["Off", "LP", "BP", "HP"],
           13: ["Off", "Chorus", "Flanger", "Phaser", "Bitcrush", "Downsample", "Ring Mod", "Fuzz"],
           16: ["Semitones", "Hz"], 19: ["Off", "On"],
           21: ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Drift"], 22: ["Free", "Sync"],
           33: ["Off", "On"], 38: ["After", "In Loop"], 39: ["Digital", "Tape", "BBD"],
           42: ["1/4", "1/3", "1/2", "2/3", "3/4", "1/1", "4/3", "3/2", "2/1"], 44: ["Off", "On"],
           111: ["Hiss", "Air", "Crackle", "Hum", "Rain"], 113: ["60 Hz", "50 Hz"],
           117: ["Free", "Sync", "Echo"], 119: ["Clockwise", "Counter"]}
HEAD_RATIOS = [0.25, 1 / 3, 0.5, 2 / 3, 0.75, 1.0, 4 / 3, 1.5, 2.0]

# The echo trail in the centre of the HEADS page: 64 bars, each one parameter, drawn from one film strip of
# 3 colours x (dim, lit) x 16 heights. The colour says which head fed the bar; the plugin picks it.
ECHO_BARS, ECHO_LEVELS, ECHO_COLOURS = 64, 16, 3
ECHO_AREA = (306, 272, 568, 96)           # x y w h of the bars themselves
ECHO_BAR_W = 7
ECHO_HUES = (CYAN, (255, 176, 92), (178, 214, 190))     # head 1, head 2, both

# Knob geometry. Film strips stay under 11430 px tall (the SDK kit's proven size).
SMALL = dict(src="knob_small.webp", crop=300, knob=78, arc_r=46, arc_w=4, frame=100, frames=110, ticks=(54, 61))
LARGE = dict(src="knob_large.webp", crop=485, knob=112, arc_r=63, arc_w=5, frame=136, frames=84, ticks=(72, 82))
KNOB_CENTRE_SRC = (623.5, 625.5)

VALUE_COLOUR = "ffe9dfc6"
READOUT_COLOUR = "ff5fe6ee"
VALUE_FONT = "Nexa"


# ------------------------------------------------------------------------------------------------
# Drawing helpers
# ------------------------------------------------------------------------------------------------
def font(size, face=FONT_DEMI):
    return ImageFont.truetype(FONT_FILE, size, index=face)


def text_width(text, f, spacing):
    return sum(f.getlength(ch) for ch in text) + spacing * (len(text) - 1)


def draw_spaced(draw, xy, text, f, fill, spacing=2.0, anchor="mm"):
    """Letter-spaced text centred on xy (anchor 'mm') or left-aligned at xy ('lm')."""
    x, y = xy
    tw = text_width(text, f, spacing)
    if anchor == "mm":
        x -= tw / 2
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill, anchor="lm")
        x += f.getlength(ch) + spacing


def supersampled(size, scale, painter):
    """Run painter(draw, scale) on a transparent canvas `scale` times larger, return it downsampled."""
    big = Image.new("RGBA", (size[0] * scale, size[1] * scale), (0, 0, 0, 0))
    painter(ImageDraw.Draw(big), scale)
    return big.resize(size, Image.LANCZOS)


def clean_alpha(im):
    """Drop the faint haze left by background removal and make near-opaque pixels opaque."""
    r, g, b, a = im.split()
    a = a.point(lambda v: 0 if v < 16 else (255 if v > 250 else v))
    return Image.merge("RGBA", (r, g, b, a))


def load(name):
    return clean_alpha(Image.open(os.path.join(HERE, name)).convert("RGBA"))


# ------------------------------------------------------------------------------------------------
# Knob film strips: rotated knob + value arc
# ------------------------------------------------------------------------------------------------
def knob_strip(spec, bipolar=False):
    src = load(spec["src"])
    cx, cy = KNOB_CENTRE_SRC
    c = spec["crop"]
    body = src.crop((int(cx - c), int(cy - c), int(cx + c), int(cy + c)))
    ss = 4
    knob_px = spec["knob"] * ss
    body = body.resize((knob_px, knob_px), Image.LANCZOS)
    frame, n = spec["frame"], spec["frames"]
    strip = Image.new("RGBA", (frame, frame * n), (0, 0, 0, 0))
    for i in range(n):
        v = i / (n - 1)
        angle = -135.0 + 270.0 * v                       # degrees clockwise from 12 o'clock
        big = Image.new("RGBA", (frame * ss, frame * ss), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        m = frame * ss / 2
        r = spec["arc_r"] * ss
        box = (m - r, m - r, m + r, m + r)
        w = spec["arc_w"] * ss
        d.arc(box, -135 - 90, 135 - 90, fill=(255, 255, 255, 34), width=w)          # dim track
        a0, a1 = (0.0, angle) if bipolar else (-135.0, angle)
        if a1 < a0:
            a0, a1 = a1, a0
        if a1 - a0 > 0.5:
            glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
            ImageDraw.Draw(glow).arc(box, a0 - 90, a1 - 90, fill=CYAN + (200,), width=w * 2)
            big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(3 * ss)))
            ImageDraw.Draw(big).arc(box, a0 - 90, a1 - 90, fill=CYAN + (255,), width=w)
        rot = body.rotate(-angle, resample=Image.BICUBIC)
        off = int(m - knob_px / 2)
        big.alpha_composite(rot, (off, off))
        strip.alpha_composite(big.resize((frame, frame), Image.LANCZOS), (0, i * frame))
    return strip


def draw_ticks(draw, centre, ticks, scale, count=27, major_every=13):
    r0, r1 = ticks
    cx, cy = centre
    for i in range(count):
        a = math.radians(-135 + 270 * i / (count - 1))
        major = i % major_every == 0 or i == count - 1
        inner = r0 - (3 if major else 0)
        alpha = 200 if major else 120
        sx, sy = math.sin(a), -math.cos(a)
        draw.line([((cx + inner * sx) * scale, (cy + inner * sy) * scale),
                   ((cx + r1 * sx) * scale, (cy + r1 * sy) * scale)],
                  fill=CREAM + (alpha,), width=int((2 if major else 1.4) * scale))


# ------------------------------------------------------------------------------------------------
# Pills (3-slice stretch so every width keeps round ends) and the lamp
# ------------------------------------------------------------------------------------------------
PILL_BODY = {"pill_on.webp": (446, 207, 1553, 465), "pill_off.webp": (427, 201, 1568, 465)}
PILL_H, PILL_MARGIN = 34, 9


def pill_base(name, width):
    src = load(name)
    x0, y0, x1, y1 = PILL_BODY[name]
    s = PILL_H / (y1 - y0)
    scaled = src.resize((round(src.width * s), round(src.height * s)), Image.LANCZOS)
    X0, Y0, X1, Y1 = x0 * s, y0 * s, x1 * s, y1 * s
    cap = (Y1 - Y0) / 2 + 4
    m = PILL_MARGIN
    top = round(Y0 - m)
    out = Image.new("RGBA", (width + 2 * m, PILL_H + 2 * m), (0, 0, 0, 0))
    hgt = out.height
    left = scaled.crop((round(X0 - m), top, round(X0 + cap), top + hgt))
    right = scaled.crop((round(X1 - cap), top, round(X1 + m), top + hgt))
    mid_src = scaled.crop((round((X0 + X1) / 2) - 2, top, round((X0 + X1) / 2) + 2, top + hgt))
    mid_w = out.width - left.width - right.width
    out.alpha_composite(left, (0, 0))
    if mid_w > 0:
        out.alpha_composite(mid_src.resize((mid_w, hgt), Image.BILINEAR), (left.width, 0))
    out.alpha_composite(right, (out.width - right.width, 0))
    return out


def pill(state, caption, width, size=14):
    im = pill_base("pill_on.webp" if state else "pill_off.webp", width)
    d = ImageDraw.Draw(im)
    colour = (200, 248, 252) if state else (190, 182, 162)
    text, spacing = caption.upper(), 1.2
    while size > 9 and text_width(text, font(size, FONT_DEMI), spacing) > width - 14:
        size -= 1
        spacing = 0.6
    f = font(size, FONT_DEMI)
    draw_spaced(d, (im.width / 2, im.height / 2 + 1), text, f, colour + (255,), spacing=spacing)
    return im


LAMP_BODY = {"lamp_on.webp": (269, 482, 749, 1025), "lamp_off.webp": (282, 509, 740, 1014)}
LAMP_W = 100


def lamp(state):
    name = "lamp_on.webp" if state else "lamp_off.webp"
    src = load(name)
    x0, y0, x1, y1 = LAMP_BODY["lamp_on.webp"]
    lamp_h = round(LAMP_W * (y1 - y0) / (x1 - x0))
    if not state:   # stretch the unlit body onto the lit one's box so they swap without a jump
        bx0, by0, bx1, by1 = LAMP_BODY[name]
        sx, sy = (x1 - x0) / (bx1 - bx0), (y1 - y0) / (by1 - by0)
        src = src.resize((round(src.width * sx), round(src.height * sy)), Image.LANCZOS)
        dx, dy = x0 - bx0 * sx, y0 - by0 * sy
        canvas = Image.new("RGBA", (1024, 1536), (0, 0, 0, 0))
        canvas.alpha_composite(src.crop((0, 0, min(src.width, 1024), min(src.height, 1536))), (round(dx), round(dy)))
        src = canvas
    pad = 0.06 * (x1 - x0)
    crop = src.crop((round(x0 - pad), round(y0 - pad), round(x1 + pad), round(y1 + pad)))
    s = LAMP_W / (x1 - x0)
    return crop.resize((round(crop.width * s), round(crop.height * s)), Image.LANCZOS)


TUBE_BOX = (400, 28, 480, 176)      # x y w h: the tube and its glow, in tab coordinates
TUBE_FRAMES = 16


def tube_strip():
    import PIL.ImageChops as C
    """Frame 0 is the panel exactly as painted; later frames add heat to the tube's own glow."""
    x, y, w, h = TUBE_BOX
    base = base_plate().crop((x, y, x + w, y + h)).convert("RGB")
    a = base.split()
    warm = Image.eval(Image.merge("L", (a[0],)), lambda v: v)     # red channel
    cold = Image.eval(Image.merge("L", (a[2],)), lambda v: v)     # blue channel
    mask = C.subtract(warm, cold)                                  # emissive pixels only
    mask = mask.point(lambda v: min(255, int(v * 2.2)))
    halo = mask.filter(ImageFilter.GaussianBlur(26))
    core = mask.filter(ImageFilter.GaussianBlur(5))
    # Feather towards the edges of the box, so the lit tube blends into the painted panel with no visible seam.
    feather = Image.new("L", (w, h), 0)
    ImageDraw.Draw(feather).rectangle([44, 30, w - 45, h - 31], fill=255)
    feather = feather.filter(ImageFilter.GaussianBlur(22))
    halo = C.multiply(halo, feather)
    core = C.multiply(core, feather)
    strip = Image.new("RGB", (w, h * TUBE_FRAMES))
    for i in range(TUBE_FRAMES):
        t = i / (TUBE_FRAMES - 1)
        frame = base.copy()
        for layer, tint, amount in ((halo, (255, 112, 30), 0.85), (core, (255, 196, 110), 0.65)):
            glow = Image.new("RGB", (w, h), tint)
            frame = C.add(frame, C.multiply(glow, Image.merge("RGB", (layer, layer, layer))
                                            .point(lambda v, t=t, k=amount: int(v * t * k))))
        strip.paste(frame, (0, i * h))
    return strip


def echo_bar_strip():
    """Per colour (head 1, head 2, both): 16 dim heights, then the same 16 lit (the newest bar).
    A bar keeps its colour as it dims, so the whole trail shows which head each peak came off."""
    h, w = ECHO_AREA[3], ECHO_BAR_W
    bank = 2 * ECHO_LEVELS
    strip = Image.new("RGBA", (w, h * ECHO_COLOURS * bank), (0, 0, 0, 0))
    for f in range(ECHO_COLOURS * bank):
        colour, within = divmod(f, bank)
        level, lit = within % ECHO_LEVELS, within >= ECHO_LEVELS
        bar_h = max(4, round(level / (ECHO_LEVELS - 1) * h))
        hue = ECHO_HUES[colour]
        bright = tuple(min(255, int(c + (255 - c) * 0.55)) for c in hue)

        def paint(d, sc, bar_h=bar_h, lit=lit, level=level, hue=hue, bright=bright):
            top = (h - bar_h) * sc
            body = bright if lit else hue
            alpha = 255 if lit else 190
            d.rounded_rectangle([1 * sc, top, (w - 1) * sc, h * sc], radius=2 * sc, fill=body + (alpha,))
            if level > 0:                      # a brighter cap, white-hot when the bar is near the top
                cap = (255, 248, 236) if level >= ECHO_LEVELS - 2 else bright
                d.rounded_rectangle([1 * sc, top, (w - 1) * sc, top + 3 * sc], radius=2 * sc, fill=cap + (255,))
        frame = supersampled((w, h), 3, paint)
        if lit:
            frame = Image.alpha_composite(frame.filter(ImageFilter.GaussianBlur(2)), frame)
        strip.alpha_composite(frame, (0, f * h))
    return strip


def lamp_strip(frames=25):
    """Frame k for Feedback k/(frames-1) of 0..120%: lit from 100% (frame 20 of 25)."""
    on, off = lamp(True), lamp(False)
    strip = Image.new("RGBA", (on.width, on.height * frames), (0, 0, 0, 0))
    for k in range(frames):
        lit = k / (frames - 1) * 120.0 >= 99.0
        strip.alpha_composite(on if lit else off, (0, k * on.height))
    return strip


# ------------------------------------------------------------------------------------------------
# Filter response curves (match the DSP: SVF, k = 2 - 1.98 * reso, loop gain compensation)
# ------------------------------------------------------------------------------------------------
GRAPH = (626, 492, 278, 92)            # x y w h of the plot on page 1
GRAPH_DB = (-24.0, 12.0)


def response_db(mode, f, fc, reso=0.35):
    k = 2.0 - 1.98 * reso
    peak = 1.0 + 0.5 * reso
    w = f / fc
    re, im = 1.0 - w * w, k * w
    den = math.hypot(re, im)
    if mode == "LP":
        mag = min(1.0, peak * k) / den
    elif mode == "HP":
        mag = min(1.0, peak * k) * w * w / den
    elif mode == "BP":
        mag = peak * k * w / den
    else:
        mag = 1.0
    return 20 * math.log10(max(mag, 1e-6))


def graph_y(db):
    lo, hi = GRAPH_DB
    db = max(lo, min(hi, db))
    return (hi - db) / (hi - lo)


def graph_x(f):
    return (math.log10(f) - math.log10(20)) / (math.log10(20000) - math.log10(20))


def curve_image(mode, fc):
    gw, gh = GRAPH[2], GRAPH[3]

    def paint(d, s):
        pts = []
        for i in range(0, gw * s + 1, 2):
            f = 20 * 10 ** (3 * i / (gw * s))
            pts.append((i, graph_y(response_db(mode, f, fc)) * gh * s))
        fill = pts + [(gw * s, gh * s), (0, gh * s)]
        d.polygon(fill, fill=CYAN + (38,))
        d.line(pts, fill=CYAN + (255,), width=int(2.2 * s), joint="curve")
    return supersampled((gw, gh), 3, paint)


def curve_strip(mode, frames=48):
    gw, gh = GRAPH[2], GRAPH[3]
    strip = Image.new("RGBA", (gw, gh * frames), (0, 0, 0, 0))
    for i in range(frames):
        fc = 40.0 * (16000.0 / 40.0) ** (i / (frames - 1))   # Cutoff is log 40 Hz .. 16 kHz
        strip.alpha_composite(curve_image(mode, fc), (0, i * gh))
    return strip


# ------------------------------------------------------------------------------------------------
# Layout (tab coordinates, 1280 x 628). Rows: A = beside the tube, B = middle, C = bottom box.
# ------------------------------------------------------------------------------------------------
ROW_A_Y, ROW_B_Y, ROW_C_Y = 140, 340, 522
CENTRE_BOX = (400, 236, 880, 440)
BOTTOM_BOX = (50, 452, 1230, 602)
C_DIVIDERS = (330, 600, 930)


def small_knob(param, x, y, heading, bipolar=False, heading_pos="above"):
    return dict(kind="knob", size="small", param=param, centre=(x, y), heading=heading, bipolar=bipolar,
                heading_pos=heading_pos)


def large_knob(param, x, y, heading):
    return dict(kind="knob", size="large", param=param, centre=(x, y), heading=heading, bipolar=False,
                heading_pos="above")


def pills(param, items, heading=None, heading_xy=None, toggle=False):
    """items: list of (choice index, caption, x, y, width) in tab coordinates (x, y = top-left of the body).
    toggle: a single on/off button for a bool (tap flips it)."""
    return dict(kind="pills", param=param, items=items, heading=heading, heading_xy=heading_xy, toggle=toggle)


def readout(param, box, size, overlay, case="Original"):
    return dict(kind="readout", param=param, box=box, size=size, overlay=overlay, case=case)


def page_delay():
    bx0, by0, bx1, by1 = CENTRE_BOX
    mid = (bx0 + bx1) / 2
    controls = [
        small_knob(P["input"], 150, ROW_A_Y, "INPUT"),
        small_knob(P["glide"], 318, ROW_A_Y, "GLIDE"),
        small_knob(P["mix"], 962, ROW_A_Y, "DRY / WET"),
        small_knob(P["output"], 1130, ROW_A_Y, "OUTPUT"),

        large_knob(P["time"], 168, ROW_B_Y, "DELAY TIME"),
        pills(P["sync"], [(1, "Sync", 292, 312, 88), (0, "Free", 292, 356, 88)], "TIME MODE", (336, 290)),
        readout(P["time"], (bx0 + 20, by0 + 28, bx1 - bx0 - 40, 64), 58, "knob overlay"),
        pills(P["character"], [(0, "Digital", 416, 342, 128), (1, "Tape", 552, 342, 128), (2, "BBD", 712, 342, 150)]),
        pills(P["pingpong"], [(0, "Straight", 416, 392, 128), (1, "Ping-Pong", 552, 392, 128)]),
        pills(P["freeze"], [(1, "Freeze", 712, 392, 150)], toggle=True),
        large_knob(P["feedback"], 1000, ROW_B_Y, "FEEDBACK"),

        small_knob(P["cutoff"], 125, ROW_C_Y, "CUTOFF", heading_pos="right"),
        pills(P["filter"], [(0, "Off", 360, 488, 96), (1, "LP", 474, 488, 96),
                            (2, "BP", 360, 534, 96), (3, "HP", 474, 534, 96)], None),
        small_knob(P["reso"], 1155, ROW_C_Y, "RESONANCE", heading_pos="left"),
    ]
    statics = dict(headings=[("FILTER", (465, 470)), ("RESPONSE", (765, 470))],
                   centre_title="DELAY TIME", dividers_b=(281, 890), waveform=False, graph=True)
    qlinks = [[P["input"], P["glide"], P["mix"], P["output"]],
              [P["time"], P["sync"], P["character"], P["feedback"]],
              [P["cutoff"], P["filter"], P["reso"], -1]]
    return "DELAY", controls, statics, qlinks


def page_loop():
    bx0, by0, bx1, by1 = CENTRE_BOX
    fx_names = ["Off", "Chorus", "Flanger", "Phaser", "Bitcrush", "Downsample", "Ring Mod", "Fuzz"]
    pw, gap = 108, 8
    x0 = (bx0 + bx1) / 2 - (4 * pw + 3 * gap) / 2
    fx_items = [(i, fx_names[i], x0 + (i % 4) * (pw + gap), 336 + (i // 4) * 46, pw) for i in range(8)]
    controls = [
        small_knob(P["wobble"], 150, ROW_A_Y, "WOBBLE"),
        small_knob(P["limit"], 318, ROW_A_Y, "LIMIT"),
        small_knob(P["mix"], 962, ROW_A_Y, "DRY / WET"),
        small_knob(P["output"], 1130, ROW_A_Y, "OUTPUT"),

        large_knob(P["fxamount"], 168, ROW_B_Y, "FX AMOUNT"),
        small_knob(P["drive"], 334, ROW_B_Y + 6, "DRIVE"),
        readout(P["fxtype"], (bx0 + 20, by0 + 32, bx1 - bx0 - 40, 56), 46, "menu overlay", case="Upper"),
        pills(P["fxtype"], fx_items),
        large_knob(P["fxrate"], 1000, ROW_B_Y, "FX RATE"),

        small_knob(P["pitch"], 125, ROW_C_Y, "PITCH", bipolar=True, heading_pos="right"),
        pills(P["pmode"], [(0, "Semitones", 345, 512, 118), (1, "Hz", 471, 512, 118)], None),
        pills(P["reverse"], [(0, "Forward", 640, 512, 118), (1, "Reverse", 772, 512, 118)], None),
        small_knob(P["pmix"], 1155, ROW_C_Y, "PITCH MIX", heading_pos="left"),
    ]
    statics = dict(headings=[("SHIFT MODE", (465, 482)), ("DIRECTION", (765, 482))],
                   centre_title="LOOP FX", dividers_b=(), waveform=False, graph=False)
    qlinks = [[P["wobble"], P["limit"], P["mix"], P["output"]],
              [P["fxamount"], P["drive"], P["fxtype"], P["fxrate"]],
              [P["pitch"], P["pmode"], P["reverse"], P["pmix"]]]
    return "LOOP", controls, statics, qlinks


# The head-2 marker sits on the trail: the window is two delay times wide, so head 2 at Head Ratio r
# stands at r / 2 across it.
MARK_BOX = (ECHO_AREA[0], ECHO_AREA[1] - 4, ECHO_AREA[2], ECHO_AREA[3] + 28)


def head_mark_strip():
    gw, gh = MARK_BOX[2], MARK_BOX[3]
    strip = Image.new("RGBA", (gw, gh * len(HEAD_RATIOS)), (0, 0, 0, 0))
    hue = ECHO_HUES[1]
    for i, ratio in enumerate(HEAD_RATIOS):
        def paint(d, sc, ratio=ratio):
            x = min(gw - 9.0, max(9.0, ratio / 2.0 * gw))
            for y in range(2, gh - 30, 9):           # a dashed line, so it reads behind the bars
                d.line([(x * sc, y * sc), (x * sc, (y + 5) * sc)], fill=hue + (150,), width=int(1.4 * sc))
            top = gh - 15
            d.rounded_rectangle([(x - 8) * sc, top * sc, (x + 8) * sc, gh * sc], radius=3 * sc,
                                fill=(12, 12, 12, 225), outline=hue + (255,), width=int(1.2 * sc))
            d.text((x * sc, (top + 7) * sc), "2", font=font(11 * sc, FONT_DEMI), fill=hue + (255,), anchor="mm")
        strip.alpha_composite(supersampled((gw, gh), 3, paint), (0, i * gh))
    return strip


# The orbit on the SWIRL page: the listener seen from above (nose up = straight ahead), a ring round them, and
# a dot the plugin moves round it. Orbit parameter: frames 0..31 = k/32 of a turn clockwise from the front with
# the tail trailing a clockwise turn, 32..63 the same positions with the tail the other way (Counter).
ORBIT_POSITIONS = 32
ORBIT_FRAMES = 2 * ORBIT_POSITIONS
ORBIT_CENTRE, ORBIT_R = (520, 342), 70
ORBIT_BOX = (ORBIT_CENTRE[0] - ORBIT_R - 18, ORBIT_CENTRE[1] - ORBIT_R - 18, 2 * ORBIT_R + 36, 2 * ORBIT_R + 36)


def orbit_strip():
    bw, bh = ORBIT_BOX[2], ORBIT_BOX[3]
    c = bw / 2
    strip = Image.new("RGBA", (bw, bh * ORBIT_FRAMES), (0, 0, 0, 0))
    for f in range(ORBIT_FRAMES):
        k, sign = f % ORBIT_POSITIONS, (1 if f < ORBIT_POSITIONS else -1)

        def paint(d, sc, k=k, sign=sign):
            for back in range(6, 0, -1):     # a short comet tail, fading, behind the direction of travel
                a = 2 * math.pi * (k - sign * back * 0.16) / ORBIT_POSITIONS
                x, y = c + ORBIT_R * math.sin(a), c - ORBIT_R * math.cos(a)
                r = 7 - back * 0.7
                d.ellipse([(x - r) * sc, (y - r) * sc, (x + r) * sc, (y + r) * sc], fill=CYAN + (int(150 - 22 * back),))
            a = 2 * math.pi * k / ORBIT_POSITIONS
            x, y = c + ORBIT_R * math.sin(a), c - ORBIT_R * math.cos(a)
            d.ellipse([(x - 8) * sc, (y - 8) * sc, (x + 8) * sc, (y + 8) * sc], fill=CYAN + (255,))
            d.ellipse([(x - 3.5) * sc, (y - 3.5) * sc, (x + 3.5) * sc, (y + 3.5) * sc], fill=(225, 252, 255, 255))
        frame = supersampled((bw, bh), 3, paint)
        frame = Image.alpha_composite(frame.filter(ImageFilter.GaussianBlur(4)), frame)
        strip.alpha_composite(frame, (0, f * bh))
    return strip


def page_heads():
    bx0, by0, bx1, by1 = CENTRE_BOX
    pw, gap = 80, 6
    items = []
    names = CHOICES[P["hratio"]]
    for i, name in enumerate(names):
        row, col = (0, i) if i < 5 else (1, i - 5)
        count = 5 if row == 0 else 4
        x0 = 556 - (count * pw + (count - 1) * gap) / 2
        items.append((i, name, x0 + col * (pw + gap), 488 + row * 46, pw))
    controls = [
        small_knob(P["wobble"], 150, ROW_A_Y, "WOBBLE"),
        small_knob(P["age"], 318, ROW_A_Y, "AGE"),
        small_knob(P["mix"], 962, ROW_A_Y, "DRY / WET"),
        small_knob(P["output"], 1130, ROW_A_Y, "OUTPUT"),

        large_knob(P["time"], 168, ROW_B_Y, "DELAY TIME"),
        pills(P["tube"], [(1, "Display", 566, 400, 148)], toggle=True),
        large_knob(P["feedback"], 1000, ROW_B_Y, "FEEDBACK"),

        small_knob(P["head2"], 125, ROW_C_Y, "HEAD 2", heading_pos="right"),
        pills(P["hratio"], items),
        small_knob(P["cross"], 900, ROW_C_Y, "CROSS"),
        small_knob(P["h2pan"], 1155, ROW_C_Y, "HEAD 2 PAN", bipolar=True),
    ]
    statics = dict(headings=[("HEAD RATIO", (556, 462))], centre_title=None, dividers_b=(281, 890),
                   dividers_c=(330, 800, 1030), waveform=False, graph=False, echo=True,
                   images=[dict(file="head_mark.png", frames=len(HEAD_RATIOS), param=P["hratio"], box=MARK_BOX,
                                invalidating=["NonZero/Parameter %d" % P["head2"],
                                              "IndexedEnabling/1/2/Parameter %d" % P["tube"]])])
    qlinks = [[P["wobble"], P["age"], P["mix"], P["output"]],
              [P["time"], -1, P["tube"], P["feedback"]],
              [P["head2"], P["hratio"], P["cross"], P["h2pan"]]]
    return "HEADS", controls, statics, qlinks


def page_mod():
    bx0, by0, bx1, by1 = CENTRE_BOX
    shapes = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Drift"]
    pw, gap = 108, 8
    mid = (bx0 + bx1) / 2
    items = []
    for i, name in enumerate(shapes):
        row, col = divmod(i, 4)
        count = 4 if row == 0 else 3
        x0 = mid - (count * pw + (count - 1) * gap) / 2
        items.append((i, name, x0 + col * (pw + gap), 336 + row * 46, pw))
    sections = (50, 345, 640, 935)
    controls = [
        small_knob(P["envspeed"], 150, ROW_A_Y, "ENV SPEED"),
        small_knob(P["envcut"], 318, ROW_A_Y, "ENV › CUTOFF", bipolar=True),
        small_knob(P["envfx"], 962, ROW_A_Y, "ENV › FX", bipolar=True),
        small_knob(P["duck"], 1130, ROW_A_Y, "DUCK"),

        large_knob(P["lforate"], 168, ROW_B_Y, "LFO RATE"),
        pills(P["lfosync"], [(1, "Sync", 292, 312, 88), (0, "Free", 292, 356, 88)], "LFO MODE", (336, 290)),
        readout(P["lforate"], (bx0 + 20, by0 + 32, bx1 - bx0 - 40, 56), 50, "knob overlay"),
        pills(P["lfoshape"], items),
        large_knob(P["spread"], 1000, ROW_B_Y, "SPREAD"),
    ] + [small_knob(P[k], x0 + 78, ROW_C_Y, h, bipolar=True, heading_pos="right")
         for x0, k, h in zip(sections, ("lfocut", "lfopitch", "lfofx", "lfotime"),
                             ("LFO › CUTOFF", "LFO › PITCH", "LFO › FX", "LFO › TIME"))]
    statics = dict(headings=[], centre_title="LFO", dividers_b=(281, 890), dividers_c=sections[1:],
                   waveform=False, graph=False)
    qlinks = [[P["envspeed"], P["envcut"], P["envfx"], P["duck"]],
              [P["lforate"], P["lfosync"], P["lfoshape"], P["spread"]],
              [P["lfocut"], P["lfopitch"], P["lfofx"], P["lfotime"]]]
    return "MOD", controls, statics, qlinks


def page_space():
    controls = [
        small_knob(P["drip"], 150, ROW_A_Y, "DRIP"),
        small_knob(P["stone"], 318, ROW_A_Y, "TONE"),
        small_knob(P["mix"], 962, ROW_A_Y, "DRY / WET"),
        small_knob(P["output"], 1130, ROW_A_Y, "OUTPUT"),

        large_knob(P["space"], 168, ROW_B_Y, "SPACE"),
        pills(P["spos"], [(0, "After", 292, 312, 88), (1, "In Loop", 292, 356, 88)], "POSITION", (336, 290)),
        pills(P["freeze"], [(1, "Freeze", 530, 386, 220)], toggle=True),
        large_knob(P["sdecay"], 1000, ROW_B_Y, "DECAY"),

        small_knob(P["cutoff"], 125, ROW_C_Y, "CUTOFF", heading_pos="right"),
        pills(P["filter"], [(0, "Off", 360, 488, 96), (1, "LP", 474, 488, 96),
                            (2, "BP", 360, 534, 96), (3, "HP", 474, 534, 96)], None),
        small_knob(P["reso"], 1155, ROW_C_Y, "RESONANCE", heading_pos="left"),
    ]
    statics = dict(headings=[("FILTER", (465, 470)), ("RESPONSE", (765, 470))],
                   centre_title="SPRING", dividers_b=(281, 890), waveform=False, graph=True, coil=True)
    qlinks = [[P["drip"], P["stone"], P["mix"], P["output"]],
              [P["space"], P["spos"], P["freeze"], P["sdecay"]],
              [P["cutoff"], P["filter"], P["reso"], -1]]
    return "SPACE", controls, statics, qlinks


def page_tape():
    bx0, by0, bx1, by1 = CENTRE_BOX
    mid = (bx0 + bx1) / 2
    chr_names = CHOICES[P["character"]]
    cw, cgap = 128, 8
    cx0 = mid - (len(chr_names) * cw + (len(chr_names) - 1) * cgap) / 2
    chr_items = [(i, nm, cx0 + i * (cw + cgap), 300, cw) for i, nm in enumerate(chr_names)]
    nz_names = CHOICES[P["ntype"]]
    nw, ngap = 88, 6
    nx0 = mid - (len(nz_names) * nw + (len(nz_names) - 1) * ngap) / 2
    nz_items = [(i, nm, nx0 + i * (nw + ngap), 364, nw) for i, nm in enumerate(nz_names)]
    controls = [
        small_knob(P["age"], 150, ROW_A_Y, "AGE"),
        small_knob(P["wobble"], 318, ROW_A_Y, "WOBBLE"),
        small_knob(P["mix"], 962, ROW_A_Y, "DRY / WET"),
        small_knob(P["output"], 1130, ROW_A_Y, "OUTPUT"),

        large_knob(P["noise"], 168, ROW_B_Y, "NOISE"),
        pills(P["character"], chr_items),
        pills(P["ntype"], nz_items, "NOISE", (nx0, 344)),
        large_knob(P["feedback"], 1000, ROW_B_Y, "FEEDBACK"),

        small_knob(P["lfonoise"], 125, ROW_C_Y, "LFO > NOISE", bipolar=True, heading_pos="right"),
        small_knob(P["drive"], 430, ROW_C_Y, "DRIVE", heading_pos="right"),
        small_knob(P["limit"], 800, ROW_C_Y, "LIMIT", heading_pos="left"),
        pills(P["mains"], [(0, "60 Hz", 1050, 488, 110), (1, "50 Hz", 1050, 534, 110)]),
    ]
    statics = dict(headings=[("MAINS", (1105, 470))], centre_title="CHARACTER", dividers_b=(281, 890),
                   dividers_c=(330, 600, 930),
                   waveform=False, graph=False)
    qlinks = [[P["age"], P["wobble"], P["mix"], P["output"]],
              [P["noise"], P["character"], P["ntype"], P["feedback"]],
              [P["lfonoise"], P["drive"], P["limit"], P["mains"]]]
    return "TAPE", controls, statics, qlinks


def page_swirl():
    controls = [
        small_knob(P["swdepth"], 150, ROW_A_Y, "DEPTH"),
        small_knob(P["swwidth"], 318, ROW_A_Y, "WIDTH"),
        small_knob(P["mix"], 962, ROW_A_Y, "DRY / WET"),
        small_knob(P["output"], 1130, ROW_A_Y, "OUTPUT"),

        large_knob(P["swirl"], 168, ROW_B_Y, "SWIRL"),
        pills(P["swmode"], [(2, "Echo", 292, 296, 88), (1, "Sync", 292, 340, 88), (0, "Free", 292, 384, 88)],
              "MODE", (336, 276)),
        readout(P["swrate"], (612, 262, 250, 56), 40, "knob overlay"),
        pills(P["swdir"], [(0, "Clockwise", 612, 370, 118), (1, "Counter", 744, 370, 118)], "DIRECTION", (737, 348)),
        large_knob(P["swrate"], 1000, ROW_B_Y, "RATE"),

        small_knob(P["cross"], 125, ROW_C_Y, "CROSS", heading_pos="right"),
        pills(P["pingpong"], [(0, "Straight", 345, 512, 118), (1, "Ping-Pong", 471, 512, 118)], None),
        pills(P["tube"], [(1, "Display", 691, 512, 148)], toggle=True),
        small_knob(P["h2pan"], 1155, ROW_C_Y, "HEAD 2 PAN", bipolar=True, heading_pos="left"),
    ]
    statics = dict(headings=[("ECHOES", (465, 482)), ("ORBIT", (765, 482))], centre_title=None,
                   dividers_b=(281, 890), waveform=False, graph=False, orbit=True,
                   images=[dict(file="orbit.png", frames=ORBIT_FRAMES, param=P["orbit"], box=ORBIT_BOX,
                                invalidating=["NonZero/Parameter %d" % P["swirl"],
                                              "IndexedEnabling/1/2/Parameter %d" % P["tube"]])])
    qlinks = [[P["swdepth"], P["swwidth"], P["mix"], P["output"]],
              [P["swirl"], P["swmode"], P["swdir"], P["swrate"]],
              [P["cross"], P["pingpong"], P["tube"], P["h2pan"]]]
    return "SWIRL", controls, statics, qlinks


LAMP_POS = (1118, 284)
QLINK_RECTS = ["36 46 1208 180", "36 230 1208 216", "46 448 1188 158"]


# ------------------------------------------------------------------------------------------------
# Backgrounds
# ------------------------------------------------------------------------------------------------
def base_plate():
    src = Image.open(os.path.join(HERE, "background-src.png")).convert("RGB")
    return src.resize((W, H), Image.LANCZOS).convert("RGBA")


def heading_xy(ctl):
    spec = SMALL if ctl["size"] == "small" else LARGE
    cx, cy = ctl["centre"]
    r1 = spec["ticks"][1]
    if ctl["heading_pos"] == "above":
        return (cx, cy - r1 - 14), "mm"
    if ctl["heading_pos"] == "right":
        return (cx + r1 + 18, cy - 4), "lm"
    return (cx - r1 - 18, cy - 4), "rm"


def draw_background(controls, statics):
    im = base_plate()
    s = 3

    def paint(d, sc):
        line = CREAM + (70,)
        # Boxes and dividers.
        for box in statics.get("boxes", (CENTRE_BOX, BOTTOM_BOX)):
            d.rounded_rectangle([v * sc for v in box], radius=14 * sc, outline=line, width=int(1.4 * sc))
        for x in statics.get("dividers_c", C_DIVIDERS):
            d.line([(x * sc, 470 * sc), (x * sc, 586 * sc)], fill=CREAM + (55,), width=int(1.2 * sc))
        for x in statics["dividers_b"]:
            d.line([(x * sc, 262 * sc), (x * sc, 420 * sc)], fill=CREAM + (55,), width=int(1.2 * sc))
        # Tick scales.
        for ctl in controls:
            if ctl["kind"] == "knob":
                spec = SMALL if ctl["size"] == "small" else LARGE
                draw_ticks(d, ctl["centre"], spec["ticks"], sc)
        # The echo trail: baseline, the quarter / half / three-quarter marks of the two-delay window, and
        # head 1's badge, which never moves - it reads the tape at exactly one delay.
        if statics.get("echo"):
            ax, ay, aw, ah = ECHO_AREA
            d.line([(ax * sc, (ay + ah + 3) * sc), ((ax + aw) * sc, (ay + ah + 3) * sc)], fill=CREAM + (70,), width=sc)
            for frac in (0.25, 0.5, 0.75):
                x = ax + frac * aw
                d.line([(x * sc, (ay - 4) * sc), (x * sc, (ay + ah + 3) * sc)],
                       fill=CREAM + (70 if frac == 0.5 else 34,), width=sc)
            hx, hy = ax + 0.5 * aw, ay + ah + 9
            d.rounded_rectangle([(hx - 8) * sc, hy * sc, (hx + 8) * sc, (hy + 15) * sc], radius=3 * sc,
                                fill=(12, 12, 12, 225), outline=ECHO_HUES[0] + (255,), width=int(1.2 * sc))
            d.text((hx * sc, (hy + 7) * sc), "1", font=font(11 * sc, FONT_DEMI), fill=ECHO_HUES[0] + (255,), anchor="mm")

        # The orbit: a ring with tick marks every eighth of a turn, and the listener from above.
        if statics.get("orbit"):
            cx, cy = ORBIT_CENTRE
            r = ORBIT_R
            d.ellipse([(cx - r) * sc, (cy - r) * sc, (cx + r) * sc, (cy + r) * sc], outline=CREAM + (80,),
                      width=int(1.4 * sc))
            for k in range(8):
                a = 2 * math.pi * k / 8
                r0, r1 = r - (6 if k % 2 else 9), r + (6 if k % 2 else 9)
                d.line([((cx + r0 * math.sin(a)) * sc, (cy - r0 * math.cos(a)) * sc),
                        ((cx + r1 * math.sin(a)) * sc, (cy - r1 * math.cos(a)) * sc)],
                       fill=CREAM + (110 if k % 2 == 0 else 60,), width=int(1.2 * sc))
            head = (205, 140, 90)
            d.ellipse([(cx - 6 - 19) * sc, (cy - 5) * sc, (cx - 19 + 6) * sc, (cy + 9) * sc], outline=head + (220,),
                      width=int(1.4 * sc))                                                    # left ear
            d.ellipse([(cx + 19 - 6) * sc, (cy - 5) * sc, (cx + 19 + 6) * sc, (cy + 9) * sc], outline=head + (220,),
                      width=int(1.4 * sc))                                                    # right ear
            d.ellipse([(cx - 19) * sc, (cy - 23) * sc, (cx + 19) * sc, (cy + 23) * sc], fill=(18, 16, 14, 255),
                      outline=head + (235,), width=int(1.6 * sc))                             # head
            d.polygon([((cx - 6) * sc, (cy - 21) * sc), ((cx + 6) * sc, (cy - 21) * sc), (cx * sc, (cy - 31) * sc)],
                      fill=head + (235,))                                                     # nose: front

        # Graph grid.
        if statics["graph"]:
            gx, gy, gw, gh = GRAPH
            for db in (12, 0, -12, -24):
                y = gy + graph_y(db) * gh
                d.line([(gx * sc, y * sc), ((gx + gw) * sc, y * sc)], fill=CREAM + (40 if db else 70,), width=sc)
            for f in (100, 1000, 10000):
                x = gx + graph_x(f) * gw
                d.line([(x * sc, gy * sc), (x * sc, (gy + gh) * sc)], fill=CREAM + (40,), width=sc)
            d.rectangle([gx * sc, gy * sc, (gx + gw) * sc, (gy + gh) * sc], outline=CREAM + (60,), width=sc)
        # Decorative echo trail in the centre box.
        if statics["waveform"]:
            x0, x1, yc = 440, 840, 360
            pts = []
            for i in range((x1 - x0) * sc):
                x = i / sc
                t = x / (x1 - x0)
                burst = (x % 80) / 80.0
                env = math.exp(-2.6 * t) * math.exp(-9.0 * burst) * (1 if burst < 0.6 else 0)
                y = yc + 21 * env * math.sin(i / sc * 1.7)
                pts.append(((x0 + x) * sc, y * sc))
            d.line(pts, fill=ORANGE + (255,), width=int(1.6 * sc))
            d.line([(x0 * sc, yc * sc), (x1 * sc, yc * sc)], fill=ORANGE + (60,), width=sc)

        # A spring, side view, in the centre box.
        if statics.get("coil"):
            x0, x1, yc, turns, rx, ry = 452, 828, 322, 22, 9.0, 24.0
            pts = []
            steps = 2400
            for i in range(steps + 1):
                t = i / steps
                th = t * turns * 2 * math.pi
                pts.append(((x0 + 18 + t * (x1 - x0 - 36) + rx * math.cos(th)) * sc, (yc + ry * math.sin(th)) * sc))
            d.line(pts, fill=(205, 140, 90, 235), width=int(1.6 * sc), joint="curve")
            for bx in (x0, x1 - 12):   # mounting tabs
                d.rounded_rectangle([bx * sc, (yc - 30) * sc, (bx + 12) * sc, (yc + 30) * sc], radius=3 * sc,
                                    outline=CREAM + (120,), width=int(1.2 * sc))

    overlay = supersampled((W, H), s, paint)
    glow = overlay.copy()
    if statics["waveform"] or statics.get("coil"):
        r, g, b, a = glow.split()
        glow = Image.merge("RGBA", (r, g, b, a)).filter(ImageFilter.GaussianBlur(5))
        im = Image.alpha_composite(Image.alpha_composite(im, glow), glow)
    im = Image.alpha_composite(im, overlay)

    d = ImageDraw.Draw(im)
    big, mid, small_f = font(17, FONT_DEMI), font(14, FONT_DEMI), font(11, FONT_MEDIUM)
    for ctl in controls:
        if ctl["kind"] == "knob" and ctl["heading"]:
            (x, y), anchor = heading_xy(ctl)
            size = 17 if ctl["heading_pos"] == "above" else 14
            max_w = 158 if ctl["heading_pos"] == "above" else 132
            while size > 10 and text_width(ctl["heading"], font(size, FONT_DEMI), 2.2) > max_w:
                size -= 1
            f = font(size, FONT_DEMI)
            if anchor == "rm":
                x -= text_width(ctl["heading"], f, 2.2)
                anchor = "lm"
            draw_spaced(d, (x, y), ctl["heading"], f, CREAM + (255,), spacing=2.2, anchor=anchor)
        if ctl["kind"] == "pills" and ctl.get("heading"):
            draw_spaced(d, ctl["heading_xy"], ctl["heading"], font(12, FONT_DEMI), CREAM + (230,), spacing=1.8)
    for text, xy in statics["headings"]:
        draw_spaced(d, xy, text, mid, CREAM + (235,), spacing=2.0)
    if statics["centre_title"]:
        bx0, by0, bx1, _ = CENTRE_BOX
        draw_spaced(d, ((bx0 + bx1) / 2, by0 + 18), statics["centre_title"], mid, CREAM + (235,), spacing=2.4)
    if statics.get("echo"):
        ax, ay, aw, ah = ECHO_AREA
        draw_spaced(d, (ax + 2, ay - 26), "ECHO TRAIL", mid, CREAM + (235,), spacing=2.0, anchor="lm")
        for frac, text in ((0.25, "half"), (0.5, "one delay"), (0.75, "one and a half")):
            d.text((ax + frac * aw, ay - 12), text, font=small_f, fill=CREAM + (130,), anchor="mm")
        d.text((ax + aw, ay - 12), "two", font=small_f, fill=CREAM + (130,), anchor="rm")
    if statics.get("orbit"):
        cx, cy = ORBIT_CENTRE
        r = ORBIT_R
        d.text((cx, cy - r - 20), "front", font=small_f, fill=CREAM + (150,), anchor="mm")
        d.text((cx, cy + r + 20), "behind", font=small_f, fill=CREAM + (150,), anchor="mm")
        d.text((cx - r - 24, cy), "L", font=mid, fill=CREAM + (170,), anchor="mm")
        d.text((cx + r + 24, cy), "R", font=mid, fill=CREAM + (170,), anchor="mm")
    if statics["graph"]:
        gx, gy, gw, gh = GRAPH
        for f_hz, lab in ((100, "100"), (1000, "1k"), (10000, "10k")):
            d.text((gx + graph_x(f_hz) * gw, gy + gh + 7), lab, font=small_f, fill=CREAM + (150,), anchor="mm")
    return im.convert("RGB")


# ------------------------------------------------------------------------------------------------
# TUI.json
# ------------------------------------------------------------------------------------------------
def label_data(label_type, colour, height, justification, text=None, font_name=VALUE_FONT, style="Regular"):
    pairs = [("version", 1),
             ("textStyle", obj(("version", 1),
                               ("font", obj(("version", 1), ("name", font_name), ("style", style), ("height", float(height)))),
                               ("colour", colour), ("justification", justification), ("case", "Original"))),
             ("type", label_type), ("handleName", "Data")]
    if text is not None:
        pairs.append(("text", text))
    return Obj(pairs)


def focus_child(w, h):
    return G.entry("focus", "Focus", G.focus_data("ff3fd9e3"), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


def knob_component(spec, strip_file, overlay):
    """Component of size (frame + 4) wide, frame + value line tall; knob frame at the top."""
    f = spec["frame"]
    pad_x = 10
    w = f + 2 * pad_x
    val_y = f // 2 + int(0.76 * spec["ticks"][1]) - 8
    h = val_y + 26
    children = [focus_child(w, h),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d 0 %d %d" % (pad_x, f, f), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, 19.0, "horizontallyCentred verticallyCentred"),
                        "0 %d %d 24" % (val_y, w), "No", "Show", "Always")]
    return G.component_definition(G.control_actions(overlay), children, "00000000"), (w, h, pad_x)


def readout_component(w, h, size, overlay, case):
    data = obj(("version", 4), ("knobType", "ValueSlider"),
               ("textStyle", obj(("version", 1),
                                 ("font", obj(("version", 1), ("name", VALUE_FONT), ("style", "Bold"), ("height", float(size)))),
                                 ("colour", READOUT_COLOUR), ("justification", "horizontallyCentred verticallyCentred"),
                                 ("case", case))),
               ("dragOrientation", "Vertical"), ("handleName", "Data"))
    children = [focus_child(w, h), G.entry("readout", "Knob", data, "0 0 %d %d" % (w, h), "No", "Show", "Always")]
    return G.component_definition(G.control_actions(overlay), children, "00000000")


def pill_component(ctl, files, n_choices):
    xs = [it[2] for it in ctl["items"]]
    ys = [it[3] for it in ctl["items"]]
    ox, oy = min(xs) - PILL_MARGIN, min(ys) - PILL_MARGIN
    w = max(it[2] + it[4] for it in ctl["items"]) + PILL_MARGIN - ox
    h = max(ys) + PILL_H + PILL_MARGIN - oy
    children = [focus_child(int(w), int(h))]
    for (choice, caption, x, y, pw), (on, off) in zip(ctl["items"], files):
        bid, group = (1, 1) if ctl.get("toggle") else (choice, n_choices)
        children.append(G.entry("pill %d" % choice, "Button", G.button_data(on, off, bid, group),
                                "%d %d %d %d" % (round(x - PILL_MARGIN - ox), round(y - PILL_MARGIN - oy),
                                                 pw + 2 * PILL_MARGIN, PILL_H + 2 * PILL_MARGIN),
                                "No", "Show", "Always"))
    comp = G.component_definition(G.control_actions("menu overlay", double_click=False), children, "00000000")
    return comp, (round(ox), round(oy), round(w), round(h))


def build():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    images = {}

    def save(name, im):
        im.save(os.path.join(OUT, name), optimize=True)
        images[name] = im
        return name

    save("knob_small.png", knob_strip(SMALL))
    save("knob_small_bi.png", knob_strip(SMALL, bipolar=True))
    save("knob_large.png", knob_strip(LARGE))
    lamp_file = save("lamp.png", lamp_strip())
    save("head_mark.png", head_mark_strip())
    save("orbit.png", orbit_strip())
    echo_file = save("echo_bar.png", echo_bar_strip())
    tube_file = "tube.png"
    tube = tube_strip()
    tube.save(os.path.join(OUT, tube_file), optimize=True)
    images[tube_file] = tube.convert("RGBA")
    for mode in ("LP", "BP", "HP"):
        save("curve_%s.png" % mode.lower(), curve_strip(mode))
    save("curve_off.png", curve_image("Off", 1000.0))

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("knob overlay bipolar", G.overlay_component("KnobOverlay", G.knob_overlay_data(True), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    knob_defs = {}
    for key, spec, strip, overlay in (("knob small", SMALL, "knob_small.png", "knob overlay"),
                                      ("knob small bipolar", SMALL, "knob_small_bi.png", "knob overlay bipolar"),
                                      ("knob large", LARGE, "knob_large.png", "knob overlay")):
        comp, geom = knob_component(spec, strip, overlay)
        defs.append((key, comp))
        knob_defs[key] = (spec, geom)

    tabs, qlink_entries, previews = [], [], []
    pill_files = {}
    for ti, (name, controls, statics, qlinks) in enumerate((page_delay(), page_heads(), page_tape(), page_loop(), page_mod(), page_space(),
                                                             page_swirl())):
        bg_file = "bg_%s.jpg" % name.lower()
        bg = draw_background(controls, statics)
        bg.save(os.path.join(OUT, bg_file), quality=92)
        children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                           ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
        preview = bg.convert("RGBA")
        for ci, ctl in enumerate(controls):
            handle = "Parameter %d" % ctl["param"]
            if ctl["kind"] == "knob":
                key = "knob %s%s" % (ctl["size"], " bipolar" if ctl["bipolar"] else "")
                spec, (w, h, pad_x) = knob_defs[key]
                cx, cy = ctl["centre"]
                x, y = cx - spec["frame"] // 2 - pad_x, cy - spec["frame"] // 2
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "knob", ctl, (x, y, w, h), key))
            elif ctl["kind"] == "readout":
                bx, by, bw, bh = [int(v) for v in ctl["box"]]
                comp_name = "readout %d %d" % (ti, ci)
                defs.append((comp_name, readout_component(bw, bh, ctl["size"], ctl["overlay"], ctl["case"])))
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (bx, by, bw, bh), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "readout", ctl, (bx, by, bw, bh), None))
            elif ctl["kind"] == "pills":
                files = []
                for choice, caption, x, y, pw in ctl["items"]:
                    pair = []
                    for state in (True, False):
                        slug = "".join(ch for ch in caption.lower() if ch.isalnum())
                        fname = "pill_%s_%s_%d.png" % ("on" if state else "off", slug, pw)
                        if fname not in pill_files:
                            pill_files[fname] = save(fname, pill(state, caption, pw))
                        pair.append(fname)
                    files.append(tuple(pair))
                comp, (ox, oy, w, h) = pill_component(ctl, files, len(CHOICES[ctl["param"]]))
                comp_name = "pills %d %d" % (ti, ci)
                defs.append((comp_name, comp))
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (ox, oy, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "pills", ctl, files, None))

        # Page pictures bound to a parameter (the tape path on HEADS).
        for spec in statics.get("images", []):
            ix, iy, iw, ih = spec["box"]
            children.append(G.entry("picture", "Image",
                                    obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                        ("image", spec["file"]), ("numFrames", spec["frames"]),
                                        ("handleName", "Data")),
                                    "%d %d %d %d" % (ix, iy, iw, ih), "No", "Hide", "Always",
                                    remap=[("Data", "Parameter %d" % spec["param"])],
                                    invalidating=spec.get("invalidating")))
            previews.append((ti, "image", None, (spec["file"], DEFAULTS[spec["param"]][0], (ix, iy), spec["frames"]),
                             None))

        # The ECHO bars: one parameter each, hidden together with the tube when Display is off.
        if statics.get("echo"):
            ax, ay, aw, ah = ECHO_AREA
            pitch = aw / ECHO_BARS
            for i in range(ECHO_BARS):
                children.append(G.entry("bar %d" % i, "Image",
                                        obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                            ("image", echo_file), ("numFrames", 2 * ECHO_LEVELS * ECHO_COLOURS),
                                            ("handleName", "Data")),
                                        "%d %d %d %d" % (round(ax + i * pitch + (pitch - ECHO_BAR_W) / 2), ay,
                                                         ECHO_BAR_W, ah),
                                        "No", "Hide", "Always",
                                        remap=[("Data", "Parameter %d" % (P["echo"] + i))],
                                        invalidating=["IndexedEnabling/1/2/Parameter %d" % P["tube"]]))
            previews.append((ti, "echo", None, (echo_file, (ax, ay, aw, ah)), None))

        # The tube, lit by the plugin's level parameter: hidden (so the painted tube shows) when Tube is off.
        tx, ty, tw, th = TUBE_BOX
        children.append(G.entry("tube", "Image",
                                obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", tube_file),
                                    ("numFrames", TUBE_FRAMES), ("handleName", "Data")),
                                "%d %d %d %d" % (tx, ty, tw, th), "No", "Hide", "Always",
                                remap=[("Data", "Parameter %d" % P["level"])],
                                invalidating=["IndexedEnabling/1/2/Parameter %d" % P["tube"]]))
        previews.append((ti, "image", None, (tube_file, 9 if ti == 0 else 3, (tx, ty)), None))

        # Feedback lamp.
        lw, lh = images[lamp_file].width, images[lamp_file].height // 25
        children.append(G.entry("feedback lamp", "Image",
                                obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                    ("image", lamp_file), ("numFrames", 25), ("handleName", "Data")),
                                "%d %d %d %d" % (LAMP_POS[0], LAMP_POS[1], lw, lh), "No", "Show", "Always",
                                remap=[("Data", "Parameter %d" % P["feedback"])]))
        if statics["graph"]:
            gx, gy, gw, gh = GRAPH
            for idx, mode in enumerate(("off", "lp", "bp", "hp")):
                frames = 1 if mode == "off" else 48
                bound = P["filter"] if mode == "off" else P["cutoff"]
                children.append(G.entry("response %s" % mode, "Image",
                                        obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                            ("image", "curve_%s.png" % mode), ("numFrames", frames), ("handleName", "Data")),
                                        "%d %d %d %d" % (gx, gy, gw, gh), "No", "Hide", "Always",
                                        remap=[("Data", "Parameter %d" % bound)],
                                        invalidating=["IndexedEnabling/%d/4/Parameter %d" % (idx, P["filter"])]))

        tabs.append(obj(("version", 1), ("tabName", name), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", G.component_definition([], children, "ff000000")),
                        ("qlinkBoundsData", list(QLINK_RECTS))))
        qmap = {}
        for r, row in enumerate(qlinks):
            for c, pidx in enumerate(row):
                qmap[13 + c - 4 * r] = pidx
        qlink_entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"),
                                 ("Q-Links", Obj(("Q-Link %d" % q, qmap.get(q, -1)) for q in range(1, 17)))))
        previews.append((ti, "bg", None, preview, None))

    replacements = [obj(("key", "Parameter %d" % k), ("value", v)) for k, v in sorted(CHOICES.items())]
    tui = obj(("pageData", obj(
        ("version", 1),
        ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                     ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs]))),
        ("replacementStrings", replacements),
        ("info", obj(("version", 1), ("type", "CompleteDescription"))),
        ("tabs", tabs))))
    program = Obj(("Q-Link %d" % (q + 1), q if q < 16 else -1) for q in range(16))
    qlinks_json = obj(("version", 4), ("info", obj(("version", 1), ("type", "CompleteDescription"))),
                      ("Screen Mode Q-Links", obj(("version", 4), ("map", qlink_entries))),
                      ("Program Mode Q-Links", program))
    with open(os.path.join(OUT, "TUI.json"), "w", encoding="utf-8") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(OUT, "Q-Links.json"), "w", encoding="utf-8") as f:
        f.write(G.dump_json(qlinks_json) + "\n")
    make_previews(previews, images)


# ------------------------------------------------------------------------------------------------
# Preview (approximate: Avenir Next stands in for the MPC's Nexa, values at their defaults)
# ------------------------------------------------------------------------------------------------
DEFAULTS = {0: (1.0, "0.0 dB"), 1: (1, None), 3: (0.5625, "1/8D"), 4: (55 / 120, "55 %"), 5: (0.35, "35 %"),
            6: (1, None), 7: (0.15, "15 %"), 8: (0.3, "30 %"), 9: (1, None), 10: (0.635, "1800 Hz"),
            11: (0.35, "35 %"), 12: (0.25, "25 %"), 13: (2, "FLANGER"), 14: (0.5, "50 %"), 15: (0.3, "30 %"),
            16: (0, None), 17: (0.5 + 7 / 48, "+7 st"), 18: (0.6, "60 %"), 19: (1, None), 20: (30 / 42, "0.0 dB"),
            21: (5, None), 22: (1, None), 23: (0.2, "1 bar"), 24: (0.5, "90 deg"), 25: (0.5 + 2.5 / 8, "+2.5 oct"),
            26: (0.5, "0 st"), 27: (0.75, "+50 %"), 28: (0.5, "0 %"), 29: (0.5 + 1.5 / 8, "+1.5 oct"), 30: (0.5, "0 %"),
            31: (0.3, "30 %"), 32: (0.4, "40 %"), 33: (1, None), 34: (0.45, "45 %"), 35: (0.57, "2.4 s"),
            36: (0.45, "45 %"), 37: (0.7, "70 %"), 38: (0, None), 39: (1, None), 40: (0.5, "50 %"),
            41: (0.45, "45 %"), 42: (3, None), 43: (0.5 + 0.275, "+55 %"), 44: (1, None), 45: (0.6, None),
            110: (0.4, "40 %"), 111: (0, None), 112: (0.5 + 0.15, "+30 %"), 113: (0, None),
            114: ((150 / 2000) ** 0.4, "150 ms"), 115: (0.6, "60 %"),
            116: (0.8, "80 %"), 117: (2, None), 118: (4 / 9, "1/8 turn"), 119: (0, None), 120: (0.0, "0 deg"),
            121: (0.7, "70 %"), 122: (5, None)}


def make_previews(previews, images):
    pages = {}
    for ti, kind, ctl, data, extra in previews:
        if kind == "bg":
            pages[ti] = data
    for ti, kind, ctl, data, key in previews:
        if kind == "bg":
            continue
        im = pages[ti]
        d = ImageDraw.Draw(im)
        if kind == "echo":
            file, (ax, ay, aw, ah) = data
            strip = images[file]
            pitch = aw / ECHO_BARS
            import math as _m
            bank = 2 * ECHO_LEVELS
            # Head 1 at one delay (0.5 of the window) and head 2 at the default 2/3 ratio (0.333).
            for i in range(ECHO_BARS):
                t = i / ECHO_BARS
                n1 = min(abs(t - 0.015), abs(t - 0.5), abs(t - 0.985))
                n2 = min(abs(t - 0.333), abs(t - 0.833))
                a1 = _m.exp(-n1 * 60) * _m.exp(-t * 1.1)
                a2 = 0.8 * _m.exp(-n2 * 60) * _m.exp(-t * 1.1)
                level = max(0, round((ECHO_LEVELS - 1) * max(a1, a2)))
                share = a2 / (a1 + a2) if a1 + a2 > 1e-6 else 0.0
                colour = 0 if share < 0.35 else (1 if share > 0.65 else 2)
                frame = colour * bank + level + (ECHO_LEVELS if i == 38 else 0)
                im.alpha_composite(strip.crop((0, frame * ah, strip.width, (frame + 1) * ah)),
                                   (round(ax + i * pitch + (pitch - ECHO_BAR_W) / 2), ay))
        elif kind == "image":
            file, frame, (x, y) = data[:3]
            strip = images[file]
            fh = strip.height // (data[3] if len(data) > 3 else TUBE_FRAMES)
            im.alpha_composite(strip.crop((0, int(frame) * fh, strip.width, (int(frame) + 1) * fh)), (x, y))
        elif kind == "knob":
            spec = SMALL if ctl["size"] == "small" else LARGE
            strip = images["knob_small_bi.png" if ctl["bipolar"] else
                           ("knob_small.png" if ctl["size"] == "small" else "knob_large.png")]
            v, text = DEFAULTS[ctl["param"]]
            fi = round(v * (spec["frames"] - 1))
            f = spec["frame"]
            x, y, w, h = data
            im.alpha_composite(strip.crop((0, fi * f, f, (fi + 1) * f)), (x + (w - f) // 2, y))
            val_y = f // 2 + int(0.76 * spec["ticks"][1]) - 8
            d.text((x + w / 2, y + val_y + 12), text, font=font(17, FONT_MEDIUM), fill=CREAM, anchor="mm")
        elif kind == "readout":
            x, y, w, h = data
            d.text((x + w / 2, y + h / 2), DEFAULTS[ctl["param"]][1], font=font(int(ctl["size"] * 0.9), FONT_DEMI),
                   fill=(95, 230, 238), anchor="mm")
        elif kind == "pills":
            sel = DEFAULTS[ctl["param"]][0]
            for (choice, caption, x, y, pw), (on, off) in zip(ctl["items"], data):
                pim = images[on if choice == sel else off]
                im.alpha_composite(pim, (round(x - PILL_MARGIN), round(y - PILL_MARGIN)))
    for ti, im in pages.items():
        if ti != 5:
            lamp = images["lamp.png"]
            lh = lamp.height // 25
            fi = 22 if ti == 0 else 9
            im.alpha_composite(lamp.crop((0, fi * lh, lamp.width, (fi + 1) * lh)), LAMP_POS)
        if ti in (0, 3):   # pages with the filter response
            curve = images["curve_lp.png"]
            gx, gy, gw, gh = GRAPH
            fi = 30
            im.alpha_composite(curve.crop((0, fi * gh, gw, (fi + 1) * gh)), (gx, gy))
        # MPC chrome around the tab, for context.
        full = Image.new("RGBA", (1280, 800), (24, 24, 26, 255))
        full.alpha_composite(im, (0, 110))
        fd = ImageDraw.Draw(full)
        fd.text((24, 55), "MPC header (drawn by the MPC)", font=font(18, FONT_MEDIUM), fill=(120, 120, 120), anchor="lm")
        for k, nm in enumerate(("DELAY", "HEADS", "TAPE", "LOOP", "MOD", "SPACE", "SWIRL")):
            fd.text((80 + k * 180, 770), nm, font=font(18, FONT_DEMI), fill=(200, 200, 200), anchor="mm")
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        full.convert("RGB").save(os.path.join(PREVIEW_DIR, "skin-preview-%d.png" % (ti + 1)))


if __name__ == "__main__":
    build()
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("wrote %s (%d files, %.1f MB)" % (OUT, len(os.listdir(OUT)), total / 1e6))
