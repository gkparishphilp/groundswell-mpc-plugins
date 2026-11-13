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
         character=39, age=40)
CHOICES = {1: ["Free", "Sync"], 6: ["Off", "On"], 9: ["Off", "LP", "BP", "HP"],
           13: ["Off", "Chorus", "Flanger", "Phaser", "Bitcrush", "Downsample", "Ring Mod", "Fuzz"],
           16: ["Semitones", "Hz"], 19: ["Off", "On"],
           21: ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Drift"], 22: ["Free", "Sync"],
           33: ["Off", "On"], 38: ["After", "In Loop"], 39: ["Digital", "Tape", "BBD"]}

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
        small_knob(P["age"], 318, ROW_A_Y, "AGE"),
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
    qlinks = [[P["input"], P["age"], P["mix"], P["output"]],
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
        for box in (CENTRE_BOX, BOTTOM_BOX):
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
    bx0, by0, bx1, _ = CENTRE_BOX
    draw_spaced(d, ((bx0 + bx1) / 2, by0 + 18), statics["centre_title"], mid, CREAM + (235,), spacing=2.4)
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
    for ti, (name, controls, statics, qlinks) in enumerate((page_delay(), page_loop(), page_mod(), page_space())):
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
                        fname = "pill_%s_%s_%d.png" % ("on" if state else "off",
                                                        caption.lower().replace(" ", "").replace("-", ""), pw)
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

        # Feedback lamp (all pages) and, on DELAY, the filter response picture.
        lw, lh = images[lamp_file].width, images[lamp_file].height // 25
        children.append(G.entry("feedback lamp", "Image",
                                obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", lamp_file),
                                    ("numFrames", 25), ("handleName", "Data")),
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
            36: (0.45, "45 %"), 37: (0.7, "70 %"), 38: (0, None), 39: (1, None), 40: (0.5, "50 %")}


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
        if kind == "knob":
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
        for k, nm in enumerate(("DELAY", "LOOP", "MOD", "SPACE")):
            fd.text((80 + k * 213, 770), nm, font=font(18, FONT_DEMI), fill=(200, 200, 200), anchor="mm")
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        full.convert("RGB").save(os.path.join(PREVIEW_DIR, "skin-preview-%d.png" % (ti + 1)))


if __name__ == "__main__":
    build()
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("wrote %s (%d files, %.1f MB)" % (OUT, len(os.listdir(OUT)), total / 1e6))
