#!/usr/bin/env python3
"""Generate the Terminal skin's per-scheme assets from a palette definition.

The Terminal skin keeps all structure in ``style.qss`` and all colour in a
per-scheme ``style_<dir>.qss`` plus a per-scheme asset directory.  Both are
derived from a single palette JSON in ``palettes/``, so recolouring the skin --
including collapsing it to a single hue -- means editing one small file and
re-running this script.

Geometry for the icon glyphs is borrowed from LateNight/palemoon.  Those SVGs
are Inkscape output carrying gradients, blur filters and a black "halo"
outline layer under every shape.  A terminal UI wants the opposite, so we
flatten them: drop the halo layers, defs, filters and opacity, and paint what
is left in one palette colour.  Chrome that a terminal draws with box-rules
rather than pictures (button frames, knob bezels, slider grooves) is replaced
by generated primitives instead, and the borders come from QSS.

Usage:
    python tools/terminal_skin/generate.py            # all palettes
    python tools/terminal_skin/generate.py --palette btop
    python tools/terminal_skin/generate.py --report   # categorise only
"""

from __future__ import annotations
import argparse
import json
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SKIN = REPO / "res" / "skins" / "Terminal"
# Geometry donor: we reuse LateNight's icon shapes, not its rendering style.
REF = REPO / "res" / "skins" / "LateNight" / "palemoon"
# A few assets (the waveform marks) exist only in the other scheme.
REF_CLASSIC = REPO / "res" / "skins" / "LateNight" / "classic"
# Library sidebar icons live in Mixxx's own resources, not in any skin.
REF_LIBRARY = REPO / "res" / "images" / "library"
# The names LibraryFeature can be constructed with; see
# src/library/libraryfeature.cpp, which looks for a skin override first.
LIBRARY_ICONS = (
    "autodj",
    "banshee",
    "computer",
    "crates",
    "hidden",
    "history",
    # Set by tree items rather than by a feature: the current history session,
    # and the padlocks on a locked playlist, crate or session.
    "history_current",
    "itunes",
    "locked",
    "locked_tracklist",
    "playlist",
    "prepare",
    "recordings",
    "rekordbox",
    "rhythmbox",
    "serato",
    "tracks",
    "traktor",
)
SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)
# Monospace stack: Cascadia/Consolas on Windows, DejaVu on Linux, Menlo on mac.
MONO = "'Cascadia Mono','Consolas','DejaVu Sans Mono','Menlo',monospace"
# Colours LateNight uses for the black outline layer that haloes every glyph.
DARK_INK = {
    "#000",
    "#000000",
    "#000001",
    "#010101",
    "#020202",
    "#050505",
    "black",
}
# The wordmark for the launch screen, in figlet's "standard" font: a terminal
# draws its splash out of characters, so this one does too.  One tuple per
# letter of MIXXX, five rows each, every row of a letter the same width so the
# columns line up once the letters are joined.
ASCII_LOGO = (
    (" __  __ ", "|  \\/  |", "| |\\/| |", "| |  | |", "|_|  |_|"),
    (" ___ ", "|_ _|", " | | ", " | | ", "|___|"),
    ("__  __", "\\ \\/ /", " \\  / ", " /  \\ ", "/_/\\_\\"),
    ("__  __", "\\ \\/ /", " \\  / ", " /  \\ ", "/_/\\_\\"),
    ("__  __", "\\ \\/ /", " \\  / ", " /  \\ ", "/_/\\_\\"),
)
# Character cell and type size for that art, in px.  The cell is deliberately
# a little smaller than the glyphs so neighbours overlap: character art is made
# of runs of "_" and "|" that have to read as continuous bars and uprights, and
# at a cell the size of the advance width every run breaks up into dashes.  The
# narrowest face in the stack sets the bound -- Consolas advances 0.55em, so a
# 9px cell at 17px type still overlaps.
ASCII_LOGO_CELL_W = 9.0
ASCII_LOGO_CELL_H = 16.0
ASCII_LOGO_FONT = 17.0
# Glyphs whose source colours mean something worth keeping.  Flattening a
# picture to one colour is right for an icon, where the colour is decoration,
# but the fx mix mode button draws a diagram: LateNight puts the dry signal in
# grey and the wet one in red, and which curve is which is half of what the
# button says.  Map those two onto palette roles instead of collapsing them.
TWO_TONE = {
    "btn__fx_mixmode_d+w": {"#918273": "fg_dim", "#da0606": "fx"},
    "btn__fx_mixmode_d-w": {"#918273": "fg_dim", "#da0606": "fx"},
}
DROP_ATTRS = (
    "filter",
    "opacity",
    "fill-opacity",
    "stroke-opacity",
    "stop-color",
    "stop-opacity",
    "solid-color",
    "solid-opacity",
    "color",
    "color-rendering",
    "image-rendering",
    "shape-rendering",
    "text-rendering",
    "color-interpolation",
    "color-interpolation-filters",
    "enable-background",
    "isolation",
    "mix-blend-mode",
    "dominant-baseline",
    "paint-order",
)
# style: declarations worth keeping once everything else is flattened away.
KEEP_STYLE = (
    "fill",
    "stroke",
    "stroke-width",
    "stroke-linecap",
    "stroke-linejoin",
    "font-family",
    "font-size",
    "font-weight",
    "text-anchor",
)


# --------------------------------------------------------------------------- #
# palette
# --------------------------------------------------------------------------- #
class Palette:
    def __init__(self, data: dict):
        self.name = data["name"]
        self.dir = data["dir"]
        self.description = data.get("description", "")
        # A single-hue palette. Cue and track colours come from the
        # user's colour palette rather than from here, and a scheme with
        # one hue has nowhere to put them, so it opts out of them.
        self.mono = bool(data.get("mono", False))
        self.colors = dict(data["colors"])
        # Roles are indirection: role -> colour key -> hex.  Expose both so a
        # template can say {{accent}} or {{green}}.
        self.roles = {}
        for role, key in data.get("roles", {}).items():
            if key not in self.colors:
                raise SystemExit(
                    f"palette {self.name}: role {role!r}"
                    f" -> unknown colour {key!r}"
                )
            self.roles[role] = self.colors[key]

    def __getitem__(self, key: str) -> str:
        if key in self.roles:
            return self.roles[key]
        if key in self.colors:
            return self.colors[key]
        raise KeyError(key)

    def get(self, key: str, default=None):
        try:
            return self[key]
        except KeyError:
            return default

    def tokens(self) -> dict:
        merged = dict(self.colors)
        merged.update(self.roles)
        return merged


