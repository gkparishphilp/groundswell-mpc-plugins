#!/usr/bin/env python3
"""Build Bearings' hand-made skin (plugins/Bearings/skins/) from the artwork in this folder.

    python3 plugins/Bearings/art/make_skin.py      # writes skins/ and build/skin-preview-*.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    background-src.webp     the plate: title, the wireframe globe round a listener's head, empty gutters either side
                            (1790x878, the 1280:628 aspect; the corners outside the frame are already black)
    mock-reference.webp     the designer's mock-up, for the look only - its controls are not Bearings'
    knob_small.webp         knob with skirt, pointer at 12 o'clock (shared with Fathom and Whalesong)
    pill_on/off.webp        button pill (shared with Fathom and Whalesong)

Layout: four boxes down each gutter, eight controls to a tab, the Q-Links on the outer columns.

The globe is the point of the panel: a glowing dot shows where the sound is, live. We are behind the listener, so
left and right are the screen's, and "in front of them" is up into the globe: near is just above their head, far
is near the top, and the dot shrinks and turns from cyan to violet as it goes back. A film strip cannot hold a
2-D position at this size (strips stay under ~11400 px tall, so a globe-sized frame allows ~30 frames), so the
plane is cut into 8 rows, each its own strip of 32 columns driven by its own meter (Dot 1..8, parameters 36..43):
the row the sound is in holds (column + 1) / 32, the others hold 0, which is a blank frame.

The skin binds to parameter indices, so it must be regenerated if the parameter list changes. The drawing
machinery is a copy of Fathom's generator (deliberately not a shared import: a plugin's artwork should not break
because a sibling's did).
"""
import json
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
CREAM = (214, 240, 246)
GOLD = (232, 204, 148)
CYAN = (63, 217, 227)
VIOLET = (178, 112, 255)
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match src/Bearings.cpp).
P = dict(out=0, mix=1, pan=2, distance=3, depth=4, doppler=5, room=6, roomsize=7, monosafe=8,
         path=9, sync=10, rate=11, direction=12, sizex=13, sizey=14, phase=15, glide=16, step=17, hitsens=18,
         folatt=19, folrel=20, folgain=21, folsize=22, folrate=23, folpush=24, throw=25,
         split=26, lowx=27, highx=28, lowsize=29, midsize=30, highsize=31, midphase=32, highphase=33,
         xnow=34, ynow=35, dot0=36)
DOT_ROWS, DOT_COLS = 8, 32

PATHS = ["Fixed", "Circle", "Figure 8", "Pendulum", "Ping-Pong", "Drift", "Tide", "Step", "Scatter"]
STEP, SCATTER = 7, 8
CHOICES = {P["path"]: PATHS,
           P["sync"]: ["Free", "Sync"],
           P["direction"]: ["Clockwise", "Counter"],
           P["monosafe"]: ["Off", "On"],
           P["split"]: ["Off", "On"]}

# Knob geometry: Fathom's, a 90 px box. Film strips stay under 11430 px tall.
KNOB = dict(src="knob_small.webp", crop=300, knob=68, arc_r=40, arc_w=4, frame=88, frames=100, ticks=(47, 53))
KNOB_CENTRE_SRC = (623.5, 625.5)

VALUE_COLOUR = "ffdff4f7"
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


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


# ------------------------------------------------------------------------------------------------
# Knob film strips: rotated knob + value arc
# ------------------------------------------------------------------------------------------------
def knob_strip(spec, colour, bipolar=False):
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
            ImageDraw.Draw(glow).arc(box, a0 - 90, a1 - 90, fill=colour + (200,), width=w * 2)
            big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(3 * ss)))
            ImageDraw.Draw(big).arc(box, a0 - 90, a1 - 90, fill=colour + (255,), width=w)
        rot = body.rotate(-angle, resample=Image.BICUBIC)
        off = int(m - knob_px / 2)
        big.alpha_composite(rot, (off, off))
        strip.alpha_composite(big.resize((frame, frame), Image.LANCZOS), (0, i * frame))
    return strip


