#!/usr/bin/env python3
"""Build Wipeout's hand-made skin (plugins/Wipeout/skins/) from the artwork in this folder.

    python3 plugins/Wipeout/art/make_skin.py      # writes skins/ and build/skin-preview-1.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    plate-src.webp          the plate: the WIPEOUT title and the breaking wave, dark water either side
                            (1790x878, the 1280:628 aspect)
    mock-reference.webp     the designer's mock-up, for the look only - its controls are not Wipeout's
    knob_small.webp         knob with skirt, pointer at 12 o'clock (shared with Largemouth, Bearings and others)
    pill_on/off.webp        button pill (shared likewise)

Layout: one tab, PLAY. Four rows, each one Q-Link row: two boxes of two controls, one either side of the wave,
so the wave stays in view in the middle. Under the wave, level with row 4, a box holds the SCENE pills A-D and
Smooth (the two controls without a Q-Link).

    row  left box               right box             Q-Links (left to right)
    1    TRIGGER  Chance Hold   TIMING  Slice Length   13 14 15 16
    2    MANGLE   Reverse Half  BREAK   Tape  Gate     9 10 11 12
    3    SHAPE    Roll Decay    DICE    Vary Pattern    5  6  7  8
    4    GRID     Grid Offset   OUTPUT  Mix Output      1  2  3  4

The skin binds to parameter indices, so it must be regenerated if the parameter list changes. The drawing machinery
is a copy of Largemouth's generator (deliberately not a shared import: a plugin's artwork should not break because a
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
CREAM = (226, 240, 244)
GOLD = (232, 204, 158)                 # the bronze of the title
CYAN = (64, 218, 240)                  # the wave
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match src/Wipeout.cpp).
P = dict(out=0, chance=1, slice=2, length=3, mix=4, reverse=5, half=6, tape=7, gate=8, vary=9, roll=10,
         decay=11, hold=12, grid=13, offset=14, smooth=15, pattern=16, scene=17)

CHOICES = {P["slice"]: ["1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"],
           P["length"]: ["1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"],
           P["hold"]: ["Off", "On"],
           P["grid"]: ["1/4", "1/2", "1 Bar", "2 Bars", "4 Bars", "8 Bars"],
           P["offset"]: ["%d.%d.%d" % (o // 16 + 1, (o // 4) % 4 + 1, o % 4 + 1) for o in range(128)],   # bar.beat.16th
           P["pattern"]: ["Free", "1 Bar", "2 Bars", "4 Bars", "8 Bars"],
           P["scene"]: ["A", "B", "C", "D"]}

# Knob geometry: Largemouth's knob, a little smaller (76 px box) so a heading fits above each row.
KNOB = dict(src="knob_small.webp", crop=300, knob=58, arc_r=34, arc_w=4, frame=76, frames=100, ticks=(40, 46))
KNOB_CENTRE_SRC = (623.5, 625.5)

VALUE_COLOUR = "ffe2f0f4"
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
# Knob film strip: rotated knob + value arc
# ------------------------------------------------------------------------------------------------
def knob_strip(spec, colour):
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
        a0, a1 = -135.0, angle
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
PILL_ON_INK, PILL_OFF_INK = (210, 250, 255), (190, 182, 162)


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
# Layout
# ------------------------------------------------------------------------------------------------
COL_X = (16, 890)                # left edge of the two box columns; the wave shows between them
COL_W = 374
ROW_Y = (136, 252, 368, 484)
BOX_H = 106
SLOT_W = 178                     # two control slots per box
SLOT_X = (6, 190)                # slot left edges inside a box
KNOB_X = 42                      # knob centre from the slot's left edge
KNOB_Y = 64                      # knob centre from the box's top (the box heading sits above)
TAB_NAMES = ("PLAY",)

# Under the wave, level with row 4: the SCENE pills A-D and Smooth (neither has a Q-Link).
CENTRE_BOX = (404, 484, 876, 590)
SCENE_PILL_W = 58


def box(col, row):
    return (COL_X[col], ROW_Y[row], COL_X[col] + COL_W, ROW_Y[row] + BOX_H)


def slot_x(col, slot):
    return COL_X[col] + SLOT_X[slot]


def knob_in(param, col, row, slot, name):
    bx0, by0, _, _ = box(col, row)
    return dict(kind="knob", param=param, centre=(slot_x(col, slot) + KNOB_X, by0 + KNOB_Y), name=name,
                menu=param in CHOICES)


def pills_in(param, col, row, slot, name, captions):
    """Pills filling a slot, the control's name above them like a knob's."""
    bx0, by0, _, _ = box(col, row)
    n = len(captions)
    x0 = slot_x(col, slot) + 6
    pw = int((SLOT_W - 12 - (n - 1) * 8) / n)
    top = by0 + KNOB_Y - 2
    items = [(i, nm, x0 + i * (pw + 8), top, pw) for i, nm in enumerate(captions)]
    return dict(kind="pills", param=param, items=items, name=name, name_xy=(x0 + 2, by0 + KNOB_Y - 20))


ROWS = [
    # (left heading, right heading, four controls left to right)
    ("TRIGGER", "TIMING",
     [knob_in(P["chance"], 0, 0, 0, "CHANCE"), pills_in(P["hold"], 0, 0, 1, "HOLD", ["OFF", "ON"]),
      knob_in(P["slice"], 1, 0, 0, "SLICE"), knob_in(P["length"], 1, 0, 1, "LENGTH")]),
    ("MANGLE", "BREAK",
     [knob_in(P["reverse"], 0, 1, 0, "REVERSE"), knob_in(P["half"], 0, 1, 1, "HALF SPEED"),
      knob_in(P["tape"], 1, 1, 0, "TAPE STOP"), knob_in(P["gate"], 1, 1, 1, "GATE")]),
    ("SHAPE", "DICE",
     [knob_in(P["roll"], 0, 2, 0, "ROLL"), knob_in(P["decay"], 0, 2, 1, "DECAY"),
      knob_in(P["vary"], 1, 2, 0, "VARY"), knob_in(P["pattern"], 1, 2, 1, "PATTERN")]),
    ("GRID", "OUTPUT",
     [knob_in(P["grid"], 0, 3, 0, "GRID"), knob_in(P["offset"], 0, 3, 1, "OFFSET"),
      knob_in(P["mix"], 1, 3, 0, "MIX"), knob_in(P["out"], 1, 3, 1, "OUTPUT")]),
]
SCENE = dict(kind="pills", param=P["scene"], name="SCENE", heading=True,
             items=[(i, nm, CENTRE_BOX[0] + 18 + i * (SCENE_PILL_W + 8), CENTRE_BOX[1] + KNOB_Y - 18, SCENE_PILL_W)
                    for i, nm in enumerate(["A", "B", "C", "D"])],
             name_xy=(CENTRE_BOX[0] + 16, CENTRE_BOX[1] + 16))
SMOOTH = dict(kind="knob", param=P["smooth"], centre=(CENTRE_BOX[2] - SLOT_W + KNOB_X, CENTRE_BOX[1] + KNOB_Y),
              name="SMOOTH", menu=False)

QLINK_RECTS = ["%d %d %d %d" % (COL_X[0], ROW_Y[r], COL_X[1] + COL_W - COL_X[0], BOX_H) for r in range(4)]


# ------------------------------------------------------------------------------------------------
# Background
# ------------------------------------------------------------------------------------------------
def base_plate():
    src = Image.open(os.path.join(HERE, "plate-src.webp")).convert("RGB")
    return src.resize((W, H), Image.LANCZOS).convert("RGBA")


def all_boxes():
    return [box(c, r) for r in range(4) for c in range(2)]


def draw_background(controls):
    im = base_plate()

    def paint(d, sc):
        for bx in all_boxes() + [CENTRE_BOX]:
            d.rounded_rectangle([v * sc for v in bx], radius=14 * sc, fill=(2, 10, 16, 168),
                                outline=CYAN + (72,), width=int(1.4 * sc))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    d = ImageDraw.Draw(im)
    for r, (lh, rh, _) in enumerate(ROWS):
        for c, heading in enumerate((lh, rh)):
            bx0, by0, _, _ = box(c, r)
            draw_spaced(d, (bx0 + 16, by0 + 16), heading, font(13, FONT_DEMI), GOLD + (240,), spacing=3.2, anchor="lm")
    for ctl in controls:
        if ctl["kind"] == "knob":
            cx, cy = ctl["centre"]
            x, y = cx + KNOB["frame"] // 2 + VALUE_X_GAP, cy - 16
            size = 13
            while size > 9 and text_width(ctl["name"], font(size, FONT_DEMI), 2.0) > VALUE_W - 2:
                size -= 1
            draw_spaced(d, (x, y), ctl["name"], font(size, FONT_DEMI), CREAM + (230,), spacing=2.0, anchor="lm")
        elif ctl["kind"] == "pills":
            hx, hy = ctl["name_xy"]
            if ctl.get("heading"):     # a box heading (gold, like the others) rather than a control name
                draw_spaced(d, (hx, hy), ctl["name"], font(13, FONT_DEMI), GOLD + (240,), spacing=3.2, anchor="lm")
            else:
                draw_spaced(d, (hx, hy), ctl["name"], font(13, FONT_DEMI), CREAM + (230,), spacing=2.0, anchor="lm")
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
    return G.entry("focus", "Focus", G.focus_data("ff40daf0"), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


VALUE_X_GAP = 6
VALUE_W = SLOT_W - KNOB_X - KNOB["frame"] // 2 - VALUE_X_GAP - 2


def knob_component(spec, strip_file, overlay):
    """Knob at the left, its value to the right (the name is painted into the background above it)."""
    f = spec["frame"]
    pad_x = KNOB_X - f // 2
    w, h = pad_x + f + VALUE_X_GAP + VALUE_W, f
    children = [focus_child(w, h),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d 0 %d %d" % (pad_x, f, f), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, 22.0, "left verticallyCentred"),
                        "%d %d %d 32" % (pad_x + f + VALUE_X_GAP, f // 2 - 4, VALUE_W), "No", "Show", "Always")]
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


def build():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    images = {}

    def save(name, im):
        im.save(os.path.join(OUT, name), optimize=True)
        images[name] = im
        return name

    strip = save("knob_cyan.png", knob_strip(KNOB, CYAN))

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    knob_defs = {}
    for overlay in ("knob overlay", "menu overlay"):
        key = "knob cyan" + (" menu" if overlay == "menu overlay" else "")
        comp, geom = knob_component(KNOB, strip, overlay)
        defs.append((key, comp))
        knob_defs[key] = geom

    controls = [c for _, _, row in ROWS for c in row] + [SCENE, SMOOTH]
    previews = []
    bg_file = "bg_play.jpg"
    bg = draw_background(controls)
    bg.save(os.path.join(OUT, bg_file), quality=92)
    children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                       ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
    previews.append(("bg", bg.convert("RGBA")))
    pill_files = {}
    for ci, ctl in enumerate(controls):
        handle = "Parameter %d" % ctl["param"]
        if ctl["kind"] == "knob":
            key = "knob cyan" + (" menu" if ctl["menu"] else "")
            w, h, pad_x = knob_defs[key]
            cx, cy = ctl["centre"]
            x, y = cx - KNOB["frame"] // 2 - pad_x, cy - KNOB["frame"] // 2
            children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                    "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
            previews.append(("knob", (ctl, (x, y))))
        else:
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
            comp_name = "pills %d" % ci
            defs.append((comp_name, comp))
            children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                    "%d %d %d %d" % (ox, oy, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
            previews.append(("pills", (ctl, files)))

    tabs = [obj(("version", 1), ("tabName", TAB_NAMES[0]), ("fnKeyIndex", 0), ("fnKeySubIndex", 0),
                ("componentDefinition", G.component_definition([], children, "ff000000")),
                ("qlinkBoundsData", list(QLINK_RECTS)))]
    qmap = {}
    for r, (_, _, row) in enumerate(ROWS):
        for c, ctl in enumerate(row):
            qmap[13 + c - 4 * r] = ctl["param"]
    qlink_entries = [obj(("Tab", 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"),
                         ("Q-Links", Obj(("Q-Link %d" % q, qmap.get(q, -1)) for q in range(1, 17))))]

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
    make_preview(previews, images, strip)


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
    return vals


def make_preview(previews, images, strip):
    vals = preview_values()
    im = previews[0][1]
    d = ImageDraw.Draw(im)

    def choice_index(param):
        v = vals.get(param, (0, ""))[0]
        return int(round(v * (len(CHOICES[param]) - 1)))

    for kind, data in previews[1:]:
        if kind == "knob":
            ctl, (x, y) = data
            v, text = vals.get(ctl["param"], (0.5, ""))
            if ctl["param"] in CHOICES:
                text = CHOICES[ctl["param"]][choice_index(ctl["param"])]
            f = KNOB["frame"]
            fi = round(v * (KNOB["frames"] - 1))
            pad_x = KNOB_X - f // 2
            im.alpha_composite(images[strip].crop((0, fi * f, f, (fi + 1) * f)), (x + pad_x, y))
            d.text((x + pad_x + f + VALUE_X_GAP, y + f / 2 + 12), text, font=font(20, FONT_MEDIUM), fill=CREAM, anchor="lm")
        elif kind == "pills":
            ctl, files = data
            sel = choice_index(ctl["param"])
            for (choice, caption, x, y, pw), (on, off) in zip(ctl["items"], files):
                im.alpha_composite(images[on if choice == sel else off], (round(x - PILL_MARGIN), round(y - PILL_MARGIN)))
    full = Image.new("RGBA", (1280, 800), (24, 24, 26, 255))
    full.alpha_composite(im, (0, 110))
    fd = ImageDraw.Draw(full)
    fd.text((24, 55), "MPC header (drawn by the MPC)", font=font(18, FONT_MEDIUM), fill=(120, 120, 120), anchor="lm")
    fd.text((1280 / 2, 770), TAB_NAMES[0], font=font(18, FONT_DEMI), fill=(255, 255, 255), anchor="mm")
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    full.convert("RGB").save(os.path.join(PREVIEW_DIR, "skin-preview-1.png"))


if __name__ == "__main__":
    build()
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print("wrote %s (%d files, %.1f MB)" % (OUT, len(os.listdir(OUT)), total / 1e6))
