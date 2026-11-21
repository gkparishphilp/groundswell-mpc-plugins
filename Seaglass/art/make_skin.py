#!/usr/bin/env python3
"""Build Seaglass's hand-made skin (plugins/Seaglass/skins/) from the artwork in this folder.

    python3 plugins/Seaglass/art/make_skin.py      # writes skins/ and build/skin-preview-1.png

Render BEFORE tools/build.py: the build copies skins/ as it finds it and does not re-render.

Source art:
    plate-src.webp          the plate, supplied by the user: the Seaglass title over a sunset beach, a wave
                            breaking on the left, glowing sea glass on the pebbles (1790x878, the 1280:628 aspect)
    knob_small.webp         knob with skirt, pointer at 12 o'clock (shared with Abyss, Krill, Wipeout, Largemouth)
    pill_on/off.webp        button pill (shared likewise)

Layout: two tabs. Each row is one Q-Link row: two boxes of two controls, one either side, so the title, the
wave and the glass on the beach stay in view in the middle.

    SEA      SPACE | TAIL,  FREEZE | PAD,  SHIMMER | SWELL,  COLOUR | OUTPUT
             (SHIMMER's Shift is a bipolar knob, centre = no shift; Interval, its 1.0/1.1 predecessor,
             is no longer on the panel)
    DETAIL   SWELL | SENSE,  COLOUR | OUTPUT

ADD is a single tap button (a Toggle: every tap flips the parameter, and every flip adds a layer) with the
plugin's own value text beside it ("Tap", "3 layers").

The skin binds to parameter indices, so it must be regenerated if the parameter list changes (the script checks its
parameter table against build/params.json when that exists). The drawing machinery is a copy of Abyss's
generator (deliberately not a shared import: a plugin's artwork should not break because a sibling's did).
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
CREAM = (236, 246, 250)
CORAL = (206, 178, 255)                # the lavender glass: box headings (name kept from Abyss's generator)
CYAN = (122, 236, 206)                 # the seafoam glass: knob arcs, box outlines, focus
FONT_FILE = "/System/Library/Fonts/Avenir Next.ttc"   # build-time only: baked into the PNGs
FONT_DEMI, FONT_MEDIUM, FONT_REGULAR = 2, 5, 7        # face indices in Avenir Next.ttc

# Parameter indices (must match the enum in src/Seaglass.cpp).
NAMES = ["mix", "decay", "predelay", "tone", "lowcut", "motion", "width", "output",
         "freeze", "fade", "shimmer", "interval", "swell", "rise", "sense",
         # 1.1
         "add", "live",
         # 1.2
         "shift"]
P = {n: i for i, n in enumerate(NAMES)}

CHOICES = {P["freeze"]: ["Off", "On"],
           P["interval"]: ["Octave", "Fifth", "2 Oct", "Oct Down"]}

# Knob geometry: Wipeout's (a 76 px box so a heading fits above each row).
KNOB = dict(src="knob_small.webp", crop=300, knob=58, arc_r=34, arc_w=4, frame=76, frames=100)
KNOB_CENTRE_SRC = (623.5, 625.5)

VALUE_COLOUR = "ffecf6ff"
VALUE_FONT = "Nexa"
VALUE_SIZE = 20.0


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


# ------------------------------------------------------------------------------------------------
# Knob film strip: rotated knob + value arc (bipolar arcs grow from 12 o'clock)
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
        a0, a1 = (min(0.0, angle), max(0.0, angle)) if bipolar else (-135.0, angle)
        if a1 - a0 > 0.5:
            glow = Image.new("RGBA", big.size, (0, 0, 0, 0))
            ImageDraw.Draw(glow).arc(box, a0 - 90, a1 - 90, fill=colour + (200,), width=w * 2)
            big = Image.alpha_composite(big, glow.filter(ImageFilter.GaussianBlur(3 * ss)))
            ImageDraw.Draw(big).arc(box, a0 - 90, a1 - 90, fill=colour + (255,), width=w)
        if bipolar:                                       # a small notch marks the centre
            ImageDraw.Draw(big).line([m, m - r - w, m, m - r + w], fill=(255, 255, 255, 120), width=ss)
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
PILL_ON_INK, PILL_OFF_INK = (210, 250, 255), (176, 170, 196)


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
COL_X = (16, 890)                # left edge of the two box columns; the wave and the glass show between them
ROW_Y = (136, 252, 368, 484)
BOX_H = 106
SLOT_W = 178                     # two control slots per box
SLOT_X = (6, 190)                # slot left edges inside a box
KNOB_X = 42                      # knob centre from the slot's left edge
KNOB_Y = 64                      # knob centre from the box's top (the box heading sits above)


def slot_x(col, slot):
    return COL_X[col] + SLOT_X[slot]


SLOT_GAP = SLOT_X[1] - SLOT_X[0] - SLOT_W


def knob(name, label, bipolar=False):
    return dict(kind="knob", param=P[name], name=label, bipolar=bipolar, menu=P[name] in CHOICES)


def pills(name, label, captions=None, width=SLOT_W):
    return dict(kind="pills", param=P[name], name=label, captions=captions or [c.upper() for c in CHOICES[P[name]]],
                width=width)


TAP_W = 82                       # width of a tap button


def tap(name, label, caption):
    return dict(kind="tap", param=P[name], name=label, caption=caption)


def place(ctl, col, row, slot, sx):
    """Give a control its position on the screen; sx is its slot's left edge."""
    ctl = dict(ctl)
    by0 = ROW_Y[row]
    if ctl["kind"] == "knob":
        ctl["centre"] = (sx + KNOB_X, by0 + KNOB_Y)
    elif ctl["kind"] == "tap":
        x0, top = sx + 6, by0 + KNOB_Y - 2
        ctl["button"] = (x0, top, TAP_W)
        ctl["value_xy"] = (x0 + TAP_W + 12, top + PILL_H / 2)
        ctl["name_xy"] = (x0 + 2, by0 + KNOB_Y - 20)
    else:
        n = len(ctl["captions"])
        x0 = sx + 6
        pw = int((ctl["width"] - 12 - (n - 1) * 8) / n)
        top = by0 + KNOB_Y - 2
        ctl["items"] = [(i, nm, x0 + i * (pw + 8), top, pw) for i, nm in enumerate(ctl["captions"])]
        ctl["name_xy"] = (x0 + 2, by0 + KNOB_Y - 20)
    ctl["slot"] = (col, row, slot)
    return ctl


