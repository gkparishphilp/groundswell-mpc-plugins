#!/usr/bin/env python3
"""Build Tidepools's hand-made skin (plugins/Tidepools/skins/) from the artwork in this folder.

    python3 plugins/Tidepools/art/make_skin.py            # writes skins/ and build/skin-preview-*.png

Source art:
    background-src.png      the panel plate, vessel and title (any size with a 1280:628 aspect)
    knob_small.webp         knob with skirt, pointer at 12 o'clock, transparent background
    knob_large.webp         big scalloped knob, same
    pill_on/off.webp        button pill

Everything else (tick scales, headings, section boxes, value arcs, captions, the filter curve) is
drawn here. The skin binds to parameter indices, so it must be regenerated if the parameter list
changes.

The drawing machinery below started as Undertow's generator and is deliberately a copy rather than
a shared import: Undertow's skin is shipping and verified, and a plugin's artwork should not break
because a sibling's did. Only the layout, the parameter map and the pages differ.
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
# Parameter indices (must match src/Tidepools.cpp).
P = dict(out=0, mix=1, mode=2, activity=3, shape=4, scatter=5, motion=6, sync=7, time=8, repeats=9,
         tone=10, space=11, decay=12, listen=13, gap=14, answer=15, reply=16, interval=17, scale=18,
         root=19, trade=20, spread=21, hold=22, input=23, level=24, state=25, keymode=26, heard=27,
         gapsync=28, sens=29)
CHOICES = {2: ["Cloud", "Blocks", "Glide", "Bubble", "Chord", "Roll", "Ebb"],
           7: ["Free", "Sync"],
           13: ["Off", "Auto", "Hold"], 15: ["Now", "Beat", "Bar"],
           18: ["Off", "Major", "Minor", "Penta Maj", "Penta Min", "Dorian", "Whole Tone"],
           19: ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"],
           22: ["Off", "On"], 25: ["Idle", "Hear", "Wait", "Talk"], 26: ["Fixed", "Follow"],
           27: ["-", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"],
           28: ["Free", "Sync"]}

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


LAMP_W = 100




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


HEADING_W, HEADING_H = 206, 28


def mode_heading_strip(words, dim=()):
    """One frame per Mode: the word that knob means in that engine. Frames listed in `dim` are
    drawn faint, which is how a knob says it does nothing in the mode you are in."""
    strip = Image.new("RGBA", (HEADING_W, HEADING_H * len(words)), (0, 0, 0, 0))
    for i, word in enumerate(words):
        alpha = 80 if i in dim else 255

        def paint(d, sc, word=word, alpha=alpha):
            draw_spaced(d, (1 * sc, HEADING_H * sc / 2), word, font(14 * sc, FONT_DEMI),
                        CREAM + (alpha,), spacing=2.2 * sc, anchor="lm")

        strip.alpha_composite(supersampled((HEADING_W, HEADING_H), 3, paint), (0, i * HEADING_H))
    return strip


# ------------------------------------------------------------------------------------------------
# Layout (tab coordinates, 1280 x 628).
#
# Four columns of boxes, up to three rows, following the mock-ups: a knob at the left of each box
# with its name and value set beside it, or a group of pills filling the box. The MPC draws the
# value text itself (a Label bound to the parameter); the names are painted into the background so
# they keep the panel's letter-spaced caps.
# ------------------------------------------------------------------------------------------------
COL_X = (40, 342, 644, 946)      # left edge of each column
COL_W = 282
ROW3_Y = (166, 281, 396)         # box tops when a page has three rows
ROW2_Y = (196, 346)              # ...and when it has two
BOX_H = 100
TAB_NAMES = ("LISTEN", "GRAINS", "TONE")
GRAPH = (64, 302, 536, 62)       # the filter curve, inside the wide box on TONE


def box(col, top, span=1):
    return (COL_X[col], top, COL_X[col] + span * COL_W + (span - 1) * 20, top + BOX_H)


def knob_in(param, col, top, heading, bipolar=False, span=1):
    """A knob at the left of its box, name and value to the right."""
    bx0, by0, bx1, by1 = box(col, top, span)
    return dict(kind="knob", size="wide", param=param, centre=(bx0 + 62, (by0 + by1) // 2),
                heading=heading, bipolar=bipolar, heading_pos="wide")


def knob_compact(param, cx, cy):
    """A knob with its value underneath, for sharing a box with something else."""
    return dict(kind="knob", size="small", param=param, centre=(cx, cy),
                heading=None, bipolar=False, heading_pos="above")


def switch_at(param, caption, x, y, width=96):
    """A single lit/unlit pill at an exact spot, for tucking into another control's box."""
    return dict(kind="pills", param=param, heading=None, heading_xy=(x, y),
                items=[(1, caption, x, y, width)], toggle=True)