# ------------------------------------------------------------------------------------------------
# Pills (3-slice stretch so every width keeps round ends), with a caption or a drawn icon
# ------------------------------------------------------------------------------------------------
PILL_BODY = {"pill_on.webp": (446, 207, 1553, 465), "pill_off.webp": (427, 201, 1568, 465)}
PILL_H, PILL_MARGIN = 34, 9
PILL_ON_INK, PILL_OFF_INK = (200, 248, 252), (190, 182, 162)


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
    colour = PILL_ON_INK if state else PILL_OFF_INK
    text, spacing = caption.upper(), 1.2
    while size > 9 and text_width(text, font(size, FONT_DEMI), spacing) > width - 12:
        size -= 1
        spacing = 0.4
    f = font(size, FONT_DEMI)
    draw_spaced(d, (im.width / 2, im.height / 2 + 1), text, f, colour + (255,), spacing=spacing)
    return im


def draw_path_icon(d, name, cx, cy, s, ink):
    """A small drawing of each path, about 24 x 18 px, centred on (cx, cy), at supersample scale s."""
    lw = max(1, round(1.8 * s))

    def P_(x, y):
        return (cx + x * s, cy + y * s)

    def dot(x, y, r, fill=True):
        x0, y0 = P_(x - r, y - r)
        x1, y1 = P_(x + r, y + r)
        d.ellipse([x0, y0, x1, y1], fill=ink if fill else None, outline=None if fill else ink, width=lw)

    def poly(pts):
        d.line([P_(x, y) for x, y in pts], fill=ink, width=lw, joint="curve")

    def arrow_head(tip, back):
        tx, ty = tip
        bx, by = back
        ang = math.atan2(ty - by, tx - bx)          # the direction the arrow points
        for da in (0.6, -0.6):                       # two wings, about 35 degrees either side
            poly([(tx, ty), (tx - 4.5 * math.cos(ang + da), ty - 4.5 * math.sin(ang + da))])

    if name == "Fixed":
        dot(0, 0, 3.2)
        dot(0, 0, 8, fill=False)
    elif name == "Circle":
        dot(0, 0, 8, fill=False)
        dot(5.66, -5.66, 2.6)
    elif name == "Figure 8":
        poly([(11 * math.sin(t), 5.5 * math.sin(2 * t)) for t in [i * 2 * math.pi / 60 for i in range(61)]])
    elif name == "Pendulum":
        pts = [(x, 5 - 8 * (x / 11.0) ** 2) for x in [-11 + 22 * i / 30 for i in range(31)]]
        poly(pts)
        dot(6, 5 - 8 * (6 / 11.0) ** 2, 2.6)
    elif name == "Ping-Pong":
        poly([(-10, 0), (10, 0)])
        arrow_head((-11, 0), (0, 0))
        arrow_head((11, 0), (0, 0))
    elif name == "Drift":
        poly([(x, 3.2 * math.sin(x * 0.55) + 1.6 * math.sin(x * 1.3 + 1.0)) for x in [-11 + 22 * i / 40 for i in range(41)]])
    elif name == "Tide":
        poly([(0, -8), (0, 8)])
        arrow_head((0, -9), (0, 0))
        arrow_head((0, 9), (0, 0))
    elif name == "Step":
        d.arc([*P_(-8, -8), *P_(8, 8)], 0, 360, fill=ink[:3] + (110,), width=max(1, round(1.2 * s)))
        for k in range(4):
            a = k * math.pi / 2
            dot(8 * math.sin(a), -8 * math.cos(a), 2.6 if k == 1 else 1.7)
    elif name == "Scatter":
        for x, y, r in ((-8, -4, 2.2), (-2, 5, 1.8), (3, -6, 2.0), (9, 2, 2.4), (1, 0, 1.5), (-9, 6, 1.4)):
            dot(x, y, r)