def load_palettes(only: str | None) -> list[Palette]:
    found = []
    for path in sorted((HERE / "palettes").glob("*.json")):
        if only and path.stem != only:
            continue
        found.append(Palette(json.loads(path.read_text(encoding="utf-8"))))
    if not found:
        raise SystemExit(f"no palette matched {only!r} in {HERE / 'palettes'}")
    return found


# --------------------------------------------------------------------------- #
# svg helpers
# --------------------------------------------------------------------------- #
def local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_style(raw: str) -> dict:
    out = {}
    for decl in raw.split(";"):
        if ":" in decl:
            key, value = decl.split(":", 1)
            out[key.strip()] = value.strip()
    return out


def dump_style(style: dict) -> str:
    return ";".join(f"{k}:{v}" for k, v in style.items() if k in KEEP_STYLE)


def is_dark(value: str | None) -> bool:
    """True for LateNight's near-black outline ink.

    The halo layers are not drawn in a single colour but in a scatter of
    almost-blacks (#060202, #0d0505, #1a1a1a, ...), so match on brightness
    rather than by listing them.  A named colour that is not in DARK_INK falls
    through as not dark, which is what we want: an outline is always a hex.
    """
    if value is None:
        return False
    value = value.strip().lower()
    if value in DARK_INK:
        return True
    if not value.startswith("#"):
        return False
    digits = value[1:]
    if len(digits) == 3:
        digits = "".join(c * 2 for c in digits)
    if len(digits) != 6:
        return False
    try:
        channels = [int(digits[i : i + 2], 16) for i in (0, 2, 4)]
    except ValueError:
        return False
    return max(channels) <= 0x20


def resolve_source(path: Path) -> Path:
    """Follow a git symlink that a Windows checkout materialised as text.

    Two of LateNight's glyphs are mode-120000 entries; without core.symlinks
    they land on disk as a one-line file holding the target name.
    """
    try:
        head = path.read_bytes()[:1]
    except OSError:
        return path
    if head == b"<":
        return path
    target = path.read_text(encoding="utf-8").strip()
    if target and "\n" not in target:
        candidate = (path.parent / target).resolve()
        if candidate.is_file():
            return candidate
    return path


def read_size(path: Path) -> tuple[str, str, str | None]:
    """Return (width, height, viewBox) of an SVG as authored."""
    root = ET.parse(resolve_source(path)).getroot()
    width = root.get("width", "16")
    height = root.get("height", "16")
    return width, height, root.get("viewBox")


DRAWABLE = {
    "path",
    "rect",
    "circle",
    "ellipse",
    "polygon",
    "polyline",
    "line",
    "text",
    "image",
    "use",
}


def count_drawables(root: ET.Element) -> int:
    return sum(1 for el in root.iter() if local(el.tag) in DRAWABLE)


def paints_in_colour(root: ET.Element) -> bool:
    """True if the glyph draws anything in something other than near-black.

    Hidden subtrees do not count.  Several LateNight glyphs park an unused
    alternative drawing behind ``display:none``, and flatten_svg removes those
    before it looks at a single colour -- counting them would decide a glyph
    paints in colour when everything it actually draws is black.
    """

    def visible(el: ET.Element) -> bool:
        style = parse_style(el.get("style", ""))
        return el.get("display") != "none" and style.get("display") != "none"

    def walk(el: ET.Element) -> bool:
        style = parse_style(el.get("style", ""))
        for attr in ("fill", "stroke", "stop-color"):
            value = el.get(attr, style.get(attr))
            if value is None or value == "none":
                continue
            if not is_dark(value):
                return True
        return any(walk(child) for child in el if visible(child))

    return visible(root) and walk(root)


def flatten_svg(
    path: Path,
    color: str,
    strip_dark: bool = True,
    tones: dict | None = None,
) -> str:
    """Repaint an SVG as a single-colour silhouette.

    Removes the black halo layers, gradient/filter machinery and every opacity,
    then paints what is left in ``color``.  Text is re-emitted in the monospace
    stack so word glyphs match the rest of the skin.

    ``strip_dark`` says what near-black ink means in this file.  Normally it is
    decoration -- LateNight outlines every glyph in it -- so a layer drawn only
    in near-black goes, and a near-black stroke sharing an element with a real
    fill is dropped.  In a glyph drawn *entirely* in near-black it is instead
    the drawing itself, and the caller passes False so it is repainted rather
    than removed.

    ``tones`` maps a source colour onto a specific palette colour, for the few
    glyphs that are diagrams rather than icons and whose colours therefore say
    something.  Anything it does not name still flattens to ``color``.
    """
    tones = tones or {}
    tree = ET.parse(resolve_source(path))
    root = tree.getroot()

    def visit(parent: ET.Element) -> None:
        for child in list(parent):
            tag = local(child.tag)
            if tag in ("defs", "metadata", "filter", "title", "desc"):
                parent.remove(child)
                continue
            style = parse_style(child.get("style", ""))
            if (
                child.get("display") == "none"
                or style.get("display") == "none"
            ):
                parent.remove(child)
                continue
            fill = child.get("fill", style.get("fill"))
            stroke = child.get("stroke", style.get("stroke"))
            # A pure halo layer: dark stroke, nothing real to fill.
            if (
                strip_dark
                and is_dark(stroke)
                and (fill is None or fill == "none" or is_dark(fill))
            ):
                parent.remove(child)
                continue
            recolor(child, style, color)
            visit(child)

    def recolor(el: ET.Element, style: dict, color: str) -> None:
        for attr in DROP_ATTRS:
            el.attrib.pop(attr, None)
            style.pop(attr, None)
        for holder, setter in (
            (el.attrib, el.set),
            (style, style.__setitem__),
        ):
            for attr in ("fill", "stroke"):
                value = holder.get(attr)
                if value is None:
                    continue
                if strip_dark and attr == "stroke" and is_dark(value):
                    # Halo riding on the same element as the real fill.
                    holder.pop("stroke", None)
                    holder.pop("stroke-width", None)
                    continue
                if value != "none":
                    setter(attr, tones.get(value.strip().lower(), color))
        if local(el.tag) in ("text", "tspan"):
            el.set("font-family", MONO)
            el.set("fill", color)
            el.attrib.pop("stroke", None)
            el.attrib.pop("stroke-width", None)
            el.attrib.pop("letter-spacing", None)
            el.attrib.pop("word-spacing", None)
            style.pop("stroke", None)
            style.pop("line-height", None)
            style["font-family"] = MONO
            style["fill"] = color
            # Word glyphs (SYNC, KEY, ...) go lowercase to match the button
            # labels in the XML; digits are unaffected.
            if el.text:
                el.text = el.text.lower()
        rendered = dump_style(style)
        if rendered:
            el.set("style", rendered)
        else:
            el.attrib.pop("style", None)

    visit(root)
    # Anything left without an explicit fill inherits from the root rather than
    # falling back to SVG's default black.
    root.set("fill", color)
    for junk in ("{http://www.w3.org/XML/1998/namespace}space", "id"):
        root.attrib.pop(junk, None)
    return ET.tostring(root, encoding="unicode")


