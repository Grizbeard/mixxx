#include "engine/spatial/spatialbinaural.h"

#include <algorithm>

namespace mixxx::spatial {

namespace {

constexpr double kHeadRadius = 0.0875; // m
constexpr double kSpeedOfSound = 343;  // m/s
constexpr double kShadowMinAlpha = 0.1;
constexpr double kShadowMinAngle = 150; // degrees from the ear
constexpr double kEarAzimuth[2] = {-90, 90};
constexpr double kRearCutDb = 5;
constexpr double kRearSplitHz = 4000;
constexpr double kOutputGain = 0.7;
constexpr double kMaxSampleRate = 192000;
// Early reflections: per ear, two taps (ms, gain).
constexpr double kReflectionMs[2][2] = {{7.1, 16.9}, {11.3, 23.7}};
constexpr double kReflectionGain[2] = {0.30, 0.22};
constexpr double kReflectionToneHz = 6000;

/// AutoEq, oratory1990 measurement: Sennheiser HD 280 Pro to the Harman
/// over-ear target. Preamp -5.8 dB.
struct EqBand {
    EqBiquad::Type type;
    double frequency;
    double gainDb;
    double q;
};
constexpr double kHd280PreampDb = -5.8;
constexpr EqBand kHd280[10] = {
        {EqBiquad::Type::LowShelf, 105, -4.2, 0.70},
        {EqBiquad::Type::Peak, 475, -2.1, 0.42},
        {EqBiquad::Type::Peak, 3946, 5.9, 1.82},
        {EqBiquad::Type::Peak, 77, 4.7, 2.17},
        {EqBiquad::Type::Peak, 41, -3.0, 1.35},
        {EqBiquad::Type::HighShelf, 10000, 3.0, 0.70},
        {EqBiquad::Type::Peak, 126, -3.7, 3.71},
        {EqBiquad::Type::Peak, 178, 2.3, 3.81},
        {EqBiquad::Type::Peak, 101, 2.7, 6.00},
        {EqBiquad::Type::Peak, 7834, 1.1, 3.95},
};

} // namespace

void EqBiquad::set(Type type, double frequency, double gainDb, double q, double sampleRate) {
    frequency = std::min(frequency, 0.45 * sampleRate);
    const double a = std::pow(10.0, gainDb / 40);
    const double w0 = 2 * kPi * frequency / sampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2 * q);
    double nb0;
    double nb1;
    double nb2;
    double na0;
    double na1;
    double na2;
    switch (type) {
    case Type::Peak:
        nb0 = 1 + alpha * a;
        nb1 = -2 * cosw;
        nb2 = 1 - alpha * a;
        na0 = 1 + alpha / a;
        na1 = -2 * cosw;
        na2 = 1 - alpha / a;
        break;
    case Type::LowShelf: {
        const double s = 2 * std::sqrt(a) * alpha;
        nb0 = a * ((a + 1) - (a - 1) * cosw + s);
        nb1 = 2 * a * ((a - 1) - (a + 1) * cosw);
        nb2 = a * ((a + 1) - (a - 1) * cosw - s);
        na0 = (a + 1) + (a - 1) * cosw + s;
        na1 = -2 * ((a - 1) + (a + 1) * cosw);
        na2 = (a + 1) + (a - 1) * cosw - s;
        break;
    }
    case Type::HighShelf:
    default: {
        const double s = 2 * std::sqrt(a) * alpha;
        nb0 = a * ((a + 1) + (a - 1) * cosw + s);
        nb1 = -2 * a * ((a - 1) + (a + 1) * cosw);
        nb2 = a * ((a + 1) + (a - 1) * cosw - s);
        na0 = (a + 1) - (a - 1) * cosw + s;
        na1 = 2 * ((a - 1) - (a + 1) * cosw);
        na2 = (a + 1) - (a - 1) * cosw - s;
        break;
    }
    }
    b0 = nb0 / na0;
    b1 = nb1 / na0;
    b2 = nb2 / na0;
    a1 = na1 / na0;
    a2 = na2 / na0;
}

SpatialBinaural::SpatialBinaural() {
    const double headSeconds = kHeadRadius / kSpeedOfSound;
    for (auto& speaker : m_speakers) {
        // The longest path round the head is ~2.6 head radii.
        speaker.line.allocate(static_cast<int>(3 * headSeconds * kMaxSampleRate) + 8);
    }
    m_reflections.allocate(static_cast<int>(0.03 * kMaxSampleRate));
}