def pills_in(param, col, top, heading, captions=None, width=None, rows=1, span=1):
    """A row (or two) of pills filling a box, with the heading above them."""
    names = captions or CHOICES[param]
    bx0, by0, bx1, by1 = box(col, top, span)
    per = (len(names) + rows - 1) // rows
    pw = width or int((bx1 - bx0 - 24 - (per - 1) * 8) / per)
    items = []
    for i, nm in enumerate(names):
        r, c = divmod(i, per)
        count = min(per, len(names) - r * per)
        x0 = (bx0 + bx1) / 2 - (count * pw + (count - 1) * 8) / 2
        y = by0 + (BOX_H - rows * PILL_H - (rows - 1) * 6) / 2 + 9 + r * (PILL_H + 6)
        items.append((i, nm, x0 + c * (pw + 8), y, pw))
    # Two rows of pills fill the box: the glow around each pill needs the margin a heading would
    # otherwise sit in. Those groups go without one, which costs nothing - a column of note names
    # or of engine names says what it is.
    return dict(kind="pills", param=param, items=items, heading=None if rows > 1 else heading,
                heading_xy=(bx0 + 24, by0 + 21), toggle=False)


def toggle_in(param, col, top, heading, caption):
    bx0, by0, bx1, by1 = box(col, top)
    pw = 96
    return dict(kind="pills", param=param, heading=heading, heading_xy=(bx0 + 24, by0 + 21),
                items=[(1, caption, bx1 - 24 - pw, (by0 + by1) / 2 - PILL_H / 2, pw)], toggle=True)


def page_listen():
    # Top row: everything that decides what counts as a phrase. Sens is the one that matters most
    # on a patch with a long tail, so it sits up here rather than among the levels. Gap and its
    # Free/Sync switch are two halves of one idea, so they share the third box.
    gx0, gy0, gx1, gy1 = box(2, ROW3_Y[0])
    controls = [
        pills_in(P["state"], 0, ROW3_Y[0], "STATE"),
        pills_in(P["listen"], 1, ROW3_Y[0], "LISTEN"),
        knob_compact(P["gap"], gx0 + 112, gy0 + 46),
        switch_at(P["gapsync"], "Sync", gx1 - 22 - 80, (gy0 + gy1) / 2 - PILL_H / 2, 80),
        knob_in(P["sens"], 3, ROW3_Y[0], "SENS"),

        pills_in(P["answer"], 0, ROW3_Y[1], "ANSWER"),
        knob_in(P["trade"], 1, ROW3_Y[1], "TRADE"),
        toggle_in(P["hold"], 2, ROW3_Y[1], "HOLD", "Hold"),
        knob_in(P["reply"], 3, ROW3_Y[1], "REPLY"),

        knob_in(P["repeats"], 0, ROW3_Y[2], "REPEATS"),
        knob_in(P["mix"], 1, ROW3_Y[2], "MIX"),
        knob_in(P["input"], 2, ROW3_Y[2], "INPUT"),
        knob_in(P["out"], 3, ROW3_Y[2], "OUTPUT"),
    ]
    statics = dict(headings=[("GAP", (gx0 + 24, gy0 + 21))],
                   boxes=[box(c, t) for t in ROW3_Y for c in range(4)],
                   graph=False, waveform=False)
    qlinks = [[P["state"], P["listen"], P["gap"], P["sens"]],
              [P["answer"], P["trade"], P["hold"], P["reply"]],
              [P["repeats"], P["mix"], P["input"], P["out"]]]
    return "LISTEN", controls, statics, qlinks


def page_grains():
    controls = [
        pills_in(P["mode"], 0, ROW2_Y[0], "MODE", rows=2),
        knob_in(P["activity"], 1, ROW2_Y[0], "ACTIVITY"),
        knob_in(P["shape"], 2, ROW2_Y[0], "SHAPE"),
        knob_in(P["scatter"], 3, ROW2_Y[0], None),

        knob_in(P["motion"], 0, ROW2_Y[1], None),
        pills_in(P["sync"], 1, ROW2_Y[1], "TIME MODE", width=110),
        knob_in(P["time"], 2, ROW2_Y[1], "TIME"),
        knob_in(P["spread"], 3, ROW2_Y[1], "SPREAD"),
    ]
    # Both knobs mean something different in each engine, so the panel says which: the heading is a
    # picture bound to Mode, and it goes faint in the engine where the knob does nothing.
    sx, sy = COL_X[3] + 62 + 77, (ROW2_Y[0] + ROW2_Y[0] + BOX_H) // 2 - HEADING_H // 2 - 10
    mx, my = COL_X[0] + 62 + 77, (ROW2_Y[1] + ROW2_Y[1] + BOX_H) // 2 - HEADING_H // 2 - 10
    statics = dict(headings=[], boxes=[box(c, t) for t in ROW2_Y for c in range(4)],
                   graph=False, waveform=False,
                   images=[dict(file="head_scatter.png", frames=7, param=P["mode"],
                                box=(sx, sy, HEADING_W, HEADING_H)),
                           dict(file="head_motion.png", frames=7, param=P["mode"],
                                box=(mx, my, HEADING_W, HEADING_H))])
    qlinks = [[P["mode"], P["activity"], P["shape"], P["scatter"]],
              [P["motion"], P["sync"], P["time"], P["spread"]],
              [-1, -1, -1, -1]]
    return "GRAINS", controls, statics, qlinks


