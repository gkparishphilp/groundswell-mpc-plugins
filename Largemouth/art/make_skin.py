#!/usr/bin/env python3
"""Build Largemouth's hand-made skin (plugins/Largemouth/skins/) from the artwork in this folder.

    python3 plugins/Largemouth/art/make_skin.py      # writes skins/ and build/skin-preview-*.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    hero-src.webp           the plate: the title and the bass under the surface, open water either side
                            (1791x878, the 1280:628 aspect)
    mock-reference.webp     the designer's mock-up, for the look only - its controls are not Largemouth's
    knob_small.webp         knob with skirt, pointer at 12 o'clock (shared with Fathom, Whalesong and Bearings)
    pill_on/off.webp        button pill (shared likewise)

Layout: four boxes down each side of the fish, eight controls to a tab, the Q-Links on the outer columns. Under the
fish, on every tab, a status strip shows what the plugin is hearing: a light that flashes on each kick, the key it
found, how sure it is, and the chord the bass is on.

The skin binds to parameter indices, so it must be regenerated if the parameter list changes. The drawing machinery
is a copy of Bearings' generator (deliberately not a shared import: a plugin's artwork should not break because a
sibling's did).
"""
import json
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
CREAM = (226, 240, 228)
GOLD = (232, 204, 148)
TEAL = (58, 214, 186)
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match src/Largemouth.cpp).
P = dict(out=0, bass=1, follow=2, sens=3, busy=4, rate=5, swing=6, vary=7,
         changes=8, chordlen=9, walk=10, jump=11, listen=12, key=13, scale=14, octave=15,
         voice=16, tone=17, reso=18, drive=19, decay=20, punch=21, glide=22, length=23,
         duck=24, ducktime=25, solo=26, found=27, sure=28, chord=29, kick=30)

NOTES12 = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
VOICES = ["Sub", "808", "Pluck", "Acid", "Reese"]
CHOICES = {P["follow"]: ["Grid", "Hits"],
           P["rate"]: ["1/4", "1/8", "1/16"],
           P["changes"]: ["Follow", "Tonic", "1-6-7-1", "1-4-5-1", "1-5-6-4", "1-7-6-7", "1-4-6-5", "1-6-4-5"],
           P["chordlen"]: ["1/2 Bar", "1 Bar", "2 Bars", "4 Bars"],
           P["listen"]: ["Follow", "Lock", "Off"],
           P["key"]: NOTES12,
           P["scale"]: ["Minor", "Major", "Dorian", "Phrygian", "Mixolyd"],
           P["octave"]: ["-1", "0", "+1", "+2"],
           P["voice"]: VOICES,
           P["solo"]: ["Off", "On"],
           P["found"]: ["--"] + NOTES12 + [n + "m" for n in NOTES12],
           P["chord"]: ["--"] + [n + q for n in NOTES12 for q in ("", "m", "dim")]}

# Knob geometry: Bearings', a 90 px box. Film strips stay under 11430 px tall.
KNOB = dict(src="knob_small.webp", crop=300, knob=68, arc_r=40, arc_w=4, frame=88, frames=100, ticks=(47, 53))
KNOB_CENTRE_SRC = (623.5, 625.5)

VALUE_COLOUR = "ffe2f0e4"
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
# Pills (3-slice stretch so every width keeps round ends), with a caption
# ------------------------------------------------------------------------------------------------
PILL_BODY = {"pill_on.webp": (446, 207, 1553, 465), "pill_off.webp": (427, 201, 1568, 465)}
PILL_H, PILL_MARGIN = 34, 9
PILL_ON_INK, PILL_OFF_INK = (200, 248, 236), (190, 182, 162)


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


# ------------------------------------------------------------------------------------------------
# The kick light: a glowing bead, dark to bright in 11 frames (the Kick meter steps in tenths)
# ------------------------------------------------------------------------------------------------
LAMP = 48
LAMP_FRAMES = 11