def page(name, rows):
    """rows: up to four (left box, right box); a box is (heading, [controls]) or None."""
    controls, boxes, qlinks = [], [], {}
    for r, pair in enumerate(rows):
        for c, bx in enumerate(pair):
            if not bx:
                continue
            heading, ctls = bx
            x0 = COL_X[c]
            widths = [ctl.get("width", SLOT_W) for ctl in ctls]
            width = SLOT_X[0] + sum(widths) + SLOT_GAP * (len(ctls) - 1) + 6
            boxes.append(((x0, ROW_Y[r], x0 + width, ROW_Y[r] + BOX_H), heading))
            sx = x0 + SLOT_X[0]
            for s, ctl in enumerate(ctls):
                placed = place(ctl, c, r, s, sx)
                sx += widths[s] + SLOT_GAP
                controls.append(placed)
                qlinks[13 + (2 * c + s) - 4 * r] = placed["param"]
    return dict(name=name, controls=controls, boxes=boxes, qlinks=qlinks)


PAGES = [
    page("SEA", [
        (("SPACE", [knob("mix", "MIX"), knob("decay", "DECAY")]),
         ("TAIL", [knob("predelay", "PRE-DELAY"), knob("tone", "TONE")])),
        (("FREEZE", [pills("freeze", "FREEZE"), tap("add", "ADD", "ADD")]),
         ("PAD", [knob("fade", "FADE"), knob("live", "LIVE")])),
        (("SHIMMER", [knob("shimmer", "SHIMMER"), knob("shift", "SHIFT", True)]),
         ("SWELL", [knob("swell", "SWELL"), knob("rise", "RISE")])),
        (("COLOUR", [knob("lowcut", "LOW CUT"), knob("motion", "MOTION")]),
         ("OUTPUT", [knob("width", "WIDTH"), knob("output", "OUTPUT")])),
    ]),
    page("DETAIL", [
        (("SWELL", [knob("swell", "SWELL"), knob("rise", "RISE")]),
         ("SENSE", [knob("sense", "SENSE")])),
        (("COLOUR", [knob("lowcut", "LOW CUT"), knob("motion", "MOTION")]),
         ("OUTPUT", [knob("width", "WIDTH"), knob("output", "OUTPUT")])),
    ]),
]
TAB_NAMES = [p["name"] for p in PAGES]
QLINK_RECTS = ["%d %d %d %d" % (COL_X[0], ROW_Y[r], COL_X[1] + SLOT_X[1] + SLOT_W + 6 - COL_X[0], BOX_H) for r in range(4)]


