# Crate → Stems Converter — Design

*Draft 2026-10-07. Builds on the decisions in `01-engine-audit.md` §8-10.*

## Goal

Select a crate in Mixxx and choose **Convert to Stems…**. Every track in that crate and its subcrates is separated into a `.stem.mp4` that Mixxx 2.6 plays as a stem deck. The results land in the library as a mirrored crate tree. Separation settings are exposed as editable presets.

## Constraints that shape the design

- **Mixxx's stem file contract** (`src/sources/soundsourcestem.cpp`, `src/track/steminfoimporter.cpp`):
  - The file needs **5 stereo audio streams**: main mix, then exactly 4 stems, all stems with the same codec and sample rate.
  - It needs a `.stem.mp4`/`.stem.m4a` extension.
  - It needs a JSON manifest at `moov/udta/stem` (names + colours); without it the file is not treated as a stem file.
  - No non-audio stream (for example cover art as a video stream) may come before the audio streams, or the stem mask indexes shift. Cover art therefore goes in the `covr` metadata atom, not in a stream.
- **Hardware:** an RTX 500 Ada Laptop GPU with **4 GB VRAM**. Big RoFormer models need small segment/batch sizes to fit, and every track has to be able to fall back on out-of-memory (smaller segments, then CPU) instead of failing. This is a main reason the settings must be tweakable.
- **Live safety:** separation saturates the GPU and loads the CPU. The converter must never compete with the audio engine during a set.

## Architecture

```
Mixxx (C++, spatial/2.6)                          stemforge (Python, separate process)
───────────────────────────                       ─────────────────────────────────────
Crate menu "Convert to Stems…" ─┐
Track menu "Convert to Stems"  ─┤  job.json
                                ▼  (tracks, preset, output paths)
StemConverterManager ── QProcess ──────────────►  stemforge run job.json
  · queue (persisted)          ◄── JSON lines ──    per track:
  · pause while decks play         progress/done/     1 decode (ffmpeg → float, model SR)
  · below-normal priority          error/metrics      2 separation cascade (preset stages)
        │                                             3 residual "other" = mix − Σ stems
        ▼                                             4 optional resample (soxr VHQ), all streams
  on "done": add track to library                     5 encode 5× ALAC → .stem.mp4 (moov at end)
  · copy BPM/grid/key/cues/loops/                     6 inject moov/udta/stem manifest
    colour/rating from source                         7 tags + cover + provenance atom
  · place in mirrored crate tree                      8 verify: decode, Σstems vs original → dB
        │
        ▼
Sidebar "Stem Converter": queue, progress, failures/retry, per-track QA
Preferences "Stem Separation": env, device/VRAM profile, defaults, presets
```

**Why a separate Python process:**
- The best separation models and their tooling (`python-audio-separator`, which wraps RoFormer, MDX-C, Demucs and VR) are PyTorch/CUDA-based.
- A crash or out-of-memory in separation can't take down Mixxx.
- The same tool works from the command line for batch jobs.
- Mixxx only links Qt, so nothing GPU-related enters the audio engine's process.

## stemforge (Python CLI)

- **Location:** its own `uv` project at `Spatial Audio Mixxx/stemforge/`, with a pinned Python 3.12 and CUDA PyTorch.
- **Commands:**
  - `run <job.json>`: process a job. It prints JSON lines on stdout, e.g. `{"ev":"progress","track":3,"stage":"vocals","pct":41}`.
  - `doctor`: report the GPU, VRAM, CUDA, ffmpeg and model cache.
  - `models`: list available models with their stem types and parameters (feeds the preset editor).
  - `preset show|validate`.
- **Separation cascade (default).** Each stage runs on what earlier stages left behind:
  1. vocals from the mix
  2. drums from (mix − vocals)
  3. bass from (mix − vocals − drums)
  4. **other = mix − vocals − drums − bass**

  The residual guarantees that **Σ stems = original**. Separation errors land in "other" (Instruments) and are never lost or doubled. A stage can instead be set to read the original mix (`"input": "mix"`), for models that do better on the full mix.
