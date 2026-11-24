#!/usr/bin/env python3
"""Build Urchin's hand-made skin (plugins/Urchin/skins/) from the artwork in this folder.

    python3 plugins/Urchin/art/make_skin.py      # writes skins/ and build/skin-preview-*.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    plate-src.webp          the user's plate: the URCHIN title over open green water, kelp and barnacled rocks on the
                            left, a purple sea urchin on the right (2560x1256: exactly twice the MPC's 1280x628 tab area)
    knob_small.webp         black fluted knob with skirt, pointer at 12 o'clock (shared with the other Groundswell skins)
    pill_on/off.webp        button pill (shared likewise; the "on" glow is re-tinted to the urchin's magenta here)

Layout: the controls sit in the open water between the kelp and the urchin's spines, on a soft dark band per row so
the light rays do not wash out the lettering. The water narrows towards the bottom (the spines reach in from the
right), so the lower row is narrower and further left. Each row is one Q-Link bank of four. Drive gets the big knob.

    URCHIN  DRIVE SPIKE TONE BLEND   |  SPINE BIAS BLOOM GATE
    SHELL   LOW KEEP FOCUS LEVEL OUTPUT  |  SPINE R  WIDTH  STEREO (pill: lit = Mono)  (empty)

Choice parameters with three or more options are rotary selectors (a knob with detent dots; the value shows the
choice); the two-way Stereo switch is a single lit pill. The skin binds to parameter indices, so it must be regenerated
if the parameter list changes (the script checks its table against build/params.json when that exists). The drawing
machinery is adapted from Pressure's generator (deliberately not a shared import: a plugin's artwork should not break
because a sibling's did).
"""
import colorsys
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
sys.path.insert(0, os.path.join(SDK, "tools"))
import gen_ui as G  # noqa: E402  (TUI.json building blocks)
from gen_ui import obj, Obj  # noqa: E402

OUT = os.path.join(PROJECT, "skins")
PREVIEW_DIR = os.path.join(PROJECT, "build")

W, H = 1280, 628
CREAM = (238, 228, 204)                # printed lettering, like the title
SHADOW = (2, 14, 16)
SPINE = (232, 96, 190)                 # the urchin's magenta: knob arcs, lit pills
RULE = (214, 120, 196)                 # faint rule round each row
FOCUS = "ffe860be"
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match the enum in src/Urchin.cpp).
NAMES = ["output", "spine", "drive", "spike", "tone", "focus", "bias", "bloom", "gate", "blend", "low_keep", "level",
         "stereo", "spine_r", "width"]
P = {n: i for i, n in enumerate(NAMES)}

CHOICES = {P["spine"]: ["Gated Fuzz", "Octave Fuzz", "Torn Speaker", "Rectifier", "Wavefolder", "Overdrive"],
           P["focus"]: ["Flat", "Mids", "Lows"],
           P["stereo"]: ["Stereo", "Mono"],
           P["spine_r"]: ["Same", "Gated Fuzz", "Octave Fuzz", "Torn Speaker", "Rectifier", "Wavefolder", "Overdrive"]}
TWO_WAY = {P["stereo"]}                # shown as a pill, not a selector

# Rows in the open water: (top y, left x, cell width). Each row is one Q-Link bank of four.
CELL_H = 132
ROWS = ((174, 330, 135), (314, 300, 103))

# Knob geometry inside a cell: knob centre, then the printed name, then the value.
KNOB = dict(src="knob_small.webp", crop=300, knob=52, arc_r=31, arc_w=3.8, frame=70, frames=100)
KNOB_BIG = dict(src="knob_small.webp", crop=300, knob=76, arc_r=44, arc_w=4.8, frame=100, frames=100)
KNOB_CENTRE_SRC = (623.5, 625.5)
KNOB_CY = 52                           # knob centre inside the cell
NAME_Y = 105                           # name baseline-centre inside the cell
VALUE_Y, VALUE_H = 112, 19             # value label box inside the cell

VALUE_COLOUR = "fff3e6c8"
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


def shadowed_text(im, xy, text, f, fill, spacing=2.0):
    """Lettering with a soft dark shadow, so it reads over the bright water."""
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    draw_spaced(ImageDraw.Draw(sh), (xy[0] + 1, xy[1] + 1.5), text, f, SHADOW + (230,), spacing)
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(1.6)))
    draw_spaced(ImageDraw.Draw(im), xy, text, f, fill, spacing)


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


def tint(im):
    """Turn the pill's cyan glow into the urchin's magenta (hue only; the dark body stays dark)."""
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if s > 0.25 and 0.35 < h < 0.65:
                r2, g2, b2 = colorsys.hsv_to_rgb(0.885, min(1.0, s * 0.95), v)
                px[x, y] = (int(r2 * 255), int(g2 * 255), int(b2 * 255), a)
    return im


