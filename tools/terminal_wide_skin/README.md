# Terminal-wide skin generator

Builds the colour layer of [`mixxx/skins/Terminal-wide`](../../mixxx/skins/Terminal-wide).

```bash
python tools/generate.py                # all palettes
python tools/generate.py --palette btop
python tools/generate.py --check        # write nothing, fail if stale
```

Run it after editing anything in this directory. It rewrites generated files in
place, so committing the output is expected.

## Why generate anything

Qt stylesheets have no variables, and Mixxx selects a colour scheme by pointing
at a different stylesheet. "Make the colours adjustable" therefore means one
stylesheet per scheme, and hand-maintaining four near-identical ones is how
skins end up with schemes that quietly drift apart. The palette is the single
source of truth:

```text
palettes/<name>.json               colours + role assignments
        |
        +-- mixxx/skins/Terminal-wide/style_<dir>.qss   the scheme stylesheet
        +-- mixxx/skins/Terminal-wide/skin.xml          the <Schemes> block
        +-- mixxx/skins/Terminal-wide/library/<Name>/   sidebar icons (copied)
```

A new scheme is one JSON file and one command.

## Files

| Path | What it is |
| --- | --- |
| `palettes/*.json` | A scheme: `name`, `dir`, `description`, `colors`, `roles`. Copied verbatim from `tools/terminal_skin/palettes/` in the Mixxx tree so the two skins stay in step. |
| `templates/style.qss.in` | The terminal design layer. Double-brace tokens resolve from the palette. **This is where the design lives.** |
| `templates/scheme.xml.in` | The `<Scheme>` block: waveform and overview colours, marker colours, the launch image. |
| `generate.py` | The build. |

`colors` are raw hex; `roles` are indirection (`accent -> green`) so a template
can say `{{accent}}` and a monochrome palette can point every role at one hue.
Both names resolve as tokens, and an unknown token is a hard error rather than a
silently unstyled widget.

This is a fraction of the size of `tools/terminal_skin/generate.py` in the Mixxx
tree, because Terminal-wide ships **no image assets of its own**: every control
is a word on a block, so there are no icon SVGs to flatten and repaint.

## The library icons

The one exception. Sidebar icons are not reachable from a stylesheet — a
`LibraryFeature` builds a `QIcon`, and the only handle a skin has is to ship a
replacement at `<skin>/library/<scheme>/ic_library_<name>.svg`
(`src/library/libraryfeature.cpp`, `skinIconPath`). Without one the sidebar keeps
Mixxx's saturated orange, which on a green-phosphor screen is the only colour on
it.

Terminal already generates exactly that set, from exactly these palettes, by
flattening Mixxx's own icons and repainting them. So they are **copied** rather
than regenerated:

```bash
python tools/generate.py --from-terminal /path/to/mixxx-terminal-skin
```

This repo has no Mixxx source tree to flatten icons from, and duplicating that
machinery to produce byte-identical output would be the worse trade. Re-run with
`--from-terminal` after changing a palette, or the icons and the stylesheets
drift apart.

## Adding a scheme

Copy a palette, edit the colours, run the generator, then re-run with
`--from-terminal` (after adding the matching palette to the Terminal generator
in the Mixxx tree, so the icons exist). It appears in
Preferences → Interface → Colour scheme; the first `<Scheme>` in `skin.xml` is
the default, and palettes are emitted in filename order.

Optionally add `mixxx/skins/Terminal-wide/skin_preview_<SchemeNameWithoutSpaces>.png`
so Preferences shows a thumbnail instead of a placeholder.

## Notes

**A malformed skin.xml fails silently.** Mixxx logs one debug line and loads its
default skin instead, which looks like the skin "not being listed" rather than an
error. Two guards: palette descriptions run through `xml_comment_safe` (a `--`
anywhere inside an XML comment makes the document invalid, and prose hits that
easily), and the generator parses the skin.xml it just wrote and dies if it is
not well-formed.

**`--palette` never filters the `<Schemes>` block.** That block is one region of
one file; rewriting it from a filtered list would drop the schemes that were
filtered out. Stylesheets are filtered; skin.xml is always rebuilt from every
palette.

**Two band triples, kept in step.** Mixxx has two independent sets of
frequency-band colours: the Filtered and HSV renderers read `Signal*Color`, the
RGB renderer and the deck overview read `SignalRGB*Color`. The scheme sets both
to the same three hues so the waveform and the overview match whichever waveform
type is selected. Leaving one unset is how they end up disagreeing.

**Reviewing a change.** Skins are pure data, so no rebuild is needed.
`_localbuild/skinshot.ps1` in the Mixxx tree launches Mixxx against a throwaway
settings directory, sizes the window, screenshots it and reports skin warnings
from the log. Two things it needs for this skin:

- install the skin where Mixxx looks for a *user* skin —
  `<settings>/skins/Terminal-wide` — since it does not live in a Mixxx `res/`
  tree;
- pin `QT_SCALE_FACTOR=1` and `QT_ENABLE_HIGHDPI_SCALING=0`. On a display with
  fractional scaling a 1920×480 window is only 768×192 logical pixels, below the
  skin's minimum size, so Qt silently hands back a larger window and the capture
  is not the panel's layout at all.