- **Encoding:**
  - ALAC 16-bit by default, matching a CD-quality (16-bit / 44.1 kHz) collection; 24-bit optional; AAC for size, at the cost of exact summing.
  - With ALAC, "other" is taken again from the integers written, so Σ stems equals the main mix sample for sample.
  - Changing only the bit depth does not mark existing files out of date; `--force` re-converts them.
  - Sample rate: keep the source's, or 48 kHz to match the XR18.
  - Resampling is linear, so the sum property survives it.
  - The original master is stream 0.
- **Provenance atom** (`----:org.spatialmixxx:source`): source path, size, mtime, content hash, preset hash, stemforge version. This allows "skip if already converted and unchanged" and "re-convert if the preset changed", without a Mixxx database schema change.
- **Out-of-memory ladder** per stage: preset settings → halve segment → batch 1 → CPU. Each fallback is reported so the UI can show it.
- **QA metric:** after encoding, the file is decoded back and the residual error Σstems − original is reported in dB. The vocal and drum energy ratios are reported too, as a coarse "did separation do anything" check.

### Presets (the tweakable part)

JSON files in `<settings>/stemforge/presets/`, editable in Mixxx and on disk:

```json
{
  "name": "Quality (4 GB)",
  "stages": [
    {"stem": "vocals", "model": "<RoFormer vocal model>", "input": "residual",
     "params": {"segment_size": 256, "overlap": 4, "batch_size": 1, "fp16": true}},
    {"stem": "drums",  "model": "htdemucs_ft", "input": "residual",
     "params": {"segment": 7.8, "shifts": 1, "overlap": 0.25}},
    {"stem": "bass",   "model": "htdemucs_ft", "input": "residual",
     "params": {"segment": 7.8, "shifts": 1, "overlap": 0.25}}
  ],
  "other": "residual",
  "output": {"codec": "alac", "bit_depth": 16, "sample_rate": "source"},
  "manifest": {"names": ["Drums", "Bass", "Other", "Vocals"],
               "colors": ["#FD4A4A", "#FFFF00", "#00E8E8", "#AD65FF"]}
}
```

- **Shipped presets:**
  - *Fast*: HT-Demucs only, one pass for drums, bass and vocals.
  - *Quality (4 GB)*: RoFormer vocals + HT-Demucs drums and bass, tuned for 4 GB.
  - *Max*: larger overlap and shifts, slow.

  The concrete model names are chosen during phase 1 by benchmarking on this GPU.
- **Parameters exposed:** model per stage, input (mix/residual), segment size, overlap, shifts, batch size, fp16, test-time augmentation, plus a raw `extra` dict passed straight to the backend for anything not surfaced.
- **Stem slot order** stays Traktor/NI order — drums, bass, other, vocals — so files are portable. Mixxx's Instruments group (bass + other) is decided in the engine, not in the file.

## Mixxx integration (C++)

| Piece | Where | Notes |
|---|---|---|
| **"Convert to Stems…"** on crates | `cratefeature.cpp` context menu, next to "Analyze entire Crate" / "Export Track Files" | Collects tracks with `CrateStorage::collectDescendantCrateIds()` (nested-crate stack), de-duplicates. |
| **"Convert to Stems"** on tracks | `wtrackmenu.cpp` | Same job path for any selection (Serato/Rekordbox views included, once the tracks are in the library). |
| **Convert dialog** | new `dlgstemconvert` | Preset (+ Edit…), output root, mirror crate tree, include subcrates, skip unchanged, track count + estimated time. |
| **StemConverterManager** | new `src/library/stemconverter/` | One `QProcess` at a time. Persisted queue JSON in the settings dir. Pause/resume/cancel/retry. Optionally pauses while any deck is playing (default **on**). Process priority below normal. |
| **Library placement** | `TrackCollection` + `CrateStorage` | Adds finished files as tracks, then places them under a top-level crate **"Stems"** that mirrors the source crate tree (`Stems ▸ House ▸ 4am`, …). |
| **Metadata carry-over** | on import of each finished file | Copies BPM, beat grid, key, cues/hotcues/loops, colour, rating and comment from the source track. Positions are rescaled when the sample rate changed. |
| **Sidebar "Stem Converter"** | new `LibraryFeature` | Queue with per-track stage/progress, failures + log + retry, QA dB per track. Clicking a finished job shows its tracks. |
| **Preferences "Stem Separation"** | new `DlgPrefStemSeparation` | stemforge location, `doctor` output, device/VRAM profile, default preset, output root, codec/bit depth/sample rate, pause-while-playing, preset manager/editor. |

