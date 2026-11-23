#!/usr/bin/env python3
"""Build Conch's hand-made skin (plugins/Conch/skins/) from the artwork in this folder.

    python3 plugins/Conch/art/make_skin.py      # writes skins/ and build/skin-preview-*.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    plate-src.webp          the user's plate: the Conch / SYMPATHETIC RESONATOR title over the sea, a glowing conch in
                            a driftwood window in the middle, two blank teal panels either side and a plank along the
                            bottom (1790x878, the 1280:628 aspect, so it scales straight onto the MPC's tab area)
    knob_small.webp         black fluted knob with skirt, pointer at 12 o'clock (shared with the other Groundswell skins)
    pill_on/off.webp        button pill (shared likewise; the lit glow is re-tinted to the shell's gold here)

Layout: three Q-Link banks per page, laid out like the art (Driftwood's layout, same plate shape). A column of four
on the left panel, a column of four on the right panel, a row of four along the bottom plank. The shell stays clear.
Each cell is the knob (or a lit pill for an on/off switch) with its name and live value beside it, on a darkened
recess. At the top of the side panels, on both pages, read-only readouts: RINGING (the chord the resonators are
tuned to) on the left; TUNED BY (where it comes from, "Key (unsure)" while Listen is not yet sure) with FOUND and
SURE on the right. They are not touchable and have no Q-Link.

    RING    left RESONATORS (Material, Decay, Bright, Shimmer)   right EXCITE (Hits, In HPF, Drive, Duck)
            plank: Mix, Width, Freeze, Output
    NOTES   left SOURCE (Source, Key, Scale, Tones)               right CHORD (Chord Root, Quality, Prog, Chord Len)
            plank: Spread, Low Note, Tune, Freeze

Q-Links (bank direction Column): knob column 1 = the left panel top to bottom, column 2 = the right panel, column 3 =
the plank left to right. Choice parameters are rotary selectors (detent dots; the value text names the choice). The
skin binds to parameter indices, so regenerate it whenever the parameter list changes (the script checks its table
against build/params.json when that exists). Adapted from Driftwood's generator (deliberately not a shared import: a
plugin's artwork should not break because a sibling's did).
"""
import colorsys
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
CREAM = (246, 228, 200)                # the title's cream: names, headings
GOLD = (255, 190, 110)                 # the shell's glow: knob arcs, lit pills, focus
FOCUS = "ffffbe6e"
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match the enum in src/Conch.cpp).
NAMES = ["out", "mix", "material", "decay", "bright", "spread", "lownote", "tones", "width", "hits", "hpf", "drive",
         "duck", "source", "key", "scale", "chordroot", "quality", "prog", "chordlen", "freeze", "shimmer", "tune",
         "foundkey", "sure", "chordnow", "tunedby"]
P = {n: i for i, n in enumerate(NAMES)}

ONOFF = ["Off", "On"]
NOTES12 = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
CHOICES = {P["material"]: ["String", "Bell", "Pipe"],
           P["lownote"]: ["C1", "C2", "C3", "C4"],
           P["tones"]: ["Triad", "7th", "9th", "5ths"],
           P["source"]: ["Listen", "Listen Key", "Key", "Chord", "Prog"],
           P["key"]: NOTES12,
           P["scale"]: ["Minor", "Major", "Dorian", "Phrygian", "Mixolyd"],
           P["chordroot"]: NOTES12,
           P["quality"]: ["Major", "Minor", "Sus2", "Sus4", "Power", "Dim", "Aug", "Maj7", "Min7", "Dom7", "m7b5",
                          "Maj9", "Min9", "Dom9"],
           P["prog"]: ["1-6-7-1", "1-4-5-1", "1-5-6-4", "1-7-6-7", "1-4-6-5", "1-6-4-5"],
           P["chordlen"]: ["1/2 Bar", "1 Bar", "2 Bars", "4 Bars"],
           P["freeze"]: ONOFF,
           P["tunedby"]: ["Key/Scale", "Key (unsure)", "Heard", "Chord", "Prog", "Frozen"]}
DETENTS = {P["spread"]: 4}             # integer parameters drawn as selectors (no choice list to check)

