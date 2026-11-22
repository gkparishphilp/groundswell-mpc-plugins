#!/usr/bin/env python3
"""Build Breakwater's hand-made skin (plugins/Breakwater/skins/) from the artwork in this folder.

    python3 plugins/Breakwater/art/make_skin.py      # writes skins/ and build/skin-preview-*.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    plate-src.webp          the plate: the Breakwater / AMP MODEL title over a weathered amp head on the rocks at
                            sunset, valves glowing behind the grille, a blank metal faceplate (1790x878, the
                            1280:628 aspect, so it scales straight onto the MPC's tab area)
    knob_small.webp         black fluted knob with skirt, pointer at 12 o'clock (shared with the other Groundswell skins)
    pill_on/off.webp        button pill (shared likewise; the "on" glow is re-tinted to valve amber here)

Layout: the controls sit on the amp's faceplate like a real amp's, screen-printed in dark ink. The plate holds two
rows of eight; each half-row is one Q-Link bank of four, boxed by a faint engraved frame so the Q-Link focus
rectangle lands on it.

    AMP   GAIN BASS MIDDLE TREBLE | CHIME SAG MASTER OUTPUT
          INPUT COMP VOICE BRIGHT | CAB MIC DISTANCE (pilot lamp)
    FX    DWELL TONE MIX          | DEPTH RATE MODE SYNC          (one row, centred on the plate)

Choice parameters are rotary selectors (a knob with detent dots; the value shows the choice); on/off parameters are a
single lit pill. The skin binds to parameter indices, so it must be regenerated if the parameter list changes (the
script checks its table against build/params.json when that exists). The drawing machinery is adapted from Low Tide's
generator (deliberately not a shared import: a plugin's artwork should not break because a sibling's did).
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
INK = (34, 24, 14)                     # screen-print ink on the faceplate
AMBER = (255, 172, 64)                 # the valves' glow: knob arcs, lit pills, focus
FOCUS = "ffffac40"
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match the enum in src/Breakwater.cpp).
NAMES = ["output", "input", "comp", "voice", "gain", "bright", "bass", "middle", "treble", "chime", "sag", "master",
         "cab", "mic", "distance", "dwell", "revtone", "revmix", "trem_mode", "tremdepth", "tremsync", "tremrate"]
P = {n: i for i, n in enumerate(NAMES)}

ONOFF = ["Off", "On"]
CHOICES = {P["voice"]: ["American", "British", "Boutique"],
           P["bright"]: ONOFF,
           P["cab"]: ["1x12", "2x12", "4x12", "Direct"],
           P["trem_mode"]: ["Smooth", "Opto", "Harmonic"],
           P["tremsync"]: ["Free", "Sync"]}
BIPOLAR = {P["input"]}

# The faceplate in the 1280x628 picture, and the grid on it.
PLATE = (162, 288, 1118, 488)
CELL_W, CELL_H = 113, 94
BANK_X = (178, 650)                    # left edges of the two Q-Link banks on a row (4 cells each, 20 px apart)
ROW_Y_AMP = (293, 389)
ROW_Y_FX = (345,)                      # FX has one row, centred on the plate (headings above it)

# Knob geometry inside a cell: the knob at the top, its name printed under it, the value under that.
KNOB = dict(src="knob_small.webp", crop=300, knob=46, arc_r=27, arc_w=3.5, frame=62, frames=100)
KNOB_CENTRE_SRC = (623.5, 625.5)
KNOB_TOP = 1                           # frame top inside the cell
NAME_Y = 67                            # name baseline-centre inside the cell
VALUE_Y, VALUE_H = 74, 19              # value label box inside the cell

VALUE_COLOUR = "ff23180c"
VALUE_FONT = "Nexa"
VALUE_SIZE = 15.0


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
    big = Image.new("RGBA", (size[0] * scale, size[1] * scale), (0, 0, 0, 0))
    painter(ImageDraw.Draw(big), scale)
    return big.resize(size, Image.LANCZOS)


def clean_alpha(im):
    r, g, b, a = im.split()
    a = a.point(lambda v: 0 if v < 16 else (255 if v > 250 else v))
    return Image.merge("RGBA", (r, g, b, a))


def load(name):
    return clean_alpha(Image.open(os.path.join(HERE, name)).convert("RGBA"))


def amberise(im):
    """Turn the pill's cyan glow into valve amber (hue only; the dark body stays dark)."""
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if s > 0.25 and 0.35 < h < 0.65:            # the cyan/teal glow
                r2, g2, b2 = colorsys.hsv_to_rgb(0.09, min(1.0, s * 1.05), v)
                px[x, y] = (int(r2 * 255), int(g2 * 255), int(b2 * 255), a)
    return im