def page_tone():
    controls = [
        knob_in(P["tone"], 0, ROW3_Y[0], "TONE"),
        knob_in(P["space"], 1, ROW3_Y[0], "SPACE"),
        knob_in(P["decay"], 2, ROW3_Y[0], "DECAY"),
        knob_in(P["interval"], 3, ROW3_Y[0], "INTERVAL", bipolar=True),

        pills_in(P["keymode"], 2, ROW3_Y[1], "KEY", width=112),
        dict(kind="readout", param=P["heard"], box=(COL_X[3] + 30, ROW3_Y[1] + 34, COL_W - 60, 48),
             size=34, overlay="menu overlay", case="Original"),

        pills_in(P["scale"], 0, ROW3_Y[2], "SCALE", rows=2, span=2),
        pills_in(P["root"], 2, ROW3_Y[2], "ROOT", rows=2, span=2),
    ]
    statics = dict(headings=[("RESPONSE", (COL_X[0] + 24, ROW3_Y[1] + 21)),
                             ("HEARD", (COL_X[3] + 24, ROW3_Y[1] + 21))],
                   boxes=[box(c, ROW3_Y[0]) for c in range(4)]
                         + [box(0, ROW3_Y[1], span=2), box(2, ROW3_Y[1]), box(3, ROW3_Y[1]),
                            box(0, ROW3_Y[2], span=2), box(2, ROW3_Y[2], span=2)],
                   graph=True, waveform=False)
    qlinks = [[P["tone"], P["space"], P["decay"], P["interval"]],
              [-1, -1, P["keymode"], P["heard"]],
              [P["scale"], -1, P["root"], -1]]
    return "TONE", controls, statics, qlinks


QLINK_RECTS = ["36 150 1208 130", "36 286 1208 130", "36 400 1208 130"]


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
    if ctl["heading_pos"] == "wide":
        return (cx + r1 + 16, cy - 24), "lm"
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
        for bx in statics.get("boxes", ()):
            d.rounded_rectangle([v * sc for v in bx], radius=14 * sc, outline=line, width=int(1.4 * sc))
        # Tick scales.
        # No tick rings: the panel sets each knob inside its own box, and a ring wide enough to
        # read would cross the box edge. The value arc on the knob does the job instead.

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


    overlay = supersampled((W, H), s, paint)
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


