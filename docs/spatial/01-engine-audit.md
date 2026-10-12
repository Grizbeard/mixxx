# Spatial Mixxx — Engine Audit & Feasibility

*Audit date: 2026-10-07. Source audited: Mixxx `2.6` branch @ `8b7c676`, compared against stable `2.5.6`. File references are relative to the root of this repository.*

## TL;DR

- **Feasible, and the engine is friendlier to this than expected.** Mixxx already decodes stems as one 8-channel stream per deck. It time-stretches all 8 channels together, applies per-stem gain/mute/FX, and only *then* folds to stereo in one line of code. There is even a `// TODO(XXX): process stem DSP` placeholder exactly where a spatial renderer would go.
- **We must base on 2.6, not 2.5.6.** The current stable release (2.5.6) has no stem support at all. Stems ship in 2.6, which is in beta (`2.6-beta` tagged; no `2.6.0` yet). Fork the `2.6` branch now and rebase onto `2.6.0` when it is tagged.
- **Everything downstream of the stem fold-down is hard-wired stereo.** This covers the effects framework, mixer buses, output routing (max 2 channels per output) and recording. The right design is a **parallel spatial bus** that taps the stems before fold-down, leaving the stereo pipeline untouched for headphones/cue, booth and recording. Don't make Mixxx's core multichannel.
- **Spatial FX and Auto-Pan should be a standalone DSP module, not Mixxx "effects".** Making the effects framework N-channel would touch every built-in effect and the LV2 host. A standalone module can still reuse Mixxx's tempo/beat-phase plumbing and some DSP pieces.
- **Controls, MIDI and a second window need no core changes.** New ControlObjects are automatically addressable from MIDI XML/JS. Multiple controllers run at once. Mixxx already has non-modal secondary windows to copy.
- **The NI stem format has 4 stems (drums, bass, other, vocals), not 3.** We'll present 3 user-facing stems ("Instruments" = bass + other), but keep bass separate internally.

---

## 1. Version baseline

| | 2.5.6 (stable) | 2.6 branch (beta) |
|---|---|---|
| Stem files (`.stem.mp4`/`.stem.m4a`) | ❌ none | ✅ `__STEM__` build option, ON when FFmpeg is present (`CMakeLists.txt:2674`) |
| Per-stem volume/mute/color controls | ❌ | ✅ `[ChannelN_StemM]` |
| Per-stem QuickEffect & effect-unit routing | ❌ | ✅ |
| Windows build env | ✅ | ✅ `tools/windows_buildenv.bat` → prebuilt vcpkg deps (`mixxx-deps-2.6-x64-windows-*`); FFmpeg enabled |
| ASIO on Windows | ✅ | ✅ (Steinberg ASIO SDK credited in `packaging/wix/LICENSE.rtf.in`) |

**Recommendation:** fork `mixxxdj/mixxx` at `2.6`. Keep our changes in a small number of new files plus narrow hooks in existing files, so rebasing onto `2.6.x` point releases stays cheap. Mixxx is GPL-2.0-or-later, so the fork must remain GPL.

---

## 2. How audio flows today (2.6)

```
 .stem.mp4 ──► SoundSourceStem  ─ 4× SoundSourceFFmpeg (one per stem stream; main mix stream skipped)
               interleaves → 8ch float: 1L 1R 2L 2R 3L 3R 4L 4R
                     │
                     ▼
               CachingReader (8ch chunks, ~20 MB/deck)  ─►  ReadAheadManager (channel-agnostic)
                     │
                     ▼
               EngineBuffer::process  — rate / scratch / keylock on ALL 8 channels
                 · linear & SoundTouch: one 8ch instance
                 · RubberBand: 4 stereo stretchers in parallel on a worker pool
                     │
                     ▼
 ┌──── EngineDeck::processStem (src/engine/channels/enginedeck.cpp:126-198) ─────────────┐
 │  for each stem: extract stereo pair → stem volume/mute ramp → stem QuickFX + any    │
 │                 effect units routed to that stem → write back into 8ch buffer       │
 │  :143  // TODO(XXX): process stem DSP          ◄── SPATIAL TAP POINT                  │
 │  :197  SampleUtil::mixMultichannelToStereo()   ◄── fold-down to stereo (plain sum)    │
 └────────────────────────────────────────────────────────────────────────────────────────┘
                     │ stereo from here on
                     ▼
               pregain → Prefader chain (deck EQ) → VU meter          (EngineDeck::process)
                     │
                     ▼
 EngineMixer::process (src/engine/enginemixer.cpp:362-799)
   PFL → headphone bus
   channel fader × crossfader gain → Postfader chains (deck QuickFX filter, effect units 1-4)
   → bus L/C/R → main mix → main effects → balance → main / booth / headphone delays
                     │
                     ▼
 SoundManager → SoundDevicePortAudio::composeOutputBuffer → sound card (each output ≤ 2 channels)
```