def lamp_strip():
    strip = Image.new("RGBA", (LAMP, LAMP * LAMP_FRAMES), (0, 0, 0, 0))
    c = LAMP / 2
    for i in range(LAMP_FRAMES):
        t = i / (LAMP_FRAMES - 1)
        frame = Image.new("RGBA", (LAMP, LAMP), (0, 0, 0, 0))
        if t > 0:
            glow = Image.new("RGBA", (LAMP, LAMP), (0, 0, 0, 0))
            gr = 9 + 9 * t
            ImageDraw.Draw(glow).ellipse([c - gr, c - gr, c + gr, c + gr], fill=mix(TEAL, GOLD, 0.35) + (round(210 * t),))
            frame.alpha_composite(glow.filter(ImageFilter.GaussianBlur(5)))

        def paint(d, s, t=t):
            r = 8 * s
            for k in range(8, 0, -1):
                rr = r * k / 8
                base = mix((20, 44, 40), mix(TEAL, (250, 255, 240), 0.5), t)
                col = mix((250, 255, 240), base, (k / 8) ** 1.4) if t > 0 else base
                d.ellipse([c * s - rr, c * s - rr, c * s + rr, c * s + rr], fill=col + (255,))
            d.ellipse([c * s - r, c * s - r, c * s + r, c * s + r], outline=CREAM + (90,), width=max(1, round(1.2 * s)))
        frame.alpha_composite(supersampled((LAMP, LAMP), 4, paint))
        strip.alpha_composite(frame, (0, i * LAMP))
    return strip


# ------------------------------------------------------------------------------------------------
# Layout: four boxes down each side of the fish
# ------------------------------------------------------------------------------------------------
COL_X = (14, 976)                # left edge of the two control columns (the fish spans ~300..958)
COL_W = 290
ROW_Y = (138, 236, 334, 432)
BOX_H = 90
TAB_NAMES = ("GROOVE", "NOTES", "SOUND", "MIX")
KNOB_X = 54                      # knob centre from the box's left edge

# The status strip under the fish: kick light, key, sureness, chord.
STRIP = (330, 538, 950, 604)
CELLS = 4


def box(col, row):
    return (COL_X[col], ROW_Y[row], COL_X[col] + COL_W, ROW_Y[row] + BOX_H)


