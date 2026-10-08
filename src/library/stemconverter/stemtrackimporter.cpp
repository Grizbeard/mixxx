#include "library/stemconverter/stemtrackimporter.h"

#include <cmath>
#include <vector>

#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/keys.h"
#include "track/track.h"
#include "util/logger.h"
#include "util/samplebuffer.h"

namespace mixxx::stemconverter {

namespace {

const Logger kLogger("StemTrackImporter");

// Window that is cross-correlated, and the largest shift searched either way.
// MP3 decoder delays are 529-2257 frames, so 4096 (93 ms at 44.1 kHz) is
// comfortably wide.
constexpr SINT kWindowFrames = 16384;
constexpr SINT kMaxLagFrames = 4096;
// Below this normalised correlation the match is not trusted.
constexpr double kMinCorrelation = 0.9;

std::vector<double> readMono(AudioSource& source, SINT first, SINT frames) {
    const int channels = source.getSignalInfo().getChannelCount();
    SampleBuffer buffer(frames * channels);
    const auto read = source.readSampleFrames(WritableSampleFrames(
            IndexRange::forward(first, frames),
            SampleBuffer::WritableSlice(buffer)));
    std::vector<double> mono(frames, 0.0);
    const SINT readFrames = read.frameIndexRange().length();
    const SINT skip = read.frameIndexRange().start() - first;
    for (SINT i = 0; i < readFrames && skip + i < frames; ++i) {
        double sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
            sum += read.readableData()[i * channels + ch];
        }
        mono[skip + i] = sum;
    }
    return mono;
}

double energy(const std::vector<double>& x, SINT first, SINT length) {
    double sum = 0;
    for (SINT i = first; i < first + length; ++i) {
        sum += x[i] * x[i];
    }
    return sum;
}

} // namespace

std::optional<audio::FrameDiff_t> measureOffset(
        const TrackPointer& pSource,
        const TrackPointer& pStem) {
    AudioSource::OpenParams params;
    params.setChannelCount(audio::ChannelCount::stereo());
    // For a stem file a stereo request yields the sum of the stems, which
    // stemforge makes equal to the main mix.
    const AudioSourcePointer pSourceAudio = SoundSourceProxy(pSource).openAudioSource(params);
    const AudioSourcePointer pStemAudio = SoundSourceProxy(pStem).openAudioSource(params);
    if (!pSourceAudio || !pStemAudio) {
        kLogger.warning() << "Cannot open" << (pSourceAudio ? "stem file" : "source")
                          << "to measure the offset";
        return std::nullopt;
    }
    const auto closeBoth = [&] {
        pSourceAudio->close();
        pStemAudio->close();
    };
    if (pSourceAudio->getSignalInfo().getSampleRate() !=
            pStemAudio->getSignalInfo().getSampleRate()) {
        kLogger.info() << "Sample rates differ; not measuring the offset";
        closeBoth();
        return std::nullopt;
    }

    const IndexRange sourceRange = pSourceAudio->frameIndexRange();
    const IndexRange stemRange = pStemAudio->frameIndexRange();
    const SINT rate = pSourceAudio->getSignalInfo().getSampleRate();
    const SINT lo = std::max(sourceRange.start(), stemRange.start()) + kMaxLagFrames;
    const SINT hi = std::min(sourceRange.end(), stemRange.end()) - kWindowFrames - kMaxLagFrames;
    if (hi <= lo) {
        closeBoth();
        return std::nullopt;
    }

    // Try a few positions in case one of them is silent.
    for (const SINT wanted : {30 * rate, (hi - lo) / 3 + lo, (hi - lo) / 2 + lo}) {
        const SINT first = std::clamp(wanted, lo, hi);
        const std::vector<double> x = readMono(*pSourceAudio, first, kWindowFrames);
        const std::vector<double> y = readMono(
                *pStemAudio, first - kMaxLagFrames, kWindowFrames + 2 * kMaxLagFrames);
        const double xEnergy = energy(x, 0, kWindowFrames);
        if (xEnergy < 1e-6 * kWindowFrames) {
            continue;
        }
        double best = -1;
        SINT bestLag = 0;
        // Energy of the y window under the current lag, slid along with it.
        double yEnergy = energy(y, 0, kWindowFrames);
        for (SINT lag = -kMaxLagFrames; lag <= kMaxLagFrames; ++lag) {
            const SINT offset = kMaxLagFrames + lag;
            if (lag > -kMaxLagFrames) {
                const double out = y[offset - 1];
                const double in = y[offset + kWindowFrames - 1];
                yEnergy += in * in - out * out;
            }
            double dot = 0;
            for (SINT i = 0; i < kWindowFrames; ++i) {
                dot += x[i] * y[offset + i];
            }
            const double norm = yEnergy > 0 ? dot / std::sqrt(xEnergy * yEnergy) : 0;
            if (norm > best) {
                best = norm;
                bestLag = lag;
            }
        }
        if (best >= kMinCorrelation) {
            kLogger.info() << "Stem file offset" << bestLag << "frames, correlation" << best;
            closeBoth();
            return static_cast<audio::FrameDiff_t>(bestLag);
        }
        kLogger.info() << "No confident match at frame" << first << "(best" << best << ")";
    }
    closeBoth();
    return std::nullopt;
}