### Key facts behind that diagram

- **The decoder** (`src/sources/soundsourcestem.cpp`) requires exactly 1 main mix + 4 stereo stems (`kNumStreams = 5`), all with the same codec and sample rate. **The main mix stream is never decoded.** Playback and analysis use the sum of the 4 stems, and the manifest's mastering DSP (limiter/compressor) is ignored.
- **The engine is driven by one PortAudio callback.** `EngineMixer::process` runs inside the callback of the "clock reference" device at time-critical priority (`src/soundio/sounddeviceportaudio.cpp`, `callbackProcessClkRef`). All devices share one sample rate and buffer size. Secondary devices are fed through FIFOs with drop/duplicate-frame drift correction, with no resampling.
- **Buffers are float32 interleaved stereo.** Limits: `kMaxEngineFrames = 8192` and `kMaxEngineChannels = 2` (`src/util/defs.h:19-22`). Engine inputs can be up to 8 channels (`kMaxEngineChannelInputCount = stem()`), but every engine *output* is stereo (`kEngineChannelOutputCount`, `src/engine/engine.h:7`).
- **Output routing is capped at stereo.** `AudioPath::maxChannelsForType()` returns stereo for *every* type (`src/soundio/soundmanagerutil.cpp:227-230`). `composeOutputBuffer` assumes stereo source buffers (`src/soundio/sounddevice.cpp:117`, `framesReadOffset*2`). Existing output types are Main, Headphones, Booth, Bus L/C/R, Deck N and RecordBroadcast.
- **Deck EQ is Prefader.** It runs in EngineDeck, after the fold-down. The deck QuickEffect (filter knob) and effect units 1-4 are **Postfader** and run in EngineMixer (`src/effects/chains/*.cpp`). This matters a lot for the design (§4.3).

---

## 3. Where stems fit, and the 3-vs-4 stem question

The NI stem standard (which Mixxx implements) carries **4 stems in Traktor order: drums, bass, other (melody), vocals**. Several things are hard-coded to 4:

- `kRequiredStreamCount`
- `kMaxSupportedStems = 4`
- the 4-bit `StemChannel` mask (`engine.h:16-25`)
- the `for (int i = 0; i < 4; i++)` in `PlayerManager::addDeckInner` (`src/mixer/playermanager.cpp:389-397`)
- the waveform data struct

**Recommendation: keep 4 stems in the file and in the engine, and present 3 to the DJ.**

| User stem | File stems | Why |
|---|---|---|
| **Drums** | 1 | |
| **Instruments** | 2 (bass) + 3 (other) | The pan, width and FX controls drive both together. |
| **Vocals** | 4 | |

Keeping bass separate internally is useful even though the DJ never sees it. Bass is what should feed the mono LF sum and stay anchored, while the "other" content travels around the room. This also keeps us compatible with every existing `.stem.mp4` and with standard stem-creation tools (NI Stem Creator, or Demucs/other separators plus a packager), with no core changes.

### Fidelity note ("fewest artifacts from the original stereo source")

- **Stems are not the master.** Mixxx plays the sum of 4 stems and discards the main mix. ML-separated stems sum close to the original, but not bit-exactly, and pulling them apart spatially exposes separation bleed as "ghost" content in other speakers.
- **Mitigations:**
  1. Use good separation, or real multitrack stems when available.
  2. Keep per-stem width conservative by default.
  3. **Optional:** when all of a deck's stems are in stereo bypass at unity gain with no FX, crossfade to the decoded **main mix** stream (a 5th decoder) for a bit-exact original. This changes `SoundSourceStem` and the deck buffer layout, so it's a later phase.

---

## 4. Feasibility by feature