# ------------------------------------------------------------------------------------------------
# Knob film strip: rotated knob + value arc (bipolar arcs grow from 12 o'clock)
# ------------------------------------------------------------------------------------------------
def knob_strip(spec, colour, bipolar=False, selector=False):
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
            ImageDraw.Draw(big).arc(box, -135 - 90, 135 - 90, fill=(20, 12, 4, 70), width=w)   # engraved track
            a0, a1 = (min(0.0, angle), max(0.0, angle)) if bipolar else (-135.0, angle)
            if a1 - a0 > 0.5:
                glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
                ImageDraw.Draw(glow).arc(box, a0 - 90, a1 - 90, fill=colour + (150,), width=w * 2)
                big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(2.5 * ss)))
                ImageDraw.Draw(big).arc(box, a0 - 90, a1 - 90, fill=colour + (255,), width=w)
            if bipolar:                                   # a small notch marks the centre
                ImageDraw.Draw(big).line([m, m - r - w, m, m - r + w], fill=INK + (160,), width=ss)
        rot = body.rotate(-angle, resample=Image.BICUBIC)
        off = int(m - knob_px / 2)
        big.alpha_composite(rot, (off, off))
        strip.alpha_composite(big.resize((frame, frame), Image.LANCZOS), (0, i * frame))
    return strip


# ------------------------------------------------------------------------------------------------
# Pills (3-slice stretch so every width keeps round ends), with a caption
# ------------------------------------------------------------------------------------------------
PILL_BODY = {"pill_on.webp": (446, 207, 1553, 465), "pill_off.webp": (427, 201, 1568, 465)}
PILL_H, PILL_MARGIN = 32, 9
PILL_W = 92
PILL_ON_INK, PILL_OFF_INK = (255, 214, 150), (176, 168, 156)
_PILL_SRC = {}


def pill_src(name):
    if name not in _PILL_SRC:
        im = load(name)
        _PILL_SRC[name] = amberise(im) if name == "pill_on.webp" else im
    return _PILL_SRC[name]


def pill_base(name, width):
    src = pill_src(name)
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
    text, size, spacing = caption.upper(), 13, 1.6
    while size > 9 and text_width(text, font(size, FONT_DEMI), spacing) > width - 14:
        size -= 1
    draw_spaced(d, (im.width / 2, im.height / 2 + 1), text, font(size, FONT_DEMI), colour + (255,), spacing=spacing)
    return im


# ------------------------------------------------------------------------------------------------
# Layout
# ------------------------------------------------------------------------------------------------
def knob(name, label):
    return dict(kind="knob", param=P[name], name=label, bipolar=P[name] in BIPOLAR, selector=P[name] in CHOICES)


def toggle(name, caption, label):
    """One pill for an on/off (or two-way) switch: lit when on. A lone button toggles."""
    return dict(kind="toggle", param=P[name], name=label, caption=caption)


LAMP = dict(kind="lamp")


def page(name, rows_y, banks, headings=()):
    """banks: up to four lists of four controls (None = empty cell), in Q-Link bank order: row 1 left, row 1 right,
    row 2 left, row 2 right."""
    controls, qlinks, rects, deco = [], {}, [], []
    for b, cells in enumerate(banks):
        row, side = divmod(b, 2)
        x0, y0 = BANK_X[side], rows_y[row]
        rects.append((x0, y0, 4 * CELL_W, CELL_H))
        for s, ctl in enumerate(cells):
            if ctl is None:
                continue
            cx0 = x0 + s * CELL_W
            if ctl["kind"] == "lamp":
                deco.append(("lamp", (cx0 + CELL_W / 2, y0 + 34)))
                continue
            ctl = dict(ctl, cell=(cx0, y0))
            controls.append(ctl)
            qlinks[13 + s - 4 * b] = ctl["param"]
    while len(rects) < 4:                                # banks with nothing on this page: keep a rect anyway
        rects.append(rects[len(rects) % 2])
    return dict(name=name, controls=controls, qlinks=qlinks, rects=rects, deco=deco, headings=list(headings))


PAGES = [
    page("AMP", ROW_Y_AMP, [
        [knob("gain", "GAIN"), knob("bass", "BASS"), knob("middle", "MIDDLE"), knob("treble", "TREBLE")],
        [knob("chime", "CHIME"), knob("sag", "SAG"), knob("master", "MASTER"), knob("output", "OUTPUT")],
        [knob("input", "INPUT"), knob("comp", "COMP"), knob("voice", "VOICE"), toggle("bright", "BRIGHT", "BRIGHT")],
        [knob("cab", "CABINET"), knob("mic", "MIC"), knob("distance", "DISTANCE"), LAMP],
    ]),
    page("FX", ROW_Y_FX, [
        [knob("dwell", "DWELL"), knob("revtone", "TONE"), knob("revmix", "MIX"), None],
        [knob("tremdepth", "DEPTH"), knob("tremrate", "RATE"), knob("trem_mode", "MODE"), toggle("tremsync", "SYNC", "SYNC")],
    ], headings=[(BANK_X[0], ROW_Y_FX[0] - 22, "SPRING REVERB"), (BANK_X[1], ROW_Y_FX[0] - 22, "TREMOLO")]),
]
TAB_NAMES = [p["name"] for p in PAGES]