None of this touches the audio engine, and it needs **no database schema change**: the job state lives in a JSON file and the provenance lives in the stem files.

## Phases

| Phase | Deliverable | Exit criterion |
|---|---|---|
| **1** | `stemforge run` on a hand-written job; one preset; model benchmark on the 4 GB GPU | Output loads in Mixxx 2.6 as a 4-stem deck with names/colours; Σstems − original ≤ −90 dBFS; time per track known for each candidate model. |
| **2** | Presets, out-of-memory ladder, `doctor`/`models`, provenance + skip, QA metrics, JSON-lines protocol frozen | A 20-track job survives an induced out-of-memory and a kill/restart. |
| **3** | Mixxx: manager + crate/track menu + dialog + library placement + metadata carry-over | Converting a nested crate yields a mirrored `Stems` tree with grids/cues intact. |
| **4** | Sidebar feature + preferences page + preset editor | Settings are tweakable from Mixxx without touching JSON. |
| **5** | Tuning: A/B listening on club material, preset defaults, throughput | Agreed default preset. |

## Status (2026-10-08)

**Phases 1-3 done:** stemforge CLI plus the Mixxx crate integration. Phase 4 (sidebar feature, preferences page, in-app preset editor) and phase 5 (tuning) are next.

| Measured on the RTX 500 Ada (4 GB), 2:35 MP3 | Result |
|---|---|
| **Fast** (HT-Demucs, 1 pass) | 20 s total (separation 15 s) |
| **Quality (4 GB)** fp32 | 197 s (RoFormer 139 s, ~3.7 GB VRAM: at the limit, spills to shared memory) |
| **Quality (4 GB)** fp16 | **108 s** (RoFormer 69 s + Demucs-ft 33 s), 3.2 GB VRAM |
| Effect of fp16 on vocals | 27.9 dB vs 27.8 dB difference from an overlap-8 fp32 reference, i.e. none beyond the overlap setting |
| Σ stems − main mix in the file | −126 dBFS peak / −134 dBFS RMS (24-bit quantisation). Since "other" is taken from the written integers: exactly 0. |
| File size | 148 MB (5 × 24-bit ALAC). 16-bit, now the default, is about ⅔ of that. |

### Benchmark for trance/techno (2026-10-08)

Setup: separation time on a 5:48 track (Kevin Over, "Lavender"). SDR on Mixxx's trance fixture, which has true stems, scored as the three stems the Spatial build plays.

| Preset | Separation | Drums | Instruments | Vocals |
|---|---|---|---|---|
| Old Quality: RoFormer seg256 ov4 + Demucs-ft | ~220 s | 11.4 | 6.5 | −0.7 |
| RoFormer seg128 ov2 + Demucs-ft | 145 s | 11.5 | 6.6 | −0.6 |
| Demucs-ft (all four models) fp16 | 48 s | 11.4 | 6.5 | −0.1 |
| **Electronic: Demucs-ft drums+vocals specialists fp16, bass in Other** | **25 s** | 11.2 | 6.5 | −0.1 |
| Fast: Demucs fp32 | 19 s | 11.2 | 6.4 | +0.1 |
| Vocal tracks: RoFormer seg256 ov4 + drums specialist fp16 | 157 s | 11.6 | 6.6 | −0.7 |

- **The heavily processed "vocal" in the trance clip defeats every model (~0 dB).** RoFormer's advantage is on dry, prominent vocals, so it stays as the opt-in "Vocal tracks" preset.
- **This is one 12.8 s clip.** Re-check the Electronic vs Vocal choice by ear on your own tracks.