### 4.1 Spatial renderer (4-6 points on a plane + mono LF sum) — **Feasible, core of the work**

**Approach that minimises artifacts in a club:**

- **Pure amplitude panning; no HRTF, no decorrelation, no phase tricks on the dry signal.** Use pairwise constant-power panning around a ring (2-D VBAP generalises this to arbitrary speaker angles, so 4, 5 or 6 speakers at any azimuths are one code path). The signal reaching each speaker is the original stem waveform times a gain, so nothing is filtered, smeared or comb-filtered.
- **Width without upmixing.** Treat each stem's L and R as two virtual sources at `azimuth ± width/2`. At width 0 they collapse to one point; at wide settings the stem's own stereo image is spread across the arc. This preserves the original stereo content exactly instead of synthesising width.
- **Centre / "intensity".** A radius or spread parameter lets a stem bloom from a point source to all speakers (equal-power spread). This is useful for drums and for big drops.
- **Gain smoothing.** Interpolate per-sample, or per 16-32 frame sub-block, so fast auto-pan orbits don't zipper. Mixxx's own `GainCache` ramping is the pattern.
- **Mono LF sum.** Sum all stems *before* panning and low-pass them (Linkwitz-Riley 4th order, ~80-120 Hz adjustable) into a mono sub feed, so sub level doesn't change with pan position. Offer an optional matching high-pass on the mains. Many club systems already have a crossover in their speaker processor, so we should be able to send full-range mains plus a sub feed and let the house DSP do the rest.
- **Speaker configuration.** Count (4-6), azimuth per speaker, per-speaker gain trim and delay (ms) for alignment, LF cutoff, and HPF on/off.
- **Output stage.** A safety limiter per output channel, and optionally a phase-coherent "stereo fold-down" monitor of the spatial mix for the headphones.

**Panner choice (recommended): 2-D VBAP with power-normalised spread, not Ambisonics or DBAP.**

- **Position is azimuth + radius.**
  - Radius 1: pairwise VBAP on the perimeter, at most 2 speakers active.
  - Radius 0: equal-power across all speakers (centre of the room).
  - In between: a power-domain blend of the two.
