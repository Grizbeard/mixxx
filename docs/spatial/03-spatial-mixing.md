# Spatial Mixxx — Spatial Mixing (phases 2, 4, 5, 6)

*2026-10-09.* This covers the renderer, the Spatial Mixer window, auto-pan, and the Spatial FX unit (delay and reverb). Code lives in `src/engine/spatial/` (engine) and `src/spatial/` (window), on branch `spatial/2.6`.

---

## 1. Signal flow

```
deck stems: Bass folded into Other = INST (Mixxx shows 3 stems), then stem volume + stem QuickFX/FX
  ├─► stereo fold-down ─► pregain ─► deck EQ ─► fader×xfader ─► post-fader FX ─► stereo main / cue / record   (unchanged)
  └─► spatial tap: Drums | INST | Vocals
        each ─► same pregain ramp ─► its own copy of the deck EQ ─► same fader×xfader ramp
             ─► its own copy of the deck QuickEffect + assigned FX units ─► VBAP pan (pan, width, spread, auto-pan, Front)
                                                                         ├─► speaker bus
                                                                         ├─► LF sum (pre-pan mono)
                                                                         └─► FX send ─► spatial delay ─► (to reverb) ─► spatial reverb
speaker bus + FX returns ─► master volume ─► LR4 high-pass (optional) ─► per-speaker delay/trim ─► linked limiter ─► Spatial output
LF sum  + FX returns     ─► master volume ─► LR4 low-pass ─► LF delay/trim/polarity ─────────────┘
```

- **"Same as the stereo mix" is tested.** With every stem in its default place, speakers 1-2 equal the stereo main sample for sample (within 1e-4) through pregain, stem volume/mute, channel fader, crossfader, master volume and the deck EQ. See `src/test/spatialnull_test.cpp`.
- **Each stem has its own effect copies.** The deck's EQ, QuickEffect (filter) and FX units 1-4 run on each spatial stem through hidden mirrored effect inputs (`[ChannelN_Spatial1..3]`). Turning an FX unit on for a deck turns it on for that deck's three stems too. Each copy keeps its own state, so echo tails follow each stem and ring out after the fader closes.
- **Stereo tracks.** A non-stem track plays as one source, using the **INST** controls (shown as "TRACK" in the window).
- **Everything else** (samplers, microphones, aux) plays on the front pair.
- **Not on the spatial output:** effect units assigned to the *Main* bus (they process the stereo main only), and talkover ducking.
- **Latency.** The limiter adds 1.5 ms of lookahead to the spatial output. Headphones stay on the stereo path.

## 2. Per-stem controls

The controls are keys on `[ChannelN_Stem1]` (Drums), `[ChannelN_Stem3]` (INST) and `[ChannelN_Stem4]` (Vocals).

**INST = Bass + Other.**
- The engine folds the file's Bass stem (2) into Other (3), so stem 3's volume, mute, QuickEffect and FX act on both. Stem 2's own controls do nothing.
- In LateNight the Bass row is gone.
- Mixxx names the stems **DRUMS**, **INST** and **VOCAL** whatever the file says, and shows VOCAL in yellow (waveform, labels and Spatial Mixer).
- The waveform shows three lanes (Drums / INST / Vocals), with the INST lane drawing bass and other together.

| Key | Range | Meaning |
|---|---|---|
| `spatial_azimuth` | ±180°, wraps | Pan. 0 = front, 90 = right. With auto-pan, it rotates the shape. |
| `spatial_width` | 0–1 = 0–180° | Angle between the stem's L and R. The default (60°) is the front pair on a 6-speaker ring. |
| `spatial_spread` | 0–1 | 0 = a point on the ring; 1 = equal on every speaker. |
| `spatial_fx_send` | 0–1 | Post-fader send to the Spatial FX unit. |
| `spatial_autopan` | toggle | Follow an auto-pan unit. |
| `spatial_autopan_unit` | 1 / 2 | Which unit: A or B. |
| `spatial_autopan_offset` | 0–1 | Phase offset as a fraction of the shape's cycle, so two stems can chase each other. |
| `spatial_front` | toggle | Glides (250 ms) to the exact original stereo image: L on speaker 1, R on speaker 2. Overrides auto-pan. |
| read-only | | `spatial_x`, `spatial_y`, `spatial_level`, `spatial_front_blend`, `spatial_active` |

