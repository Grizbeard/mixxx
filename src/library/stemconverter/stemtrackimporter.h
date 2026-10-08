#pragma once

#include <optional>

#include "audio/frame.h"
#include "track/track_decl.h"

namespace mixxx::stemconverter {

/// How far the stem file's audio is shifted against the source track, in
/// frames at the source's sample rate: content at frame f of the source is at
/// frame f + offset of the stem file. Measured by cross-correlating a window
/// of both, decoded the way Mixxx decodes them for playback, so it absorbs
/// any difference between stemforge's and Mixxx's decoder delay handling
/// (MP3 encoder delay in particular).
///
/// Returns nullopt if either file cannot be opened, the sample rates differ,
/// or no confident match is found.
std::optional<audio::FrameDiff_t> measureOffset(
        const TrackPointer& pSource,
        const TrackPointer& pStem);

struct CarryOverResult {
    bool beats = false;
    int cues = 0;
    bool key = false;
    bool replayGain = false;
};

/// Copy what the DJ prepared on the source track (beat grid and BPM lock,
/// key, main cue, hotcues, loops, colour, rating, comment) onto the stem
/// track, shifted by `offset` frames. Fields the stem track already has are
/// left alone, so re-converting never overwrites edits made on the stem
/// version.
///
/// stemforge scales all streams down together when a stem would clip;
/// `headroomGainDb` is that gain, and the source's ReplayGain is raised by
/// the same amount so both versions play equally loud. When it is unknown the
/// ReplayGain is not copied and the analyser measures the stem file instead.
CarryOverResult carryOverMetadata(
        const Track& source,
        Track* pStem,
        audio::FrameDiff_t offset,
        std::optional<double> headroomGainDb);

} // namespace mixxx::stemconverter
