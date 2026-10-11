#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "audio/types.h"
#include "engine/spatial/spatialdsp.h"
#include "engine/spatial/spatialsource.h"
#include "soundio/soundmanagerutil.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class ControlObject;
class ControlPotmeter;
class ControlPushButton;
struct GroupFeatureState;

namespace mixxx::spatial {
class SpatialAutopan;
class SpatialBinaural;
class SpatialFxMetaControls;
class SpatialDelay;
class SpatialReverb;
} // namespace mixxx::spatial

/// The multichannel "Spatial" output and the spatial mix bus feeding it.
///
/// Each callback EngineMixer starts the bus (beginCallback), adds every deck
/// stem as a post-fader stereo source (addSource), adds anything without a
/// position (samplers, mics, aux) to the front pair (addFront), then calls
/// process(), which runs the Spatial FX unit, the master gain, the LF
/// crossover, per-speaker trims and delays and the limiter, and writes the
/// interleaved output: speakers first, then the LF feed.
///
/// Phase 1 diagnostics remain: a speaker test signal (which mutes the
/// program while it plays) and a main-mix passthrough that replaces the
/// spatial mix with the stereo main on the front pair.
///
/// Controls, group [Spatial]:
///   channel_count, channel_base  read-only: the connected output group
///   speaker_count     4-6 speakers on the ring (persisted)
///   layout_custom     0 = default angles for the count, 1 = speaker_N_azimuth
///   speaker_N_azimuth / _gain_db / _delay_ms   N = 1..6 (persisted)
///   lf_enabled        the channel after the speakers carries LF (persisted)
///   lf_crossover_hz   LR4 crossover, 40-200 Hz (persisted)
///   mains_highpass    high-pass the speakers at the crossover (persisted)
///   lf_gain_db, lf_delay_ms, lf_invert   LF trims (persisted)
///   limiter_enabled, limiter_ceiling_db  linked output limiter (persisted)
///   limiter_reduction_db   read-only: current gain reduction
///   vu_meter_N        read-only: peak of output channel N (1-based)
///   beat              read-only: the spatial clock, in beats
///   test_channel, test_signal, test_frequency, test_level_db,
///   main_passthrough  as in phase 1
///   headphone_spatial  headphones hear the spatial mix, rendered binaurally,
///                      in place of the stereo main (the cue/mix knob still
///                      blends it with the cue); works without a Spatial
///                      output assigned (persisted)
///   headphone_room     early reflections for an outside-the-head image 0..1
///   headphone_eq       Sennheiser HD 280 PRO correction (AutoEq) (persisted)
///   autopan_focus      which auto-pan unit the Basic layout and a controller's
///                      auto-pan column play: 0 = A, 1 = B
/// The Spatial FX macros live in [SpatialFX] (see SpatialFxMetaControls).
class EngineSpatialOutput : public AudioSource {
  public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kMinSpeakers = mixxx::spatial::kMinSpeakers;
    static constexpr int kMaxSpeakers = mixxx::spatial::kMaxSpeakers;
    static constexpr int kMaxDecks = 4;
    /// Drums, Instruments, Vocals.
    static constexpr int kSlots = 3;
    static constexpr int kAutopanUnits = 2;
    static constexpr double kMaxAlignmentSeconds = 0.03;

    /// The control group of a source: deck 0-based, slot 0 drums
    /// ([ChannelN_Stem1]), 1 INST ([ChannelN_Stem3], Other with the bass
    /// folded in), 2 vocals ([ChannelN_Stem4]).
    static QString sourceGroup(int deck, int slot);

    EngineSpatialOutput();
    ~EngineSpatialOutput() override;

    std::span<const CSAMPLE> buffer(const AudioOutput& output) const override;
    void onOutputConnected(const AudioOutput& output) override;
    void onOutputDisconnected(const AudioOutput& output) override;

    bool isConnected() const {
        return m_channels.load(std::memory_order_relaxed) > 0;
    }
    /// Rendering the spatial mix (not the passthrough): for a connected
    /// Spatial output, headphone monitoring, or both.
    bool isRendering() const;
    /// Headphones should hear the binaural spatial mix.
    bool isHeadphoneMonitoring() const;
    /// The binaural spatial mix of the last process(), interleaved stereo;
    /// nullptr if none was rendered.
    const CSAMPLE* binaural() const {
        return m_binauralValid ? m_binaural.data() : nullptr;
    }

    /// Engine thread: start a callback. `pTempo` holds the tempo source's
    /// beat length and phase; without it the clock runs at `fallbackBpm`.
    void beginCallback(std::size_t frames,
            mixxx::audio::SampleRate sampleRate,
            const GroupFeatureState* pTempo,
            double fallbackBpm);
    /// Engine thread: a post-fader stereo source of `frames` frames.
    void addSource(int deck, int slot, const CSAMPLE* pStereo);
    /// Engine thread: stereo content without a position, on the front pair.
    void addFront(const CSAMPLE* pStereo);
    /// Engine thread: the master gain, ramped from the previous callback.
    void setMainGain(CSAMPLE_GAIN gain) {
        m_mainGain = gain;
    }

    /// Engine thread, once per callback, after the main mix is complete.
    /// pMain is the interleaved stereo main mix of `frames` frames.
    void process(const CSAMPLE* pMain, std::size_t frames, mixxx::audio::SampleRate sampleRate);