# Where the banks go on the 1280x628 picture: (x, y, cell width, cell height, direction).
BOARD_L = (52, 170, 270, 76, "col")    # left panel, inside its driftwood frame
BOARD_R = (958, 170, 270, 76, "col")   # right panel
PLANK = (132, 520, 254, 72, "row")     # the plank along the bottom
HEAD_Y = 154                           # panel headings

# Read-only readouts at the top of the side panels: (caption, parameter, x, y, w, h, value size).
READOUTS = [("RINGING", "chordnow", 52, 70, 270, 44, 26.0),
            ("TUNED BY", "tunedby", 958, 64, 270, 30, 19.0),
            ("FOUND", "foundkey", 958, 112, 130, 24, 16.0),
            ("SURE", "sure", 1098, 112, 130, 24, 16.0)]

# Inside a cell: the knob (or pill) in a 76 px wide area on the left, the name and value to its right.
KNOB = dict(src="knob_small.webp", crop=300, knob=42, arc_r=25, arc_w=3.2, frame=58, frames=100)
KNOB_CENTRE_SRC = (623.5, 625.5)
CTRL_W = 76
TEXT_X = 80
VALUE_H = 24

VALUE_COLOUR = "fff8e4c0"
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
    """Letter-spaced text centred on xy (anchor 'mm') or left-aligned at xy ('lm')."""
    x, y = xy
    tw = text_width(text, f, spacing)
    if anchor == "mm":
        x -= tw / 2
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill, anchor="lm")
        x += f.getlength(ch) + spacing
    return tw


def shadowed_text(im, xy, text, f, fill, spacing=2.0, anchor="mm", shadow=(12, 6, 2, 200), blur=1.6):
    """Cream lettering with a soft dark shadow under it, so it reads on light and dark grain alike."""
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
# Knob film strip: rotated knob + value arc (bipolar arcs grow from 12 o'clock)
# ------------------------------------------------------------------------------------------------
def knob_strip(spec, colour, selector=False, bipolar=False):
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
        if not selector:
            ImageDraw.Draw(big).arc(box, -135 - 90, 135 - 90, fill=(10, 6, 2, 110), width=w)      # burnt track
            a0, a1 = (min(0.0, angle), max(0.0, angle)) if bipolar else (-135.0, angle)
            if a1 - a0 > 0.5:
                glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
                ImageDraw.Draw(glow).arc(box, a0 - 90, a1 - 90, fill=colour + (150,), width=w * 2)
                big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(2.5 * ss)))
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
PILL_H, PILL_MARGIN = 28, 8
PILL_W = 58
PILL_ON_INK, PILL_OFF_INK = (255, 238, 210), (176, 168, 156)


def goldify(im):
    """Turn the pill's cyan glow into the shell's gold (hue only; the dark body stays dark)."""
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if s > 0.25 and 0.35 < h < 0.65:
                r2, g2, b2 = colorsys.hsv_to_rgb(0.095, min(1.0, s * 1.0), v)
                px[x, y] = (int(r2 * 255), int(g2 * 255), int(b2 * 255), a)
    return im


def pill_base(name, width):
    src = load(name)
    if name == "pill_on.webp":
        src = goldify(src)
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
BIPOLAR = {P["tune"]}                   # arcs grow from 12 o'clock


def knob(name, label):
    return dict(kind="knob", param=P[name], name=label, selector=P[name] in CHOICES or P[name] in DETENTS,
                bipolar=P[name] in BIPOLAR)


def toggle(name, caption, label):
    """One pill for an on/off (or two-way) switch, lit for the second choice. A lone button toggles."""
    return dict(kind="toggle", param=P[name], name=label, caption=caption)


def cell_rect(bank, s):
    x, y, w, h, direction = bank
    return (x, y + s * h, w, h) if direction == "col" else (x + s * w, y, w, h)


def page(name, left, right, plank, headings):
    """Three banks: the left board (Q-Link column 1), the right board (column 2), the plank (column 3)."""
    controls, qlinks, rects = [], {}, []
    for b, (bank, cells) in enumerate(((BOARD_L, left), (BOARD_R, right), (PLANK, plank))):
        x, y, w, h, direction = bank
        bw, bh = (w, 4 * h) if direction == "col" else (4 * w, h)
        rects.append((x - 4, y - 2, bw + 8, bh + 4))
        for s, ctl in enumerate(cells):
            if ctl is None:
                continue
            ctl = dict(ctl, rect=cell_rect(bank, s))
            controls.append(ctl)
            qlinks[13 + b - 4 * s] = ctl["param"]            # column b+1, top (13) to bottom (1)
    rects.append(rects[2])                                   # knob column 4 is unused; it highlights the plank
    return dict(name=name, controls=controls, qlinks=qlinks, rects=rects, headings=headings)