def flatten_or_keep(path: Path, color: str, tones: dict | None = None) -> str:
    """flatten_svg, keeping the halo layers where they are the whole drawing.

    A handful of glyphs (``btn__undo_active``, ``btn__reverse_active``) are
    drawn *entirely* in near-black, because LateNight puts them on a bright
    active background -- the same reverse-video trick this skin uses.  Every
    stroke in them looks like a halo, so only strip halos from a glyph that
    also paints something in a real colour.  The emptiness check stays as a
    backstop for anything that slips through.
    """
    source = ET.parse(resolve_source(path)).getroot()
    text = flatten_svg(
        path, color, strip_dark=paints_in_colour(source), tones=tones
    )
    if count_drawables(ET.fromstring(text)) == 0:
        if count_drawables(source) == 0:
            return text  # LateNight's intentional transparent dummy
        text = flatten_svg(path, color, strip_dark=False, tones=tones)
    check_paints(path, ET.fromstring(text))
    return text


def check_paints(path: Path, root: ET.Element) -> None:
    """Fail on a glyph that kept its shapes but lost every visible paint.

    Inheritance makes this easy to miss: a group can set ``fill:none`` for its
    children and carry the only stroke, so dropping that stroke leaves shapes
    that draw nothing at all.  The file still looks like a drawing in a diff --
    the loop marker on the waveform overview was blank this way -- so check it
    here rather than trusting the eye.
    """

    def walk(el: ET.Element, fill: str, stroke: str) -> bool:
        style = parse_style(el.get("style", ""))
        fill = el.get("fill", style.get("fill", fill))
        stroke = el.get("stroke", style.get("stroke", stroke))
        if local(el.tag) in DRAWABLE and (fill != "none" or stroke != "none"):
            return True
        return any(walk(child, fill, stroke) for child in el)

    # SVG's own defaults, before the flattened root sets its fill.
    if count_drawables(root) and not walk(root, "black", "none"):
        raise SystemExit(
            f"{path.name}: flattened to shapes that paint nothing."
            " A fill:none was left with no stroke to draw it."
        )


def svg(width, height, body: str = "", view_box: str | None = None) -> str:
    box = f' viewBox="{view_box}"' if view_box else ""
    return (
        f'<svg xmlns="{SVG_NS}" width="{width}" height="{height}"{box} '
        f'version="1.1">{body}</svg>'
    )


def transparent(width, height, view_box=None) -> str:
    return svg(width, height, "", view_box)


def write(path: Path, text: str) -> None:
    # Exactly one trailing newline. Content that already ends in one would
    # otherwise gain a blank final line, which the repo's end-of-file hook
    # strips on commit -- leaving every generated file permanently "modified"
    # against a fresh run.
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text.rstrip("\n") + "\n", encoding="utf-8")


# --------------------------------------------------------------------------- #
# categorisation
# --------------------------------------------------------------------------- #
# btn__<name>.svg  -> glyph, drawn by QSS `image:` over a QSS-drawn frame.
# btn_<type>_<size>.svg -> frame, supplied by the XML button templates.
GLYPH_RE = re.compile(r"^btn__|^btn_colorpicker")


def is_frame(name: str) -> bool:
    return name.startswith("btn_") and not GLYPH_RE.match(name)


def glyph_color(name: str, palette: Palette) -> str:
    """Pick the palette colour a glyph should be painted in.

    Active/latched buttons get a filled accent background from QSS, so their
    glyph has to be dark to stay readable -- the reverse-video trick.
    LateNight already ships the variants we need under `_active` / `_set` /
    `_dark` names, so the mapping is purely by filename.
    """
    stem = name[:-4] if name.endswith(".svg") else name
    if re.search(r"_(active|set|dark)(_|$)", stem) or stem.endswith(
        ("_active", "_set", "_dark")
    ):
        return palette["bg"]
    if "_disabled" in stem:
        return palette["fg_faint"]
    return palette["fg"]


def glyph_tones(name: str, palette: Palette) -> dict:
    """Source-colour -> palette-colour map for the few glyphs in TWO_TONE."""
    stem = name[:-4] if name.endswith(".svg") else name
    return {
        source: palette[token]
        for source, token in TWO_TONE.get(stem, {}).items()
    }


# --------------------------------------------------------------------------- #
# generated primitives
# --------------------------------------------------------------------------- #
def num(value: str, default: float = 16.0) -> float:
    """Strip units off an SVG length."""
    return float(re.sub(r"[^0-9.]", "", value) or default)


def rect(x, y, w, h, fill, stroke: str | None = None) -> str:
    edge = f' stroke="{stroke}" stroke-width="1"' if stroke else ""
    return (
        f'<rect x="{x}" y="{y}" width="{w}" '
        f'height="{h}" fill="{fill}"{edge}/>'
    )


def circle(cx, cy, r, fill: str, stroke: str | None = None) -> str:
    edge = f' stroke="{stroke}" stroke-width="1"' if stroke else ""
    return f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{fill}"{edge}/>'