CarryOverResult carryOverMetadata(
        const Track& source,
        Track* pStem,
        audio::FrameDiff_t offset,
        std::optional<double> headroomGainDb) {
    CarryOverResult result;
    const auto shifted = [offset](audio::FramePos pos) {
        return pos.isValid() ? pos + offset : pos;
    };

    // Beat grid: only when both run at the same rate, since a grid is
    // expressed in frames of its own rate. Otherwise the analyser makes one.
    const BeatsPointer pBeats = source.getBeats();
    if (pBeats && !pStem->getBeats() &&
            pBeats->getSampleRate() == pStem->getSampleRate()) {
        BeatsPointer pShifted = pBeats;
        if (offset != 0) {
            pShifted = pBeats->tryTranslate(offset).value_or(nullptr);
        }
        if (pShifted && pStem->trySetBeats(pShifted)) {
            pStem->setBpmLocked(source.isBpmLocked());
            result.beats = true;
        }
    }

    if (pStem->getKeys().getGlobalKey() == track::io::key::INVALID &&
            source.getKeys().getGlobalKey() != track::io::key::INVALID) {
        pStem->setKeys(source.getKeys());
        result.key = true;
    }

    // Cues: copied only onto a stem track that has none of its own yet.
    bool stemHasCues = false;
    for (const CuePointer& pCue : pStem->getCuePoints()) {
        if (pCue->getType() != CueType::N60dBSound) {
            stemHasCues = true;
            break;
        }
    }
    if (!stemHasCues) {
        for (const CuePointer& pCue : source.getCuePoints()) {
            const CueType type = pCue->getType();
            if (type == CueType::Invalid || type == CueType::N60dBSound ||
                    type == CueType::MainCue) {
                continue; // N60dBSound is re-analysed; MainCue is set below.
            }
            CuePointer pNew = pStem->createAndAddCue(type,
                    pCue->getHotCue(),
                    shifted(pCue->getPosition()),
                    shifted(pCue->getEndPosition()),
                    pCue->getColor());
            if (pNew) {
                pNew->setLabel(pCue->getLabel());
                ++result.cues;
            }
        }
        const audio::FramePos mainCue = source.getMainCuePosition();
        if (mainCue.isValid()) {
            pStem->setMainCuePosition(shifted(mainCue));
        }
    }

    if (!pStem->getColor() && source.getColor()) {
        pStem->setColor(source.getColor());
    }
    if (pStem->getRating() == 0 && source.getRating() > 0) {
        pStem->setRating(source.getRating());
    }
    if (pStem->getComment().isEmpty() && !source.getComment().isEmpty()) {
        pStem->setComment(source.getComment());
    }

    // Undo stemforge's anti-clipping gain so both versions play equally loud.
    const ReplayGain sourceGain = source.getReplayGain();
    if (headroomGainDb && sourceGain.hasRatio() && !pStem->getReplayGain().hasRatio()) {
        const double headroom = std::pow(10.0, *headroomGainDb / 20.0);
        ReplayGain gain = sourceGain;
        gain.setRatio(sourceGain.getRatio() / headroom);
        if (sourceGain.hasPeak()) {
            gain.setPeak(static_cast<CSAMPLE>(sourceGain.getPeak() * headroom));
        }
        pStem->setReplayGain(gain);
        result.replayGain = true;
    }
    return result;
}

} // namespace mixxx::stemconverter