def icon_pill(state, name, width):
    im = pill_base("pill_on.webp" if state else "pill_off.webp", width)
    ink = (PILL_ON_INK if state else PILL_OFF_INK) + (255,)
    icon = supersampled(im.size, 4, lambda d, s: draw_path_icon(d, name, im.width * s / 2, (im.height / 2 + 0.5) * s,
                                                                 s, ink))
    if state:   # a soft glow under the lit icon
        im.alpha_composite(icon.filter(ImageFilter.GaussianBlur(2.5)))
    im.alpha_composite(icon)
    return im


# ------------------------------------------------------------------------------------------------
# The globe: where the sound is (see the docstring)
# ------------------------------------------------------------------------------------------------
GLOBE_C = (640, 335)
NEAR_Y, FAR_Y = 252, 168          # screen y of the nearest and farthest rows: just above the head .. near the top
HW_NEAR, HW_FAR = 176, 100        # half-width of each row: the globe's chord at that height
DOT_PAD = 44
DOT_FRAMES = DOT_COLS + 1         # frame 0 = no dot


def row_geometry(r):
    t = r / (DOT_ROWS - 1)
    return NEAR_Y + (FAR_Y - NEAR_Y) * t, HW_NEAR + (HW_FAR - HW_NEAR) * t, t