def gen_knob_indicator(
    width: str, height: str, view_box: str | None, color: str
) -> str:
    """A block needle, pointing up from the knob centre.

    The dial and value arc are drawn by KnobComposed from ArcBgColor and
    ArcColor, so the only picture a terminal knob needs is the pointer.
    """
    w, h = num(width), num(height)
    thickness = max(2.0, round(w * 0.06, 2))
    x = round((w - thickness) / 2, 2)
    body = rect(x, round(h * 0.16, 2), thickness, round(h * 0.27, 2), color)
    return svg(width, height, body, view_box)


def gen_slider_groove(
    width: str, height: str, view_box: str | None, palette: Palette
) -> str:
    """A 1px track with tick marks.

    The level bar itself is drawn by the widget from BarColor.
    """
    w, h = num(width), num(height)
    border = palette["border"]
    faint = palette["fg_faint"]
    parts = []
    if h >= w:  # vertical fader
        parts.append(
            rect(round(w / 2, 2) - 0.5, 2, 1, round(h - 4, 2), border)
        )
        # Ticks at 0/25/50/75/100%, the centre one wider.
        for i in range(5):
            y = round(2 + (h - 4) * i / 4, 2)
            run = round(w * 0.42 if i == 2 else w * 0.26, 2)
            tick = border if i == 2 else faint
            parts.append(rect(round((w - run) / 2, 2), y, run, 1, tick))
    else:  # crossfader
        parts.append(
            rect(2, round(h / 2, 2) - 0.5, round(w - 4, 2), 1, border)
        )
        for i in range(5):
            x = round(2 + (w - 4) * i / 4, 2)
            run = round(h * 0.42 if i == 2 else h * 0.26, 2)
            tick = border if i == 2 else faint
            parts.append(rect(x, round((h - run) / 2, 2), 1, run, tick))
    return svg(width, height, "".join(parts), view_box)


def gen_slider_handle(
    width: str, height: str, view_box: str | None, palette: Palette
) -> str:
    """A solid block, like a terminal cursor sitting on the track.

    The accent dash runs across the direction of travel so it reads as a
    position marker: horizontal on a vertical fader, vertical on the
    crossfader.
    """
    w, h = num(width), num(height)
    # A full-brightness outline makes the cap heavier than anything around
    # it; the accent dash is what the eye needs to read the position.
    edge = palette["fg_dim"]
    face = palette["raised_hi"]
    accent = palette["accent"]
    if h >= w:  # crossfader: tall block, dash runs vertically
        pad_x = round(w * 0.18, 2)
        body_w = round(w - 2 * pad_x, 2)
        top = round(h * 0.16, 2)
        body_h = round(h * 0.68, 2)
        body = rect(pad_x, top, body_w, body_h, face, edge) + rect(
            round(w / 2 - 0.5, 2), top, 1, body_h, accent
        )
    else:  # fader cap: wide block, dash runs horizontally
        pad_y = round(h * 0.2, 2)
        body_h = round(h - 2 * pad_y, 2)
        body_w = round(w - 2, 2)
        body = rect(1, pad_y, body_w, body_h, face, edge) + rect(
            1, round(h / 2 - 0.5, 2), body_w, 1, accent
        )
    return svg(width, height, body, view_box)


def vu_segment_color(frac: float, palette: Palette, active: bool) -> str:
    if not active:
        return palette["border"]
    if frac > 0.86:
        return palette["vu_high"]
    if frac > 0.62:
        return palette["vu_mid"]
    return palette["vu_low"]


def gen_vu(width: int, height: int, palette: Palette, active: bool) -> str:
    """A segmented block meter, low -> high from bottom to top."""
    seg, gap = 3, 1
    parts = []
    y = height
    while y - seg >= 0:
        y -= seg
        color = vu_segment_color(1.0 - (y / height), palette, active)
        parts.append(rect(0, y, width, seg, color))
        y -= gap
    return svg(width, height, "".join(parts))


def gen_hbar(width: int, height: int, palette: Palette, active: bool) -> str:
    """A horizontal segmented meter: the latency bar in the toolbar."""
    seg, gap = 3, 1
    parts = []
    x = 0
    while x + seg <= width:
        color = vu_segment_color((x + seg) / width, palette, active)
        parts.append(rect(x, 0, seg, height, color))
        x += seg + gap
    return svg(width, height, "".join(parts))


def gen_vu_clip(
    width: int, height: int, palette: Palette, active: bool
) -> str:
    color = palette["vu_high"] if active else palette["border"]
    return svg(width, height, rect(0, 0, width, height, color))


def gen_splitter(
    width: str, height: str, palette: Palette, pressed: bool
) -> str:
    w, h = num(width, 8), num(height, 8)
    color = palette["border_lit"] if pressed else palette["border"]
    if h >= w:  # vertical handle -> a dotted column
        cx = round(w / 2 - 0.5, 2)
        parts = [rect(cx, y, 1, 2, color) for y in range(2, int(h) - 1, 4)]
    else:
        cy = round(h / 2 - 0.5, 2)
        parts = [rect(x, cy, 2, 1, color) for x in range(2, int(w) - 1, 4)]
    return svg(width, height, "".join(parts))


def gen_branch(
    width: str, height: str, palette: Palette, open_: bool, selected: bool
) -> str:
    """Tree disclosure marks as terminal triangles."""
    w, h = num(width, 12), num(height, 12)
    color = palette["bg"] if selected else palette["fg_dim"]
    cx, cy = w / 2, h / 2
    r = min(w, h) * 0.26
    if open_:  # pointing down
        pts = (
            f"{cx - r},{cy - r * 0.6} {cx + r},{cy - r * 0.6} "
            f"{cx},{cy + r * 0.8}"
        )
    else:  # pointing right
        pts = (
            f"{cx - r * 0.6},{cy - r} {cx - r * 0.6},{cy + r} "
            f"{cx + r * 0.8},{cy}"
        )
    return svg(width, height, f'<polygon points="{pts}" fill="{color}"/>')


def gen_progressbar(
    width: int, height: int, palette: Palette, chunk: bool
) -> str:
    color = palette["accent"] if chunk else palette["border"]
    return svg(width, height, rect(0, 0, width, height, color))


def ascii_logo_rows() -> list[str]:
    """The wordmark as one string per row, letters joined with a gutter.

    Figlet's glyphs butt right up against each other, which leaves the M
    touching the I and the three X's merging into a fence.  A column of space
    between letters costs four characters and makes the word legible.
    """
    return [
        " ".join(letter[row] for letter in ASCII_LOGO)
        for row in range(len(ASCII_LOGO[0]))
    ]