def knob_in(param, col, row, heading, bipolar=False):
    """A knob at the left of its box, name and value to the right. A choice parameter's overlay is a menu."""
    bx0, by0, bx1, by1 = box(col, row)
    return dict(kind="knob", param=param, centre=(bx0 + KNOB_X, (by0 + by1) // 2), heading=heading, bipolar=bipolar,
                menu=param in CHOICES)


def pills_in(param, col, row, heading, captions=None):
    """A row of pills filling a box, with the heading above them."""
    names = captions or CHOICES[param]
    bx0, by0, bx1, by1 = box(col, row)
    n = len(names)
    pw = int(min(120, (bx1 - bx0 - 28 - (n - 1) * 8) / n))
    x0 = bx0 + 14
    top = by0 + (BOX_H - PILL_H) / 2 + 10
    items = [(i, nm, x0 + i * (pw + 8), top, pw) for i, nm in enumerate(names)]
    return dict(kind="pills", param=param, items=items, heading=heading, heading_xy=(bx0 + 16, by0 + 18))


def pill_grid(param, col, row, heading, per_row):
    """Pills in two rows with the heading at the left of the box (Voice: five of them)."""
    names = CHOICES[param]
    bx0, by0, bx1, by1 = box(col, row)
    gap, head_w = 6, 66
    left, right = bx0 + 12 + head_w, bx1 - 8
    pw = int((right - left - (per_row - 1) * gap) / per_row)
    tops = (by0 + 7, by1 - 7 - PILL_H)
    items = []
    for i, nm in enumerate(names):
        r, k = divmod(i, per_row)
        offset = (pw + gap) / 2 if r == 1 and len(names) - per_row < per_row else 0
        items.append((i, nm, left + k * (pw + gap) + offset, tops[r], pw))
    return dict(kind="pills", param=param, items=items, heading=heading, heading_xy=(bx0 + 16, (by0 + by1) / 2))


def page(name, left, right, scrims=()):
    """Four controls down each side (None = a deliberately empty box). Q-Links take the outer columns."""
    qlinks = [[left[r]["param"] if left[r] else -1, -1, -1, right[r]["param"] if right[r] else -1] for r in range(4)]
    boxes = [box(c, r) for c, col in ((0, left), (1, right)) for r in range(4) if col[r]]
    return name, [c for c in left + right if c], qlinks, boxes, list(scrims)


def scrim(boxes, condition, preview_when):
    """condition: the MPC visibility rule; preview_when: (param, set of choice indices) that dim it in the preview."""
    return dict(boxes=boxes, condition=condition, preview_when=preview_when)


def scrims_unless(boxes, param, active):
    """Dim `boxes` unless `param` is one of the choice indices in `active`: one scrim per run of other choices."""
    n = len(CHOICES[param])
    others = [i for i in range(n) if i not in active]
    runs, out = [], []
    for i in others:
        if runs and runs[-1][1] == i - 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    for lo, hi in runs:
        out.append(scrim(boxes, "RangedEnabling/%d/%d/%d/Parameter %d" % (lo, hi, n, param),
                         (param, set(range(lo, hi + 1)))))
    return out


def page_groove():
    # When it plays: how it follows the kick, and what it adds between the kicks.
    left = [pills_in(P["follow"], 0, 0, "FOLLOW", captions=["GRID", "HITS"]),
            knob_in(P["sens"], 0, 1, "KICK SENS"),
            knob_in(P["busy"], 0, 2, "BUSY"),
            pills_in(P["rate"], 0, 3, "RATE")]
    right = [knob_in(P["swing"], 1, 0, "SWING"),
             knob_in(P["vary"], 1, 1, "VARY"),
             knob_in(P["length"], 1, 2, "LENGTH"),
             knob_in(P["glide"], 1, 3, "GLIDE")]
    return page("GROOVE", left, right)


def page_notes():
    # What it plays: where the roots come from (left) and the key it thinks in (right).
    left = [knob_in(P["changes"], 0, 0, "CHANGES"),
            knob_in(P["chordlen"], 0, 1, "CHORD LEN"),
            knob_in(P["walk"], 0, 2, "WALK"),
            knob_in(P["jump"], 0, 3, "OCT JUMP")]
    right = [pills_in(P["listen"], 1, 0, "LISTEN"),
             knob_in(P["key"], 1, 1, "KEY"),
             knob_in(P["scale"], 1, 2, "SCALE"),
             knob_in(P["octave"], 1, 3, "OCTAVE")]
    # Chord Len only times a progression: greyed while Changes is Follow or Tonic.
    return page("NOTES", left, right, scrims_unless([box(0, 1)], P["changes"], set(range(2, 8))))


def page_sound():
    left = [pill_grid(P["voice"], 0, 0, "VOICE", per_row=3),
            knob_in(P["tone"], 0, 1, "TONE"),
            knob_in(P["reso"], 0, 2, "RESO"),
            knob_in(P["drive"], 0, 3, "DRIVE")]
    right = [knob_in(P["decay"], 1, 0, "DECAY"),
             knob_in(P["punch"], 1, 1, "PUNCH"),
             knob_in(P["duck"], 1, 2, "DUCK"),
             knob_in(P["ducktime"], 1, 3, "DUCK TIME")]
    return page("SOUND", left, right)


def page_mix():
    left = [knob_in(P["bass"], 0, 0, "BASS"),
            knob_in(P["out"], 0, 1, "OUTPUT"),
            pills_in(P["solo"], 0, 2, "BASS ONLY", captions=["OFF", "ON"]),
            None]
    right = [None, None, None, None]
    return page("MIX", left, right)


QLINK_RECTS = ["%d %d %d %d" % (COL_X[0], ROW_Y[r], COL_X[1] + COL_W - COL_X[0], BOX_H) for r in range(4)]


def strip_cells():
    x0, y0, x1, y1 = STRIP
    w = (x1 - x0) / CELLS
    return [(x0 + w * k, y0, x0 + w * (k + 1), y1) for k in range(CELLS)]


# What each cell of the strip shows: (heading, parameter, kind)
STRIP_ITEMS = [("KICK", P["kick"], "lamp"), ("KEY", P["found"], "text"),
               ("SURE", P["sure"], "text"), ("CHORD", P["chord"], "text")]
READOUT_W, READOUT_H = 120, 30


def readout_xy(cell):
    cx0, cy0, cx1, cy1 = cell
    return (round(cx0 + 18), round(cy0 + 26))


def lamp_xy(cell):
    cx0, cy0, cx1, cy1 = cell
    return (round(cx0 + 14), round((cy0 + cy1) / 2 - LAMP / 2))


# ------------------------------------------------------------------------------------------------
# Backgrounds
# ------------------------------------------------------------------------------------------------
def base_plate():
    src = Image.open(os.path.join(HERE, "hero-src.webp")).convert("RGB")
    return src.resize((W, H), Image.LANCZOS).convert("RGBA")


def scrim_image(boxes):
    """One translucent panel per box, on a canvas spanning them all. Returns (image, origin)."""
    ox, oy = min(b[0] for b in boxes), min(b[1] for b in boxes)
    w, h = max(b[2] for b in boxes) - ox, max(b[3] for b in boxes) - oy

    def paint(d, sc):
        for bx0, by0, bx1, by1 in boxes:
            d.rounded_rectangle([(bx0 - ox) * sc, (by0 - oy) * sc, (bx1 - ox) * sc, (by1 - oy) * sc],
                                radius=14 * sc, fill=(2, 12, 9, 190), outline=CREAM + (34,), width=int(1.4 * sc))
    return supersampled((int(w), int(h)), 3, paint), (ox, oy)


def draw_background(controls, boxes):
    im = base_plate()

    def paint(d, sc):
        for bx in boxes:
            d.rounded_rectangle([v * sc for v in bx], radius=14 * sc, fill=(2, 14, 11, 150),
                                outline=TEAL + (70,), width=int(1.4 * sc))
        d.rounded_rectangle([v * sc for v in STRIP], radius=14 * sc, fill=(2, 14, 11, 165),
                            outline=GOLD + (70,), width=int(1.4 * sc))
        for k in range(1, CELLS):            # hairlines between the strip's cells
            x = (STRIP[0] + (STRIP[2] - STRIP[0]) * k / CELLS) * sc
            d.line([x, (STRIP[1] + 12) * sc, x, (STRIP[3] - 12) * sc], fill=GOLD + (45,), width=int(1.2 * sc))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    d = ImageDraw.Draw(im)
    for ctl in controls:
        if ctl["kind"] == "knob":
            cx, cy = ctl["centre"]
            x, y = cx + KNOB["frame"] // 2 + VALUE_X_GAP, cy - 20
            size = 14
            while size > 10 and text_width(ctl["heading"], font(size, FONT_DEMI), 2.6) > VALUE_W - 4:
                size -= 1
            draw_spaced(d, (x, y), ctl["heading"], font(size, FONT_DEMI), GOLD + (255,), spacing=2.6, anchor="lm")
        elif ctl["kind"] == "pills" and ctl.get("heading"):
            hx, hy = ctl["heading_xy"]
            draw_spaced(d, (hx, hy), ctl["heading"], font(13, FONT_DEMI), GOLD + (240,), spacing=2.4, anchor="lm")
    for cell, (heading, _, kind) in zip(strip_cells(), STRIP_ITEMS):
        cx0, cy0, cx1, cy1 = cell
        if kind == "lamp":
            draw_spaced(d, (cx0 + 14 + LAMP + 10, (cy0 + cy1) / 2), heading, font(13, FONT_DEMI), GOLD + (240,),
                        spacing=2.4, anchor="lm")
        else:
            draw_spaced(d, (cx0 + 18, cy0 + 15), heading, font(11, FONT_DEMI), GOLD + (220,), spacing=2.2, anchor="lm")
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
    return G.entry("focus", "Focus", G.focus_data("ff3ad6ba"), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


VALUE_X_GAP = 8
VALUE_W = COL_W - KNOB_X - KNOB["frame"] // 2 - VALUE_X_GAP - 6


def knob_component(spec, strip_file, overlay):
    """Knob at the left, its value to the right (the name is painted into the background above it)."""
    f = spec["frame"]
    pad_x = KNOB_X - f // 2
    w, h = pad_x + f + VALUE_X_GAP + VALUE_W, f
    children = [focus_child(w, h),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d 0 %d %d" % (pad_x, f, f), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, 25.0, "left verticallyCentred"),
                        "%d %d %d 36" % (pad_x + f + VALUE_X_GAP, f // 2 - 2, VALUE_W), "No", "Show", "Always")]
    return G.component_definition(G.control_actions(overlay), children, "00000000"), (w, h, pad_x)


def readout_component():
    """A read-only value: just the label, no focus, no actions."""
    children = [G.entry("value", "Label", label_data("Value", VALUE_COLOUR, 24.0, "left verticallyCentred"),
                        "0 0 %d %d" % (READOUT_W, READOUT_H), "No", "Show", "Always")]
    return G.component_definition([], children, "00000000", ignore_mouse=True)


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


def strip_children(previews, ti):
    """The status strip: the kick light (a film strip on the Kick meter) and three read-only values."""
    children = []
    for k, (cell, (heading, param, kind)) in enumerate(zip(strip_cells(), STRIP_ITEMS)):
        if kind == "lamp":
            x, y = lamp_xy(cell)
            children.append(G.entry("kick light", "Image",
                                    obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"),
                                        ("image", "kick_lamp.png"), ("numFrames", LAMP_FRAMES), ("handleName", "Data")),
                                    "%d %d %d %d" % (x, y, LAMP, LAMP), "No", "Show", "Always",
                                    remap=[("Data", "Parameter %d" % param)]))
            previews.append((ti, "lamp", (x, y)))
        else:
            x, y = readout_xy(cell)
            children.append(G.entry("readout %d" % k, "readout", obj(("version", 1), ("handleName", "Data")),
                                    "%d %d %d %d" % (x, y, READOUT_W, READOUT_H), "No", "Show", "Always",
                                    remap=[("Data", "Parameter %d" % param)]))
            previews.append((ti, "readout", (param, x, y)))
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

    strips = {bi: save("knob_teal%s.png" % ("_bi" if bi else ""), knob_strip(KNOB, TEAL, bipolar=bi))
              for bi in (False,)}
    save("kick_lamp.png", lamp_strip())

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111")),
            ("readout", readout_component())]
    knob_defs = {}
    for overlay in ("knob overlay", "menu overlay"):
        key = "knob teal" + (" menu" if overlay == "menu overlay" else "")
        comp, geom = knob_component(KNOB, strips[False], overlay)
        defs.append((key, comp))
        knob_defs[key] = geom

    tabs, qlink_entries, previews = [], [], []
    pill_files = {}
    for ti, (name, controls, qlinks, boxes, scrims) in enumerate(
            (page_groove(), page_notes(), page_sound(), page_mix())):
        bg_file = "bg_%s.jpg" % name.lower()
        bg = draw_background(controls, boxes)
        bg.save(os.path.join(OUT, bg_file), quality=92)
        children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                           ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
        previews.append((ti, "bg", bg.convert("RGBA")))
        children += strip_children(previews, ti)
        for ci, ctl in enumerate(controls):
            handle = "Parameter %d" % ctl["param"]
            if ctl["kind"] == "knob":
                key = "knob teal" + (" menu" if ctl["menu"] else "")
                w, h, pad_x = knob_defs[key]
                cx, cy = ctl["centre"]
                x, y = cx - KNOB["frame"] // 2 - pad_x, cy - KNOB["frame"] // 2
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "knob", (ctl, (x, y), strips[False])))
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
# The status strip shows sample readings (a kick, A minor, 87%, F) so its layout can be judged.
# ------------------------------------------------------------------------------------------------
PREVIEW_STRIP = {P["found"]: "Am", P["sure"]: "87 %", P["chord"]: "F"}


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
        if kind == "knob":
            ctl, (x, y), strip_name = data
            v, text = vals.get(ctl["param"], (0.5, ""))
            if ctl["param"] in CHOICES:
                text = CHOICES[ctl["param"]][choice_index(ctl["param"])]
            f = KNOB["frame"]
            fi = round(v * (KNOB["frames"] - 1))
            pad_x = KNOB_X - f // 2
            im.alpha_composite(images[strip_name].crop((0, fi * f, f, (fi + 1) * f)), (x + pad_x, y))
            d.text((x + pad_x + f + VALUE_X_GAP, y + f / 2 + 16), text, font=font(22, FONT_MEDIUM), fill=CREAM, anchor="lm")
        elif kind == "lamp":
            x, y = data
            im.alpha_composite(images["kick_lamp.png"].crop((0, (LAMP_FRAMES - 1) * LAMP, LAMP, LAMP_FRAMES * LAMP)), (x, y))
        elif kind == "readout":
            param, x, y = data
            d.text((x, y + READOUT_H / 2), PREVIEW_STRIP[param], font=font(22, FONT_MEDIUM), fill=CREAM, anchor="lm")
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