# ------------------------------------------------------------------------------------------------
# Background
# ------------------------------------------------------------------------------------------------
def base_plate():
    src = Image.open(os.path.join(HERE, "plate-src.webp")).convert("RGB")
    return src.resize((W, H), Image.LANCZOS).convert("RGBA")


def engraved_rect(d, box, sc, radius=10):
    """A faint screen-printed frame with a light edge under it, so it reads as printed on metal."""
    x0, y0, x1, y1 = [v * sc for v in box]
    d.rounded_rectangle([x0 + sc, y0 + sc, x1 + sc, y1 + sc], radius=radius * sc, outline=(255, 244, 220, 60), width=int(1.2 * sc))
    d.rounded_rectangle([x0, y0, x1, y1], radius=radius * sc, outline=INK + (110,), width=int(1.2 * sc))


def draw_lamp(im, centre):
    """The pilot jewel: a chrome bezel round a faceted amber lens, lit."""
    cx, cy = centre
    sc = 4
    size = 80
    big = Image.new("RGBA", (size * sc, size * sc), (0, 0, 0, 0))
    m = size * sc / 2
    glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([m - 26 * sc, m - 26 * sc, m + 26 * sc, m + 26 * sc], fill=AMBER + (150,))
    big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(9 * sc)))
    d = ImageDraw.Draw(big)
    d.ellipse([m - 17 * sc, m - 17 * sc, m + 17 * sc, m + 17 * sc], fill=(70, 66, 60, 255))           # bezel shadow
    d.ellipse([m - 16 * sc, m - 16 * sc, m + 16 * sc, m + 16 * sc], fill=(196, 190, 178, 255))        # chrome bezel
    d.ellipse([m - 13 * sc, m - 13 * sc, m + 13 * sc, m + 13 * sc], fill=(120, 54, 8, 255))
    for k in range(12):                                                                                # facets
        a0 = k * 30
        col = (255, 150 + (k % 3) * 30, 40 + (k % 2) * 40, 255)
        d.pieslice([m - 12 * sc, m - 12 * sc, m + 12 * sc, m + 12 * sc], a0, a0 + 30, fill=col)
    d.ellipse([m - 5 * sc, m - 5 * sc, m + 5 * sc, m + 5 * sc], fill=(255, 236, 190, 255))
    d.ellipse([m - 8 * sc, m - 10 * sc, m - 3 * sc, m - 6 * sc], fill=(255, 255, 255, 200))             # glint
    lamp = big.resize((size, size), Image.LANCZOS)
    im.alpha_composite(lamp, (int(cx - size / 2), int(cy - size / 2)))
    d2 = ImageDraw.Draw(im)
    draw_spaced(d2, (cx, cy + 33), "ON", font(12, FONT_DEMI), INK + (235,), spacing=2.4)


def draw_background(pg):
    im = base_plate()

    def paint(d, sc):
        for (x, y, w, h) in pg["rects"][:len(set(pg["rects"]))]:
            engraved_rect(d, (x + 2, y + 1, x + w - 2, y + h - 1), sc)
        for ctl in pg["controls"]:
            if ctl["kind"] == "knob" and ctl["selector"]:
                # detent dots round a selector, one per choice
                cx = ctl["cell"][0] + CELL_W / 2
                cy = ctl["cell"][1] + KNOB_TOP + KNOB["frame"] / 2
                n = len(CHOICES[ctl["param"]])
                for k in range(n):
                    a = math.radians(-135 + 270 * k / (n - 1))
                    px, py = cx + 29 * math.sin(a), cy - 29 * math.cos(a)
                    d.ellipse([(px - 2.6) * sc, (py - 2.6) * sc, (px + 2.6) * sc, (py + 2.6) * sc], fill=INK + (200,))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    d = ImageDraw.Draw(im)
    for x, y, text in pg["headings"]:
        draw_spaced(d, (x + 10, y), text, font(13, FONT_DEMI), INK + (240,), spacing=3.4, anchor="lm")
    for ctl in pg["controls"]:
        cx0, cy0 = ctl["cell"]
        if ctl["kind"] == "knob":
            size = 12
            while size > 9 and text_width(ctl["name"], font(size, FONT_DEMI), 1.8) > CELL_W - 10:
                size -= 1
            draw_spaced(d, (cx0 + CELL_W / 2, cy0 + NAME_Y), ctl["name"], font(size, FONT_DEMI), INK + (240,), spacing=1.8)
        else:
            # under a toggle, the two positions it switches between
            ch = CHOICES[ctl["param"]]
            draw_spaced(d, (cx0 + CELL_W / 2, cy0 + NAME_Y), "%s / %s" % (ch[0].upper(), ch[1].upper()),
                        font(11, FONT_DEMI), INK + (190,), spacing=1.2)
    for kind, xy in pg["deco"]:
        if kind == "lamp":
            draw_lamp(im, xy)
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