def ascii_logo_size() -> tuple[int, int]:
    """Pixel size of the character-art wordmark, for the launch stylesheet."""
    rows = ascii_logo_rows()
    return (
        round(max(len(row) for row in rows) * ASCII_LOGO_CELL_W),
        round(len(rows) * ASCII_LOGO_CELL_H),
    )


def gen_ascii_logo(palette: Palette) -> str:
    """The Mixxx wordmark drawn as monospace character art.

    Every character is centred in a cell of its own rather than being left to
    the font's advance width.  Character art only reads if the columns line up,
    and the advance differs between the faces in the monospace stack -- 0.55em
    for Consolas against 0.60em for the rest -- so a row laid out as one string
    would drift out of register on whichever platform did not set the spacing.
    """
    rows = ascii_logo_rows()
    width, height = ascii_logo_size()
    color = palette["fg_hi"]
    parts = []
    for row_index, row in enumerate(rows):
        # 0.78 of the line box puts the baseline where a terminal puts it.
        y = (row_index + 0.78) * ASCII_LOGO_CELL_H
        for column, char in enumerate(row):
            if char == " ":
                continue
            char = (
                char.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;")
            )
            x = (column + 0.5) * ASCII_LOGO_CELL_W
            parts.append(
                f'<text x="{x:.1f}" y="{y:.1f}" fill="{color}"'
                f' font-family="{MONO}"'
                f' font-size="{ASCII_LOGO_FONT:.0f}"'
                f' text-anchor="middle">{char}</text>'
            )
    body = "\n  " + "\n  ".join(parts) + "\n"
    return svg(width, height, body, view_box=f"0 0 {width} {height}")


def gen_spinny_bg(
    width: str, height: str, view_box: str | None, palette: Palette
) -> str:
    """Concentric rules instead of a photographic platter."""
    w, h = num(width, 100), num(height, 100)
    cx, cy = w / 2, h / 2
    outer = min(w, h) / 2 - 1
    parts = [circle(cx, cy, outer, palette["sunken"])]
    rings = (
        (1.0, palette["border"]),
        (0.66, palette["fg_faint"]),
        (0.33, palette["fg_faint"]),
    )
    for frac, color in rings:
        parts.append(circle(cx, cy, round(outer * frac, 2), "none", color))
    parts.append(circle(cx, cy, 2, palette["border_lit"]))
    return svg(width, height, "".join(parts), view_box)


def gen_spinny_indicator(
    width: str, height: str, view_box: str | None, color: str
) -> str:
    w, h = num(width, 100), num(height, 100)
    outer = min(w, h) / 2 - 1
    body = rect(
        round(w / 2 - 1, 2),
        round(h / 2 - outer, 2),
        2,
        round(outer * 0.92, 2),
        color,
    )
    return svg(width, height, body, view_box)


def gen_spinny_mask(
    width: str, height: str, view_box: str | None, palette: Palette
) -> str:
    """Mask the platter's corners and draw a crisp ring around it.

    LateNight builds this from a translucent black rect; a terminal has no
    translucency, so paint the outside opaque and rule the edge in 1px.
    """
    w, h = num(width, 100), num(height, 100)
    cx, cy = w / 2, h / 2
    r = min(w, h) / 2 - 1
    # Even-odd fill: outer rect minus the circle, so only corners paint.
    arc = f"A{r} {r} 0 1 0"
    body = (
        f'<path fill="{palette["panel"]}" fill-rule="evenodd" '
        f'd="M0 0H{w}V{h}H0Z '
        f'M{cx} {cy - r} {arc} {cx} {cy + r} {arc} {cx} {cy - r}Z"/>'
        + circle(cx, cy, r, "none", palette["border"])
    )
    return svg(width, height, body, view_box)


def gen_cover_default(
    width: str, height: str, view_box: str | None, palette: Palette
) -> str:
    w, h = num(width, 100), num(height, 100)
    size = round(min(w, h) * 0.34, 1)
    body = rect(
        0.5, 0.5, w - 1, h - 1, palette["sunken"], palette["border"]
    ) + (
        f'<text x="{round(w / 2, 2)}" '
        f'y="{round(h / 2 + size * 0.36, 2)}" '
        f'font-family="{MONO}" font-size="{size}" '
        f'fill="{palette["fg_faint"]}" text-anchor="middle">~/</text>'
    )
    return svg(width, height, body, view_box)


# --------------------------------------------------------------------------- #
KNOB_COLOR_ROLE = {
    "grey": "eq",
    "green": "accent",
    "blue": "sync",
    "orange": "gain",
    "red": "rec",
}
SLIDER_HANDLE_RE = re.compile(r"^knob_")