**The panner** is 2-D VBAP on the ring, power-normalised, with spread as a power blend toward all speakers.
- **Collapsing width:** as L and R land on the same speakers, they are trimmed up to 3 dB.
- **Smoothing:** gains are smoothed with a 4 ms time constant, so square-wave jumps don't click.

## 3. Auto-pan (`[SpatialAutopan1]`, `[SpatialAutopan2]`)

- **Speed:**
  - `sync` on: `beats` per point (1/16 … 16), with `triplet` (× 2/3).
  - `sync` off: `seconds` per point.
  - Beats come from the sync leader if it is playing. Otherwise they come from the loudest playing deck, so the motion is phase-locked to the music. `retrigger` restarts at point 1 on the nearest beat.
- **Shapes:**
  - orbit (4 points: front, right, back, left);
  - left↔right and front↔back (through the centre, where the stem blooms to all speakers);
  - 4 corners (±45°, ±135°);
  - 5- and 7-point hypotrochoid stars (curved paths that dip toward the centre between points);
  - **random**: a new spot on the ring every step, always at least 75° from the last. The sequence is fixed by the beat, so every stem on the unit, its offset, and the window all agree.
- **Waves:**
  - *Square* snaps on each step.
  - *Sine* eases out of and into each point.
  - *Triangle* moves at constant speed.
  - *Saw* glides half way from A toward B over the step, then jumps onto B on the next beat, and so on.
  - *Reverse saw* glides half way backwards from A, then still jumps forward onto B.
  - Both take one step per point, the same rate as the other waves.
- **`direction`** reverses the point order. **`size`** scales the shape toward the centre.

## 4. Spatial FX unit

### Delay (`[SpatialDelay]`)
- **Echo time:**
  - `sync` on: `beats` (1/16 … 4, triplet).
  - `sync` off: `time_ms`.
  - Tempo changes bend the time smoothly.
- **Placement:** each echo sits on one point of the `shape` (with `direction` and `size`) for its whole length. There is no wave, so echoes never glide between points.
  - Echo 1 is on the shape's first point, echo 2 on the second, and so on round the shape.
  - **`poly`** moves the placement one point per `poly_beats` of echo time instead of one per echo. Echo k sits on point floor((k − 1) × echo time / poly_beats), so the pattern runs against the bar. For example, dotted-1/8 echoes over a 1-beat, 4-point shape land on points 1, 1, 2, 3, 4, 4, …
  - **How it works:** one feedback line holds a whole cycle of the pattern, and a tap per echo reads it, so each echo has its own fixed point. The window lights each point with its echo level.
- **`gate`:** a trance gate on the echo returns only (the dry stem is untouched).
  - `gate_beats`: the period, 1/32 … 1/2.
  - `gate_duty`: the open fraction.
  - `gate_smooth`: the edge time.
- **Tone:**
  - `feedback` (up to 0.97, soft-limited above full scale);
  - `tone`: a low-pass in the loop, 1–20 kHz;
  - `lowcut`;
  - `level`;
  - `to_reverb`: echoes on into the reverb, for vocal throws that bloom into the wash.

### Routing

```
stem FX SEND ─┬─► DELAY ──► speakers
              │      └── × TO REVERB ──┐
              └─ × DIRECT IN ──────────┴─► REVERB ──► speakers
```

- **One send per stem feeds both units in parallel.** The reverb works with the delay off.
- **DIRECT IN** (`[SpatialReverb],direct`, default 1) is how much of the send reaches the reverb directly. At 0 the reverb hears only the echoes: echo-into-wash throws.