# ------------------------------------------------------------------------------------------------
# Background
# ------------------------------------------------------------------------------------------------
def base_plate():
    src = Image.open(os.path.join(HERE, "plate-src.webp")).convert("RGB")
    return src.resize((W, H), Image.LANCZOS).convert("RGBA")


VALUE_X_GAP = 6
VALUE_W = SLOT_W - KNOB_X - KNOB["frame"] // 2 - VALUE_X_GAP - 2


def draw_background(pg):
    im = base_plate()

    def paint(d, sc):
        for bx, _ in pg["boxes"]:
            d.rounded_rectangle([v * sc for v in bx], radius=14 * sc, fill=(4, 18, 30, 200),
                                outline=CYAN + (80,), width=int(1.4 * sc))
    im = Image.alpha_composite(im, supersampled((W, H), 3, paint))

    d = ImageDraw.Draw(im)
    for (bx0, by0, _, _), heading in pg["boxes"]:
        draw_spaced(d, (bx0 + 16, by0 + 16), heading, font(13, FONT_DEMI), CORAL + (245,), spacing=3.2, anchor="lm")
    for ctl in pg["controls"]:
        if ctl["kind"] == "knob":
            cx, cy = ctl["centre"]
            x, y = cx + KNOB["frame"] // 2 + VALUE_X_GAP, cy - 16
            size = 13
            while size > 9 and text_width(ctl["name"], font(size, FONT_DEMI), 2.0) > VALUE_W - 2:
                size -= 1
            draw_spaced(d, (x, y), ctl["name"], font(size, FONT_DEMI), CREAM + (230,), spacing=2.0, anchor="lm")
        else:
            hx, hy = ctl["name_xy"]
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
    return G.entry("focus", "Focus", G.focus_data("ff7aecce"), "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


def knob_component(spec, strip_file, overlay):
    """Knob at the left, its value to the right (the name is painted into the background above it)."""
    f = spec["frame"]
    pad_x = KNOB_X - f // 2
    w, h = pad_x + f + VALUE_X_GAP + VALUE_W, f
    children = [focus_child(w, h),
                G.entry("knob", "Knob", G.knob_filmstrip_data(strip_file, spec["frames"]),
                        "%d 0 %d %d" % (pad_x, f, f), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, VALUE_SIZE, "left verticallyCentred"),
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


def tap_component(ctl, image):
    """One Toggle button (every tap flips the parameter) and the parameter's value text to its right."""
    x0, top, pw = ctl["button"]
    ox, oy = x0 - PILL_MARGIN, top - PILL_MARGIN
    vw = SLOT_W - TAP_W - 18
    w = pw + 2 * PILL_MARGIN + 12 + vw
    h = PILL_H + 2 * PILL_MARGIN
    children = [focus_child(int(w), int(h)),
                G.entry("tap", "Button", G.button_data(image, image, 1, 1),
                        "0 0 %d %d" % (pw + 2 * PILL_MARGIN, h), "No", "Show", "Always"),
                G.entry("value", "Label", label_data("Value", VALUE_COLOUR, VALUE_SIZE, "left verticallyCentred"),
                        "%d 0 %d %d" % (pw + 2 * PILL_MARGIN + 3, vw, h), "No", "Show", "Always")]
    comp = G.component_definition(G.control_actions("knob overlay", double_click=False), children, "00000000")
    return comp, (round(ox), round(oy), round(w), round(h))


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

    used_bi = sorted({c["bipolar"] for pg in PAGES for c in pg["controls"] if c["kind"] == "knob"})   # only the strips in use
    strips = {bi: save("knob_glass%s.png" % ("_bi" if bi else ""), knob_strip(KNOB, CYAN, bipolar=bi)) for bi in used_bi}

    defs = [("knob overlay", G.overlay_component("KnobOverlay", G.knob_overlay_data(False), "ff111111")),
            ("menu overlay", G.overlay_component("MenuOverlay", G.menu_overlay_data(), "ff111111"))]
    knob_defs = {}
    for bi in used_bi:
        for overlay in ("knob overlay", "menu overlay"):
            key = "knob" + (" bi" if bi else "") + (" menu" if overlay == "menu overlay" else "")
            comp, geom = knob_component(KNOB, strips[bi], overlay)
            defs.append((key, comp))
            knob_defs[key] = geom

    tabs, qlink_entries, previews = [], [], []
    pill_files = {}
    pill_defs = {}
    for ti, pg in enumerate(PAGES):
        bg_file = "bg_%s.jpg" % pg["name"].lower()
        bg = draw_background(pg)
        bg.save(os.path.join(OUT, bg_file), quality=92)
        children = [G.entry("background", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                           ("image", bg_file)), "0 0 %d %d" % (W, H), "No", "Show", "Always")]
        previews.append((ti, "bg", bg.convert("RGBA")))
        for ci, ctl in enumerate(pg["controls"]):
            handle = "Parameter %d" % ctl["param"]
            if ctl["kind"] == "knob":
                key = "knob" + (" bi" if ctl["bipolar"] else "") + (" menu" if ctl["menu"] else "")
                w, h, pad_x = knob_defs[key]
                cx, cy = ctl["centre"]
                x, y = cx - KNOB["frame"] // 2 - pad_x, cy - KNOB["frame"] // 2
                children.append(G.entry("ctl %d" % ci, key, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (x, y, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "knob", (ctl, (x, y), strips[ctl["bipolar"]])))
            elif ctl["kind"] == "tap":
                fname = "tap_%s.png" % ctl["caption"].lower()
                if fname not in pill_files:
                    pill_files[fname] = save(fname, pill(False, ctl["caption"], TAP_W))
                comp, (ox, oy, w, h) = tap_component(ctl, fname)
                comp_name = "tap %d %d" % (ti, ci)
                defs.append((comp_name, comp))
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (ox, oy, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "tap", (ctl, fname)))
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
                comp_name = "pills %d %d" % (ti, ci)
                defs.append((comp_name, comp))
                children.append(G.entry("ctl %d" % ci, comp_name, obj(("version", 1), ("handleName", "Data")),
                                        "%d %d %d %d" % (ox, oy, w, h), "Yes", "Hide", "Always", remap=[("Data", handle)]))
                previews.append((ti, "pills", (ctl, files)))

        tabs.append(obj(("version", 1), ("tabName", pg["name"]), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", G.component_definition([], children, "ff000000")),
                        ("qlinkBoundsData", list(QLINK_RECTS))))
        qlink_entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"),
                                 ("Q-Links", Obj(("Q-Link %d" % q, pg["qlinks"].get(q, -1)) for q in range(1, 17)))))

    replacements = [obj(("key", "Parameter %d" % k), ("value", v)) for k, v in sorted(CHOICES.items())]
    tui = obj(("pageData", obj(
        ("version", 1),
        ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                     ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs]))),
        ("replacementStrings", replacements),
        ("info", obj(("version", 1), ("type", "CompleteDescription"))),
        ("tabs", tabs))))
    program = Obj(("Q-Link %d" % (q + 1), q if q < len(NAMES) else -1) for q in range(16))   # 15 parameters: Q-Link 16 unused
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

    def choice_index(param):
        v = vals.get(param, (0, ""))[0]
        return int(round(v * (len(CHOICES[param]) - 1)))

    for ti, kind, data in previews:
        im = pages[ti]
        d = ImageDraw.Draw(im)
        if kind == "knob":
            ctl, (x, y), strip_name = data
            v, text = vals.get(ctl["param"], (0.5, ""))
            f = KNOB["frame"]
            fi = round(v * (KNOB["frames"] - 1))
            pad_x = KNOB_X - f // 2
            im.alpha_composite(images[strip_name].crop((0, fi * f, f, (fi + 1) * f)), (x + pad_x, y))
            d.text((x + pad_x + f + VALUE_X_GAP, y + f / 2 + 12), text, font=font(19, FONT_MEDIUM), fill=CREAM, anchor="lm")
        elif kind == "tap":
            ctl, fname = data
            x0, top, pw = ctl["button"]
            im.alpha_composite(images[fname], (round(x0 - PILL_MARGIN), round(top - PILL_MARGIN)))
            vx, vy = ctl["value_xy"]
            d.text((vx, vy + 1), "Tap", font=font(19, FONT_MEDIUM), fill=CREAM, anchor="lm")
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