# ------------------------------------------------------------------------------------------------
# Knob film strip: rotated knob + value arc
# ------------------------------------------------------------------------------------------------
def knob_strip(spec, colour, selector=False):
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
            ImageDraw.Draw(big).arc(box, -135 - 90, 135 - 90, fill=(0, 12, 14, 120), width=w)   # dark track
            a0, a1 = -135.0, angle
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
PILL_H, PILL_MARGIN = 32, 9
PILL_W = 84
PILL_ON_INK, PILL_OFF_INK = (255, 214, 240), (176, 186, 184)
_PILL_SRC = {}


def pill_src(name):
    if name not in _PILL_SRC:
        im = load(name)
        _PILL_SRC[name] = tint(im) if name == "pill_on.webp" else im
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
def knob(name, label, big=False):
    return dict(kind="knob", param=P[name], name=label, big=big, selector=P[name] in CHOICES and P[name] not in TWO_WAY)


def toggle(name, caption, under):
    """One pill for an on/off (or two-way) switch: lit when on. A lone button toggles."""
    return dict(kind="toggle", param=P[name], name=under, caption=caption)


def page(name, banks):
    """banks: two lists of four controls (None = empty cell), one per row, in Q-Link bank order."""
    controls, qlinks, rects = [], {}, []
    for b, cells in enumerate(banks):
        y0, x0, cw = ROWS[b]
        rects.append((x0, y0, 4 * cw, CELL_H))
        for s, ctl in enumerate(cells):
            if ctl is None:
                continue
            ctl = dict(ctl, cell=(x0 + s * cw, y0), cw=cw)
            controls.append(ctl)
            qlinks[13 + s - 4 * b] = ctl["param"]
    while len(rects) < 4:                                # banks with nothing on this page: keep a rect anyway
        rects.append(rects[len(rects) % 2])
    return dict(name=name, controls=controls, qlinks=qlinks, rects=rects, cells=banks)


PAGES = [
    page("URCHIN", [
        [knob("drive", "DRIVE", big=True), knob("spike", "SPIKE"), knob("tone", "TONE"), knob("blend", "BLEND")],
        [knob("spine", "SPINE"), knob("bias", "BIAS"), knob("bloom", "BLOOM"), knob("gate", "GATE")],
    ]),
    page("SHELL", [
        [knob("low_keep", "LOW KEEP"), knob("focus", "FOCUS"), knob("level", "LEVEL"), knob("output", "OUTPUT")],
        [knob("spine_r", "SPINE R"), knob("width", "WIDTH"), toggle("stereo", "MONO", "STEREO / MONO"), None],
    ]),
]
TAB_NAMES = [p["name"] for p in PAGES]


# ------------------------------------------------------------------------------------------------
# Background
# ------------------------------------------------------------------------------------------------
def plate_raw():
    src = Image.open(os.path.join(HERE, "plate-src.webp")).convert("RGB")
    full = src.resize((W, round(W * src.height / src.width)), Image.LANCZOS)
    top = (full.height - H) // 2
    return full.crop((0, top, W, top + H)).convert("RGBA")


def draw_background(pg):
    im = plate_raw()

    # A soft dark band behind each used row (the light rays are bright), with a faint magenta rule round it.
    used = []                                            # band round the cells in use (a lone control gets its own)
    for (x, y, w, h), cells in zip(pg["rects"][:2], pg["cells"]):
        idx = [i for i, c in enumerate(cells) if c is not None]
        if idx:
            cw = w // 4
            used.append((x + idx[0] * cw, y, (idx[-1] - idx[0] + 1) * cw, h))
    band = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bd = ImageDraw.Draw(band)
    for (x, y, w, h) in used:
        bd.rounded_rectangle([x + 4, y + 2, x + w - 4, y + h - 2], radius=22, fill=(1, 14, 18, 135))
    im = Image.alpha_composite(im, band.filter(ImageFilter.GaussianBlur(10)))

    def paint(d, sc):
        for (x, y, w, h) in used:
            d.rounded_rectangle([(x + 6) * sc, (y + 4) * sc, (x + w - 6) * sc, (y + h - 4) * sc], radius=18 * sc,
                                outline=RULE + (70,), width=int(1.2 * sc))
        for ctl in pg["controls"]:
            if ctl["kind"] == "knob" and ctl["selector"]:
                cx = ctl["cell"][0] + ctl["cw"] / 2
                cy = ctl["cell"][1] + KNOB_CY
                n = len(CHOICES[ctl["param"]])
                for k in range(n):
                    a = math.radians(-135 + 270 * k / (n - 1))
                    px, py = cx + 33 * math.sin(a), cy - 33 * math.cos(a)
                    d.ellipse([(px - 2.6) * sc, (py - 2.6) * sc, (px + 2.6) * sc, (py + 2.6) * sc], fill=CREAM + (220,))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    for ctl in pg["controls"]:
        cx0, cy0 = ctl["cell"]
        cw = ctl["cw"]
        if ctl["kind"] == "knob":
            size = 13 if ctl["big"] else 12
            while size > 9 and text_width(ctl["name"], font(size, FONT_DEMI), 1.8) > cw - 10:
                size -= 1
            shadowed_text(im, (cx0 + cw / 2, cy0 + NAME_Y), ctl["name"], font(size, FONT_DEMI), CREAM + (250,), spacing=2.0)
        else:
            shadowed_text(im, (cx0 + cw / 2, cy0 + NAME_Y), ctl["name"], font(10, FONT_DEMI), CREAM + (225,), spacing=0.8)
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