def knob_component(spec, strip_file, overlay):
    """A cell: the knob at the top, its value under the printed name."""
    f = spec["frame"]
    children = [focus_child(CELL_W, CELL_H),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d %d %d %d" % ((CELL_W - f) // 2, KNOB_TOP, f, f), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, VALUE_SIZE, "horizontallyCentred verticallyCentred"),
                        "%d %d %d %d" % (2, VALUE_Y, CELL_W - 4, VALUE_H), "No", "Show", "Always")]
    return G.component_definition(G.control_actions(overlay), children, "00000000")


def toggle_component(on_file, off_file):
    pw, ph = PILL_W + 2 * PILL_MARGIN, PILL_H + 2 * PILL_MARGIN
    px = (CELL_W - pw) // 2
    py = KNOB_TOP + KNOB["frame"] // 2 - ph // 2
    children = [focus_child(CELL_W, CELL_H),
                G.entry("pill", "Button", G.button_data(on_file, off_file, 1, 2), "%d %d %d %d" % (px, py, pw, ph),
                        "No", "Show", "Always")]
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
        if idx < len(params) and params[idx].get("choices") != choices:
            errors.append("parameter %d (%s): choices %s, skin has %s" % (idx, params[idx]["name"], params[idx].get("choices"), choices))
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

    strips = {
        "plain": save("knob_amber.png", knob_strip(KNOB, AMBER)),
        "bi": save("knob_amber_bi.png", knob_strip(KNOB, AMBER, bipolar=True)),
        "sel": save("knob_selector.png", knob_strip(KNOB, AMBER, selector=True)),
    }

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    for key, strip, overlay in (("knob", "plain", "knob overlay"), ("knob bi", "bi", "knob overlay"),
                                ("selector", "sel", "menu overlay")):
        defs.append((key, knob_component(KNOB, strips[strip], overlay)))

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
            x, y = ctl["cell"]
            if ctl["kind"] == "knob":
                key = "selector" if ctl["selector"] else ("knob bi" if ctl["bipolar"] else "knob")
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, CELL_W, CELL_H), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                strip = strips["sel" if ctl["selector"] else ("bi" if ctl["bipolar"] else "plain")]
                previews.append((ti, "knob", (ctl, (x, y), strip)))
            else:
                slug = ctl["caption"].lower()
                on = save("pill_on_%s.png" % slug, pill(True, ctl["caption"], PILL_W))
                off = save("pill_off_%s.png" % slug, pill(False, ctl["caption"], PILL_W))
                comp, (px, py) = toggle_component(on, off)
                comp_name = "toggle %s" % slug
                defs.append((comp_name, comp))
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, CELL_W, CELL_H), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "toggle", (ctl, (x + px, y + py), on, off)))

        rects = ["%d %d %d %d" % r for r in pg["rects"]]
        tabs.append(obj(("version", 1), ("tabName", pg["name"]), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", G.component_definition([], children, "ff000000")),
                        ("qlinkBoundsData", rects)))
        qlink_entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"),
                                 ("Q-Links", Obj(("Q-Link %d" % q, pg["qlinks"].get(q, -1)) for q in range(1, 17)))))

    replacements = [obj(("key", "Parameter %d" % k), ("value", v)) for k, v in sorted(CHOICES.items()) if v is not ONOFF]
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
        im = pages[ti]
        d = ImageDraw.Draw(im)
        if kind == "knob":
            ctl, (x, y), strip_name = data
            v, text = vals.get(ctl["param"], (0.5, ""))
            f = KNOB["frame"]
            fi = round(v * (KNOB["frames"] - 1))
            im.alpha_composite(images[strip_name].crop((0, fi * f, f, (fi + 1) * f)), (x + (CELL_W - f) // 2, y + KNOB_TOP))
            d.text((x + CELL_W / 2, y + VALUE_Y + VALUE_H / 2), text, font=font(15, FONT_MEDIUM), fill=ink, anchor="mm")
        elif kind == "toggle":
            ctl, (px, py), on, off = data
            v = vals.get(ctl["param"], (0, ""))[0]
            im.alpha_composite(images[on if v >= 0.5 else off], (px, py))
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