def dot_frame_bounds(r):
    y, _, _ = row_geometry(r)
    fw, fh = 2 * (HW_NEAR + DOT_PAD), 2 * DOT_PAD
    return (GLOBE_C[0] - fw // 2, round(y) - DOT_PAD, fw, fh)


def orb(size, centre, t):
    """The dot: a white-hot core in a coloured glow; smaller, dimmer and violet when far (t = 1)."""
    colour = mix(CYAN, VIOLET, t)
    core_r = 13.0 - 5.0 * t
    level = 1.0 - 0.25 * t
    cx, cy = centre
    glow = Image.new("RGBA", size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gr = core_r * 2.0
    gd.ellipse([cx - gr, cy - gr, cx + gr, cy + gr], fill=colour + (round(190 * level),))
    glow = glow.filter(ImageFilter.GaussianBlur(core_r * 0.9))

    def paint(d, s):
        for k in range(10, 0, -1):          # radial gradient: colour at the rim, near-white in the middle
            rr = core_r * k / 10 * s
            c = mix((250, 255, 255), colour, (k / 10) ** 1.6)
            d.ellipse([cx * s - rr, cy * s - rr, cx * s + rr, cy * s + rr], fill=c + (round(255 * level),))
        # A thin sonar ring round it, so it reads as a target against the bright globe lines.
        rr = core_r * 1.75 * s
        d.ellipse([cx * s - rr, cy * s - rr, cx * s + rr, cy * s + rr], outline=colour + (round(200 * level),),
                  width=max(1, round(1.6 * s)))
    core = supersampled(size, 4, paint)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.alpha_composite(glow)
    out.alpha_composite(glow)
    out.alpha_composite(core)
    return out


def dot_strip(r):
    _, hw, t = row_geometry(r)
    _, _, fw, fh = dot_frame_bounds(r)
    strip = Image.new("RGBA", (fw, fh * DOT_FRAMES), (0, 0, 0, 0))
    for c in range(DOT_COLS):
        u = -1.0 + 2.0 * c / (DOT_COLS - 1)
        strip.alpha_composite(orb((fw, fh), (fw / 2 + hw * u, fh / 2), t), (0, (c + 1) * fh))
    return strip


# ------------------------------------------------------------------------------------------------
# Layout: four boxes down each gutter
# ------------------------------------------------------------------------------------------------
COL_X = (24, 856)                # left edge of the two control columns (the globe spans 445..835)
COL_W = 400
ROW_Y = (146, 244, 342, 440)
BOX_H = 90
TAB_NAMES = ("PLACE", "MOTION", "SPACE", "REACT", "BANDS")


def box(col, row):
    return (COL_X[col], ROW_Y[row], COL_X[col] + COL_W, ROW_Y[row] + BOX_H)


def knob_in(param, col, row, heading, bipolar=False):
    """A knob at the left of its box, name and value to the right."""
    bx0, by0, bx1, by1 = box(col, row)
    return dict(kind="knob", param=param, centre=(bx0 + 56, (by0 + by1) // 2), heading=heading, bipolar=bipolar)


def pills_in(param, col, row, heading, captions=None):
    """A row of pills filling a box, with the heading above them."""
    names = captions or CHOICES[param]
    bx0, by0, bx1, by1 = box(col, row)
    n = len(names)
    pw = int(min(150, (bx1 - bx0 - 28 - (n - 1) * 8) / n))
    x0 = bx0 + 20
    top = by0 + (BOX_H - PILL_H) / 2 + 10
    items = [(i, nm, x0 + i * (pw + 8), top, pw) for i, nm in enumerate(names)]
    return dict(kind="pills", param=param, items=items, heading=heading, heading_xy=(bx0 + 20, by0 + 18))


def path_grid(col, row):
    """Path: nine icon pills in two rows, with the heading at the left of the box."""
    bx0, by0, bx1, by1 = box(col, row)
    gap, per_row, head_w = 6, 5, 70
    left, right = bx0 + 16 + head_w, bx1 - 10
    pw = int((right - left - (per_row - 1) * gap) / per_row)
    tops = (by0 + 7, by1 - 7 - PILL_H)
    items = []
    for i, nm in enumerate(PATHS):
        r, k = divmod(i, per_row)
        offset = (pw + gap) / 2 if r == 1 else 0      # the second row of four sits between the first row's five
        items.append((i, nm, left + k * (pw + gap) + offset, tops[r], pw))
    return dict(kind="pills", param=P["path"], items=items, heading="PATH", heading_xy=(bx0 + 20, (by0 + by1) / 2),
                icons=True)


def page(name, left, right, scrims=()):
    """Four controls down each gutter (None = a deliberately empty box). Q-Links take the outer columns.

    A scrim is a translucent panel laid OVER some boxes while a condition holds: greyed, not hidden, so the
    controls still show what they are set to, and say plainly that they do nothing right now."""
    qlinks = [[left[r]["param"] if left[r] else -1, -1, -1, right[r]["param"] if right[r] else -1] for r in range(4)]
    boxes = [box(c, r) for c, col in ((0, left), (1, right)) for r in range(4) if col[r]]
    return name, [c for c in left + right if c], qlinks, boxes, list(scrims)


def scrim(boxes, condition, preview_when):
    """condition: the MPC visibility rule; preview_when: (param, set of choice indices) that dim it in the preview."""
    return dict(boxes=boxes, condition=condition, preview_when=preview_when)


def path_scrims(boxes, active):
    """Dim `boxes` unless Path is one of `active` (a sorted list): one scrim per run of other paths."""
    others = [i for i in range(len(PATHS)) if i not in active]
    runs, out = [], []
    for i in others:
        if runs and runs[-1][1] == i - 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    for lo, hi in runs:
        out.append(scrim(boxes, "RangedEnabling/%d/%d/%d/Parameter %d" % (lo, hi, len(PATHS), P["path"]),
                         (P["path"], set(range(lo, hi + 1)))))
    return out


def page_place():
    # The performance page: where it is (left) and how it moves (right).
    left = [knob_in(P["pan"], 0, 0, "PAN", bipolar=True),
            knob_in(P["distance"], 0, 1, "DISTANCE"),
            knob_in(P["depth"], 0, 2, "DEPTH"),
            knob_in(P["mix"], 0, 3, "MIX")]
    right = [path_grid(1, 0),
             knob_in(P["rate"], 1, 1, "RATE"),
             knob_in(P["sizex"], 1, 2, "SIZE X"),
             knob_in(P["sizey"], 1, 3, "SIZE Y")]
    return page("PLACE", left, right)


def page_motion():
    # Left: the path and its clock. Right: where on it, how smoothly, and the two settings for the hit paths.
    left = [path_grid(0, 0),
            pills_in(P["sync"], 0, 1, "SYNC", captions=["FREE", "SYNC"]),
            knob_in(P["rate"], 0, 2, "RATE"),
            pills_in(P["direction"], 0, 3, "DIRECTION", captions=["CLOCKWISE", "COUNTER"])]
    right = [knob_in(P["phase"], 1, 0, "PHASE"),
             knob_in(P["glide"], 1, 1, "GLIDE"),
             knob_in(P["step"], 1, 2, "STEP ANGLE"),
             knob_in(P["hitsens"], 1, 3, "HIT SENS")]
    return page("MOTION", left, right,
                path_scrims([box(1, 2)], [STEP]) + path_scrims([box(1, 3)], [STEP, SCATTER]))


def page_space():
    # The room and the distance cues, and the levels.
    left = [knob_in(P["room"], 0, 0, "ROOM"),
            knob_in(P["roomsize"], 0, 1, "ROOM SIZE"),
            knob_in(P["doppler"], 0, 2, "DOPPLER"),
            pills_in(P["monosafe"], 0, 3, "MONO SAFE", captions=["OFF", "ON"])]
    right = [knob_in(P["distance"], 1, 0, "DISTANCE"),
             knob_in(P["depth"], 1, 1, "DEPTH"),
             knob_in(P["mix"], 1, 2, "MIX"),
             knob_in(P["out"], 1, 3, "OUTPUT", bipolar=True)]
    return page("SPACE", left, right)


def page_react():
    # Left: how it listens (the loudness follower, and the hit detector for Step / Scatter). Right: what it does.
    left = [knob_in(P["folatt"], 0, 0, "ATTACK"),
            knob_in(P["folrel"], 0, 1, "RELEASE"),
            knob_in(P["folgain"], 0, 2, "FOL GAIN"),
            knob_in(P["hitsens"], 0, 3, "HIT SENS")]
    right = [knob_in(P["folsize"], 1, 0, "FOL > SIZE", bipolar=True),
             knob_in(P["folrate"], 1, 1, "FOL > RATE", bipolar=True),
             knob_in(P["folpush"], 1, 2, "FOL > PUSH", bipolar=True),
             knob_in(P["throw"], 1, 3, "THROW")]
    return page("REACT", left, right, path_scrims([box(0, 3)], [STEP, SCATTER]))


def page_bands():
    # Left: Split and how much each band moves. Right: where the bands divide, and how far apart they travel.
    left = [pills_in(P["split"], 0, 0, "SPLIT", captions=["OFF", "ON"]),
            knob_in(P["lowsize"], 0, 1, "LOW SIZE"),
            knob_in(P["midsize"], 0, 2, "MID SIZE"),
            knob_in(P["highsize"], 0, 3, "HIGH SIZE")]
    right = [knob_in(P["lowx"], 1, 0, "LOW XOVER"),
             knob_in(P["highx"], 1, 1, "HIGH XOVER"),
             knob_in(P["midphase"], 1, 2, "MID PHASE"),
             knob_in(P["highphase"], 1, 3, "HIGH PHASE")]
    off = scrim([box(0, 1), box(0, 2), box(0, 3), box(1, 0), box(1, 1), box(1, 2), box(1, 3)],
                "IndexedEnabling/0/2/Parameter %d" % P["split"], (P["split"], {0}))
    return page("BANDS", left, right, [off])


QLINK_RECTS = ["%d %d %d %d" % (COL_X[0], ROW_Y[r], COL_X[1] + COL_W - COL_X[0], BOX_H) for r in range(4)]


# ------------------------------------------------------------------------------------------------
# Backgrounds
# ------------------------------------------------------------------------------------------------
def base_plate():
    src = Image.open(os.path.join(HERE, "background-src.webp")).convert("RGB")
    return src.resize((W, H), Image.LANCZOS).convert("RGBA")


def scrim_image(boxes):
    """One translucent panel per box, on a canvas spanning them all. Returns (image, origin)."""
    ox, oy = min(b[0] for b in boxes), min(b[1] for b in boxes)
    w, h = max(b[2] for b in boxes) - ox, max(b[3] for b in boxes) - oy

    def paint(d, sc):
        for bx0, by0, bx1, by1 in boxes:
            d.rounded_rectangle([(bx0 - ox) * sc, (by0 - oy) * sc, (bx1 - ox) * sc, (by1 - oy) * sc],
                                radius=14 * sc, fill=(3, 10, 18, 185), outline=CREAM + (34,), width=int(1.4 * sc))
    return supersampled((int(w), int(h)), 3, paint), (ox, oy)


def draw_background(controls, boxes):
    im = base_plate()

    def paint(d, sc):
        for bx in boxes:
            d.rounded_rectangle([v * sc for v in bx], radius=14 * sc, fill=(2, 10, 16, 135),
                                outline=CYAN + (70,), width=int(1.4 * sc))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    d = ImageDraw.Draw(im)
    for ctl in controls:
        if ctl["kind"] == "knob":
            cx, cy = ctl["centre"]
            x, y = cx + KNOB["frame"] // 2 + VALUE_X_GAP, cy - 20
            size = 14
            while size > 10 and text_width(ctl["heading"], font(size, FONT_DEMI), 2.6) > 250:
                size -= 1
            draw_spaced(d, (x, y), ctl["heading"], font(size, FONT_DEMI), GOLD + (255,), spacing=2.6, anchor="lm")
        elif ctl["kind"] == "pills" and ctl.get("heading"):
            hx, hy = ctl["heading_xy"]
            draw_spaced(d, (hx, hy), ctl["heading"], font(13, FONT_DEMI), GOLD + (240,), spacing=2.4, anchor="lm")
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


VALUE_X_GAP, VALUE_W = 8, 240


def knob_component(spec, strip_file, overlay):
    """Knob at the left, its value to the right (the name is painted into the background above it)."""
    f = spec["frame"]
    pad_x = 10
    w, h = pad_x + f + VALUE_X_GAP + VALUE_W, f
    children = [focus_child(w, h),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d 0 %d %d" % (pad_x, f, f), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, 25.0, "left verticallyCentred"),
                        "%d %d %d 36" % (pad_x + f + VALUE_X_GAP, f // 2 - 2, VALUE_W), "No", "Show", "Always")]
    return G.component_definition(G.control_actions(overlay), children, "00000000"), (w, h, pad_x)


def pill_component(ctl, files, n_choices):
    xs = [it[2] for it in ctl["items"]]
    ys = [it[3] for it in ctl["items"]]
    ox, oy = min(xs) - PILL_MARGIN, min(ys) - PILL_MARGIN
    w = max(it[2] + it[4] for it in ctl["items"]) + PILL_MARGIN - ox
    h = max(ys) + PILL_H + PILL_MARGIN - oy
    children = [focus_child(int(w), int(h))]
    for (choice, caption, x, y, pw), (on, off) in zip(ctl["items"], files):
        children.append(G.entry("pill %d" % choice, "Button", G.button_data(on, off, choice, n_choices),
                                "%d %d %d %d" % (round(x - PILL_MARGIN - ox), round(y - PILL_MARGIN - oy),
                                                 pw + 2 * PILL_MARGIN, PILL_H + 2 * PILL_MARGIN),
                                "No", "Show", "Always"))
    comp = G.component_definition(G.control_actions("menu overlay", double_click=False), children, "00000000")
    return comp, (round(ox), round(oy), round(w), round(h))


def globe_children(previews, ti):
    """The live dot: one film strip per row of the plane, each driven by its own meter."""
    children = []
    for r in range(DOT_ROWS):
        x, y, w, h = dot_frame_bounds(r)
        fname = "dot_row_%d.png" % (r + 1)
        children.append(G.entry("dot row %d" % (r + 1), "Image",
                                obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                    ("image", fname), ("numFrames", DOT_FRAMES), ("handleName", "Data")),
                                "%d %d %d %d" % (x, y, w, h), "No", "Hide", "Always",
                                remap=[("Data", "Parameter %d" % (P["dot0"] + r))]))
        previews.append((ti, "dot", (fname, r)))
    return children


def build():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    images = {}

    def save(name, im):
        im.save(os.path.join(OUT, name), optimize=True)
        images[name] = im
        return name

    strips = {bi: save("knob_cyan%s.png" % ("_bi" if bi else ""), knob_strip(KNOB, CYAN, bipolar=bi))
              for bi in (False, True)}
    for r in range(DOT_ROWS):
        save("dot_row_%d.png" % (r + 1), dot_strip(r))

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("knob overlay bipolar", G.overlay_component("KnobOverlay", G.knob_overlay_data(True), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    knob_defs = {}
    for bi, strip in strips.items():
        key = "knob cyan%s" % (" bipolar" if bi else "")
        comp, geom = knob_component(KNOB, strip, "knob overlay bipolar" if bi else "knob overlay")
        defs.append((key, comp))
        knob_defs[key] = geom

    tabs, qlink_entries, previews = [], [], []
    pill_files = {}
    for ti, (name, controls, qlinks, boxes, scrims) in enumerate(
            (page_place(), page_motion(), page_space(), page_react(), page_bands())):
        bg_file = "bg_%s.jpg" % name.lower()
        bg = draw_background(controls, boxes)
        bg.save(os.path.join(OUT, bg_file), quality=92)
        children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                           ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
        previews.append((ti, "bg", bg.convert("RGBA")))
        children += globe_children(previews, ti)
        for ci, ctl in enumerate(controls):
            handle = "Parameter %d" % ctl["param"]
            if ctl["kind"] == "knob":
                key = "knob cyan%s" % (" bipolar" if ctl["bipolar"] else "")
                w, h, pad_x = knob_defs[key]
                cx, cy = ctl["centre"]
                x, y = cx - KNOB["frame"] // 2 - pad_x, cy - KNOB["frame"] // 2
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "knob", (ctl, (x, y), strips[ctl["bipolar"]])))
            elif ctl["kind"] == "pills":
                files = []
                for choice, caption, x, y, pw in ctl["items"]:
                    pair = []
                    for state in (True, False):
                        slug = "".join(ch if ch.isalnum() else "x" for ch in caption.lower())
                        fname = "%s_%s_%s_%d.png" % ("icon" if ctl.get("icons") else "pill",
                                                     "on" if state else "off", slug, pw)
                        if fname not in pill_files:
                            im = icon_pill(state, caption, pw) if ctl.get("icons") else pill(state, caption, pw)
                            pill_files[fname] = save(fname, im)
                        pair.append(fname)
                    files.append(tuple(pair))
                comp, (ox, oy, w, h) = pill_component(ctl, files, len(CHOICES[ctl["param"]]))
                comp_name = "pills %d %d" % (ti, ci)
                defs.append((comp_name, comp))
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (ox, oy, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "pills", (ctl, files)))

        # Scrims go on after the controls, so they dim what is underneath. acceptsHWFocus "No".
        for si, spec in enumerate(scrims):
            img, (ox, oy) = scrim_image(spec["boxes"])
            fname = save("scrim_%s_%d.png" % (name.lower(), si), img)
            children.append(G.entry("scrim %d" % si, "Image",
                                    obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", fname)),
                                    "%d %d %d %d" % (ox, oy, img.width, img.height), "No", "Show", "Always",
                                    invalidating=[spec["condition"]]))
            previews.append((ti, "scrim", (fname, (ox, oy), spec["preview_when"])))

        tabs.append(obj(("version", 1), ("tabName", name), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", G.component_definition([], children, "ff000000")),
                        ("qlinkBoundsData", list(QLINK_RECTS))))
        qmap = {}
        for r, row in enumerate(qlinks):
            for c, pidx in enumerate(row):
                qmap[13 + c - 4 * r] = pidx
        qlink_entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"),
                                 ("Q-Links", Obj(("Q-Link %d" % q, qmap.get(q, -1)) for q in range(1, 17)))))

    replacements = [obj(("key", "Parameter %d" % k), ("value", v)) for k, v in sorted(CHOICES.items())]
    tui = obj(("pageData", obj(
        ("version", 1),
        ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                     ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs]))),
        ("replacementStrings", replacements),
        ("info", obj(("version", 1), ("type", "CompleteDescription"))),
        ("tabs", tabs))))
    program = Obj(("Q-Link %d" % (q + 1), q) for q in range(16))
    qlinks_json = obj(("version", 4), ("info", obj(("version", 1), ("type", "CompleteDescription"))),
                      ("Screen Mode Q-Links", obj(("version", 4), ("map", qlink_entries))),
                      ("Program Mode Q-Links", program))
    with open(os.path.join(OUT, "TUI.json"), "w", encoding="utf-8") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(OUT, "Q-Links.json"), "w", encoding="utf-8") as f:
        f.write(G.dump_json(qlinks_json) + "\n")
    make_previews(previews, images)