PAGES = [
    page("RING",
         [knob("material", "MATERIAL"), knob("decay", "DECAY"), knob("bright", "BRIGHT"), knob("shimmer", "SHIMMER")],
         [knob("hits", "HITS"), knob("hpf", "IN HPF"), knob("drive", "DRIVE"), knob("duck", "DUCK")],
         [knob("mix", "MIX"), knob("width", "WIDTH"), toggle("freeze", "FREEZE", "FREEZE"), knob("out", "OUTPUT")],
         ["RESONATORS", "EXCITE"]),
    page("NOTES",
         [knob("source", "SOURCE"), knob("key", "KEY"), knob("scale", "SCALE"), knob("tones", "TONES")],
         [knob("chordroot", "CHORD ROOT"), knob("quality", "QUALITY"), knob("prog", "PROG"), knob("chordlen", "CHORD LEN")],
         [knob("spread", "SPREAD"), knob("lownote", "LOW NOTE"), knob("tune", "TUNE"), toggle("freeze", "FREEZE", "FREEZE")],
         ["SOURCE", "CHORD"]),
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
        for (x, y, w, h) in pg["rects"][:3]:
            d.rounded_rectangle([x * sc, y * sc, (x + w) * sc, (y + h) * sc], radius=12 * sc, fill=(0, 14, 16, 120))
            d.rounded_rectangle([(x + 1) * sc, (y + 1.5) * sc, (x + w + 1) * sc, (y + h + 1.5) * sc], radius=12 * sc,
                                outline=(255, 236, 200, 40), width=int(1.2 * sc))
            d.rounded_rectangle([x * sc, y * sc, (x + w) * sc, (y + h) * sc], radius=12 * sc,
                                outline=(10, 5, 2, 150), width=int(1.2 * sc))
        for ctl in pg["controls"]:
            if ctl["kind"] == "knob" and ctl["selector"]:
                # detent dots round a selector, one per choice
                cx, cy = knob_centre(ctl["rect"])
                n = len(CHOICES[ctl["param"]]) if ctl["param"] in CHOICES else DETENTS[ctl["param"]]
                for k in range(n):
                    a = math.radians(-135 + 270 * k / (n - 1))
                    px, py = cx + 26.5 * math.sin(a), cy - 26.5 * math.cos(a)
                    d.ellipse([(px - 2.1) * sc, (py - 2.1) * sc, (px + 2.1) * sc, (py + 2.1) * sc], fill=CREAM + (215,))
        for (_, _, x, y, w, h, _) in READOUTS:
            d.rounded_rectangle([x * sc, (y - 15) * sc, (x + w) * sc, (y + h + 2) * sc], radius=9 * sc,
                                fill=(0, 10, 12, 95))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))
    for (caption, _, x, y, w, h, _) in READOUTS:
        shadowed_text(im, (x + w / 2, y - 6), caption, font(10, FONT_DEMI), GOLD + (235,), spacing=2.6)

    # Board headings, in the style of the title's "— POLYPHONIC CHORD STRUMMER —".
    for bank, text in zip((BOARD_L, BOARD_R), pg["headings"]):
        x, y, w, h, _ = bank
        cx = x + w / 2
        f = font(14, FONT_DEMI)
        tw = text_width(text, f, 4.0)
        shadowed_text(im, (cx, HEAD_Y), text, f, CREAM + (250,), spacing=4.0)
        d = ImageDraw.Draw(im)
        for sgn in (-1, 1):
            x0 = cx + sgn * (tw / 2 + 10)
            d.line([x0, HEAD_Y, x0 + sgn * 22, HEAD_Y], fill=CREAM + (200,), width=1)

    for ctl in pg["controls"]:
        x, y, w, h = ctl["rect"]
        size = 14
        while size > 10 and text_width(ctl["name"], font(size, FONT_DEMI), 1.8) > w - TEXT_X - 6:
            size -= 1
        shadowed_text(im, (x + TEXT_X, y + h / 2 - 12), ctl["name"], font(size, FONT_DEMI), CREAM + (255,),
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
    for idx, choices in CHOICES.items():
        got = params[idx].get("choices") if idx < len(params) else None
        if choices is ONOFF:
            if got not in (None, ONOFF):
                errors.append("parameter %d (%s) should be on/off" % (idx, params[idx]["name"]))
        elif got != choices:
            errors.append("parameter %d (%s): choices %s, skin has %s" % (idx, params[idx]["name"] if idx < len(params) else "?", got, choices))
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

    strips = {"plain": save("knob_gold.png", knob_strip(KNOB, GOLD)),
              "sel": save("knob_selector.png", knob_strip(KNOB, GOLD, selector=True)),
              "bi": save("knob_bipolar.png", knob_strip(KNOB, GOLD, bipolar=True))}

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    sizes = sorted({(c["rect"][2], c["rect"][3]) for pg in PAGES for c in pg["controls"]})
    for (w, h) in sizes:
        defs.append(("knob %dx%d" % (w, h), knob_component(KNOB, strips["plain"], "knob overlay", w, h)))
        defs.append(("selector %dx%d" % (w, h), knob_component(KNOB, strips["sel"], "menu overlay", w, h)))
        defs.append(("bipolar %dx%d" % (w, h), knob_component(KNOB, strips["bi"], "knob overlay", w, h)))

    toggles_done = set()
    tabs, qlink_entries, previews = [], [], []
    for ti, pg in enumerate(PAGES):
        bg_file = "bg_%s.jpg" % pg["name"].lower()
        bg = draw_background(pg)
        bg.save(os.path.join(OUT, bg_file), quality=92)
        children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                           ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
        previews.append((ti, "bg", bg.convert("RGBA")))
        for ci, ctl in enumerate(pg["controls"]):
            handle = "Parameter %d" % ctl["param"]
            x, y, w, h = ctl["rect"]
            if ctl["kind"] == "knob":
                kind = "selector" if ctl["selector"] else ("bipolar" if ctl["bipolar"] else "knob")
                key = "%s %dx%d" % (kind, w, h)
                strip_key = "sel" if ctl["selector"] else ("bi" if ctl["bipolar"] else "plain")
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "knob", (ctl, strips[strip_key])))
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

        for (caption, name, x, y, w, h, size) in READOUTS:
            children.append(G.entry("readout %s" % name, "Label",
                                    label_data("Value", VALUE_COLOUR, size, "horizontallyCentred verticallyCentred"),
                                    "%d %d %d %d" % (x, y, w, h), "No", "Show", "Always",
                                    remap=[("Data", "Parameter %d" % P[name])]))
            previews.append((ti, "readout", (name, x, y, w, h, size)))
        rects = ["%d %d %d %d" % r for r in pg["rects"]]
        tabs.append(obj(("version", 1), ("tabName", pg["name"]), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", G.component_definition([], children, "ff000000")),
                        ("qlinkBoundsData", rects)))
        qlink_entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "Column"),
                                 ("Q-Links", Obj(("Q-Link %d" % q, pg["qlinks"].get(q, -1)) for q in range(1, 17)))))

    replacements = [obj(("key", "Parameter %d" % k), ("value", v)) for k, v in sorted(CHOICES.items()) if v is not ONOFF]
    tui = obj(("pageData", obj(
        ("version", 1),
        ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                     ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs]))),
        ("replacementStrings", replacements),
        ("info", obj(("version", 1), ("type", "CompleteDescription"))),
        ("tabs", tabs))))
    # Program-mode Q-Links: the sixteen controls you reach for most.
    favourites = ["material", "decay", "bright", "mix", "hits", "hpf", "drive", "duck",
                  "source", "key", "scale", "tones", "spread", "lownote", "shimmer", "freeze"]
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
        if kind == "readout":
            name, x, y, w, h, size = data
            sample = {"chordnow": "Am", "tunedby": "Key (unsure)", "foundkey": "--", "sure": "0%"}[name]
            d.text((x + w / 2, y + h / 2), sample, font=font(int(size), FONT_MEDIUM), fill=ink, anchor="mm")
            continue
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
