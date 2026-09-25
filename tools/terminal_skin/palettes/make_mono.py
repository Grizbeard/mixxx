#!/usr/bin/env python3
"""Emit the three monochrome phosphor palettes for the Terminal skin.

Each is one hue at nine brightnesses.  Written as a script rather than by hand
so the ladders stay in step with one another and the hex is not fudged: the
three schemes are one design at three wavelengths, and hand-picked values
would drift apart.

Run this, then rebuild the schemes from the result:

    python tools/terminal_skin/palettes/make_mono.py
    python tools/terminal_skin/generate.py
"""

import colorsys
import json
from pathlib import Path

OUT = Path(__file__).resolve().parent

# (saturation, lightness) per rung.  The structural rungs -- block through
# border_lit -- are unlit phosphor: dark and less saturated.  The text
# rungs are the lit phosphor and run to full saturation.
LADDER_SATURATED = {
    "block": (0.60, 0.055),
    "block_hi": (0.55, 0.105),
    "border": (0.50, 0.175),
    "border_lit": (0.48, 0.270),
    "dim": (0.60, 0.300),
    "fg_faint": (0.55, 0.360),
    "fg_dim": (0.75, 0.480),
    "fg": (1.00, 0.600),
    "fg_hi": (1.00, 0.850),
    "select_bg": (0.50, 0.155),
}
# A white phosphor has no hue to speak of; keep a trace of one so it reads as
# glass and not as a greyscale mock-up.
LADDER_NEUTRAL = {
    "block": (0.12, 0.075),
    "block_hi": (0.10, 0.130),
    "border": (0.09, 0.215),
    "border_lit": (0.08, 0.330),
    "dim": (0.08, 0.330),
    "fg_faint": (0.07, 0.430),
    "fg_dim": (0.06, 0.600),
    "fg": (0.06, 0.860),
    "fg_hi": (0.04, 0.970),
    "select_bg": (0.10, 0.200),
}

SCHEMES = (
    {
        "name": "Green Phosphor",
        "dir": "green",
        "hue": 122,
        "ladder": LADDER_SATURATED,
        "fg_l": 0.600,
        "description": (
            "P1 green phosphor: one hue, separated by brightness alone, the"
            " way a monochrome terminal separated everything."
        ),
    },
    {
        "name": "Amber",
        "dir": "amber",
        "hue": 41,
        "ladder": LADDER_SATURATED,
        # Amber reads brighter than green at the same lightness, so the lit
        # rungs sit a little lower to keep the two schemes matched in weight.
        "fg_l": 0.520,
        "fg_hi_l": 0.800,
        "description": (
            "P3 amber phosphor: one hue, separated by brightness alone, the"
            " warm monochrome of a late-eighties terminal."
        ),
    },
    {
        "name": "White Phosphor",
        "dir": "white",
        "hue": 205,
        "ladder": LADDER_NEUTRAL,
        "fg_l": 0.900,
        "description": (
            "P4 white phosphor: one barely-blue white, separated by brightness"
            " alone, like a paper-white monitor."
        ),
    },
)

# Which rung each name takes.  The hue-named keys are aliases: the stylesheet
# donor map and one template reach for magenta/yellow/cyan/green by name, so
# every palette has to define them whatever its hue actually is.
ALIASES = {
    "magenta": "fg",
    "yellow": "fg",
    "cyan": "fg",
    "green": "fg",
    "magenta_dim": "dim",
    "yellow_dim": "dim",
    "green_dim": "dim",
    # Intro/outro markers on the waveform: recessive, but it still has to be
    # findable against a lit waveform.
    "cyan_dim": "fg_faint",
}

# Everything latched shares the one lit phosphor: with no second hue available,
# a per-control colour would only be a second brightness, and dim behind black
# text is the one thing that does not survive. The glyph says which control it
# is -- the same reasoning as the mode selectors.
ROLES = {
    "accent": "green",
    "accent_dim": "green_dim",
    "play": "green",
    "cue": "green",
    "loop": "green",
    "sync": "green",
    "fx": "green",
    "rec": "green",
    "key": "green",
    "vinyl": "green",
    "eq": "fg",
    "gain": "fg",
    # Waveform colours, not button fills, so brightness can carry deck
    # identity here without a contrast problem.
    "deck12": "fg",
    "deck34": "fg_dim",
    "vu_low": "fg_dim",
    "vu_mid": "fg",
    "vu_high": "fg_hi",
    "band_low": "fg_dim",
    "band_mid": "fg",
    "band_high": "fg_hi",
    "select": "select_bg",
    "select_text": "select_fg",
}


def hex_of(hue: int, sat: float, light: float) -> str:
    r, g, b = colorsys.hls_to_rgb(hue / 360.0, light, sat)
    return "#{:02x}{:02x}{:02x}".format(
        round(r * 255), round(g * 255), round(b * 255)
    )


for scheme in SCHEMES:
    hue = scheme["hue"]
    ladder = dict(scheme["ladder"])
    ladder["fg"] = (ladder["fg"][0], scheme["fg_l"])
    if "fg_hi_l" in scheme:
        ladder["fg_hi"] = (ladder["fg_hi"][0], scheme["fg_hi_l"])
    rungs = {name: hex_of(hue, s, l) for name, (s, l) in ladder.items()}

    colors = {
        "bg": "#000000",
        "panel": "#000000",
        "sunken": "#000000",
        "raised": "#000000",
        "raised_hi": rungs["block_hi"],
        "block": rungs["block"],
        "block_hi": rungs["block_hi"],
        "border": rungs["border"],
        "border_lit": rungs["border_lit"],
        "fg_faint": rungs["fg_faint"],
        "fg_dim": rungs["fg_dim"],
        "fg": rungs["fg"],
        "fg_hi": rungs["fg_hi"],
        "select_bg": rungs["select_bg"],
        "select_fg": rungs["fg_hi"],
        "played_overlay": "#dd000000",
    }
    for alias, rung in ALIASES.items():
        colors[alias] = rungs[rung]

    doc = {
        "_comment": (
            "Generated by make_mono.py in this directory. One hue at"
            " nine brightnesses. The magenta/yellow/cyan/green keys are"
            " aliases onto rungs of that one ramp, not separate hues: the"
            " stylesheet donor map and the intro/outro marker reach for"
            " them by"
            " name, so every palette has to define them."
        ),
        "mono": True,
        "name": scheme["name"],
        "dir": scheme["dir"],
        "description": scheme["description"],
        "colors": colors,
        "roles": dict(ROLES),
    }
    path = OUT / f"mono-{scheme['dir']}.json"
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {path.name}")
    for k in (
        "block",
        "border",
        "border_lit",
        "fg_faint",
        "fg_dim",
        "fg",
        "fg_hi",
    ):
        print(f"    {k:11} {colors[k]}")