# ------------------------------------------------------------------------------------------------
# Preview (approximate: Avenir Next stands in for the MPC's Nexa). Values come from the plugin's own
# build/params.json when it exists (written by tools/build.py), so the preview shows real defaults.
# ------------------------------------------------------------------------------------------------
PREVIEW_DOTS = [(2, 17), (5, 27)]     # (row, column + 1) of the dot in odd / even previews, to show near and far


def preview_values():
    path = os.path.join(PROJECT, "build", "params.json")
    vals = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for p in json.load(f).get("params", []):
                text = p["display"] + ((" " + p["label"]) if p["label"] else "")
                vals[p["index"]] = (p["default"], text)
    return vals


def make_previews(previews, images):
    vals = preview_values()
    pages = {}
    for ti, kind, data in previews:
        if kind == "bg":
            pages[ti] = data

    def choice_index(param):
        v = vals.get(param, (0, ""))[0]
        return int(round(v * (len(CHOICES[param]) - 1)))

    for ti, kind, data in previews:
        im = pages[ti]
        d = ImageDraw.Draw(im)
        if kind == "dot":
            fname, r = data
            row, frame = PREVIEW_DOTS[ti % 2]
            if r != row:
                continue
            x, y, w, h = dot_frame_bounds(r)
            im.alpha_composite(images[fname].crop((0, frame * h, w, (frame + 1) * h)), (x, y))
        elif kind == "knob":
            ctl, (x, y), strip_name = data
            v, text = vals.get(ctl["param"], (0.5, ""))
            f = KNOB["frame"]
            fi = round(v * (KNOB["frames"] - 1))
            im.alpha_composite(images[strip_name].crop((0, fi * f, f, (fi + 1) * f)), (x + 10, y))
            d.text((x + 10 + f + VALUE_X_GAP, y + f / 2 + 16), text, font=font(22, FONT_MEDIUM), fill=CREAM, anchor="lm")
        elif kind == "scrim":
            fname, (ox, oy), (param, idxs) = data
            if choice_index(param) in idxs:
                im.alpha_composite(images[fname], (ox, oy))
        elif kind == "pills":
            ctl, files = data
            sel = choice_index(ctl["param"])
            for (choice, caption, x, y, pw), (on, off) in zip(ctl["items"], files):
                im.alpha_composite(images[on if choice == sel else off], (round(x - PILL_MARGIN), round(y - PILL_MARGIN)))
    for ti, im in pages.items():
        full = Image.new("RGBA", (1280, 800), (24, 24, 26, 255))
        full.alpha_composite(im, (0, 110))
        fd = ImageDraw.Draw(full)
        fd.text((24, 55), "MPC header (drawn by the MPC)", font=font(18, FONT_MEDIUM), fill=(120, 120, 120), anchor="lm")
        step = 1280 / len(TAB_NAMES)
        for k, nm in enumerate(TAB_NAMES):
            fd.text((step * (k + 0.5), 770), nm, font=font(18, FONT_DEMI),
                    fill=(255, 255, 255) if k == ti else (140, 140, 140), anchor="mm")
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        full.convert("RGB").save(os.path.join(PREVIEW_DIR, "skin-preview-%d.png" % (ti + 1)))


if __name__ == "__main__":
    build()
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("wrote %s (%d files, %.1f MB)" % (OUT, len(os.listdir(OUT)), total / 1e6))
