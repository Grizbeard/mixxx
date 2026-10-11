#pragma once

#include "audio/signalinfo.h"


namespace mixxx {
static constexpr audio::ChannelCount kEngineChannelOutputCount =
        audio::ChannelCount::stereo();
static constexpr audio::ChannelCount kMaxEngineChannelInputCount =
        audio::ChannelCount::stem();
// The following constant is always defined as it used for the waveform data
// struct, which must stay consistent, whether the STEM feature is enabled or
// not.
constexpr int kMaxSupportedStems = 4;
// Spatial Mixxx presents three stems: Drums, INST and Vocals. The engine
// folds the file's Bass stem (2) into Other (3), which becomes INST, so the
// Other stem's controls cover both; waveforms show three lanes.
constexpr int kFoldedStem = 1;     // Bass, 0-based
constexpr int kInstrumentStem = 2; // Other = INST, 0-based
constexpr int kDisplayedStems = 3;
/// The waveform lane of a stem (0-based) other than the folded one.
constexpr int displayedLaneOfStem(int stemIdx) {
    return stemIdx < kFoldedStem ? stemIdx : stemIdx - 1;
}
#ifdef __STEM__
enum class StemChannel {
    First = 0x1,
    Second = 0x2,
    Third = 0x4,
    Fourth = 0x8,

    None = 0,
    All = First | Second | Third | Fourth
};
Q_DECLARE_FLAGS(StemChannelSelection, StemChannel);
#endif

// Contains the information needed to process a buffer of audio
class EngineParameters final {
  public:
    SINT framesPerBuffer() const {
        return m_framesPerBuffer;
    }
    SINT samplesPerBuffer() const {
        return m_outputSignal.frames2samples(framesPerBuffer());
    }

    audio::ChannelCount channelCount() const {
        return m_outputSignal.getChannelCount();
    }

    audio::SampleRate sampleRate() const {
        return m_outputSignal.getSampleRate();
    }

    explicit EngineParameters(
            audio::SampleRate sampleRate,
            SINT framesPerBuffer)
            : m_outputSignal(
                      kEngineChannelOutputCount,
                      sampleRate),
              m_framesPerBuffer(framesPerBuffer) {
        DEBUG_ASSERT(framesPerBuffer > 0);
        DEBUG_ASSERT(sampleRate > 0);
    }

  private:
    const audio::SignalInfo m_outputSignal;
    const SINT m_framesPerBuffer;
};
}
