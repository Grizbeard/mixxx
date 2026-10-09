#pragma once

#include <atomic>
#include <memory>

#include "audio/types.h"
#include "soundio/soundmanagerutil.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class ControlObject;
class ControlPotmeter;
class ControlPushButton;

/// The multichannel "Spatial" output: one interleaved buffer for a group of
/// up to kMaxChannels sound-card channels. The speakers come first, then the
/// LF (sub) feed when it is enabled; any further channels stay silent.
///
/// Phase 1 of the spatial build (docs/01-engine-audit.md): this produces
/// silence, a speaker test signal for one channel at a time, and optionally a
/// passthrough of the main mix to the front pair (speakers 1 and 2) plus a
/// low-passed mono LF feed, so a room can be checked and played through
/// before the spatial renderer exists.
///
/// Controls, group [Spatial]:
///   channel_count     read-only: channels of the connected output, 0 if none
///   channel_base      read-only: its first sound-card channel (0-based)
///   speaker_count     4-6 speakers on the ring (persisted)
///   lf_enabled        the channel after the speakers carries LF (persisted)
///   lf_crossover_hz   LF low-pass corner for the passthrough (persisted)
///   test_channel      -1 = off, else the channel (0-based) playing the test
///   test_signal       0 = pink noise, 1 = sine
///   test_frequency    sine frequency in Hz
///   test_level_db     test level in dBFS (persisted), clamped to -60..-6
///   main_passthrough  play the main mix on speakers 1-2 and LF (persisted)
class EngineSpatialOutput : public AudioSource {
  public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kMinSpeakers = 4;
    static constexpr int kMaxSpeakers = 6;

    EngineSpatialOutput();
    ~EngineSpatialOutput() override;

    std::span<const CSAMPLE> buffer(const AudioOutput& output) const override;
    void onOutputConnected(const AudioOutput& output) override;
    void onOutputDisconnected(const AudioOutput& output) override;

    bool isConnected() const {
        return m_channels.load(std::memory_order_relaxed) > 0;
    }

    /// Engine thread, once per callback, after the main mix is complete.
    /// pMain is the interleaved stereo main mix of `frames` frames.
    void process(const CSAMPLE* pMain, std::size_t frames, mixxx::audio::SampleRate sampleRate);

  private:
    /// Second-order Butterworth low-pass; two in series give the
    /// Linkwitz-Riley 4th order used for the LF feed.
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        void setLowPass(double frequency, double sampleRate);
        double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    CSAMPLE nextTestSample(double sampleRate);

    mixxx::SampleBuffer m_buffer;
    std::atomic<int> m_channels{0};

    std::unique_ptr<ControlObject> m_pChannelCount;
    std::unique_ptr<ControlObject> m_pChannelBase;
    std::unique_ptr<ControlPotmeter> m_pSpeakerCount;
    std::unique_ptr<ControlPushButton> m_pLfEnabled;
    std::unique_ptr<ControlPotmeter> m_pLfCrossover;
    std::unique_ptr<ControlObject> m_pTestChannel;
    std::unique_ptr<ControlPushButton> m_pTestSignal;
    std::unique_ptr<ControlPotmeter> m_pTestFrequency;
    std::unique_ptr<ControlPotmeter> m_pTestLevelDb;
    std::unique_ptr<ControlPushButton> m_pMainPassthrough;

    // Engine-thread state.
    int m_testActiveChannel = -1; // channel the test is fading on
    CSAMPLE_GAIN m_testGain = 0;  // current test gain, ramped per callback
    double m_sinePhase = 0;
    uint32_t m_noiseState = 0x12345678u;
    double m_pink[7] = {};
    Biquad m_testLowPass[2];   // LF test signal: band-limited like a sub feed
    Biquad m_passLowPass[2];   // passthrough LF feed
    double m_filterRate = 0;
    double m_filterCrossover = 0;
    CSAMPLE_GAIN m_passGain = 0; // passthrough fade
};
