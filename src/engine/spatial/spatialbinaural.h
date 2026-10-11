#pragma once

#include <array>
#include <vector>

#include "engine/spatial/spatialdsp.h"

namespace mixxx::spatial {

/// A peaking / shelving biquad (RBJ cookbook), as headphone EQ presets
/// specify them.
struct EqBiquad {
    enum class Type {
        Peak,
        LowShelf,
        HighShelf,
    };
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    void set(Type type, double frequency, double gainDb, double q, double sampleRate);
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/// Headphone monitoring of the speaker ring: each speaker becomes a virtual
/// source around the listener's head, rendered with a spherical-head model
/// (Brown & Duda 1998: interaural delay and the head's high-frequency
/// shadow), a gentle rear (pinna) darkening so sounds behind stay behind,
/// and a few early reflections for distance, so the mix is heard outside the
/// head rather than between the ears.
///
/// Optionally corrects the Sennheiser HD 280 PRO towards the Harman target
/// with AutoEq's parametric EQ (oratory1990 measurement).
class SpatialBinaural {
  public:
    static constexpr int kMaxFrames = 8192;

    SpatialBinaural();

    /// `pSpeakers` holds layout.count() planar full-range speaker feeds;
    /// writes `frames` interleaved stereo frames to `pOut`.
    void process(const std::array<const float*, kMaxSpeakers>& speakers,
            const SpeakerLayout& layout,
            int frames,
            double sampleRate,
            double room,
            bool headphoneEq,
            float* pOut);

  private:
    struct Ear {
        double delay = 0; // samples
        // One-pole/one-zero head shadow.
        double b0 = 1, b1 = 0, a1 = 0;
        double x1 = 0, y1 = 0;
        double filter(double x) {
            const double y = b0 * x + b1 * x1 - a1 * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };
    struct Speaker {
        DelayLine line;
        Ear ears[2];
        OnePole rearSplit;
        double rearCut = 0; // high-band gain change for sources behind
    };

    void configure(const SpeakerLayout& layout, double sampleRate);

    std::array<Speaker, kMaxSpeakers> m_speakers;
    DelayLine m_reflections;
    OnePole m_reflectionTone;
    std::array<EqBiquad, 10> m_eq[2];
    double m_rate = 0;
    std::array<double, kMaxSpeakers> m_azimuths{};
    int m_count = 0;
};

} // namespace mixxx::spatial