def build_assets(
    palette: Palette, report: bool = False, default: bool = False
) -> dict:
    out = SKIN / palette.dir
    stats = {
        "glyph": 0,
        "frame": 0,
        "knob": 0,
        "slider": 0,
        "style": 0,
        "library": 0,
    }
    if not report and out.exists():
        shutil.rmtree(out)
    if not report and default:
        # library/ holds a subdirectory per scheme, so it can only be cleared
        # on the first palette of a run -- clearing it on each one would leave
        # nothing but the last scheme's icons behind.
        shutil.rmtree(SKIN / "library", ignore_errors=True)
    # ---- buttons -------------------------------------------------------- #
    for src in sorted((REF / "buttons").glob("*.svg")):
        dst = out / "buttons" / src.name
        if is_frame(src.name):
            # Frames become no-ops: QSS draws every button border and fill, so
            # they stay 1px crisp instead of stretching with the widget.
            stats["frame"] += 1
            if not report:
                width, height, view_box = read_size(src)
                write(dst, transparent(width, height, view_box))
        else:
            stats["glyph"] += 1
            if not report:
                write(
                    dst,
                    flatten_or_keep(
                        src,
                        glyph_color(src.name, palette),
                        glyph_tones(src.name, palette),
                    ),
                )
    # ---- knobs ---------------------------------------------------------- #
    for src in sorted((REF / "knobs").glob("*.svg")):
        dst = out / "knobs" / src.name
        stats["knob"] += 1
        if report:
            continue
        width, height, view_box = read_size(src)
        if src.name.startswith("knob_bg_"):
            # The value arc and the QSS border are the whole bezel now.
            write(dst, transparent(width, height, view_box))
        else:
            match = re.match(r"knob_indicator_\w+?_(\w+)\.svg$", src.name)
            role = KNOB_COLOR_ROLE.get(match.group(1) if match else "", "fg")
            write(
                dst,
                gen_knob_indicator(
                    width, height, view_box, palette.get(role, palette["fg"])
                ),
            )
    # ---- sliders -------------------------------------------------------- #
    for src in sorted((REF / "sliders").glob("*.svg")):
        dst = out / "sliders" / src.name
        stats["slider"] += 1
        if report:
            continue
        width, height, view_box = read_size(src)
        if SLIDER_HANDLE_RE.match(src.name):
            write(dst, gen_slider_handle(width, height, view_box, palette))
        else:
            write(dst, gen_slider_groove(width, height, view_box, palette))
    # ---- style ---------------------------------------------------------- #
    stats["style"] = build_style_assets(palette, out, report)

    # ---- library sidebar icons ------------------------------------------- #
    # These are Qt resources compiled into the binary, so they cannot be
    # restyled from a stylesheet. Ship flattened copies; LibraryFeature picks
    # them up and falls back to its own when absent.
    #
    # One directory per scheme, named after the scheme rather than after its
    # asset directory, because the scheme name is what LibraryFeature has to
    # hand: it reads [Config]/Scheme from the settings, having no way to map a
    # name onto a directory without parsing skin.xml. The copy directly under
    # library/ is the fallback for a settings file that names no scheme yet,
    # which is every first run.
    for name in LIBRARY_ICONS:
        src = REF_LIBRARY / f"ic_library_{name}.svg"
        if not src.is_file():
            continue
        stats["library"] += 1
        if not report:
            icon = flatten_or_keep(src, palette["fg_dim"])
            write(SKIN / "library" / palette.name / src.name, icon)
            if default:
                write(SKIN / "library" / src.name, icon)
    return stats


# VU meter geometry, carried over from the PNGs LateNight ships.
VU_SIZES = {
    ("deck", "level"): (6, 81),
    ("deck", "clipping"): (6, 11),
    ("micaux", "level"): (6, 41),
    ("micaux", "clipping"): (6, 9),
    ("preview", "level"): (6, 41),
    ("preview", "clipping"): (6, 9),
    ("sampler", "level"): (6, 49),
    ("sampler", "clipping"): (6, 9),
}
# PathBack variants the vumeter templates ask for, by VuColor.
VU_VARIANTS = {
    "deck": ("dark", "light"),
    "micaux": ("",),
    "preview": ("",),
    "sampler": ("",),
}


def build_style_assets(palette: Palette, out: Path, report: bool) -> int:
    count = 0
    src_dir = REF / "style"
    dst_dir = out / "style"
    generated: set[str] = set()

    def emit(name: str, text: str) -> None:
        nonlocal count
        generated.add(name)
        count += 1
        if not report:
            write(dst_dir / name, text)

    # VU meters: SVG rather than PNG so they follow the palette.  The vumeter
    # templates are pointed at .svg by the skin's XML.
    for (size, kind), (width, height) in VU_SIZES.items():
        maker = gen_vu if kind == "level" else gen_vu_clip
        emit(
            f"vu_{size}_{kind}_active.svg", maker(width, height, palette, True)
        )
        for variant in VU_VARIANTS[size]:
            emit(
                f"vu_{size}_{kind}_bg_{variant}.svg",
                maker(width, height, palette, False),
            )
    # Toolbar latency meter.  The XML asks for these by two different names
    # (toolbar.xml and mixer/vumeter_latency.xml), so emit both.
    for stem in ("latency", "vumeter_latency"):
        emit(f"{stem}_bg.svg", gen_hbar(59, 5, palette, False))
        emit(f"{stem}_over.svg", gen_hbar(59, 5, palette, True))
    # Splitters, tree marks, progress bar, platter, cover placeholder.
    for name, pressed in (
        ("splitter_handle_horizontal.svg", False),
        ("splitter_handle_horizontal_pressed.svg", True),
        ("splitter_handle_vertical.svg", False),
        ("splitter_handle_vertical_pressed.svg", True),
        ("library_splitter_handle_unchecked.svg", False),
    ):
        width, height, _ = read_size(src_dir / name)
        emit(name, gen_splitter(width, height, palette, pressed))
    for name, open_, selected in (
        ("library_branch_closed.svg", False, False),
        ("library_branch_closed_selected.svg", False, True),
        ("library_branch_open.svg", True, False),
        ("library_branch_open_selected.svg", True, True),
    ):
        width, height, _ = read_size(src_dir / name)
        emit(name, gen_branch(width, height, palette, open_, selected))
    # Only the launch screen uses these, so size them to its wordmark.
    bar_width = ascii_logo_size()[0] + 4
    emit("progressbar.svg", gen_progressbar(bar_width, 5, palette, True))
    emit("progressbar_bg.svg", gen_progressbar(bar_width, 5, palette, False))
    # The launch wordmark. LateNight's is a vector logotype sitting on a page
    # plate that flattening cannot tell from the drawing, so it came out as a
    # filled square; a terminal would spell its name out in characters anyway.
    emit("mixxx_logo.svg", gen_ascii_logo(palette))
    for name in ("spinny_bg.svg",):
        width, height, view_box = read_size(src_dir / name)
        emit(name, gen_spinny_bg(width, height, view_box, palette))
    for name, role in (
        ("spinny_indicator.svg", "accent"),
        ("spinny_indicator_ghost.svg", "fg_faint"),
    ):
        width, height, view_box = read_size(src_dir / name)
        emit(
            name,
            gen_spinny_indicator(
                width, height, view_box, palette.get(role, palette["fg"])
            ),
        )
    for name in ("spinny_mask_12.svg", "spinny_mask_34.svg"):
        width, height, view_box = read_size(src_dir / name)
        emit(name, gen_spinny_mask(width, height, view_box, palette))
    width, height, view_box = read_size(src_dir / "cover_default.svg")
    emit(
        "cover_default.svg",
        gen_cover_default(width, height, view_box, palette),
    )
    # Everything else in style/ is an icon: flatten it like the button glyphs.
    for src in sorted(src_dir.glob("*.svg")):
        if src.name in generated:
            continue
        count += 1
        if not report:
            write(
                dst_dir / src.name,
                flatten_or_keep(
                    src,
                    glyph_color(src.name, palette),
                    glyph_tones(src.name, palette),
                ),
            )
    # Waveform mark icons (intro/outro/loop/jump). LateNight ships these only
    # under classic/, and points at them even from the PaleMoon scheme.
    for src in sorted((REF_CLASSIC / "style").glob("mark_*.svg")):
        count += 1
        if not report:
            write(dst_dir / src.name, flatten_or_keep(src, palette["fg_hi"]))
    # Battery icons in the toolbar.
    for src in sorted((src_dir / "batt").glob("*.svg")):
        count += 1
        if not report:
            write(
                dst_dir / "batt" / src.name,
                flatten_or_keep(src, palette["fg_dim"]),
            )
    return count