### Throughput work (2026-10-09): target < 5 s per minute of music

| Change | Effect |
|---|---|
| On AC power instead of battery | 11.4 → 7.5 s/min (the GPU and CPU are throttled on battery) |
| Direct Demucs runner instead of audio-separator (in-memory, models resident) | separation 5.6 → 3.5 s/min steady; GPU busy 72% → 96%; SDR identical |
| cudnn autotune off | −2 s on the first track, no steady-state change |
| Piped parallel ALAC encode, in-memory verify plus a 10 s decode spot-check | finalize 15 → 6-8 s per track (it overlaps the next separation) |
| `stemforge serve` + preload when the Convert dialog opens | removes ~6 s of torch/CUDA/model start-up per job |
| **Result** | **4-track crate 4.0 s/min; single 2.9-min track 4.9 s/min** |
| Optional "Electronic (fast)" preset (overlap 0.1) | about 15% faster again, −0.3 dB on drums |

These made no difference or made things worse: batching chunks (the GPU is already saturated at batch 1, and VRAM spills past batch 4), and native fp16 (some Demucs layers need fp32).

**GPU contention.** Any app holding VRAM (Illustrator took about 800 MB) pushes RoFormer at segment 256 over 4 GB. WDDM then spills to shared memory with no out-of-memory error: a 30 s clip took 148 s instead of 9 s. Segment 128 needs 1.9 GB. Optionally set NVIDIA Control Panel → *CUDA – Sysmem Fallback Policy* → *Prefer No Sysmem Fallback* for the stemforge Python, so that it raises out-of-memory and stemforge's fallback ladder steps down instead.

**Output drive.** The T7 reads at about 40 MB/s and writes at 3-12 MB/s uncached, i.e. a USB 2 link, even on the new cable. Copying a 260-270 MB file takes 25-60 s, longer than separating it. On USB 3.x the copy would take about a second.

**Findings that shaped the code:**

- **MP3 timing.** Mixxx's MP3 decoding and ffmpeg's disagree by exactly **1152 frames (one MP3 frame, 26 ms)**. The importer measures the offset per track by cross-correlation and shifts the cues and grid by it; without that every hotcue would flam.
- **Clipping headroom.** MP3 sources decode above 0 dBFS (+2 dB here), and stems can peak higher than the mix (drums +3.5 dB). ALAC is integer, so all streams are scaled down together (−3.35 dB here). The importer raises the ReplayGain by the same amount.
- **audio-separator normalisation.** It peak-normalises its input and outputs by default and rejects thresholds above 1.0. Inputs are pre-scaled to a 0.5 peak and the scaling is undone afterwards.
- **Windows stdin.** A pending read on the stdin pipe deadlocks every child process (DuplicateHandle). The command reader uses a private copy and children get NUL.
- **D: (T7 Shield over USB).** Write speed varied from 3.5 to 253 MB/s between tests. Files are built on the internal SSD and copied once.
- **Crate names are unique across the collection**, so mirrored crates are named `<name> (Stems)`.

**Filing rules** (the same for the crate menu and the track menu):

- A converted track goes into the mirror of each of its crates, and each mirror sits under the mirrors of that crate's parents: `Stems ▸ House (Stems) ▸ 4am (Stems)`.
- From the crate menu, "each of its crates" means the crates inside the converted subtree. From the track menu, it means every crate the track is in.
- A track in no crate goes into `Stems` itself.
- The output folder follows the track's deepest crate (ties: alphabetical path). That depends only on the track, so the same track always maps to the same file and is skipped next time, whichever menu it came from.

## Decisions (2026-10-07)

1. **Output location:** a dedicated root, default `D:\Stems\`, mirroring the crate path: `D:\Stems\House\4am\<file>.stem.mp4`.
2. **Library placement:** a mirrored crate tree under a top-level **"Stems"** crate. The original crates are not touched.
3. **Environment:** approved. `uv`-managed Python 3.12, torch 2.14.x + CUDA 13.0 wheels, `audio-separator` 0.47.x, installed in `stemforge/.venv`.