### Presets

- **One FX engine, one preset list.** The window shows delay and reverb as one **SPATIAL FX** tab, with DELAY / REVERB sub-tabs, each with its ON button beside it.
- **An FX preset holds both units' settings and which of them are on.** So a preset can be reverb only, delay only, or both together. Freeze is never stored.
- **Factory FX presets:**
  - **Reverb only:** Vocal Space, Shimmer, Infinite, Plate.
  - **Delay only:** Dotted Ping, Quarter Orbit, Triplet Corners, Trance Gate.
  - **Together:** Space Echo (dotted ping into Vocal Space), Star Shimmer (poly stars into Shimmer), Wash Throw (echoes only, into a long wash).
- **Auto-pan:** units A and B share their own list: Slow Orbit, Corner Snap, L-R Sweep, Saw Orbit, Star Glide, Random Hits.
- **Using the strip:**
  - Tap a preset to load it; the one matching the current settings lights.
  - **SAVE** names and stores the current settings.
  - **DELETE** removes one of your own presets.
- **Files:** your presets are saved in the Mixxx settings folder under `spatial_presets/` (`autopan.json`, `fx.json`). Presets from the earlier separate delay and reverb lists are moved into the FX list once.

### Reverb (`[SpatialReverb]`)
- **Engine:** an 8-line modulated feedback delay network. Each speaker gets its own orthogonal (Hadamard) mix, so the tail surrounds the room, decorrelated. Only the wet signal is decorrelated.
- **Controls:**
  - `decay` (0.5–30 s), `size`, `predelay_ms`, `diffusion`;
  - `damping`, and `lowcut` (default 250 Hz, which keeps kick and bass out);
  - `modulation`, for a lush tail without ringing;
  - `shimmer`: an octave-up feedback, the "angelic" trance pad;
  - `duck`: pulls the tail down while the send is loud, so the vocal stays clear and the wash fills the gaps;
  - `freeze`: holds the tail.
- **Presets:** see above; **Vocal Space** (9 s, 60 ms pre-delay, light duck) is the starting point.

## 5. Headphone monitoring

- **What it does:** headphones hear the spatial mix. Each speaker of the ring becomes a virtual source around your head, rendered with a spherical-head model:
  - the delay between the ears;
  - the head's shadow on the far ear's highs;
  - a slight rear darkening, so sounds behind stay behind;
  - a few early reflections ("room"), so the mix sits outside your head.
- **The cue/mix knob still works.** The spatial render takes the place of the stereo main in the headphone mix, so the knob blends it with the cue.
- **No speakers needed.** It works without a Spatial output assigned: practise the moves at home on a virtual ring of the configured size.
- **HD 280 PRO correction:** on by default. It is AutoEq's parametric EQ from oratory1990's measurement: −5.8 dB preamp, 10 filters, towards the Harman target.
- **Switch it on:**
  - **PHONES 3D** in the Spatial Mixer's top bar;
  - or Preferences → Spatial Audio → Headphone monitoring, which also has the room amount and the EQ.
- **Control keys** (`[Spatial]`): `headphone_spatial`, `headphone_room`, `headphone_eq`.

## 6. Room setup (Preferences → Spatial Audio)

- **Ring:** speaker count (3–12), LF on/off, crossover.
  - The Spatial output needs one sound-card channel per speaker, plus one for the LF feed, which is always the channel after the last speaker.
  - On the XR18 there are 8 line outputs (Aux 1–6, Main L/R), so it can feed up to 7 speakers plus LF. Bigger rings need an interface with more outputs.
  - The reverb's level is scaled with the speaker count, so its balance against the dry stems stays the same on any ring.
- **Room alignment:**
  - angle, trim and delay per speaker (about 2.9 ms per metre of distance difference);
  - LF trim, delay and polarity;
  - mains high-pass at the crossover;
  - limiter ceiling (default −0.3 dBFS).