# --------------------------------------------------------------------------- #
# stylesheet
# --------------------------------------------------------------------------- #
# LateNight's scheme stylesheet is the donor for widget *scoping*: 612
# already name the right widget for every colour and pin all 280 glyph icons to
# their buttons.  We keep that mapping and replace the look: flatten
# square the corners, drop the border-images now that the frame SVGs are blank,
# and translate every colour literal to a palette token.  The terminal design
# proper then lands in templates/terminal.qss.in, appended last so it wins.
DONOR_QSS = REPO / "res" / "skins" / "LateNight" / "style_palemoon.qss"
GRADIENT_RE = re.compile(r"q(?:linear|radial|conical)gradient\s*\(", re.I)
HEX_RE = re.compile(r"#[0-9a-fA-F]{3,8}\b")
RGBA_RE = re.compile(
    r"rgba?\(\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*"
    r"(?:,\s*([0-9.]+)\s*)?\)"
)
RADIUS_RE = re.compile(r"(border(?:-[a-z]+)?-radius\s*:\s*)([^;]+)")
BORDER_IMAGE_RE = re.compile(r"border-image\s*:\s*url\([^)]*\)[^;]*")


def load_color_map() -> tuple[dict, dict]:
    data = json.loads((HERE / "donor_colors.json").read_text(encoding="utf-8"))
    return data["hex"], data["rgba"]


def match_parens(text: str, open_at: int) -> int:
    """Index just past the ')' closing the '(' at open_at."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def flatten_gradients(decls: str) -> str:
    """Collapse a gradient to its first stop -- terminals do not shade."""
    while True:
        match = GRADIENT_RE.search(decls)
        if not match:
            return decls
        end = match_parens(decls, match.end() - 1)
        inner = decls[match.end() : end - 1]
        stops = HEX_RE.findall(inner) or RGBA_RE.findall(inner)
        replacement = (
            stops[0] if stops and isinstance(stops[0], str) else "#000000"
        )
        decls = decls[: match.start()] + replacement + decls[end:]


def map_colors(
    decls: str, palette: Palette, hex_map: dict, rgba_map: dict, unknown: set
) -> str:
    def sub_hex(match: re.Match) -> str:
        raw = match.group(0).lower()
        token = hex_map.get(raw)
        if token is None:
            unknown.add(raw)
            return match.group(0)
        return palette[token]

    def sub_rgba(match: re.Match) -> str:
        triple = ",".join(match.group(i) for i in (1, 2, 3))
        token = rgba_map.get(triple)
        if token is None:
            unknown.add(match.group(0))
            return match.group(0)
        hex_value = palette[token].lstrip("#")
        red, green, blue = (int(hex_value[i : i + 2], 16) for i in (0, 2, 4))
        alpha = match.group(4)
        if alpha is None:
            return f"rgb({red}, {green}, {blue})"
        return f"rgba({red}, {green}, {blue}, {alpha})"

    return HEX_RE.sub(sub_hex, RGBA_RE.sub(sub_rgba, decls))


BORDER_SIDE_RE = re.compile(r"border-(top|right|bottom|left)\s*:\s*([^;]+)")


def luminance(color: str) -> float:
    raw = color.lstrip("#")
    if len(raw) == 3:
        raw = "".join(ch * 2 for ch in raw)
    if len(raw) < 6:
        return 0.0
    r, g, b = (int(raw[i : i + 2], 16) / 255 for i in (0, 2, 4))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def flatten_borders(decls: str) -> str:
    """Give a widget one edge colour instead of four.

    LateNight shades the four sides of a box differently to fake a bevel --
    a lit top, a dark bottom. A terminal draws one flat rule, so pick the
    lightest of the sides the rule already set and use it on all of them.
    Only colours change: which sides exist is left alone, because a border
    consumes space in Qt's box model and the layout is sized around it.
    """
    found = BORDER_SIDE_RE.findall(decls)
    colors = [
        value.split()[-1]
        for _, value in found
        if "none" not in value and value.split()[-1].startswith("#")
    ]
    if len(set(colors)) < 2:
        return decls
    lightest = max(set(colors), key=luminance)

    def repaint(match: re.Match) -> str:
        value = match.group(2).strip()
        if "none" in value or not value.split()[-1].startswith("#"):
            return match.group(0)
        parts = value.split()
        parts[-1] = lightest
        return f"border-{match.group(1)}: {' '.join(parts)}"

    return BORDER_SIDE_RE.sub(repaint, decls)


def transform_donor(text: str, palette: Palette) -> tuple[str, set]:
    hex_map, rgba_map = load_color_map()
    unknown: set = set()
    # Comments contain braces, so park them before splitting on declarations.
    comments: list[str] = []

    def park(match: re.Match) -> str:
        comments.append(match.group(0))
        return f"@@qsscomment{len(comments) - 1}@@"

    text = re.sub(r"/\*.*?\*/", park, text, flags=re.S)
    text = text.replace(
        "skins:LateNight/palemoon/", f"skins:Terminal/{palette.dir}/"
    )
    out: list[str] = []
    depth = 0
    for chunk in re.split(r"([{}])", text):
        if chunk == "{":
            depth += 1
            out.append(chunk)
        elif chunk == "}":
            depth -= 1
            out.append(chunk)
        elif depth > 0:
            decls = flatten_gradients(chunk)
            decls = map_colors(decls, palette, hex_map, rgba_map, unknown)
            decls = RADIUS_RE.sub(r"\g<1>0px", decls)
            decls = flatten_borders(decls)
            # The frame SVGs are blank now, and a border-image suppresses the
            # real border -- so drop it and let the override layer draw a rule.
            decls = BORDER_IMAGE_RE.sub("border-image: none", decls)
            out.append(decls)
        else:
            out.append(chunk)
    result = "".join(out)
    result = re.sub(
        r"@@qsscomment(\d+)@@", lambda m: comments[int(m.group(1))], result
    )
    return result, unknown


def render_template(
    path: Path, palette: Palette, extra: dict | None = None
) -> str:
    tokens = palette.tokens()
    tokens.update(extra or {})
    missing: set = set()

    def sub(match: re.Match) -> str:
        key = match.group(1).strip()
        if key in tokens:
            return tokens[key]
        missing.add(key)
        return match.group(0)

    text = re.sub(r"\{\{([^}]+)\}\}", sub, path.read_text(encoding="utf-8"))
    if missing:
        raise SystemExit(
            f"{path.name}: unknown palette tokens {sorted(missing)}"
        )
    return text


def build_qss(palette: Palette, report: bool = False) -> set:
    donor, unknown = transform_donor(
        DONOR_QSS.read_text(encoding="utf-8"), palette
    )
    overrides = render_template(
        HERE / "templates" / "terminal.qss.in",
        palette,
        {
            "scheme_name": palette.name,
            "scheme_dir": palette.dir,
            "hide_track_color": "true" if palette.mono else "false",
        },
    )
    header = f"""/* Terminal skin -- colour scheme: {palette.name}
 *
 * GENERATED by tools/terminal_skin/generate.py -- do not edit.
 * Edit tools/terminal_skin/palettes/{palette.dir}.json (colours),
 * tools/terminal_skin/templates/terminal.qss.in (the terminal design), or
 * tools/terminal_skin/donor_colors.json (donor colour -> token), then rerun.
 *
 * {palette.description}
 *
 * Widget scoping and icon assignment derive from LateNight's PaleMoon
 * stylesheet (CC-BY-SA 3.0, jus / owilliams / ronso0).
 */