def knob_component(spec, strip_file, overlay, wide=False):
    """Knob plus its value. Normally the value sits under the knob; `wide` puts it beside, which is
    the panel's own layout - knob at the left of a box, name and reading set to its right."""
    f = spec["frame"]
    pad_x = 10
    if wide:
        w, h = pad_x + f + 16 + 190, f
        children = [focus_child(w, h),
                    G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                            "%d 0 %d %d" % (pad_x, f, f), "No", "Show", "Always"),
                    G.entry("value", "Label", label_data("Value", VALUE_COLOUR, 27.0, "left verticallyCentred"),
                            "%d %d 190 40" % (pad_x + f + 16, f // 2 - 4), "No", "Show", "Always")]
        return G.component_definition(G.control_actions(overlay), children, "00000000"), (w, h, pad_x)
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
    save("curve_lp.png", curve_strip("LP"))
    # Cloud / Blocks / Glide / Bubbles / Chord / Roll / Ebb, in that order.
    save("head_scatter.png",
         mode_heading_strip(["SCATTER", "SHUFFLE", "SCATTER", "SCATTER", "STRUM", "JUMP",
                             "SCATTER"], dim=(3,)))
    save("head_motion.png",
         mode_heading_strip(["MOTION", "REVERSE", "BEND", "RANGE", "VOICING", "SPEED", "SLOW"],
                            dim=(0,)))

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("knob overlay bipolar", G.overlay_component("KnobOverlay", G.knob_overlay_data(True), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    knob_defs = {}
    for key, spec, strip, overlay in (("knob wide", SMALL, "knob_small.png", "knob overlay"),
                                      ("knob wide bipolar", SMALL, "knob_small_bi.png", "knob overlay bipolar"),
                                      ("knob small", SMALL, "knob_small.png", "knob overlay"),
                                      ("knob large", LARGE, "knob_large.png", "knob overlay")):
        comp, geom = knob_component(spec, strip, overlay, wide=key.startswith("knob wide"))
        defs.append((key, comp))
        knob_defs[key] = (spec, geom)

    tabs, qlink_entries, previews = [], [], []
    pill_files = {}
    for ti, (name, controls, statics, qlinks) in enumerate((page_listen(), page_grains(), page_tone())):
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
                        slug = "".join(ch if ch.isalnum() else "x" for ch in caption.lower())
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
            previews.append((ti, "image", None,
                             (spec["file"], DEFAULTS[spec["param"]][0], (ix, iy), spec["frames"]), None))

        # The one picture bound to a parameter: the response curve, which follows Tone.
        if statics["graph"]:
            gx, gy, gw, gh = GRAPH
            children.append(G.entry("response", "Image",
                                    obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                        ("image", "curve_lp.png"), ("numFrames", 48), ("handleName", "Data")),
                                    "%d %d %d %d" % (gx, gy, gw, gh), "No", "Show", "Always",
                                    remap=[("Data", "Parameter %d" % P["tone"])]))
            previews.append((ti, "image", None, ("curve_lp.png", DEFAULTS[P["tone"]][0], (gx, gy), 48), None))

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
# Where each control stands in the preview pictures: (value, text). Previews only.
DEFAULTS = {0: (30 / 36, "0.0 dB"), 1: (0.50, "50 %"), 2: (5, None), 3: (0.50, "50 %"),
            4: (0.45, "45 %"), 5: (0.35, "35 %"), 6: (0.30, "30 %"), 7: (1, None),
            8: (7 / 16, "1/8"), 9: (0.40, "40 %"), 10: (0.8687, "9000 Hz"), 11: (0.30, "30 %"),
            12: (0.695, "3.9 s"), 13: (1, None), 14: (0.4703, "220 ms"), 15: (1, None),
            16: (0.35, "35 %"), 17: (1.0, "+12 st"), 18: (0, None), 19: (0, None),
            20: (0.50, "50 %"), 21: (0.70, "70 %"), 22: (0, None), 23: (30 / 42, "0.0 dB"),
            24: (0.0, None), 25: (0, None), 26: (1, None), 27: (0, "-"), 28: (1, None),
            29: (0.50, "50 %")}


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
        if kind == "image":
            file, frame, (x, y), frames = data
            strip = images[file]
            fh = strip.height // frames
            fi = int(round(frame * (frames - 1))) if frames > 1 and frame <= 1.0 else int(frame)
            fi = max(0, min(frames - 1, fi))
            im.alpha_composite(strip.crop((0, fi * fh, strip.width, (fi + 1) * fh)), (x, y))
        elif kind == "knob":
            spec = SMALL if ctl["size"] == "large" else SMALL
            strip = images["knob_small_bi.png" if ctl["bipolar"] else "knob_small.png"]
            v, text = DEFAULTS[ctl["param"]]
            fi = round(v * (spec["frames"] - 1))
            f = spec["frame"]
            x, y, w, h = data
            im.alpha_composite(strip.crop((0, fi * f, f, (fi + 1) * f)), (x + 10, y))
            if text:
                d.text((x + 10 + f + 16, y + f / 2 + 14), text, font=font(26, FONT_MEDIUM),
                       fill=CREAM, anchor="lm")
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
        # MPC chrome around the tab, for context.
        full = Image.new("RGBA", (1280, 800), (24, 24, 26, 255))
        full.alpha_composite(im, (0, 110))
        fd = ImageDraw.Draw(full)
        fd.text((24, 55), "MPC header (drawn by the MPC)", font=font(18, FONT_MEDIUM), fill=(120, 120, 120), anchor="lm")
        for k, nm in enumerate(TAB_NAMES):
            fd.text((213 + k * 427, 770), nm, font=font(18, FONT_DEMI), fill=(200, 200, 200), anchor="mm")
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        full.convert("RGB").save(os.path.join(PREVIEW_DIR, "skin-preview-%d.png" % (ti + 1)))


if __name__ == "__main__":
    build()
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("wrote %s (%d files, %.1f MB)" % (OUT, len(os.listdir(OUT)), total / 1e6))
