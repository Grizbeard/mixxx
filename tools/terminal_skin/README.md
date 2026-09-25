# Terminal skin generator

Builds the colour layer of `res/skins/Terminal` from a palette definition.

```bash
python tools/terminal_skin/generate.py             # all palettes
python tools/terminal_skin/generate.py --palette btop
python tools/terminal_skin/generate.py --report    # categorise, write nothing
```

Run it after editing anything in this directory. It rewrites generated files in
place, so committing the output is expected.

## Why generate anything

Mixxx skins carry colour in two places that cannot share a variable: a
stylesheet (Qt QSS has no variables) and a directory of image assets. LateNight
solves this by keeping two near-identical 3,000-line stylesheets and two
390-file asset directories, one per scheme. That is fine for two hand-drawn
themes and hopeless for "make the core colours adjustable, even monochrome".

So the palette is the single source of truth, and everything downstream is
derived from it:

```text
palettes/<name>.json          colours + role assignments
        |
        +-- res/skins/Terminal/<dir>/**        icon + primitive assets
        +-- res/skins/Terminal/style_<dir>.qss the scheme stylesheet
        +-- res/skins/Terminal/skin.xml        the <Schemes> block
```

A new colour scheme is one JSON file and one command. Nothing else is touched.

## Files

| Path | What it is |
| --- | --- |
| `palettes/*.json` | A scheme: `name`, `dir`, `description`, `colors`, `roles`. Only `.json` is picked up — `*.json.example` is parked. |
| `donor_colors.json` | Maps every colour literal in LateNight's PaleMoon stylesheet onto a palette token. Palette-independent. |
| `templates/terminal.qss.in` | The terminal design layer. Double-brace tokens resolve from the palette. **This is where the design lives.** |
| `templates/scheme.xml.in` | The `<Scheme>` block: waveform colours, knob arcs, fader bars, marker labels. |
| `generate.py` | The build. |

`colors` are raw hex; `roles` are indirection (`accent -> green`) so a template
can say `{{accent}}` and a monochrome palette can point every role at one hue.
Both names resolve as tokens, and an unknown token is a hard error rather than a
silently unstyled widget.

## What is generated, and how

**The stylesheet** is two layers concatenated, in this order:

1. *Inherited scoping, repainted.* LateNight's `style_palemoon.qss` names the
   right widget for every colour in the application and pins all 257 glyph icons
   to their buttons — months of work we do not need to redo. The generator keeps
   that mapping and replaces the look: gradients collapse to their first stop,
   every radius goes to `0px`, `border-image` declarations are dropped (their
   frame SVGs are blank now, and a border-image suppresses the real border), and
   every colour literal is translated through `donor_colors.json`.
2. *The terminal design layer*, from `templates/terminal.qss.in`. Mixxx appends
   the scheme stylesheet after the skin's own `style.qss`, and this block comes
   last, so on equal specificity it wins.

**The assets** come from `res/skins/LateNight/palemoon` — geometry only, never
appearance:

- *Icon glyphs* (`btn__*.svg`, and most of `style/`) are flattened: defs,
  gradients, filters and opacity are stripped, the black halo layer LateNight
  puts under every shape is removed, and what is left is painted in one palette
  colour. Glyphs named `_active` / `_set` / `_dark` are painted in the
  *background* colour, because their button gets a filled accent background —
  reverse video, the way a terminal shows a selected cell.
- *Button frames* (`btn_embedded_*`, 43 files) become blank SVGs. Their borders
  come from QSS instead, so they stay 1px crisp rather than stretching with the
  widget.
- *Knob bezels* become blank too. What you see on a knob is the value arc that
  `KnobComposed` draws from `ArcColor`/`ArcBgColor`, plus a generated block
  needle.
- *Slider grooves, VU meters, splitters, tree marks, the platter and its mask,
  the latency bar and the cover placeholder* are generated from scratch as
  terminal primitives — 1px rules, segmented blocks, square caps.

Two of LateNight's glyphs are git symlinks; on a Windows checkout without
`core.symlinks` they land on disk as a one-line text file, and `resolve_source`
follows them.

## Adding a scheme

Copy a palette, edit the colours, run the generator. It appears in
Preferences → Interface → Color scheme; the first `<Scheme>` in `skin.xml` is
the default, and palettes are emitted in filename order.

A verified monochrome palette ships as `palettes/mono-green.json.example` —
single hue, separated by brightness alone, every accent role pointed at the same
green. To enable it:

```bash
mv tools/terminal_skin/palettes/mono-green.json.example \
   tools/terminal_skin/palettes/mono-green.json
python tools/terminal_skin/generate.py
```

Optionally add `res/skins/Terminal/skin_preview_<SchemeNameWithoutSpaces>.png`
(1920x1080) so Preferences shows a thumbnail instead of the placeholder.

## Reviewing a change

`_localbuild/skinshot.ps1` launches Mixxx against the source `res/` tree with a
throwaway settings directory, sizes the window, screenshots it and reports any
asset or stylesheet warnings from the log. Skins are pure data, so no rebuild is
needed:

```powershell
_localbuild\skinshot.ps1 -Out shot.png -Tracks @("some.wav","other.wav")
_localbuild\skinshot.ps1 -Out shot.png -Skin LateNight -Scheme PaleMoon   # A/B
```

## Notes

**The palette is four hues and one grey.** The scheme is limited to the colours
in btop's own panels: magenta, yellow, cyan, green, on black. There is no red,
orange or blue -- `donor_colors.json` sends LateNight's warm colours to the
nearest hue that does exist. Every neutral is one sage grey, a single hue and
saturation stepped only by brightness, so "grey" is one decision rather than
eight.

**Buttons carry hue only when engaged.** Off is the same sage grey for every
button, whatever it does; on is the control's hue with the glyph flipped to the
background. Per-family off-tints were tried and removed -- with forty-odd
buttons visible at once, forty dim hues read as noise rather than information.

**Selection is muted, engagement is not.** A highlight that follows the cursor
(library rows, the sidebar, menus) uses a dark muted green behind normal text.
The toolbar tabs and the skin-settings toggles get the same treatment: they sit
shoulder to shoulder with `margin: 0`, so a full-accent fill merged them into
one bright band across the top of the window. Full accent is reserved for deck
controls, where "this is engaged" is worth shouting.

**One edge colour per widget.** LateNight shades the four sides of a box
differently to fake a bevel -- lit top, dark bottom. `flatten_borders` finds any
rule that set more than one edge colour and gives every side the lightest of
them, so a box reads as one flat rule. Which sides exist is left alone, because
a border consumes space in Qt's box model and the layout is sized around it.

**Two band triples, kept in step.** Mixxx has two independent sets of
frequency-band colours: the Filtered and HSV renderers read `Signal*Color`, the
RGB renderer and the deck overview read `SignalRGB*Color`. The scheme sets both
to the same magenta/yellow/cyan, so the waveform and the overview match
whichever waveform type the user has selected. Leaving one unset is how they
end up disagreeing.

**Specificity, not order.** The design layer is appended last, but QSS resolves
specificity before order, so an inherited rule like
`#MixerMainHeadphone #FxAssignButtons WPushButton[displayValue="0"]` beats a
bare `WPushButton` rule however late it appears. Button selectors in
`terminal.qss.in` are therefore anchored to `#Mixxx`, the skin root, which adds
the id they were missing: they tie on specificity and win on order. If a button
in a nested container ignores the design layer, this is why.

**Transparent overlays.** A few of LateNight's "buttons" are invisible click
targets stacked over a display widget -- the record button over its dot and
label, the `Blank` placeholders, the preview indicator over the play button. A
blanket button fill paints over whatever they sit on, so the design layer
re-asserts `background-color: transparent` for those by name.

**A malformed skin.xml fails silently.** Mixxx logs one debug line and loads its
default skin instead, which looks like the skin "not being listed" rather than
an error. Two guards: palette descriptions are run through `xml_comment_safe`
(a `--` anywhere inside an XML comment makes the document invalid, and prose
hits that easily), and the generator parses the skin.xml it just wrote and dies
if it is not well-formed.

**The RGB overview** used to ignore skin colours: `drawWaveformPartRGB` computed
`red`/`green`/`blue` from the configured low/mid/high colours and then
normalised `low`/`mid`/`high` instead, hardwiring bass=red, mid=green,
treble=blue. That is fixed in
`src/waveform/renderers/waveformoverviewrenderer.cpp`, matching what the main
waveform renderer already did. No shipped skin sets `SignalRGB*Color` (they are
all empty), so the fix is behaviour-preserving for Deere, Tango, LateNight and
Shade, and only Terminal -- which does set them -- changes.

## Attribution

The layout, widget XML and icon geometry derive from **LateNight** by jus,
Owen Williams and ronso0, licensed CC-BY-SA 3.0 Unported. Terminal keeps that
licence; see the header of `res/skins/Terminal/skin.xml`.
