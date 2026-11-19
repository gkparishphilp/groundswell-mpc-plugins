#!/usr/bin/env python3
"""Build Fathom's hand-made skin (plugins/Fathom/skins/) from the artwork in this folder.

    python3 plugins/Fathom/art/make_skin.py      # writes skins/ and build/skin-preview-*.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    background-src.webp     the plate: title, the lit display window in the middle, empty gutters either side
                            (1790x878, the 1280:628 aspect; the corners outside the frame are a baked-in
                            checkerboard and are masked off here)
    mock-reference.webp     the designer's mock-up, for reference only - its controls are not Fathom's
    knob_small.webp         knob with skirt, pointer at 12 o'clock (shared with Whalesong)
    pill_on/off.webp        button pill (shared with Whalesong)

Layout: four boxes down each gutter, eight controls to a tab, the Q-Links on the outer columns.

The display window is the point of the panel. The painted response curve in the art is dimmed behind a
glass scrim and replaced by two live ones: cyan for filter A, violet for filter B, drawn from the plugin's
A Cutoff / B Cutoff meters (parameters 27 and 28), so you watch the filter breathe. There is one film strip
per filter type, each shown only while that type is selected. B's curve is hidden while B is Off.

Controls for filter B have violet arcs; everything else is cyan.

The skin binds to parameter indices, so it must be regenerated if the parameter list changes. The drawing
machinery is a copy of Whalesong's generator (deliberately not a shared import: a plugin's artwork should
not break because a sibling's did).
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
CYAN = (63, 217, 227)
VIOLET = (190, 110, 255)
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match src/Fathom.cpp).
P = dict(out=0, mix=1, drive=2, breathe=3,
         atype=4, afreq=5, ares=6, routing=7,
         btype=8, bfreq=9, bres=10,
         folatt=11, folrel=12, folgain=13, follfo=14,
         shape=15, sync=16, rate=17, spread=18,
         envsens=19, envdec=20,
         fola=21, lfoa=22, enva=23, folb=24, lfob=25, envb=26,
         metera=27, meterb=28,
         apan=29, bpan=30, lfobmode=31, lfobshape=32, lfobsync=33, lfobrate=34,
         envswell=35)

A_TYPES = ["Wah", "Lowpass", "Bandpass", "Highpass", "Notch"]
B_TYPES = ["Off"] + A_TYPES
SHAPES = ["Sine", "Triangle", "Ramp Up", "Ramp Down", "Square", "Steps", "Drift"]
CHOICES = {P["atype"]: A_TYPES,
           P["routing"]: ["Series", "Parallel"],
           P["btype"]: B_TYPES,
           P["shape"]: SHAPES,
           P["sync"]: ["Free", "Sync"],
           P["lfobmode"]: ["Link A", "Own"],
           P["lfobshape"]: SHAPES,
           P["lfobsync"]: ["Free", "Sync"]}

# Filter B's controls get violet arcs, so the colour on the knob matches the colour of B's curve.
VIOLET_PARAMS = {P["bfreq"], P["bres"], P["folb"], P["lfob"], P["envb"], P["bpan"], P["lfobshape"], P["lfobrate"]}

# Knob geometry: Whalesong's small knob, scaled to fit a 90 px box. Film strips stay under 11430 px tall.
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
# Pills (3-slice stretch so every width keeps round ends)
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
    while size > 9 and text_width(text, font(size, FONT_DEMI), spacing) > width - 12:
        size -= 1
        spacing = 0.4
    f = font(size, FONT_DEMI)
    draw_spaced(d, (im.width / 2, im.height / 2 + 1), text, f, colour + (255,), spacing=spacing)
    return im


# ------------------------------------------------------------------------------------------------
# The display: response curves matching the DSP (Cytomic SVF, analog prototype, src/Fathom.cpp)
# ------------------------------------------------------------------------------------------------
GLASS = (378, 182, 904, 404)            # the dark glass over the painted curve
GRAPH = (394, 194, 494, 150)            # x y w h of the plot
GRAPH_DB = (-36.0, 12.0)
GRAPH_FREQ = (20.0, 20000.0)            # same axis as the meters: 20 Hz .. 20 kHz, log
CURVE_FRAMES = 48
READ_Y = 378                            # the live readouts under the plot


def response_db(ftype, f, fc, reso):
    """Magnitude of FilterSlot::process for one type, as the analog prototype (ignores warping)."""
    r = 0.35 + 0.60 * reso if ftype == "Wah" else 0.95 * reso
    k = 2.0 - 1.98 * r
    w = f / fc
    den = math.hypot(1.0 - w * w, k * w)
    if ftype == "Wah":
        mag = math.hypot(0.2, 1.6 * k * w) / den
    elif ftype == "Lowpass":
        mag = 1.0 / den
    elif ftype == "Bandpass":
        mag = 1.4 * k * w / den
    elif ftype == "Highpass":
        mag = w * w / den
    else:
        mag = abs(1.0 - w * w) / den
    return 20 * math.log10(max(mag, 1e-6))


def graph_y(db):
    lo, hi = GRAPH_DB
    db = max(lo, min(hi, db))
    return (hi - db) / (hi - lo)


def graph_x(f):
    lo, hi = GRAPH_FREQ
    return (math.log10(f) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))


def curve_image(ftype, fc, reso, colour):
    gw, gh = GRAPH[2], GRAPH[3]

    def paint(d, s):
        pts = []
        for i in range(0, gw * s + 1, 2):
            f = GRAPH_FREQ[0] * (GRAPH_FREQ[1] / GRAPH_FREQ[0]) ** (i / (gw * s))
            pts.append((i, graph_y(response_db(ftype, f, fc, reso)) * gh * s))
        d.polygon(pts + [(gw * s, gh * s), (0, gh * s)], fill=colour + (34,))
        d.line(pts, fill=colour + (255,), width=int(2.4 * s), joint="curve")
        # The marker at the cutoff, like the mock's: where the filter is right now.
        mx = graph_x(fc) * gw * s
        my = graph_y(response_db(ftype, fc, fc, reso)) * gh * s
        rr = 4.5 * s
        d.ellipse([mx - rr, my - rr, mx + rr, my + rr], fill=(235, 250, 255, 255), outline=colour + (255,),
                  width=int(1.6 * s))

    sharp = supersampled((gw, gh), 3, paint)
    glow = sharp.filter(ImageFilter.GaussianBlur(4))
    out = Image.new("RGBA", (gw, gh), (0, 0, 0, 0))
    out.alpha_composite(glow)
    out.alpha_composite(sharp)
    return out


def curve_strip(ftype, reso, colour):
    gw, gh = GRAPH[2], GRAPH[3]
    strip = Image.new("RGBA", (gw, gh * CURVE_FRAMES), (0, 0, 0, 0))
    lo, hi = GRAPH_FREQ
    for i in range(CURVE_FRAMES):
        fc = lo * (hi / lo) ** (i / (CURVE_FRAMES - 1))
        strip.alpha_composite(curve_image(ftype, fc, reso, colour), (0, i * gh))
    return strip


# ------------------------------------------------------------------------------------------------
# Layout: four boxes down each gutter
# ------------------------------------------------------------------------------------------------
COL_X = (26, 926)                # left edge of the two control columns
COL_W = 328
ROW_Y = (146, 244, 342, 440)
BOX_H = 90
TAB_NAMES = ("MAIN", "FILTERS", "MOTION", "LFO", "DEPTH")


def box(col, row):
    return (COL_X[col], ROW_Y[row], COL_X[col] + COL_W, ROW_Y[row] + BOX_H)


def knob_in(param, col, row, heading, bipolar=False):
    """A knob at the left of its box, name and value to the right."""
    bx0, by0, bx1, by1 = box(col, row)
    return dict(kind="knob", param=param, centre=(bx0 + 56, (by0 + by1) // 2), heading=heading,
                bipolar=bipolar, violet=param in VIOLET_PARAMS)


def pills_in(param, col, row, heading, captions=None):
    """A row of pills filling a box, with the heading above them."""
    names = captions or CHOICES[param]
    bx0, by0, bx1, by1 = box(col, row)
    n = len(names)
    pw = int((bx1 - bx0 - 28 - (n - 1) * 8) / n)
    x0 = (bx0 + bx1) / 2 - (n * pw + (n - 1) * 8) / 2
    top = by0 + (BOX_H - PILL_H) / 2 + 10
    items = [(i, nm, x0 + i * (pw + 8), top, pw) for i, nm in enumerate(names)]
    return dict(kind="pills", param=param, items=items, heading=heading, heading_xy=(bx0 + 20, by0 + 18))


def page(name, left, right, scrims=()):
    """Four controls down each gutter (None = a deliberately empty box). Q-Links take the outer columns.

    A scrim is a translucent panel laid OVER some boxes while a condition holds: greyed, not hidden, so the
    controls still show what they are set to, and say plainly that they do nothing right now."""
    qlinks = [[left[r]["param"] if left[r] else -1, -1, -1, right[r]["param"] if right[r] else -1] for r in range(4)]
    boxes = [box(c, r) for c, col in ((0, left), (1, right)) for r in range(4) if col[r]]
    return name, [c for c in left + right if c], qlinks, boxes, list(scrims)


def scrim(boxes, condition, preview_when):
    """condition: the MPC visibility rule; preview_when: (param, choice index) under which the preview dims."""
    return dict(boxes=boxes, condition=condition, preview_when=preview_when)


def page_main():
    # The breathing wah on one page: filter A, how far the follower and LFO move it, Breathe, levels.
    left = [pills_in(P["atype"], 0, 0, "FILTER A", captions=["WAH", "LP", "BP", "HP", "NOTCH"]),
            knob_in(P["afreq"], 0, 1, "FREQ"),
            knob_in(P["ares"], 0, 2, "RESO"),
            knob_in(P["breathe"], 0, 3, "BREATHE")]
    right = [knob_in(P["fola"], 1, 0, "FOLLOW", bipolar=True),
             knob_in(P["lfoa"], 1, 1, "LFO", bipolar=True),
             knob_in(P["mix"], 1, 2, "MIX"),
             knob_in(P["out"], 1, 3, "OUTPUT", bipolar=True)]
    return page("MAIN", left, right)


def page_filters():
    left = [pills_in(P["atype"], 0, 0, "FILTER A", captions=["WAH", "LP", "BP", "HP", "NOTCH"]),
            knob_in(P["afreq"], 0, 1, "A FREQ"),
            knob_in(P["ares"], 0, 2, "A RESO"),
            knob_in(P["drive"], 0, 3, "DRIVE")]
    right = [pills_in(P["btype"], 1, 0, "FILTER B", captions=["OFF", "WAH", "LP", "BP", "HP", "NOTCH"]),
             knob_in(P["bfreq"], 1, 1, "B FREQ"),
             knob_in(P["bres"], 1, 2, "B RESO"),
             pills_in(P["routing"], 1, 3, "ROUTING", captions=["SERIES", "PARALLEL"])]
    return page("FILTERS", left, right)


def page_motion():
    # Left: the follower and Breathe, which together are the breathing. Right: the transient envelope (Sens,
    # Swell = its rise, Decay), and Spread (it offsets the right channel of both LFOs and of Breathe's drift).
    left = [knob_in(P["folatt"], 0, 0, "ATTACK"),
            knob_in(P["folrel"], 0, 1, "RELEASE"),
            knob_in(P["folgain"], 0, 2, "FOL GAIN"),
            knob_in(P["breathe"], 0, 3, "BREATHE")]
    right = [knob_in(P["envsens"], 1, 0, "ENV SENS"),
             knob_in(P["envswell"], 1, 1, "SWELL"),
             knob_in(P["envdec"], 1, 2, "ENV DECAY"),
             knob_in(P["spread"], 1, 3, "SPREAD")]
    return page("MOTION", left, right)


def page_lfo():
    # LFO A down the left, LFO B down the right. B's own settings grey out while it is linked to A.
    left = [knob_in(P["shape"], 0, 0, "A SHAPE"),
            pills_in(P["sync"], 0, 1, "LFO A SYNC", captions=["FREE", "SYNC"]),
            knob_in(P["rate"], 0, 2, "A RATE"),
            knob_in(P["follfo"], 0, 3, "FOL > LFO", bipolar=True)]
    right = [pills_in(P["lfobmode"], 1, 0, "LFO B", captions=["LINK A", "OWN"]),
             knob_in(P["lfobshape"], 1, 1, "B SHAPE"),
             pills_in(P["lfobsync"], 1, 2, "LFO B SYNC", captions=["FREE", "SYNC"]),
             knob_in(P["lfobrate"], 1, 3, "B RATE")]
    linked = scrim([box(1, 1), box(1, 2), box(1, 3)],
                   "IndexedEnabling/0/2/Parameter %d" % P["lfobmode"], (P["lfobmode"], 0))
    return page("LFO", left, right, [linked])


def page_depth():
    # Everything per filter: how far each mover pushes it, and where it sits. A left, B right.
    left = [knob_in(P["fola"], 0, 0, "FOL > A", bipolar=True),
            knob_in(P["lfoa"], 0, 1, "LFO > A", bipolar=True),
            knob_in(P["enva"], 0, 2, "ENV > A", bipolar=True),
            knob_in(P["apan"], 0, 3, "A PAN", bipolar=True)]
    right = [knob_in(P["folb"], 1, 0, "FOL > B", bipolar=True),
             knob_in(P["lfob"], 1, 1, "LFO > B", bipolar=True),
             knob_in(P["envb"], 1, 2, "ENV > B", bipolar=True),
             knob_in(P["bpan"], 1, 3, "B PAN", bipolar=True)]
    series = scrim([box(0, 3), box(1, 3)],
                   "IndexedEnabling/0/2/Parameter %d" % P["routing"], (P["routing"], 0))
    return page("DEPTH", left, right, [series])


QLINK_RECTS = ["%d %d %d %d" % (COL_X[0], ROW_Y[r], COL_X[1] + COL_W - COL_X[0], BOX_H) for r in range(4)]


# ------------------------------------------------------------------------------------------------
# Backgrounds
# ------------------------------------------------------------------------------------------------
FRAME = (13, 12, 1267, 615)       # the plate's outer edge in the art; outside it is a baked-in checkerboard


def base_plate():
    src = Image.open(os.path.join(HERE, "background-src.webp")).convert("RGB")
    plate = src.resize((W, H), Image.LANCZOS).convert("RGBA")
    # Mask off everything outside the plate's rounded frame.
    mask = supersampled((W, H), 4, lambda d, s: d.rounded_rectangle(
        [FRAME[0] * s, FRAME[1] * s, FRAME[2] * s, FRAME[3] * s], radius=20 * s, fill=(255, 255, 255, 255)))
    out = Image.new("RGBA", (W, H), (6, 8, 10, 255))
    out.paste(plate, (0, 0), mask.split()[3])
    return out


def scrim_image(boxes):
    """One translucent panel per box, on a canvas spanning them all. Returns (image, origin)."""
    ox, oy = min(b[0] for b in boxes), min(b[1] for b in boxes)
    w, h = max(b[2] for b in boxes) - ox, max(b[3] for b in boxes) - oy

    def paint(d, sc):
        for bx0, by0, bx1, by1 in boxes:
            d.rounded_rectangle([(bx0 - ox) * sc, (by0 - oy) * sc, (bx1 - ox) * sc, (by1 - oy) * sc],
                                radius=14 * sc, fill=(3, 12, 18, 180), outline=CREAM + (34,), width=int(1.4 * sc))
    return supersampled((int(w), int(h)), 3, paint), (ox, oy)


def draw_background(controls, boxes):
    im = base_plate()

    def paint(d, sc):
        line = CREAM + (70,)
        # The glass over the display window: dims the painted curve so the live ones can replace it.
        gx0, gy0, gx1, gy1 = GLASS
        d.rounded_rectangle([gx0 * sc, gy0 * sc, gx1 * sc, gy1 * sc], radius=14 * sc,
                            fill=(2, 12, 18, 236), outline=CYAN + (80,), width=int(1.2 * sc))
        gx, gy, gw, gh = GRAPH
        for db in (12, 0, -12, -24, -36):
            y = gy + graph_y(db) * gh
            d.line([(gx * sc, y * sc), ((gx + gw) * sc, y * sc)], fill=CREAM + (60 if db == 0 else 26,), width=sc)
        for f in (20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000):
            x = gx + graph_x(f) * gw
            d.line([(x * sc, gy * sc), (x * sc, (gy + gh) * sc)], fill=CREAM + (26,), width=sc)
        for bx in boxes:
            d.rounded_rectangle([v * sc for v in bx], radius=14 * sc, fill=(2, 10, 14, 120), outline=line,
                                width=int(1.4 * sc))

    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    d = ImageDraw.Draw(im)
    gx, gy, gw, gh = GRAPH
    small = font(11, FONT_MEDIUM)
    for f_hz, lab in ((20, "20"), (50, "50"), (100, "100"), (200, "200"), (500, "500"), (1000, "1k"),
                      (2000, "2k"), (5000, "5k"), (10000, "10k"), (20000, "20k")):
        d.text((gx + graph_x(f_hz) * gw, gy + gh + 10), lab, font=small, fill=CREAM + (150,), anchor="mm")
    # Captions for the two live readouts.
    draw_spaced(d, (414, READ_Y), "FILTER A", font(12, FONT_DEMI), CYAN + (255,), spacing=1.8, anchor="lm")
    # "FILTER B" is a live Label (display_children), hidden with B's readout while B is Off.

    for ctl in controls:
        if ctl["kind"] == "knob":
            cx, cy = ctl["centre"]
            x, y = cx + KNOB["frame"] // 2 + VALUE_X_GAP, cy - 20
            size = 14
            while size > 10 and text_width(ctl["heading"], font(size, FONT_DEMI), 2.2) > 180:
                size -= 1
            draw_spaced(d, (x, y), ctl["heading"], font(size, FONT_DEMI), CREAM + (255,), spacing=2.2, anchor="lm")
        elif ctl["kind"] == "pills" and ctl.get("heading"):
            draw_spaced(d, ctl["heading_xy"], ctl["heading"], font(12, FONT_DEMI), CREAM + (230,),
                        spacing=1.8, anchor="lm")
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


VALUE_X_GAP, VALUE_W = 8, 190


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


def display_children(previews, ti):
    """The live part of the display window: one curve strip per filter type, and the two readouts."""
    gx, gy, gw, gh = GRAPH
    children = []
    for slot, meter, sel, types, colour_name in (("a", P["metera"], P["atype"], A_TYPES, "cyan"),
                                                  ("b", P["meterb"], P["btype"], B_TYPES, "violet")):
        for idx, ftype in enumerate(types):
            if ftype == "Off":
                continue
            fname = "curve_%s_%s.png" % (slot, ftype.lower())
            children.append(G.entry("curve %s %s" % (slot, ftype), "Image",
                                    obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                        ("image", fname), ("numFrames", CURVE_FRAMES), ("handleName", "Data")),
                                    "%d %d %d %d" % (gx, gy, gw, gh), "No", "Hide", "Always",
                                    remap=[("Data", "Parameter %d" % meter)],
                                    invalidating=["IndexedEnabling/%d/%d/Parameter %d" % (idx, len(types), sel)]))
            previews.append((ti, "curve", (fname, meter, sel, idx)))
    b_on = ["RangedEnabling/1/%d/%d/Parameter %d" % (len(B_TYPES) - 1, len(B_TYPES), P["btype"])]
    children.append(G.entry("caption b", "Label",
                            G.label_data("Text", "ff%02x%02x%02x" % VIOLET, 15.0, "left verticallyCentred",
                                         text="FILTER B"),
                            "664 %d 84 24" % (READ_Y - 12), "No", "Hide", "Always",
                            remap=[("Data", "Parameter %d" % P["btype"])], invalidating=b_on))
    for x, meter, colour, cond in ((500, P["metera"], CYAN, None), (750, P["meterb"], VIOLET, b_on)):
        children.append(G.entry("readout %d" % meter, "Label",
                                G.label_data("Value", "ff%02x%02x%02x" % colour, 22.0, "left verticallyCentred"),
                                "%d %d 140 28" % (x, READ_Y - 14), "No", "Hide", "Always",
                                remap=[("Data", "Parameter %d" % meter)], invalidating=cond))
        previews.append((ti, "readout", (x, meter, colour)))
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

    strips = {}
    for colour_name, colour in (("cyan", CYAN), ("violet", VIOLET)):
        for bi in (False, True):
            name = "knob_%s%s.png" % (colour_name, "_bi" if bi else "")
            strips[(colour_name, bi)] = save(name, knob_strip(KNOB, colour, bipolar=bi))
    # Curves at each slot's default resonance: the shape is a guide, the position is live.
    for ftype in A_TYPES:
        save("curve_a_%s.png" % ftype.lower(), curve_strip(ftype, 0.6, CYAN))
        save("curve_b_%s.png" % ftype.lower(), curve_strip(ftype, 0.4, VIOLET))

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("knob overlay bipolar", G.overlay_component("KnobOverlay", G.knob_overlay_data(True), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    knob_defs = {}
    for (colour_name, bi), strip in strips.items():
        key = "knob %s%s" % (colour_name, " bipolar" if bi else "")
        comp, geom = knob_component(KNOB, strip, "knob overlay bipolar" if bi else "knob overlay")
        defs.append((key, comp))
        knob_defs[key] = geom

    tabs, qlink_entries, previews = [], [], []
    pill_files = {}
    for ti, (name, controls, qlinks, boxes, scrims) in enumerate(
            (page_main(), page_filters(), page_motion(), page_lfo(), page_depth())):
        bg_file = "bg_%s.jpg" % name.lower()
        bg = draw_background(controls, boxes)
        bg.save(os.path.join(OUT, bg_file), quality=92)
        children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                           ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
        previews.append((ti, "bg", bg.convert("RGBA")))
        children += display_children(previews, ti)
        for ci, ctl in enumerate(controls):
            handle = "Parameter %d" % ctl["param"]
            if ctl["kind"] == "knob":
                key = "knob %s%s" % ("violet" if ctl["violet"] else "cyan", " bipolar" if ctl["bipolar"] else "")
                w, h, pad_x = knob_defs[key]
                cx, cy = ctl["centre"]
                x, y = cx - KNOB["frame"] // 2 - pad_x, cy - KNOB["frame"] // 2
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "knob", (ctl, (x, y), strips[("violet" if ctl["violet"] else "cyan", ctl["bipolar"])])))
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
def preview_values():
    path = os.path.join(PROJECT, "build", "params.json")
    vals = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for p in json.load(f).get("params", []):
                text = p["display"] + ((" " + p["label"]) if p["label"] else "")
                vals[p["index"]] = (p["default"], text)
    # The meters are not in older params.json files; show them somewhere sensible.
    vals.setdefault(P["metera"], (0.42, "380 Hz"))
    vals.setdefault(P["meterb"], (0.6, "1.50 kHz"))
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
        if kind == "curve":
            fname, meter, sel, idx = data
            if choice_index(sel) != idx:
                continue
            strip = images[fname]
            gh = GRAPH[3]
            fi = int(round(vals[meter][0] * (CURVE_FRAMES - 1)))
            im.alpha_composite(strip.crop((0, fi * gh, strip.width, (fi + 1) * gh)), (GRAPH[0], GRAPH[1]))
        elif kind == "readout":
            x, meter, colour = data
            if meter == P["meterb"]:
                if choice_index(P["btype"]) == 0:
                    continue
                draw_spaced(d, (664, READ_Y), "FILTER B", font(12, FONT_DEMI), VIOLET + (255,), spacing=1.8, anchor="lm")
            d.text((x, READ_Y), vals[meter][1], font=font(19, FONT_MEDIUM), fill=colour, anchor="lm")
        elif kind == "knob":
            ctl, (x, y), strip_name = data
            v, text = vals.get(ctl["param"], (0.5, ""))
            f = KNOB["frame"]
            fi = round(v * (KNOB["frames"] - 1))
            im.alpha_composite(images[strip_name].crop((0, fi * f, f, (fi + 1) * f)), (x + 10, y))
            d.text((x + 10 + f + VALUE_X_GAP, y + f / 2 + 16), text, font=font(22, FONT_MEDIUM), fill=CREAM, anchor="lm")
        elif kind == "scrim":
            fname, (ox, oy), (param, idx) = data
            if choice_index(param) == idx:
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