"""
    if not report:
        write(
            SKIN / f"style_{palette.dir}.qss",
            header
            + "/* ---- inherited widget scoping, repainted ---- */\n"
            + donor
            + "\n\n/* ---- terminal design layer ---- */\n"
            + overrides,
        )
    return unknown


SKIN_XML = SKIN / "skin.xml"
SCHEMES_BEGIN = "  <!-- TERMINAL-SCHEMES-BEGIN"
SCHEMES_END = "  <!-- TERMINAL-SCHEMES-END -->"


def xml_comment_safe(text: str) -> str:
    """Make text legal inside an XML comment.

    A comment may not contain "--" anywhere, which is easy to hit in prose
    written for a palette description -- exactly the sort of thing that
    silently makes the whole skin unparsable and sends Mixxx back to its
    default skin.
    """
    while "--" in text:
        text = text.replace("--", "-")
    return text.rstrip("-")


def scheme_block(palette: Palette) -> str:
    logo_width, logo_height = ascii_logo_size()
    return render_template(
        HERE / "templates" / "scheme.xml.in",
        palette,
        {
            "scheme_name": palette.name,
            "scheme_dir": palette.dir,
            "scheme_description": xml_comment_safe(palette.description),
            "use_cue_color": "false" if palette.mono else "true",
            "track_color_opacity": "0" if palette.mono else "0.175",
            # The launch stylesheet pins the label to the wordmark's own size,
            # so it is never scaled and the character cells stay square.
            "logo_width": str(logo_width),
            "logo_height": str(logo_height),
            "logo_bar_width": str(logo_width + 4),
        },
    )


def build_skin_xml(palettes: list[Palette], report: bool = False) -> None:
    """Rewrite the <Schemes> block so every palette shows up in Preferences.

    Mixxx offers one entry per <Scheme> under Interface -> Color scheme, so the
    palette list and the picker stay in step without any hand editing.
    """
    text = SKIN_XML.read_text(encoding="utf-8")
    start = text.find(SCHEMES_BEGIN)
    end = text.find(SCHEMES_END)
    if start < 0 or end < 0:
        raise SystemExit(
            f"{SKIN_XML}: missing TERMINAL-SCHEMES markers"
            " -- cannot place schemes"
        )
    head = text[:start]
    tail = text[end + len(SCHEMES_END) :]
    newline = "\n"
    body = (
        SCHEMES_BEGIN
        + ": generated by tools/terminal_skin/generate.py, do not edit -->"
        + newline
        + newline.join(scheme_block(p) for p in palettes)
        + newline
        + SCHEMES_END
    )
    if not report:
        SKIN_XML.write_text(head + body + tail, encoding="utf-8")
        # An unparsable skin.xml does not raise anywhere visible: Mixxx logs
        # a debug line and quietly loads its default skin instead. Catch it
        # here, where it is obvious.
        try:
            ET.parse(SKIN_XML)
        except ET.ParseError as exc:
            raise SystemExit(f"{SKIN_XML} is not valid XML: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--palette", help="palette stem to build (default: all)"
    )
    parser.add_argument(
        "--report",
        action="store_true",
        help="categorise assets without writing",
    )
    args = parser.parse_args()
    if not REF.exists():
        raise SystemExit(f"geometry donor missing: {REF}")
    palettes = load_palettes(args.palette)
    for palette in palettes:
        # The first palette is the skin's default scheme, and the one
        # whose library icons sit at the fallback path.
        stats = build_assets(
            palette, args.report, default=palette is palettes[0]
        )
        unknown = build_qss(palette, args.report)
        where = "would write" if args.report else f"wrote {SKIN / palette.dir}"
        print(
            f"{palette.name}: {where} -- "
            + ", ".join(f"{n} {k}" for k, n in stats.items())
            + f", style_{palette.dir}.qss"
        )
        if unknown:
            print(
                f"  warning: {len(unknown)} donor colour(s) missing from "
                f"donor_colors.json: {', '.join(sorted(unknown))}"
            )
    # The scheme picker must list every palette, not just the one just built.
    build_skin_xml(load_palettes(None), args.report)
    print(
        f"{'would update' if args.report else 'updated'} "
        f"{SKIN_XML.name} <Schemes>"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