  private:
    struct Limiter {
        std::vector<float> delay; // interleaved lookahead, kMaxChannels wide
        std::vector<float> windowValue;
        std::vector<int> windowIndex;
        int head = 0;
        int tail = 0;
        int position = 0;
        float gain = 1;
    };

    CSAMPLE nextTestSample(double sampleRate);
    void updateLayout();
    void updateFilters(double sampleRate);
    void renderBus(std::size_t frames, int speakers, int lfChannel, int channels);
    void runLimiter(CSAMPLE* pOut, std::size_t frames, int channels, double sampleRate);

    mixxx::SampleBuffer m_buffer;
    std::atomic<int> m_channels{0};

    std::unique_ptr<ControlObject> m_pChannelCount;
    std::unique_ptr<ControlObject> m_pChannelBase;
    std::unique_ptr<ControlPotmeter> m_pSpeakerCount;
    std::unique_ptr<ControlPushButton> m_pLayoutCustom;
    std::array<std::unique_ptr<ControlPotmeter>, kMaxSpeakers> m_pSpeakerAzimuth;
    std::array<std::unique_ptr<ControlPotmeter>, kMaxSpeakers> m_pSpeakerGainDb;
    std::array<std::unique_ptr<ControlPotmeter>, kMaxSpeakers> m_pSpeakerDelayMs;
    std::unique_ptr<ControlPushButton> m_pLfEnabled;
    std::unique_ptr<ControlPotmeter> m_pLfCrossover;
    std::unique_ptr<ControlPushButton> m_pMainsHighpass;
    std::unique_ptr<ControlPotmeter> m_pLfGainDb;
    std::unique_ptr<ControlPotmeter> m_pLfDelayMs;
    std::unique_ptr<ControlPushButton> m_pLfInvert;
    std::unique_ptr<ControlPushButton> m_pLimiterEnabled;
    std::unique_ptr<ControlPotmeter> m_pLimiterCeilingDb;
    std::unique_ptr<ControlObject> m_pLimiterReduction;
    std::array<std::unique_ptr<ControlObject>, kMaxChannels> m_pVuMeter;
    std::unique_ptr<ControlObject> m_pBeat;
    std::unique_ptr<ControlObject> m_pTestChannel;
    std::unique_ptr<ControlPushButton> m_pTestSignal;
    std::unique_ptr<ControlPotmeter> m_pTestFrequency;
    std::unique_ptr<ControlPotmeter> m_pTestLevelDb;
    std::unique_ptr<ControlPushButton> m_pMainPassthrough;
    std::unique_ptr<ControlPushButton> m_pHeadphoneSpatial;
    std::unique_ptr<ControlPotmeter> m_pHeadphoneRoom;
    std::unique_ptr<ControlPushButton> m_pHeadphoneEq;

    std::vector<std::unique_ptr<mixxx::spatial::SpatialSource>> m_sources;
    std::array<std::unique_ptr<mixxx::spatial::SpatialAutopan>, kAutopanUnits> m_autopans;
    std::array<const mixxx::spatial::SpatialAutopan*, kAutopanUnits> m_autopanPointers{};
    std::unique_ptr<mixxx::spatial::SpatialDelay> m_pDelay;
    std::unique_ptr<mixxx::spatial::SpatialReverb> m_pReverb;
    std::unique_ptr<mixxx::spatial::SpatialBinaural> m_pBinaural;
    std::unique_ptr<mixxx::spatial::SpatialFxMetaControls> m_pFxMeta;
    std::unique_ptr<ControlPushButton> m_pAutopanFocus;
    std::vector<float> m_binaural;
    bool m_binauralValid = false;

    // Engine-thread state.
    mixxx::spatial::SpeakerLayout m_layout;
    std::array<double, kMaxSpeakers> m_layoutAzimuths{};
    int m_layoutCount = 0;
    mixxx::spatial::SpatialClock m_clock;
    bool m_begun = false;
    std::size_t m_frames = 0;
    double m_sampleRate = 0;
    std::vector<bool> m_rendered;
    // Planar bus: speakers, LF, stereo send, reverb input.
    std::array<std::vector<float>, kMaxSpeakers> m_bus;
    std::vector<float> m_lf;
    std::vector<float> m_sendLeft;
    std::vector<float> m_sendRight;
    std::vector<float> m_reverbIn;
    CSAMPLE_GAIN m_mainGain = 1;
    CSAMPLE_GAIN m_mainGainOld = 1;

    std::array<mixxx::spatial::LinkwitzRiley4, kMaxSpeakers> m_highPass;
    mixxx::spatial::LinkwitzRiley4 m_lowPass;
    std::array<mixxx::spatial::DelayLine, kMaxSpeakers + 1> m_alignment;
    std::array<float, kMaxSpeakers + 1> m_trimGain{};
    Limiter m_limiter;
    std::array<float, kMaxChannels> m_vuPeak{};

    int m_testActiveChannel = -1; // channel the test is fading on
    CSAMPLE_GAIN m_testGain = 0;  // current test gain, ramped per callback
    double m_sinePhase = 0;
    uint32_t m_noiseState = 0x12345678u;
    double m_pink[7] = {};
    mixxx::spatial::LinkwitzRiley4 m_testLowPass; // LF test: band-limited like a sub feed
    mixxx::spatial::LinkwitzRiley4 m_passLowPass; // passthrough LF feed
    double m_filterRate = 0;
    double m_filterCrossover = 0;
    CSAMPLE_GAIN m_passGain = 0;    // passthrough fade
    CSAMPLE_GAIN m_programGain = 0; // spatial mix fade (off during tests)
};