def knob_component(spec, strip_file, overlay, cw):
    """A cell: the knob, its value under the printed name."""
    f = spec["frame"]
    children = [focus_child(cw, CELL_H),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d %d %d %d" % ((cw - f) // 2, KNOB_CY - f // 2, f, f), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, VALUE_SIZE, "horizontallyCentred verticallyCentred"),
                        "%d %d %d %d" % (2, VALUE_Y, cw - 4, VALUE_H), "No", "Show", "Always")]
    return G.component_definition(G.control_actions(overlay), children, "00000000")


def toggle_component(on_file, off_file, cw):
    pw, ph = PILL_W + 2 * PILL_MARGIN, PILL_H + 2 * PILL_MARGIN
    px = (cw - pw) // 2
    py = KNOB_CY - ph // 2
    children = [focus_child(cw, CELL_H),
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
        "plain": save("knob_spine.png", knob_strip(KNOB, SPINE)),
        "big": save("knob_spine_big.png", knob_strip(KNOB_BIG, SPINE)),
        "sel": save("knob_selector.png", knob_strip(KNOB, SPINE, selector=True)),
    }

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    have = set()

    def comp_key(kind, cw):
        key = "%s %d" % (kind, cw)
        if key not in have:
            have.add(key)
            if kind == "knob":
                defs.append((key, knob_component(KNOB, strips["plain"], "knob overlay", cw)))
            elif kind == "big":
                defs.append((key, knob_component(KNOB_BIG, strips["big"], "knob overlay", cw)))
            elif kind == "selector":
                defs.append((key, knob_component(KNOB, strips["sel"], "menu overlay", cw)))
        return key

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
            cw = ctl["cw"]
            if ctl["kind"] == "knob":
                kind = "selector" if ctl["selector"] else ("big" if ctl["big"] else "knob")
                key = comp_key(kind, cw)
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, cw, CELL_H), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                spec = KNOB_BIG if kind == "big" else KNOB
                strip = strips["sel" if kind == "selector" else ("big" if kind == "big" else "plain")]
                previews.append((ti, "knob", (ctl, (x, y), strip, spec)))
            else:
                slug = ctl["caption"].lower().replace("/", "")
                on = save("pill_on_%s.png" % slug, pill(True, ctl["caption"], PILL_W))
                off = save("pill_off_%s.png" % slug, pill(False, ctl["caption"], PILL_W))
                comp, (px, py) = toggle_component(on, off, cw)
                comp_name = "toggle %s" % slug
                defs.append((comp_name, comp))
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, cw, CELL_H), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "toggle", (ctl, (x + px, y + py), on, off)))

        rects = ["%d %d %d %d" % r for r in pg["rects"]]
        tabs.append(obj(("version", 1), ("tabName", pg["name"]), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", G.component_definition([], children, "ff000000")),
                        ("qlinkBoundsData", rects)))
        qlink_entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"),
                                 ("Q-Links", Obj(("Q-Link %d" % q, pg["qlinks"].get(q, -1)) for q in range(1, 17)))))

    replacements = [obj(("key", "Parameter %d" % k), ("value", v)) for k, v in sorted(CHOICES.items()) if k not in TWO_WAY]
    tui = obj(("pageData", obj(
        ("version", 1),
        ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                     ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs]))),
        ("replacementStrings", replacements),
        ("info", obj(("version", 1), ("type", "CompleteDescription"))),
        ("tabs", tabs))))
    program = Obj(("Q-Link %d" % (q + 1), q if q < len(NAMES) else -1) for q in range(16))
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
            ctl, (x, y), strip_name, spec = data
            v, text = vals.get(ctl["param"], (0.5, ""))
            f = spec["frame"]
            fi = round(v * (spec["frames"] - 1))
            im.alpha_composite(images[strip_name].crop((0, fi * f, f, (fi + 1) * f)), (x + (ctl["cw"] - f) // 2, y + KNOB_CY - f // 2))
            fs = 15
            while fs > 10 and font(fs, FONT_MEDIUM).getlength(text) > ctl["cw"] - 4:
                fs -= 1
            d.text((x + ctl["cw"] / 2, y + VALUE_Y + VALUE_H / 2), text, font=font(fs, FONT_MEDIUM), fill=ink, anchor="mm")
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