- **Width = two virtual sources** (the stem's L and R) at `az ± w/2`.
- **Why not the alternatives:**
  - **Ambisonics** at the orders 4-6 speakers allow (1st-2nd) drives every speaker with coherent copies. That gives a blurry image, a strong sweet spot and comb filtering for off-centre listeners, and irregular layouts need AllRAD-style decoders.
  - **DBAP** suits audiences spread everywhere, but it also keeps most speakers active. Its centre behaviour is effectively what the radius parameter already provides.
- **Real-time implementation:**
  - Precompute gain tables per azimuth when the speaker config changes.
  - Ramp gains linearly per sub-block (16-32 frames).
  - Do beat-quantised jumps as 5-20 ms crossfades.
  - No decorrelation on the dry path.

**CPU cost is negligible.** Panning 2 decks × 4 stems × 2 channels into 7 outputs is roughly 112 multiply-adds per frame, or about 5-6 M/s at 48 kHz. The dominant engine cost stays what it is today: 8-channel keylock (RubberBand) on each deck.

### 4.2 Per-stem controls — **Feasible**

Volume (already `[ChannelN_StemM],volume`), pan, width, FX send, auto-pan on/off, and stereo bypass.

- **Front** (formerly "bypass") is a pan preset rather than a separate path. It places the stem's L/R exactly on the Front-L/Front-R speakers, the closest match to the original stereo mix (see §8).
- **Naming.** Add new keys to the existing `[ChannelN_StemM]` groups (e.g. `spatial_azimuth`, `spatial_width`, `spatial_fx_send`, `spatial_autopan`, `spatial_bypass`). This keeps the midi-components `Deck` group-rewriting regex working (`res/controllers/midi-components-0.0.js:721`).
- **Grouped "Instruments" control.** Expose it as group-level keys that write both underlying stems, or keep the 4 raw stem groups and do the grouping in the controller mapping. The first is cleaner for the GUI.

### 4.3 Making the spatial path obey the normal mixer — **Feasible, needs care (main design risk)**

The DJ will mix on the Reloop Ready, so gain, EQ, filter, line fader, crossfader and the FX units **must affect the spatial output too**, not just the stereo main. Those controls are currently applied *after* the stem fold-down, in two different places (EngineDeck and EngineMixer).

| Mixer stage | Type | How the spatial path reproduces it |
|---|---|---|
| Pregain, line fader, crossfader | Scalar gains | Apply the same `GainCalculator`/`GainCache` values to each deck's spatial bus. The sum is exactly equivalent. |
| Deck EQ, QuickEffect filter | Linear filters | Run them **per stem** before panning. Since `EQ(Σstems) = Σ EQ(stems)`, the result is mathematically identical to today's stereo path. Cost is ~4× a biquad chain per deck, which is trivial. |
| Effect units 1-4 (echo, reverb…) | Mostly linear, stereo | 2.6 can already route effect units to individual stems (`effectsmanager.cpp:85-97`). When a unit is enabled for a deck, enable it for that deck's stems instead, so the FX happen before panning and their tails follow each stem. Non-linear effects (distortion, bitcrusher) will sound slightly different per stem than on the sum. That's acceptable but should be documented. |

**Detail from the follow-up review:**

- **Per-stem state already exists.** Effect units keep a separate processor state for each input channel (`ChannelHandleMap … m_channelStateMatrix`, `effectprocessor.h:258`), so per-stem routing gives true parallel instances driven by the same knobs. Standard units already register every stem as an input (`effectsmanager.cpp:85-97`). The EQ and QuickEffect chains "only process one input channel" (same file), so per-stem EQ and filter need new wiring.
- **The fader would move ahead of the effects (important).** Deck effect units are currently *post-fader*: pull the line fader down and an echo tail keeps ringing ("echo-out"). Processing them per stem in `processStem` puts them *before* the fader, so the tail would be cut. The fix is to apply the deck's `volume × crossfader` gain to the stems before the per-stem effects in spatial mode, then use unity for that deck in `EngineMixer`. That moves one gain calculation earlier and keeps echo-outs working.
- **One assign button, no doubled effect.** An engine-level link mirrors `group_[ChannelN]_enable` onto that deck's stems and suppresses deck-level processing, so the effect never runs twice. The skin buttons and the Reloop mapping stay unchanged.
- **Effect classes:**

  | Class | Effects | Behaviour when run per stem |
  |---|---|---|
  | Linear | EQs, Filter, Echo, Reverb, Flanger, Phaser, Tremolo, Glitch, Balance, Graphic/Parametric EQ, Loudness Contour | Same as on the sum. LFO effects stay in step because every instance is enabled in the same callback; Tremolo also locks to the beat. |
  | Non-linear | Distortion, Bitcrusher, Compressor, Moog Ladder | Sound somewhat different per stem than on the sum. Acceptable, but document it. |
  | Generators | Metronome, White Noise | Must **not** be duplicated: 3 instances give about +9.5 dB of clicks or +4.8 dB of noise. Run them once and send to the Front pair. |

- **CPU.** About 3× the effect cost per deck. Reverb is the heaviest.

**Pregain and VU.** Pregain is a scalar, so it's easy. The deck VU should keep metering the stereo sum, so the Reloop's meters behave normally.

**What stays stereo and untouched:** headphones/PFL (the DJ cues in stereo), booth, recording/broadcast, and the stereo Main output (which can feed a booth or stay unused).

### 4.4 Multichannel sound card routing — **Feasible, small/medium change**

- **Add a new output type,** e.g. `AudioPathType::Spatial`, allowing up to 8 channels (6 speakers + sub + spare). The header comment at `src/soundio/soundmanagerutil.h:54-59` lists every function that must change. Also:
  - fix the stereo assumption in `composeOutputBuffer` (`framesReadOffset*2` → `*channelCount`), and the other `*2` offsets in `sounddevice.cpp`;
  - let the preferences item offer N-channel groups (`src/preferences/dialog/dlgprefsounditem.cpp:93-122`);
  - make Spatial a clock-reference candidate (`soundmanager.cpp:430-438`).
- **The spatial bus should be its own `AudioSource`.** That means a stable-address buffer, written each callback. Register it like EngineMixer's other outputs (`enginemixer.cpp:1047-1077`).
- **Hardware constraint: spatial outputs and the clock reference must live on one device.** A second device goes through the drop/duplicate drift path, which can click and drift in phase. The Reloop Ready has its own built-in audio interface. Two consequences:
  - Headphones are safest on the multichannel interface as well.
  - Using the Reloop's interface for cue only is possible but sub-optimal. Mixxx uses one host API for all devices, and an ASIO host generally loads only one driver at a time, so the Reloop's card would likely be unusable alongside a multichannel ASIO card.
  - **Recommendation:** a multichannel interface with ≥ 8 outputs plus a headphone out, on ASIO, with the Reloop used as MIDI only.

### 4.5 Spatial FX unit (tempo-synced delays, reverb, granulizer) — **Feasible as standalone module**

- **Why not the existing effects framework:** it is stereo by construction. The `EngineParameters` channel count is fixed at 2, effects step `i += 2`, the LV2 host only maps L/R, and so does latency compensation. Making it N-channel is high cost and high risk.
- **Instead,** build a `SpatialFxUnit` fed by per-stem post-fader sends (the "FX send" knob) and returning N speaker channels:
  - **Delays:** beat-synced (1/16 … 2 bars, triplets) with spatial patterns such as ring ping-pong, opposite-speaker bounce, and rotating taps. This reuses Echo's beat math (`src/effects/backends/builtin/echoeffect.cpp:129-158`).
  - **Reverb:** a multichannel FDN with one decorrelated output per speaker. Decorrelation is fine here because it applies only to the wet signal, never the dry.
  - **Granulizer:** a capture buffer, with grain size and density quantised to the beat and grains scattered over speakers. Mixxx has nothing like this (the closest is `glitcheffect.cpp`), so it is new DSP.
- **Tempo source:** `GroupFeatureState` gives `beat_length` (seconds, rate-adjusted) and `beat_fraction_buffer_end` (phase) per deck (`src/engine/controls/bpmcontrol.cpp:1309-1338`). A shared FX unit should follow the sync leader / `[InternalClock]` (`bpm`, `beat_distance`), with an option to follow a specific deck.

### 4.6 Auto-pan unit (orbits, jumps…) — **Feasible as standalone module**

- **Implementation:** a beat-locked modulator that writes each assigned stem's effective azimuth or spread.
  - **Modes:** orbit (N beats per revolution, CW/CCW), jump (step to next/random/opposite speaker every 1/4…4 beats, with a short gain crossfade to avoid clicks), ping-pong (arc sweep), and figure-8/front-back.
  - **Per-stem phase offset,** so drums and vocals can orbit in opposition.
- **Phase lock is the important part.** Mixxx's own AutoPan only syncs period and has an explicit `// TODO(xxx) sync phase` (`autopaneffect.cpp:91-97`). Tremolo and Metronome *do* phase-lock using `beat_fraction_buffer_end` (`tremoloeffect.cpp:142-163`), and they are the model to copy.
- **The user's switch:** "send channel to autopan" = `spatial_autopan` on a stem. Its manual pan becomes the centre/offset of the motion.

### 4.7 Second "Spatial" window — **Feasible, no core changes**

- **Pattern to copy:** `DlgDeveloperTools`, a non-modal top-level window owned by `MixxxMainWindow` (`src/mixxxmainwindow.cpp:1071-1086`). The main skin and Mixxx UI stay unaltered.
- **Contents:** a QPainter-drawn speaker ring with a dot per stem per deck (position = current azimuth including auto-pan, size/brightness = level), FX and auto-pan panels, and a speaker-configuration page (count, angles, trims, delays, LF crossover, output channel map). This is natural for a second monitor.
- **Data feed:** the engine publishes read-only controls per stem: `spatial_azimuth_actual`, `spatial_level` (stems currently have **no VU meters**, so these are new). The window polls via `ControlProxy` on a ~60 Hz timer, the same way existing VU widgets work.
- **Decided: a separate window** opened from the View menu, with a shortcut and an optional open-on-startup setting. It remembers its screen, position, size and fullscreen state. The pieces:
  - **Custom C++ widgets bound to controls.** The main one is a speaker-field widget showing stem dots, levels and auto-pan paths. Writing it as a self-contained widget lets it be exposed to skins later through a new skin tag, if wanted.
  - **Keyboard.** Install Mixxx's keyboard event filter on this window as well, so DJ keyboard shortcuts keep working when it has focus.
  - **Drawing load.** Draw with lightweight QPainter at about 60 Hz. The window shares the GUI thread with the main window's waveforms.
  - **Setup.** Speaker layout, delays, crossover and output map go in a new Preferences page (DlgPrefSpatial), with a quick "Setup" tab in the window.
- **QWidget over QML.** QML is a build option (`option(QML ... OFF)`) and its UI mode is either/or with the legacy skin. Mounting a QML window inside the legacy UI looks possible but is untested. Start with QWidgets; revisit QML later if wanted.

### 4.8 Secondary MIDI controller — **Feasible, no core changes**

- **The plumbing already exists:**
  - multiple controllers run simultaneously, each with its own script engine (`src/controllers/controllermanager.cpp:202-217`);
  - MIDI XML and JS can address any `[group],key`;
  - the spatial controls just need to be created before mappings load (at engine construction).
  - New controls only show up in the MIDI-learn wizard if added to `src/controllers/controlpickermenu.cpp`. That's optional, since we'll ship a hand-written mapping.
- **Layout fit:** 3 stems × 2 decks = **6 strips**. Each strip has 4 continuous controls (volume, pan, width, FX send) and 2 buttons (auto-pan, bypass), plus FX/auto-pan unit controls.
  - **Novation Launch Control XL** is a near-perfect fit: 8 columns of 3 knobs + fader + 2 buttons. That gives 6 stem strips plus 2 columns for the FX unit and auto-pan.
  - **MIDI Fighter Twister** is the alternative. Its endless encoders suit 360° pan with wrap-around, and Mixxx already ships Twister stem mappings.

### 4.9 Reloop Ready (main controller) — **Works as-is**

- **The existing mapping** (a separate Reloop Ready mapping repository) uses only standard groups: `[Channel1/2]`, `[Master]`, `[Library]`, `[EqualizerRack1_…]`, `[QuickEffectRack1_…]` and `[EffectRack1_EffectUnit1/2…]`. It contains no stem controls, so it should load unchanged on 2.6 and on our fork.
- **Because of §4.3,** its EQ, filter, fader and FX knobs will drive the spatial output too.
- **One check:** the mapping declares `mixxxVersion="2.5.0+"` and should be tested on 2.6.

---

## 5. Real-time and robustness notes for club use

- **Pre-existing audio-thread allocations to fix:**
  - `m_stemBuffer` is lazily resized inside `processStem` (`enginedeck.cpp:136-137`). Preallocate it.
  - Loading a stem track after a stereo track triggers scaler re-setup on the audio thread (`enginebuffer.cpp:1230-1233`). Consider forcing the 8-channel configuration permanently on our two decks.
- **Hard real-time rules for the spatial module:** no allocation, locking or logging in the callback. Read parameters through `PollingControlProxy` / atomics, and pass configuration changes through a lock-free `MessagePipe` (`src/util/messagepipe.h`), as the effects system does.
- **The fold-down has no headroom.** `mixMultichannelToStereo` is a plain sum (`src/util/sample.cpp:799-818`). The spatial bus should apply gain staging and an output limiter, because a club rig will expose overs.
- **Memory:** ~20 MB of read cache per stem deck (4× a stereo deck). That's fine.
- **Latency:** the spatial bus is computed in the same callback as the stereo path, so headphones and mains stay sample-aligned. Mixxx's main/booth/headphone delay compensation still applies to the stereo outputs. The spatial outputs need their own per-speaker delays (§4.1).

---

## 6. Proposed architecture

```
EngineDeck::processStem (per deck)               EngineMixer::process
───────────────────────────────────              ────────────────────────────────────────
8ch stems (stretched, stem gain, stem FX)
   │
   ├─► [new] per-stem deck EQ + filter ──► SpatialDeckRenderer
   │        (mirrors deck EQ/QuickFX)        · VBAP pan + width + spread
   │                                         · auto-pan modulation      ──► per-deck N-ch buffer
   │                                         · FX sends                  ─┐        │
   │                                         · bypass → stereo pair       │        │ × pregain × fader
   │                                                                      │        │ × xfader gain
   └─► existing fold-down → stereo deck path (cue, booth, record, Main)   │        ▼
                                                                          │   SpatialBus (N+1 ch)
                                                      SpatialFxUnit ◄─────┘   + SpatialFxUnit return
                                                      (delay/reverb/grain,    + LF mono sum (LR4)
                                                       beat-synced)           + per-speaker trim/delay
                                                                              + limiter
                                                                                     │
                                                              AudioSource ──► AudioPathType::Spatial
                                                                              (≤8ch on clock-ref device)
```

Suggested new code lives in `src/engine/spatial/` (renderer, bus, FX unit, auto-pan) plus `src/widget/spatial/` or `src/dialog/spatialwindow*` for the window. The changes to existing files are narrow hooks:

- `enginedeck.cpp`: the tap
- `enginemixer.cpp`: gain application and summing into the spatial bus
- `soundmanagerutil.*`, `sounddevice.cpp`, `dlgprefsounditem.cpp`: the output type
- `mixxxmainwindow.cpp`: opening the window

---

## 7. Phased plan

| Phase | Goal | Exit criterion |
|---|---|---|
| **0. Baseline** | Build unmodified 2.6 on Windows (buildenv + MSVC). Load stem files, run the Reloop Ready mapping, and open the multichannel card on ASIO. | Stock 2.6 plays a `.stem.mp4` through the target interface; Reloop works. |
| **1. Plumbing** ✅ *(2026-10-09: Spatial output type, EngineSpatialOutput, Spatial Audio prefs page with speaker tests and main passthrough)* | `AudioPathType::Spatial` (N channels), spatial `AudioSource`, preallocated stem buffers, test-tone/identify-speaker routing. | Each speaker and the sub can be addressed and identified from preferences. |
| **2. Renderer core** ✅ *(2026-10-09: per-stem tap with mirrored EQ/QuickEffect/FX units, VBAP + width/spread/Front, LR4 LF crossover, trims/delays, limiter; null tests pass — see 03)* | Spatial tap, VBAP ring, width/spread, bypass, LF sum and crossover, speaker config, mixer-equivalence (§4.3), limiter. | Stems pan around 4-6 speakers; EQ/fader/xfader on the Reloop behave identically on the spatial and stereo outputs (null test). |
| **3. Controls & 2nd controller** ◐ *(controls done; LCXL MK3 mapping waits for the hardware)* | `[ChannelN_StemM] spatial_*` controls plus a mapping for the chosen controller. | All per-stem spatial controls work from hardware. |
| **4. Spatial window** ✅ *(2026-10-09: LateNight-style touch window; not yet timed on the touch monitor)* | Speaker ring, stem dots and levels, config page. | Usable on a second monitor at 60 fps with no audio dropouts. |
| **5. Auto-pan** ✅ *(2026-10-09: 2 units, 6 shapes, 5 waves, synced/triplet/seconds)* | Beat- and phase-locked orbit/jump/ping-pong. | Motion stays locked to the beat across tempo changes and sync. |
| **6. Spatial FX** ◐ *(2026-10-09: delay and reverb done; granulizer not started)* | Delay → reverb → granulizer. | Each effect is beat-synced, multichannel, and click-free on enable/disable. |
| **7. Club hardening** | CPU/latency profiling at small buffers, device-loss behaviour, long-run soak test, preset save/load of speaker layouts. | 4-hour soak test at the target buffer size with zero xruns. |

---

## 8. Decisions (2026-10-07)

| Topic | Decision |
|---|---|
| Secondary controller | **Novation Launch Control XL 3 (MK3)**: 24 endless RGB encoders, 8 × 60 mm faders, 16 buttons, OLED. Columns 1-6 are stem strips (3 stems × 2 decks):<br>• encoder row 1 = pan (endless, wraps 360°)<br>• row 2 = width<br>• row 3 = FX send<br>• fader = stem volume<br>• buttons = Auto-pan / Front<br>Columns 7-8 hold the spatial FX and auto-pan units. Encoder RGB rings can show stem colour or state. Still to verify from Novation's programmer's reference: relative encoder mode and LED/OLED control. |
| Spatial GUI | **A separate top-level window**, not part of the skin, so it can be arranged or fullscreened on a second monitor. Details below. |
| Audio interface | **Behringer XR18** (18×18 USB, ASIO) as the single clock-reference device. Physical outputs: Aux 1-6 → speakers, Main L/R → LF sub (+ spare), phones → stereo cue. Routing details are still to verify (§9). |
| Stereo Main | **Not used.** Mixxx's stereo Main is left unassigned in Sound Hardware. It is still computed internally, which keeps recording, the master VU and the Reloop's master knob working. The Reloop's Master Vol (`[Master],gain`) should also scale the spatial bus. |
| "Stereo bypass" | **Renamed "Front".** This is not a separate signal path. It is a pan preset: azimuth = front, width = the front-pair angle, spread = 0. With VBAP that puts the stem's L exactly on Front-L and R exactly on Front-R, i.e. the original stereo image. It glides in and out (~250 ms or 1 beat) and overrides auto-pan while engaged. A deck-level "all stems Front" is available on shift. |
| Internal stems | **Bass and Other are summed into one Instruments stem** inside `processStem`. Spatial processing then handles 3 stems per deck. The file still carries 4 streams (Mixxx requires 4). Summing before the time-stretch would save about 25% of keylock CPU, but it means deeper changes to the decoder and controls; keep it as a later optimisation. |
| LF output | **One adjustable crossover across all speaker outputs.** A Linkwitz-Riley crossover (LR4 default; 60-150 Hz; slope option) high-passes every speaker feed and low-passes a mono sub feed, with sub gain/delay/polarity trims. The sub feed is taken from the deck/stem sum *before panning* (scaled by the same gains), not from the summed speaker feeds. Constant-power panning makes the amplitude sum of the speaker gains vary from 1.0 to 1.41 with position, so summing the speaker feeds would make bass pump by up to +3 dB as stems move. Spatial-FX returns are added to the sub feed the same way. |
| XR18 output map | Confirmed: every output can be fed directly from USB. **Aux 1-6 → speakers, Main L → LF (Main R spare / second sub), Phones L/R → cue.** |
| Mixxx FX units & EQ/filter | **Run in parallel on every stem of the deck** (one instance per stem, same knobs). Details and caveats in §4.3. |
| Stem files | Keep `.stem.mp4` (Mixxx requires exactly 4 stem streams). Fill slots as drums / bass / other / vocals; the UI shows Instruments = bass + other. Separation happens offline (GPU), with residual-based "other" and lossless ALAC — see §10. |

## 9. Still open / to verify

1. ~~XR18 routing~~ — resolved: all outputs are directly addressable via USB.
2. ~~Deck effect units in spatial mode~~ — resolved: per-stem, in parallel.
3. ~~Launch Control XL generation~~ — resolved: XL 3 (MK3) with endless encoders, so pan wraps 360°.
4. **ALAC-in-`.stem.mp4`.** Confirm Mixxx 2.6 loads it (FFmpeg decodes ALAC; Mixxx only checks that the 4 stems share a codec and sample rate, at `soundsourcestem.cpp:162-175`).

## 10. Stem preparation pipeline (offline, outside Mixxx)

**Mixxx performs no source separation.** It only decodes pre-made stem files, and 2.6 contains no separation code. The separation model is our choice and runs ahead of time on the RTX GPU (CUDA). That keeps the GPU, its drivers and model latency out of the live audio path.

```
original.wav ─► vocal model (e.g. BS-/Mel-Band RoFormer) ─► vocals
             ─► drum model (e.g. HT-Demucs) on (mix − vocals) ─► drums
             ─► bass model on (mix − vocals − drums) ─► bass
             ─► other = mix − vocals − drums − bass        (residual, computed in float)
             ─► encode 5 streams ALAC @ 48 kHz: [original master, drums, bass, other, vocals]
             ─► mux into MP4 + NI "stem" manifest atom (names/colours) ─► track.stem.mp4
```

- **The residual "other" makes the stems sum back to the original master exactly.** Separation errors land in "other"/Instruments instead of being lost or doubled.
- **ALAC (lossless) keeps that true after encoding.** AAC would add independent codec noise to each stem that no longer cancels in the sum.
- **Render at the XR18's 48 kHz.** At 0 % pitch with keylock off, Mixxx's linear scaler then passes samples through unchanged instead of interpolating 44.1→48.
- **Put the untouched original in the main-mix slot.** Mixxx ignores it today, but it keeps the file standard and enables a later "play original when all stems are Front" mode (§3).
- **Tooling candidates:** `python-audio-separator` (RoFormer/Demucs/MDX models, CUDA), `stempeg` or NI's stem tools for muxing the manifest. Whether these tools handle ALAC still needs checking.
