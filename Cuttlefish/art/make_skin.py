#!/usr/bin/env python3
"""Build Cuttlefish's hand-made skin (plugins/Cuttlefish/skins/) from the artwork in this folder.

    python3 plugins/Cuttlefish/art/make_skin.py      # writes skins/ and build/skin-preview-*.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    plate-src.webp          the user's plate: the Cuttlefish / MORPHING WAVETABLE SYNTH title, an iridescent cuttlefish
                            trailing waveforms, a stack of wavetable frames under it, dark water either side and a coral
                            seabed along the bottom (1790x878, the 1280:628 aspect, so it scales straight onto the MPC's
                            tab area)
    knob_small.webp         black fluted knob with skirt, pointer at 12 o'clock (shared with the other Groundswell skins)
    pill_on/off.webp        button pill (shared likewise; its teal glow matches the water)

Layout: four Q-Link banks per page, laid out round the cuttlefish. A column of four in the dark water on the left, a
column of four on the right, and two rows of four on the seabed (each with its heading at the far left). The
cuttlefish, the title and the wavetable frames stay clear. Each cell is the knob (or a lit pill for an on/off switch)
with its name and live value beside it, on a darkened recess so the pale text reads on the bright water.

    OSC       left OSC 1 (Table, Morph, Warp Mode, Warp)   right OSC 2     rows TUNE 1, TUNE 2 (Oct, Semi, Fine, Level)
    VOICE     left SUB & NOISE   right UNISON (Unison, Detune, Width, Drift)   rows PLAY, OUTPUT
    MOTION    left COLOUR (Colour, Col>Morph 1/2, Col>Filter)   right MORPH ENV   rows LFO 1, LFO 2
    MOD       left SOURCE 1-4   right DESTINATION 1-4   rows AMOUNT 1-4, ROUTES (Env>Morph, LFO1 Fade)
    FILT/AMP  left FILTER   right FILTER MOD (Env Amt, Key Track, Vel>Filter, Vel>Level)   rows FILTER ENV, AMP ENV
    FX        left CHORUS   right REVERB   row DELAY

Q-Links (bank direction Column): knob column 1 = the left column top to bottom, column 2 = the right column, column 3 =
the upper seabed row left to right, column 4 = the lower row. Choice parameters are rotary selectors (detent dots; the
value text names the choice); signed parameters have violet arcs that grow from 12 o'clock. The skin binds to parameter
indices, so regenerate it whenever the parameter list changes (the script checks its table against build/params.json).
Drawing machinery adapted from Driftwood's generator (deliberately not a shared import: a plugin's artwork should not
break because a sibling's did).
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
PALE = (222, 248, 246)                 # the title's glow: names, headings
TEAL = (72, 228, 216)                  # the water: unipolar arcs, detent dots
VIOLET = (190, 146, 255)               # the cuttlefish's iridescence: signed (bipolar) arcs
FOCUS = "ff48e4d8"
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter names in index order (must match src/Cuttlefish.cpp; checked against build/params.json).
NAMES = ["Output",
         "Table 1", "Morph 1", "Warp Mode 1", "Warp 1", "Oct 1", "Semi 1", "Fine 1", "Level 1",
         "Table 2", "Morph 2", "Warp Mode 2", "Warp 2", "Oct 2", "Semi 2", "Fine 2", "Level 2",
         "Sub", "Sub Type", "Noise", "Noise Tone",
         "Unison", "Detune", "Width", "Drift", "Phase",
         "Play Mode", "Glide", "Bend Range", "Voices",
         "Colour", "Col>Morph 1", "Col>Morph 2", "Col>Filter",
         "M Attack", "M Decay", "M Sustain", "M Release", "Env>Morph",
         "LFO1 Rate", "LFO1 Div", "LFO1 Shape", "LFO1 Mode", "LFO1 Fade",
         "LFO2 Rate", "LFO2 Div", "LFO2 Shape", "LFO2 Mode",
         "Src 1", "Dst 1", "Amt 1", "Src 2", "Dst 2", "Amt 2", "Src 3", "Dst 3", "Amt 3", "Src 4", "Dst 4", "Amt 4",
         "Filter", "Cutoff", "Reso", "Drive", "Env Amt", "Key Track", "Vel>Filter", "Vel>Level",
         "F Attack", "F Decay", "F Sustain", "F Release",
         "Attack", "Decay", "Sustain", "Release",
         "Chorus", "Ch Rate", "Ch Depth", "Ch Mode",
         "Delay", "Dly Time", "Dly Fdbk", "Dly Tone",
         "Reverb", "Rev Size", "Rev Tone", "Rev Pre"]
P = {n: i for i, n in enumerate(NAMES)}

TABLES = ["Basic", "PWM", "Sync", "Reso", "Vowel", "Formant", "Organ", "Tine", "Glass", "Metal", "FM", "Fold",
          "Digital", "Hollow", "Brass", "Spectra", "Comb"]
WARPS = ["Bend", "Squash", "Sync", "FM"]
SHAPES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth Rnd"]
LMODES = ["Free", "Retrig", "Sync", "Sync Rtg"]
LDIVS = ["8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/2T", "1/4D", "1/4", "1/4T", "1/8D", "1/8", "1/8T", "1/16",
         "1/16T", "1/32"]
SRCS = ["Off", "LFO 1", "LFO 2", "Mod Env", "Filt Env", "Amp Env", "Velocity", "Mod Wheel", "Aftertouch", "Key",
        "LFO1 x MW", "Random", "Colour"]
DSTS = ["Morph 1", "Morph 2", "Morph 1+2", "Warp 1", "Warp 2", "Cutoff", "Reso", "Pitch", "Pitch 2", "Osc Mix", "Pan",
        "Level", "Detune", "Noise"]
CHOICES = {P["Table 1"]: TABLES, P["Table 2"]: TABLES, P["Warp Mode 1"]: WARPS, P["Warp Mode 2"]: WARPS,
           P["Sub Type"]: ["Sine -1", "Sine -2", "Tri -1", "Sqr -1"], P["Phase"]: ["Free", "Reset"],
           P["Play Mode"]: ["Poly", "Mono", "Legato"],
           P["LFO1 Div"]: LDIVS, P["LFO1 Shape"]: SHAPES, P["LFO1 Mode"]: LMODES,
           P["LFO2 Div"]: LDIVS, P["LFO2 Shape"]: SHAPES, P["LFO2 Mode"]: LMODES,
           P["Filter"]: ["LP12", "LP24", "BP", "HP", "Notch"], P["Ch Mode"]: ["Chorus", "Ensemble", "Flange"],
           P["Dly Time"]: ["1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2"]}
for k in range(1, 5):
    CHOICES[P["Src %d" % k]] = SRCS
    CHOICES[P["Dst %d" % k]] = DSTS
DETENTS = {P["Oct 1"]: 7, P["Oct 2"]: 7, P["Unison"]: 4}   # integer parameters drawn as selectors
BIPOLAR = {P[n] for n in ["Oct 1", "Oct 2", "Semi 1", "Semi 2", "Fine 1", "Fine 2", "Col>Morph 1", "Col>Morph 2",
                          "Col>Filter", "Env>Morph", "Env Amt", "Amt 1", "Amt 2", "Amt 3", "Amt 4"]}

# Where the banks go on the 1280x628 picture: (x, y, cell width, cell height, direction).
COL_L = (16, 110, 226, 82, "col")      # dark water left of the cuttlefish's fin
COL_R = (1040, 110, 226, 82, "col")    # dark water right of its trailing ribbon
ROW_1 = (128, 458, 256, 78, "row")     # the seabed, under the wavetable frames
ROW_2 = (128, 538, 256, 78, "row")
HEAD_Y = 94                            # column headings
ROW_HEAD_X = 120                       # row headings, right-aligned left of each row

# Inside a cell: the knob (or pill) in a 76 px wide area on the left, the name and value to its right.
KNOB = dict(src="knob_small.webp", crop=300, knob=42, arc_r=25, arc_w=3.2, frame=58, frames=100)
KNOB_CENTRE_SRC = (623.5, 625.5)
CTRL_W = 76
TEXT_X = 80
VALUE_H = 24

VALUE_COLOUR = "ffdef8f6"
VALUE_FONT = "Nexa"
VALUE_SIZE = 17.0


# ------------------------------------------------------------------------------------------------
# Drawing helpers
# ------------------------------------------------------------------------------------------------
def font(size, face=FONT_DEMI):
    return ImageFont.truetype(FONT_FILE, size, index=face)


def text_width(text, f, spacing):
    return sum(f.getlength(ch) for ch in text) + spacing * (len(text) - 1)


def draw_spaced(draw, xy, text, f, fill, spacing=2.0, anchor="mm"):
    """Letter-spaced text centred on xy ('mm'), left-aligned ('lm') or right-aligned ('rm')."""
    x, y = xy
    tw = text_width(text, f, spacing)
    if anchor == "mm":
        x -= tw / 2
    elif anchor == "rm":
        x -= tw
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill, anchor="lm")
        x += f.getlength(ch) + spacing
    return tw


def shadowed_text(im, xy, text, f, fill, spacing=2.0, anchor="mm", shadow=(0, 6, 12, 220), blur=1.8):
    """Pale lettering with a soft dark shadow under it, so it reads on the bright water and caustics."""
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    draw_spaced(ImageDraw.Draw(sh), (xy[0] + 1, xy[1] + 1.5), text, f, shadow, spacing, anchor)
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)))
    return draw_spaced(ImageDraw.Draw(im), xy, text, f, fill, spacing, anchor)


def supersampled(size, scale, painter):
    big = Image.new("RGBA", (size[0] * scale, size[1] * scale), (0, 0, 0, 0))
    painter(ImageDraw.Draw(big), scale)
    return big.resize(size, Image.LANCZOS)


def clean_alpha(im):
    r, g, b, a = im.split()
    a = a.point(lambda v: 0 if v < 16 else (255 if v > 250 else v))
    return Image.merge("RGBA", (r, g, b, a))


def load(name):
    return clean_alpha(Image.open(os.path.join(HERE, name)).convert("RGBA"))


# ------------------------------------------------------------------------------------------------
# Knob film strips: rotated knob + value arc (unipolar from 7 o'clock, bipolar from 12 o'clock, selector none)
# ------------------------------------------------------------------------------------------------
def knob_strip(spec, colour, mode="uni"):
    src = load(spec["src"])
    cx, cy = KNOB_CENTRE_SRC
    c = spec["crop"]
    body = src.crop((int(cx - c), int(cy - c), int(cx + c), int(cy + c)))
    ss = 4
    knob_px = int(spec["knob"] * ss)
    body = body.resize((knob_px, knob_px), Image.LANCZOS)
    frame, n = spec["frame"], spec["frames"]
    strip = Image.new("RGBA", (frame, frame * n), (0, 0, 0, 0))
    for i in range(n):
        v = i / (n - 1)
        angle = -135.0 + 270.0 * v                       # degrees clockwise from 12 o'clock
        big = Image.new("RGBA", (frame * ss, frame * ss), (0, 0, 0, 0))
        m = frame * ss / 2
        r = spec["arc_r"] * ss
        box = (m - r, m - r, m + r, m + r)
        w = int(spec["arc_w"] * ss)
        if mode != "sel":
            ImageDraw.Draw(big).arc(box, -135 - 90, 135 - 90, fill=(0, 8, 14, 140), width=w)      # dark track
            a0, a1 = (-135.0, angle) if mode == "uni" else (min(0.0, angle), max(0.0, angle))
            if a1 - a0 > 0.5:
                glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
                ImageDraw.Draw(glow).arc(box, a0 - 90, a1 - 90, fill=colour + (150,), width=w * 2)
                big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(2.5 * ss)))
                ImageDraw.Draw(big).arc(box, a0 - 90, a1 - 90, fill=colour + (255,), width=w)
            if mode == "bi":       # a small centre tick at 12 o'clock
                ImageDraw.Draw(big).line([m, m - r - w, m, m - r + w], fill=PALE + (200,), width=max(1, ss))
        rot = body.rotate(-angle, resample=Image.BICUBIC)
        off = int(m - knob_px / 2)
        big.alpha_composite(rot, (off, off))
        strip.alpha_composite(big.resize((frame, frame), Image.LANCZOS), (0, i * frame))
    return strip


# ------------------------------------------------------------------------------------------------
# Pills (3-slice stretch so every width keeps round ends), with a caption
# ------------------------------------------------------------------------------------------------
PILL_BODY = {"pill_on.webp": (446, 207, 1553, 465), "pill_off.webp": (427, 201, 1568, 465)}
PILL_H, PILL_MARGIN = 28, 8
PILL_W = 58
PILL_ON_INK, PILL_OFF_INK = (214, 255, 250), (150, 170, 176)


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


def pill(state, caption, width):
    im = pill_base("pill_on.webp" if state else "pill_off.webp", width)
    d = ImageDraw.Draw(im)
    colour = PILL_ON_INK if state else PILL_OFF_INK
    text, size, spacing = caption.upper(), 12, 1.4
    while size > 8 and text_width(text, font(size, FONT_DEMI), spacing) > width - 12:
        size -= 1
    draw_spaced(d, (im.width / 2, im.height / 2 + 1), text, font(size, FONT_DEMI), colour + (255,), spacing=spacing)
    return im


# ------------------------------------------------------------------------------------------------
# Layout
# ------------------------------------------------------------------------------------------------
def knob(name, label=None):
    i = P[name]
    mode = "sel" if (i in CHOICES or i in DETENTS) else ("bi" if i in BIPOLAR else "uni")
    return dict(kind="knob", param=i, name=(label or name).upper(), mode=mode)


def toggle(name, caption, label=None):
    """One pill for a two-way switch, lit for the second choice. A lone button toggles."""
    return dict(kind="toggle", param=P[name], name=(label or name).upper(), caption=caption)


def cell_rect(bank, s):
    x, y, w, h, direction = bank
    return (x, y + s * h, w, h) if direction == "col" else (x + s * w, y, w, h)


def page(name, left, right, row1, row2, headings):
    """Four banks: left column (Q-Link column 1), right column (2), upper seabed row (3), lower row (4)."""
    controls, qlinks, rects = [], {}, []
    for b, (bank, cells) in enumerate(((COL_L, left), (COL_R, right), (ROW_1, row1), (ROW_2, row2))):
        x, y, w, h, direction = bank
        used = [s for s, c in enumerate(cells) if c is not None]
        if direction == "col":
            rects.append((x - 4, y - 2, w + 8, 4 * h + 4))
        else:
            n = (max(used) + 1) if used else 4
            rects.append((x - 4, y - 2, n * w + 8, h + 4))
        for s, ctl in enumerate(cells):
            if ctl is None:
                continue
            ctl = dict(ctl, rect=cell_rect(bank, s))
            controls.append(ctl)
            qlinks[13 + b - 4 * s] = ctl["param"]            # column b+1, top (13) to bottom (1)
    shown = [bool(c) and any(x is not None for x in c) for c in (left, right, row1, row2)]
    return dict(name=name, controls=controls, qlinks=qlinks, rects=rects, shown=shown, headings=headings)


PAGES = [
    page("OSC",
         [knob("Table 1", "Table"), knob("Morph 1", "Morph"), knob("Warp Mode 1", "Warp Mode"), knob("Warp 1", "Warp")],
         [knob("Table 2", "Table"), knob("Morph 2", "Morph"), knob("Warp Mode 2", "Warp Mode"), knob("Warp 2", "Warp")],
         [knob("Oct 1", "Octave"), knob("Semi 1", "Semitone"), knob("Fine 1", "Fine"), knob("Level 1", "Level")],
         [knob("Oct 2", "Octave"), knob("Semi 2", "Semitone"), knob("Fine 2", "Fine"), knob("Level 2", "Level")],
         ["OSC 1", "OSC 2", "TUNE 1", "TUNE 2"]),
    page("VOICE",
         [knob("Sub"), knob("Sub Type"), knob("Noise"), knob("Noise Tone")],
         [knob("Unison"), knob("Detune"), knob("Width"), knob("Drift")],
         [knob("Play Mode"), knob("Glide"), knob("Bend Range"), knob("Voices")],
         [toggle("Phase", "Reset"), None, None, knob("Output")],
         ["SUB & NOISE", "UNISON", "PLAY", "OUTPUT"]),
    page("MOTION",
         [knob("Colour"), knob("Col>Morph 1", "Col > Morph 1"), knob("Col>Morph 2", "Col > Morph 2"),
          knob("Col>Filter", "Col > Filter")],
         [knob("M Attack", "Attack"), knob("M Decay", "Decay"), knob("M Sustain", "Sustain"), knob("M Release", "Release")],
         [knob("LFO1 Rate", "Rate"), knob("LFO1 Div", "Division"), knob("LFO1 Shape", "Shape"), knob("LFO1 Mode", "Mode")],
         [knob("LFO2 Rate", "Rate"), knob("LFO2 Div", "Division"), knob("LFO2 Shape", "Shape"), knob("LFO2 Mode", "Mode")],
         ["COLOUR", "MORPH ENV", "LFO 1", "LFO 2"]),
    page("MOD",
         [knob("Src %d" % k, "Source %d" % k) for k in range(1, 5)],
         [knob("Dst %d" % k, "Dest %d" % k) for k in range(1, 5)],
         [knob("Amt %d" % k, "Amount %d" % k) for k in range(1, 5)],
         [knob("Env>Morph", "Env > Morph"), knob("LFO1 Fade"), None, None],
         ["SOURCE", "DESTINATION", "AMOUNT", "ROUTES"]),
    page("FILT/AMP",
         [knob("Cutoff"), knob("Reso"), knob("Drive"), knob("Filter", "Mode")],
         [knob("Env Amt"), knob("Key Track"), knob("Vel>Filter", "Vel > Filter"), knob("Vel>Level", "Vel > Level")],
         [knob("F Attack", "Attack"), knob("F Decay", "Decay"), knob("F Sustain", "Sustain"), knob("F Release", "Release")],
         [knob("Attack"), knob("Decay"), knob("Sustain"), knob("Release")],
         ["FILTER", "FILTER MOD", "FILTER ENV", "AMP ENV"]),
    page("FX",
         [knob("Chorus", "Mix"), knob("Ch Rate", "Rate"), knob("Ch Depth", "Depth"), knob("Ch Mode", "Mode")],
         [knob("Reverb", "Mix"), knob("Rev Size", "Size"), knob("Rev Tone", "Tone"), knob("Rev Pre", "Pre-Delay")],
         [knob("Delay", "Mix"), knob("Dly Time", "Time"), knob("Dly Fdbk", "Feedback"), knob("Dly Tone", "Tone")],
         [None, None, None, None],
         ["CHORUS", "REVERB", "DELAY", ""]),
]
TAB_NAMES = [p["name"] for p in PAGES]


# ------------------------------------------------------------------------------------------------
# Background
# ------------------------------------------------------------------------------------------------
def base_plate():
    src = Image.open(os.path.join(HERE, "plate-src.webp")).convert("RGB")
    return src.resize((W, H), Image.LANCZOS).convert("RGBA")


def knob_centre(rect):
    x, y, w, h = rect
    return x + CTRL_W / 2, y + h / 2


def draw_background(pg):
    im = base_plate()

    def paint(d, sc):
        # A darkened recess behind each bank (it is also where the Q-Link focus rectangle lands).
        for (x, y, w, h), shown in zip(pg["rects"], pg["shown"]):
            if not shown:
                continue
            d.rounded_rectangle([x * sc, y * sc, (x + w) * sc, (y + h) * sc], radius=14 * sc, fill=(0, 8, 14, 150))
            d.rounded_rectangle([(x + 1) * sc, (y + 1.5) * sc, (x + w + 1) * sc, (y + h + 1.5) * sc], radius=14 * sc,
                                outline=TEAL + (60,), width=int(1.2 * sc))
            d.rounded_rectangle([x * sc, y * sc, (x + w) * sc, (y + h) * sc], radius=14 * sc,
                                outline=(0, 4, 8, 170), width=int(1.2 * sc))
        for ctl in pg["controls"]:
            if ctl["kind"] == "knob" and ctl["mode"] == "sel":
                # detent dots round a selector, one per choice
                cx, cy = knob_centre(ctl["rect"])
                n = len(CHOICES[ctl["param"]]) if ctl["param"] in CHOICES else DETENTS[ctl["param"]]
                r = 2.1 if n <= 8 else 1.7
                for k in range(n):
                    a = math.radians(-135 + 270 * k / (n - 1))
                    px, py = cx + 26.5 * math.sin(a), cy - 26.5 * math.cos(a)
                    d.ellipse([(px - r) * sc, (py - r) * sc, (px + r) * sc, (py + r) * sc], fill=TEAL + (225,))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    # Column headings above the columns, in the style of the title's "— MORPHING WAVETABLE SYNTH —".
    for bank, text in zip((COL_L, COL_R), pg["headings"][:2]):
        x, y, w, h, _ = bank
        cx = x + w / 2
        f = font(14, FONT_DEMI)
        tw = text_width(text, f, 4.0)
        shadowed_text(im, (cx, HEAD_Y), text, f, PALE + (250,), spacing=4.0)
        d = ImageDraw.Draw(im)
        for sgn in (-1, 1):
            x0 = cx + sgn * (tw / 2 + 10)
            d.line([x0, HEAD_Y, x0 + sgn * 18, HEAD_Y], fill=PALE + (190,), width=1)
    # Row headings at the far left of each seabed row.
    for bank, text, shown in zip((ROW_1, ROW_2), pg["headings"][2:], pg["shown"][2:]):
        if not shown or not text:
            continue
        x, y, w, h, _ = bank
        size = 13
        while size > 9 and text_width(text, font(size, FONT_DEMI), 2.5) > ROW_HEAD_X - 22:
            size -= 1
        shadowed_text(im, (ROW_HEAD_X - 10, y + h / 2), text, font(size, FONT_DEMI), PALE + (250,), spacing=2.5, anchor="rm")

    for ctl in pg["controls"]:
        x, y, w, h = ctl["rect"]
        size = 14
        while size > 10 and text_width(ctl["name"], font(size, FONT_DEMI), 1.8) > w - TEXT_X - 6:
            size -= 1
        shadowed_text(im, (x + TEXT_X, y + h / 2 - 12), ctl["name"], font(size, FONT_DEMI), PALE + (255,),
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
    return G.entry("focus", "Focus", G.focus_data(FOCUS), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


def value_child(w, h):
    return G.entry("value", "Label", label_data("Value", VALUE_COLOUR, VALUE_SIZE, "left verticallyCentred"),
                   "%d %d %d %d" % (TEXT_X, h // 2 - 1, w - TEXT_X - 4, VALUE_H), "No", "Show", "Always")


def knob_component(spec, strip_file, overlay, w, h):
    """A cell: the knob on the left, its value under the printed name to the right."""
    f = spec["frame"]
    children = [focus_child(w, h),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d %d %d %d" % ((CTRL_W - f) // 2, (h - f) // 2, f, f), "No", "Show", "Always"),
                value_child(w, h)]
    return G.component_definition(G.control_actions(overlay), children, "00000000")


def toggle_component(on_file, off_file, w, h):
    pw, ph = PILL_W + 2 * PILL_MARGIN, PILL_H + 2 * PILL_MARGIN
    px, py = (CTRL_W - pw) // 2, (h - ph) // 2
    children = [focus_child(w, h),
                G.entry("pill", "Button", G.button_data(on_file, off_file, 1, 2), "%d %d %d %d" % (px, py, pw, ph),
                        "No", "Show", "Always"),
                value_child(w, h)]
    return G.component_definition(G.control_actions("menu overlay", double_click=False), children, "00000000"), (px, py)


def check_params():
    """Refuse to render a skin that binds to the wrong parameters."""
    path = os.path.join(PROJECT, "build", "params.json")
    if not os.path.exists(path):
        print("note: build/params.json not found; parameter table not checked (run tools/build.py once)")
        return
    with open(path, encoding="utf-8") as f:
        params = json.load(f)["params"]
    errors = []
    if len(params) != len(NAMES):
        errors.append("plugin has %d parameters, the skin expects %d" % (len(params), len(NAMES)))
    for i, n in enumerate(NAMES):
        if i < len(params) and params[i]["name"] != n:
            errors.append("parameter %d is %r in the plugin, %r in the skin" % (i, params[i]["name"], n))
    for idx, choices in CHOICES.items():
        got = params[idx].get("choices") if idx < len(params) else None
        if got != choices:
            errors.append("parameter %d (%s): choices %s, skin has %s" % (idx, NAMES[idx], got, choices))
    for idx, n in DETENTS.items():
        p = params[idx]
        if p.get("kind") not in ("int", "choice") or (p.get("kind") == "choice" and len(p.get("choices", [])) != n):
            errors.append("parameter %d (%s) should be an integer for its %d detents" % (idx, NAMES[idx], n))
    if errors:
        sys.exit("make_skin.py: parameter table out of date:\n  " + "\n  ".join(errors))


def build():
    check_params()
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    images = {}

    def save(name, im):
        im.save(os.path.join(OUT, name), optimize=True)
        images[name] = im
        return name

    strips = {"uni": save("knob_teal.png", knob_strip(KNOB, TEAL, "uni")),
              "bi": save("knob_violet.png", knob_strip(KNOB, VIOLET, "bi")),
              "sel": save("knob_selector.png", knob_strip(KNOB, TEAL, "sel"))}

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("bipolar overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(True), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    sizes = sorted({(c["rect"][2], c["rect"][3]) for pg in PAGES for c in pg["controls"]})
    for (w, h) in sizes:
        defs.append(("knob %dx%d" % (w, h), knob_component(KNOB, strips["uni"], "knob overlay", w, h)))
        defs.append(("bipolar %dx%d" % (w, h), knob_component(KNOB, strips["bi"], "bipolar overlay", w, h)))
        defs.append(("selector %dx%d" % (w, h), knob_component(KNOB, strips["sel"], "menu overlay", w, h)))

    toggles_done = set()
    tabs, qlink_entries, previews = [], [], []
    kind_name = {"uni": "knob", "bi": "bipolar", "sel": "selector"}
    for ti, pg in enumerate(PAGES):
        bg_file = "bg_%s.jpg" % pg["name"].lower().replace("/", "_")
        bg = draw_background(pg)
        bg.save(os.path.join(OUT, bg_file), quality=92)
        children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                           ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
        previews.append((ti, "bg", bg.convert("RGBA")))
        for ci, ctl in enumerate(pg["controls"]):
            handle = "Parameter %d" % ctl["param"]
            x, y, w, h = ctl["rect"]
            if ctl["kind"] == "knob":
                key = "%s %dx%d" % (kind_name[ctl["mode"]], w, h)
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "knob", (ctl, strips[ctl["mode"]])))
            else:
                slug = ctl["caption"].lower()
                on = "pill_on_%s.png" % slug
                off = "pill_off_%s.png" % slug
                if slug not in toggles_done:
                    save(on, pill(True, ctl["caption"], PILL_W))
                    save(off, pill(False, ctl["caption"], PILL_W))
                comp, (px, py) = toggle_component(on, off, w, h)
                comp_name = "toggle %s %dx%d" % (slug, w, h)
                if slug not in toggles_done:
                    defs.append((comp_name, comp))
                    toggles_done.add(slug)
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "toggle", (ctl, (x + px, y + py), on, off)))

        rects = ["%d %d %d %d" % r for r in pg["rects"]]
        tabs.append(obj(("version", 1), ("tabName", pg["name"]), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", G.component_definition([], children, "ff000000")),
                        ("qlinkBoundsData", rects)))
        qlink_entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "Column"),
                                 ("Q-Links", Obj(("Q-Link %d" % q, pg["qlinks"].get(q, -1)) for q in range(1, 17)))))

    replacements = [obj(("key", "Parameter %d" % k), ("value", v)) for k, v in sorted(CHOICES.items())]
    tui = obj(("pageData", obj(
        ("version", 1),
        ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                     ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs]))),
        ("replacementStrings", replacements),
        ("info", obj(("version", 1), ("type", "CompleteDescription"))),
        ("tabs", tabs))))
    # Program-mode Q-Links: the sixteen controls you reach for most.
    favourites = ["Colour", "Morph 1", "Morph 2", "Warp 1", "Cutoff", "Reso", "Env Amt", "Drift",
                  "Attack", "Decay", "Sustain", "Release", "Detune", "Glide", "Delay", "Reverb"]
    program = Obj(("Q-Link %d" % (q + 1), P[n]) for q, n in enumerate(favourites))
    qlinks_json = obj(("version", 4), ("info", obj(("version", 1), ("type", "CompleteDescription"))),
                      ("Screen Mode Q-Links", obj(("version", 4), ("map", qlink_entries))),
                      ("Program Mode Q-Links", program))
    with open(os.path.join(OUT, "TUI.json"), "w", encoding="utf-8") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(OUT, "Q-Links.json"), "w", encoding="utf-8") as f:
        f.write(G.dump_json(qlinks_json) + "\n")
    make_previews(previews, images)


# ------------------------------------------------------------------------------------------------
# Preview (approximate: Avenir Next stands in for the MPC's Nexa). Values come from build/params.json.
# ------------------------------------------------------------------------------------------------
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
    pages = {ti: data for ti, kind, data in previews if kind == "bg"}
    ink = tuple(int(VALUE_COLOUR[i:i + 2], 16) for i in (2, 4, 6))
    for ti, kind, data in previews:
        if kind == "bg":
            continue
        im = pages[ti]
        d = ImageDraw.Draw(im)
        if kind == "knob":
            ctl, strip_name = data
            x, y, w, h = ctl["rect"]
            v, text = vals.get(ctl["param"], (0.5, ""))
            f = KNOB["frame"]
            fi = round(v * (KNOB["frames"] - 1))
            im.alpha_composite(images[strip_name].crop((0, fi * f, f, (fi + 1) * f)), (x + (CTRL_W - f) // 2, y + (h - f) // 2))
        else:
            ctl, (px, py), on, off = data
            x, y, w, h = ctl["rect"]
            v, text = vals.get(ctl["param"], (0, ""))
            im.alpha_composite(images[on if v >= 0.5 else off], (px, py))
        d.text((x + TEXT_X, y + h // 2 - 1 + VALUE_H / 2), text, font=font(17, FONT_MEDIUM), fill=ink, anchor="lm")
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