void SpatialBinaural::configure(const SpeakerLayout& layout, double sampleRate) {
    m_count = layout.count();
    m_rate = sampleRate;
    const double headSeconds = kHeadRadius / kSpeedOfSound;
    const double tau = kHeadRadius / (2 * kSpeedOfSound); // 1 / (2 omega0)
    const double k = 2 * sampleRate;                      // bilinear transform
    for (int i = 0; i < m_count; ++i) {
        Speaker& speaker = m_speakers[i];
        m_azimuths[i] = layout.azimuth(i);
        for (int e = 0; e < 2; ++e) {
            // Angle between the source and this ear's axis, 0..180.
            const double incidence = std::abs(wrapDegrees(m_azimuths[i] - kEarAzimuth[e]));
            const double radians = incidence * kPi / 180;
            // Woodworth: straight to the near ear, round the head to the far.
            const double itd = incidence < 90
                    ? headSeconds * (1 - std::cos(radians))
                    : headSeconds * (1 + (radians - kPi / 2));
            Ear& ear = speaker.ears[e];
            ear.delay = 1 + itd * sampleRate;
            // Head shadow: +6 dB of highs facing the ear, -20 dB at 150°.
            const double alpha = (1 + kShadowMinAlpha / 2) +
                    (1 - kShadowMinAlpha / 2) * std::cos(incidence / kShadowMinAngle * kPi);
            const double a0 = 1 + tau * k;
            ear.b0 = (1 + alpha * tau * k) / a0;
            ear.b1 = (1 - alpha * tau * k) / a0;
            ear.a1 = (1 - tau * k) / a0;
        }
        // Sources behind lose some highs (the pinna faces forward).
        const double rear = std::clamp((std::abs(m_azimuths[i]) - 90) / 90, 0.0, 1.0);
        speaker.rearCut = std::pow(10.0, -kRearCutDb * rear / 20) - 1;
        speaker.rearSplit.setCutoff(kRearSplitHz, sampleRate);
    }
    m_reflectionTone.setCutoff(kReflectionToneHz, sampleRate);
    for (auto& channel : m_eq) {
        for (int b = 0; b < 10; ++b) {
            channel[b].set(kHd280[b].type,
                    kHd280[b].frequency,
                    kHd280[b].gainDb,
                    kHd280[b].q,
                    sampleRate);
        }
    }
}

void SpatialBinaural::process(const std::array<const float*, kMaxSpeakers>& speakers,
        const SpeakerLayout& layout,
        int frames,
        double sampleRate,
        double room,
        bool headphoneEq,
        float* pOut) {
    bool changed = layout.count() != m_count || sampleRate != m_rate;
    for (int i = 0; !changed && i < m_count; ++i) {
        changed = layout.azimuth(i) != m_azimuths[i];
    }
    if (changed) {
        configure(layout, sampleRate);
    }
    room = std::clamp(room, 0.0, 1.0);
    double reflectionDelay[2][2];
    for (int e = 0; e < 2; ++e) {
        for (int t = 0; t < 2; ++t) {
            reflectionDelay[e][t] = kReflectionMs[e][t] / 1000 * sampleRate;
        }
    }
    const double preamp = std::pow(10.0, kHd280PreampDb / 20);

    for (int f = 0; f < frames; ++f) {
        double ears[2] = {0, 0};
        double mono = 0;
        for (int i = 0; i < m_count; ++i) {
            Speaker& speaker = m_speakers[i];
            const double x = speakers[i][f];
            mono += x;
            const double high = x - speaker.rearSplit.lowPass(x);
            speaker.line.write(static_cast<float>(x + speaker.rearCut * high));
            for (int e = 0; e < 2; ++e) {
                Ear& ear = speaker.ears[e];
                ears[e] += ear.filter(speaker.line.read(ear.delay));
            }
        }
        m_reflections.write(static_cast<float>(m_reflectionTone.lowPass(mono)));
        for (int e = 0; e < 2; ++e) {
            double reflected = 0;
            for (int t = 0; t < 2; ++t) {
                reflected += kReflectionGain[t] * m_reflections.read(reflectionDelay[e][t]);
            }
            double y = (ears[e] + room * reflected) * kOutputGain;
            if (headphoneEq) {
                y *= preamp;
                for (auto& band : m_eq[e]) {
                    y = band.process(y);
                }
            }
            pOut[2 * f + e] = static_cast<float>(y);
        }
    }
}

} // namespace mixxx::spatial