- **Default angles:** evenly spaced, with speakers 1 and 2 as the front pair either side of 0°, then clockwise. For example:
  - 3 speakers: FL −60, FR 60, rear 180.
  - 4 speakers: FL −45, FR 45, RR 135, RL −135.
  - 6 speakers: FL −30, FR 30, SR 90, RR 150, RL −150, SL −90.
  - 8 speakers: ±22.5, then every 45°.
  - 12 speakers: ±15, then every 30°.
- **Panning (dual band, split at 700 Hz):** two switches, both off by default. Off, stems pan exactly as before. Based on Hughes et al., "Dual frequency band amplitude panning for multichannel audio systems" (AES 2018).
  - **Even lows** (`pan_even_lows`): below 700 Hz speakers add up in phase, so with plain constant-power panning a stem half way between two speakers is about 3 dB louder in the lows (at the centre of the floor) than one on a speaker; an orbit pumps the bass and kick at the orbit rate. This holds the low band's summed pressure instead.
    - **Room** (`pan_low_room`, default 0.5): 0 = dry room, full correction (the 3 dB rise goes); 1 = very live room, no correction. 0.5 leaves about 2 dB. Live rooms need less, because reflections already even out the lows.
  - **Sharp highs** (`pan_sharp_highs`): above 700 Hz the ear goes by level, and constant-power panning pulls a treble image towards the nearer speaker (up to about 9° on 6 speakers, 16° on 3, 4.5° on 12). This pans the high band with the intensity law, so the treble image sits where it is panned. The paper's listening test found the gain subtle, so compare by ear.
  - The split is subtractive (the low band's gain difference is added on a low-passed copy), so where both bands agree, for example a stem on a speaker or in Front, the output is the dry stem with no crossover phase. Switching either on or off glides over a few milliseconds.
  - Stems only: the delay's echoes and the reverb are unchanged.
- **Tests:** speaker tests (which mute the program while they play), and **stereo passthrough** as a troubleshooting fallback.

## 7. The Spatial Mixer window

- **Opening:** View → **Spatial Mixer** (Ctrl+Shift+S).
  - The first time, it goes full screen on a second screen if there is one.
  - After that it remembers its screen, size and full-screen state. It reopens at start-up if it was open when Mixxx quit.
  - DJ keyboard shortcuts still work while it has focus.
- **Room view:**
  - Each stem is a dot: colour = stem, ring = deck (deck 1 tan, deck 2 teal), letter D / I / V.
  - Arcs show its width (L / R), with live speaker glow and an LF meter.
  - Dashed lines show auto-pan paths; ECHO marks where the delay is placing echoes.
- **Touch:**
  - Drag a dot: angle = pan, distance from the centre = spread. With auto-pan, dragging turns the shape.
  - Double-tap a dot to toggle Front.
  - Every finger drags its own dot or fader, so several can move at once.
- **Deck strips:** volume, pan, width, spread, FX send, AUTO with A / B, offset, FRONT, and ALL FRONT per deck.
  - Faders are relative, so touching never makes them jump.
  - Double-tap a fader to reset it.
- **Tabs:** Auto-pan A, Auto-pan B, Spatial Delay, Spatial Reverb.
- **Style:** LateNight PaleMoon colours and Open Sans; touch targets of at least 40 px.

### Basic and Advanced layouts

**BASIC / ADVANCED** in the top bar switch layouts. The choice is remembered.
- **Advanced** is every setting: the deck strips and the AUTO-PAN A / B and SPATIAL FX tabs.
- **Basic** is the room view plus eight columns laid out like the **Novation Launch Control XL MK3**. Each column has 3 knobs, 1 fader and 2 buttons, and is the mapping spec for the controller:

| Column | Knob 1 | Knob 2 | Knob 3 | Fader | Button 1 | Button 2 |
|---|---|---|---|---|---|---|
| 1 AUTO-PAN | `shape` | `wave` | `beats` | `size` | `direction` (reverse) | `[Spatial],autopan_focus` (A / B) |
| 2-4 deck 1 DRUMS / INST / VOCAL, 5-7 deck 2 | `spatial_azimuth` (pan) | `spatial_width` | `spatial_fx_send` | `volume` | `spatial_autopan` | `spatial_front` |
| 8 SPATIAL FX | `[SpatialFX],space` | `[SpatialFX],echo` | `[SpatialFX],tone` | `[SpatialFX],level` | `[SpatialDelay],enabled` | `[SpatialReverb],enabled` |

- **PRESETS** at the foot of the AUTO-PAN and SPATIAL FX columns opens that tool's preset list; tap one to load it. Auto-pan presets load onto the unit the column is playing. These buttons are touch only, not on the controller.
- **Unit colours:** auto-pan unit **A is green and B is orange** everywhere: the AUTO-PAN B tab, every B button, the AUTO button (which follows the stem's unit), the Basic auto-pan column (it changes colour with A / B), and the paths in the room view.

Notes on the columns:
- **Stem columns:**
  - The stem groups are `[ChannelN_Stem1]` (DRUMS), `[ChannelN_Stem3]` (INST) and `[ChannelN_Stem4]` (VOCAL).
  - The A / B choice of auto-pan unit stays on the touch screen (`spatial_autopan_unit`).
- **Auto-pan column:**
  - It plays unit A or B (`[SpatialAutopan1]` / `[SpatialAutopan2]`), chosen by `autopan_focus`. On the controller, the column's last button switches between them.
  - SHAPE, WAVE and BEATS are stepped knobs; the window draws the shape and wave on them.
- **SPATIAL FX column:**
  - The **macros** play delay and reverb together, like an effect unit's meta knob:
    - **SPACE:** reverb decay, size and pre-delay, from a small room to an endless hall.
    - **ECHO:** delay feedback, and how much of it goes into the reverb.
    - **TONE:** dark to bright; it lifts the delay's tone and lowers the reverb's damping together.
  - At the middle (shown as 0) a macro leaves the settings exactly as set or as the preset loaded them; turning it pushes them towards their extremes.
  - **LEVEL** is the whole unit's wet level, like an effect unit's mix.
  - **Presets and macros:** loading an FX preset re-centres the macros. Saving one first writes what the macros are doing into the settings, so the preset sounds as heard.
  - The macros are not saved between sessions.

## 8. Trying it on the XR18

1. **Assign the outputs.** In Preferences → Sound Hardware, choose the XR18 on ASIO. Set Spatial → Channels 1–7 and Headphones → 9–10.
2. **Walk the ring.** In Preferences → Spatial Audio, set 6 speakers with LF on, and walk the ring with the amps low.
3. **Play a stem track.** Load a `.stem.mp4` and play it. It starts as the original stereo image on the front pair.
4. **Open the Spatial Mixer and try things:**
   - Drag the vocal dot around the room.
   - Turn on AUTO for the drums with unit A on a 4-corner square wave at 1 beat.
   - Send the vocal to FX and try the delay on dotted 1/8 with POLY at 1 beat, then the reverb's Vocal Space preset.
5. **Check the mixer still drives the room.** The Reloop's EQ, filter, faders, crossfader and FX should all act on the spatial mix.

## 9. Known limits and next steps

- **Not yet tested on hardware or the touch monitor:** the frame rate and multi-touch feel.
- **LCXL MK3 mapping** (phase 3) waits for the controller.
- **Granulizer** (phase 6) is not started.
- **Main-bus effects and talkover ducking** don't reach the spatial output.
- **CPU:** each deck's effects run 4× (stereo plus 3 stems). Reverb-type FX units cost the most, but the cost should be fine on this laptop.
- **Club hardening** (phase 7) is still to do: small-buffer profiling and a 4-hour soak.
