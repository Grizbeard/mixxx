#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <numeric>
#include <optional>
#include <vector>

#include "control/controlobject.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/spatial/enginespatialoutput.h"
#include "engine/spatial/spatialautopan.h"
#include "engine/spatial/spatialbinaural.h"
#include "engine/spatial/spatialdelay.h"
#include "engine/spatial/spatialdsp.h"
#include "engine/spatial/spatialfxmeta.h"
#include "engine/spatial/spatialmath.h"
#include "engine/spatial/spatialreverb.h"
#include "test/mixxxtest.h"

using namespace mixxx::spatial;

namespace {

constexpr double kRate = 48000;
constexpr int kFrames = 480;           // 10 ms callbacks
constexpr double kBeatSeconds = 0.5;   // 120 BPM
constexpr int kLookahead = 72;         // the limiter's 1.5 ms at 48 kHz
constexpr int kFramesPerBeat = 24000;

double sumOfSquares(const float* pGains, int count) {
    double sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += pGains[i] * pGains[i];
    }
    return sum;
}

double rmsOf(const std::vector<double>& x, std::size_t begin = 0, std::size_t end = 0) {
    if (end == 0 || end > x.size()) {
        end = x.size();
    }
    double sum = 0;
    for (std::size_t i = begin; i < end; ++i) {
        sum += x[i] * x[i];
    }
    return end > begin ? std::sqrt(sum / (end - begin)) : 0;
}

void set(const QString& group, const char* key, double value) {
    ControlObject::set(ConfigKey(group, key), value);
}

/// A tempo source at 120 BPM whose beat 0 is at frame 0.
GroupFeatureState tempoAfter(int callbacks) {
    GroupFeatureState features;
    features.beat_length = GroupFeatureBeatLength{kBeatSeconds, 1.0};
    const double beats = static_cast<double>(callbacks) * kFrames / (kRate * kBeatSeconds);
    features.beat_fraction_buffer_end = beats - std::floor(beats);
    return features;
}

// ---------------------------------------------------------------------------
// Panner and motion maths.

TEST(SpatialMathTest, PanningOnASpeakerUsesOnlyThatSpeaker) {
    for (int count = kMinSpeakers; count <= kMaxSpeakers; ++count) {
        SpeakerLayout layout;
        const auto azimuths = SpeakerLayout::defaultAzimuths(count);
        layout.setSpeakers(count, azimuths.data());
        float gains[kMaxSpeakers];
        for (int s = 0; s < count; ++s) {
            layout.gains(azimuths[s], 0, gains);
            for (int i = 0; i < count; ++i) {
                EXPECT_NEAR(gains[i], i == s ? 1.0 : 0.0, 1e-6)
                        << count << " speakers: speaker " << s << " gain " << i;
            }
        }
    }
}

TEST(SpatialMathTest, DefaultRingsAreEvenWithTheFrontPairAroundZero) {
    for (int count = kMinSpeakers; count <= kMaxSpeakers; ++count) {
        const auto azimuths = SpeakerLayout::defaultAzimuths(count);
        const double spacing = 360.0 / count;
        EXPECT_NEAR(azimuths[0], -spacing / 2, 1e-9) << count;
        EXPECT_NEAR(azimuths[1], spacing / 2, 1e-9) << count;
        // Clockwise from speaker 2, one spacing apart, back round to speaker 1.
        for (int i = 1; i < count; ++i) {
            const double step = wrapDegrees(azimuths[(i + 1) % count] - azimuths[i]);
            EXPECT_NEAR(step < 0 ? step + 360 : step, spacing, 1e-9)
                    << count << " speakers, speaker " << i + 1;
        }
    }
    // The six-speaker ring keeps its angles.
    const auto six = SpeakerLayout::defaultAzimuths(6);
    const double expected[] = {-30, 30, 90, 150, -150, -90};
    for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(six[i], expected[i], 1e-9);
    }
}

TEST(SpatialMathTest, PanningIsPowerNormalisedAllAround) {
    for (int count = kMinSpeakers; count <= kMaxSpeakers; ++count) {
        SpeakerLayout layout;
        const auto azimuths = SpeakerLayout::defaultAzimuths(count);
        layout.setSpeakers(count, azimuths.data());
        float gains[kMaxSpeakers];
        for (double spread : {0.0, 0.3, 1.0}) {
            for (int az = -180; az <= 180; ++az) {
                layout.gains(az, spread, gains);
                EXPECT_NEAR(sumOfSquares(gains, count), 1.0, 1e-5)
                        << count << " speakers, az " << az << ", spread " << spread;
                if (spread == 0) {
                    int active = 0;
                    for (int i = 0; i < count; ++i) {
                        active += gains[i] > 1e-6 ? 1 : 0;
                    }
                    EXPECT_LE(active, 2) << "az " << az;
                }
            }
        }
    }
}

TEST(SpatialMathTest, FullSpreadIsEqualOnEverySpeaker) {
    for (int count = kMinSpeakers; count <= kMaxSpeakers; ++count) {
        SpeakerLayout layout;
        const auto azimuths = SpeakerLayout::defaultAzimuths(count);
        layout.setSpeakers(count, azimuths.data());
        float gains[kMaxSpeakers];
        layout.gains(77, 1, gains);
        for (int i = 0; i < count; ++i) {
            EXPECT_NEAR(gains[i], 1 / std::sqrt(static_cast<double>(count)), 1e-6) << count;
        }
    }
}

TEST(SpatialMathTest, AWideGapStillPansSmoothly) {
    // Four speakers in a half circle: nothing behind.
    SpeakerLayout layout;
    const double azimuths[] = {-30, 30, 90, -90};
    layout.setSpeakers(4, azimuths);
    float gains[kMaxSpeakers];
    float previous[kMaxSpeakers];
    layout.gains(90, 0, previous);
    for (int az = 91; az <= 270; ++az) {
        layout.gains(az, 0, gains);
        EXPECT_NEAR(sumOfSquares(gains, 4), 1.0, 1e-5);
        for (int i = 0; i < 4; ++i) {
            EXPECT_LT(std::abs(gains[i] - previous[i]), 0.05) << "az " << az;
            previous[i] = gains[i];
        }
    }
}

TEST(SpatialMathTest, ShapesPassThroughTheirPoints) {
    const Point front = shapeAt(Shape::Orbit, 0);
    EXPECT_NEAR(front.y, 1, 1e-9);
    EXPECT_NEAR(azimuthOf(shapeAt(Shape::Orbit, 1)), 90, 1e-9);
    EXPECT_NEAR(azimuthOf(shapeAt(Shape::Orbit, 1.5)), 135, 1e-9);
    EXPECT_NEAR(shapeAt(Shape::LeftRight, 0).x, -1, 1e-9);
    EXPECT_NEAR(shapeAt(Shape::LeftRight, 1).x, 1, 1e-9);
    EXPECT_NEAR(radiusOf(shapeAt(Shape::LeftRight, 0.5)), 0, 1e-9) << "through the centre";
    EXPECT_NEAR(shapeAt(Shape::FrontBack, 1).y, -1, 1e-9);
    EXPECT_NEAR(azimuthOf(shapeAt(Shape::Corners, 2)), 135, 1e-9);

    // Stars: every point on the ring, each step skipping ahead, dipping
    // towards the centre between points.
    struct Star {
        Shape shape;
        int points;
        double stepDegrees;
    };
    for (const Star& star : {Star{Shape::Star5, 5, 216}, Star{Shape::Star7, 7, 4 * 360.0 / 7}}) {
        for (int k = 0; k < star.points; ++k) {
            const Point p = shapeAt(star.shape, k);
            EXPECT_NEAR(radiusOf(p), 1, 1e-9) << "point " << k;
            EXPECT_NEAR(wrapDegrees(azimuthOf(p) - k * star.stepDegrees), 0, 1e-6) << "point " << k;
            EXPECT_LT(radiusOf(shapeAt(star.shape, k + 0.5)), 0.4) << "between " << k;
        }
    }
}

TEST(SpatialMathTest, WavesMoveBetweenPointsAsSpecified) {
    // Square: holds the point for the whole step.
    EXPECT_EQ(wavePosition(Wave::Square, 0.0, 4, false), 0);
    EXPECT_EQ(wavePosition(Wave::Square, 0.99, 4, false), 0);
    EXPECT_EQ(wavePosition(Wave::Square, 1.0, 4, false), 1);
    // Triangle: constant speed.
    EXPECT_NEAR(wavePosition(Wave::Triangle, 2.25, 4, false), 2.25, 1e-12);
    // Sine: eases, halfway at mid-step, slow near the points.
    EXPECT_NEAR(wavePosition(Wave::Sine, 1.5, 4, false), 1.5, 1e-12);
    EXPECT_LT(wavePosition(Wave::Sine, 1.1, 4, false) - 1, 0.05);
    // Saw: glides half way A->B over the step, snaps to B on the next, at
    // the same one point per step as the other waves.
    EXPECT_NEAR(wavePosition(Wave::Saw, 0.0, 4, false), 0, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 0.5, 4, false), 0.25, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 0.999, 4, false), 0.4995, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 1.0, 4, false), 1, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 4.0, 4, false), 0, 1e-12) << "a cycle per 4 steps";
    EXPECT_EQ(waveCycleSteps(Wave::Saw, 4), 4);
    // Reverse saw: glides half way back from A, then still jumps forward to B.
    EXPECT_NEAR(wavePosition(Wave::ReverseSaw, 0.0, 4, false), 0, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::ReverseSaw, 0.5, 4, false), 3.75, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::ReverseSaw, 1.0, 4, false), 1, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::ReverseSaw, 1.5, 4, false), 0.75, 1e-12);
    // Reverse direction visits the points backwards.
    EXPECT_EQ(wavePosition(Wave::Square, 1.0, 4, true), 3);
    EXPECT_EQ(wavePosition(Wave::Square, 2.0, 4, true), 2);
}

TEST(SpatialMathTest, RandomJumpsFarAndIsTheSameEveryTime) {
    for (std::int64_t k = -50; k < 2000; ++k) {
        const double a = randomAzimuth(k);
        EXPECT_EQ(a, randomAzimuth(k)) << "deterministic";
        EXPECT_GE(std::abs(wrapDegrees(a - randomAzimuth(k - 1))), kRandomMinJump - 1e-9)
                << "step " << k;
        EXPECT_NEAR(radiusOf(shapeAt(Shape::Random, static_cast<double>(k))), 1, 1e-9);
    }
    // Between points it moves round the ring the short way.
    const double from = randomAzimuth(10);
    const double to = randomAzimuth(11);
    EXPECT_NEAR(wrapDegrees(azimuthOf(shapeAt(Shape::Random, 10.5)) -
                        (from + wrapDegrees(to - from) / 2)),
            0,
            1e-6);
    // Many points: the whole ring gets used.
    int quadrants[4] = {};
    for (std::int64_t k = 0; k < 400; ++k) {
        ++quadrants[static_cast<int>((randomAzimuth(k) + 180) / 90) % 4];
    }
    for (int q : quadrants) {
        EXPECT_GT(q, 60);
    }
}

TEST(SpatialMathTest, ClockLocksToTheSourcePhase) {
    SpatialClock clock;
    for (int c = 1; c <= 100; ++c) {
        const auto features = tempoAfter(c);
        clock.advance(kFrames, kRate, features.beat_length->seconds,
                *features.beat_fraction_buffer_end, 0);
    }
    EXPECT_NEAR(clock.beatsEnd(), 100.0 * kFrames / kFramesPerBeat, 1e-9);
    // A paused source holds its phase: the clock holds too.
    const double held = clock.beatsEnd();
    const auto paused = tempoAfter(100);
    for (int c = 0; c < 10; ++c) {
        clock.advance(kFrames, kRate, paused.beat_length->seconds,
                *paused.beat_fraction_buffer_end, 0);
    }
    EXPECT_NEAR(clock.beatsEnd(), held, 1e-9);
    // Without a source it free-runs at the fallback tempo.
    clock.advance(kRate, kRate, std::nullopt, std::nullopt, 60);
    EXPECT_NEAR(clock.beatsEnd(), held + 1, 1e-9);
}

// ---------------------------------------------------------------------------
// The spatial bus.

class SpatialBusTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pOutput = std::make_unique<EngineSpatialOutput>();
        connect(6, false);
        set("[Spatial]", "limiter_enabled", 0);
        set("[SpatialDelay]", "enabled", 0);
        set("[SpatialReverb]", "enabled", 0);
    }

    void connect(int speakers, bool lf) {
        if (m_out) {
            m_pOutput->onOutputDisconnected(*m_out);
        }
        m_channels = speakers + (lf ? 1 : 0);
        m_out.emplace(AudioPath::AudioPathType::Spatial,
                0,
                mixxx::audio::ChannelCount(m_channels));
        m_pOutput->onOutputConnected(*m_out);
        set("[Spatial]", "speaker_count", speakers);
        set("[Spatial]", "lf_enabled", lf ? 1 : 0);
    }

    /// Runs `callbacks` callbacks; `fill` writes the stereo input of deck 1's
    /// source `slot` for the callback starting at `frame`. Returns each
    /// output channel.
    std::vector<std::vector<double>> run(int callbacks,
            int slot,
            const std::function<void(CSAMPLE*, int frame)>& fill) {
        std::vector<std::vector<double>> out(m_channels);
        std::vector<CSAMPLE> stereo(2 * kFrames);
        for (int c = 0; c < callbacks; ++c) {
            const auto tempo = tempoAfter(++m_callbacks);
            m_pOutput->beginCallback(kFrames, mixxx::audio::SampleRate(48000), &tempo, 0);
            fill(stereo.data(), c * kFrames);
            m_pOutput->addSource(0, slot, stereo.data());
            m_pOutput->process(nullptr, kFrames, mixxx::audio::SampleRate(48000));
            const auto buffer = m_pOutput->buffer(*m_out);
            for (int f = 0; f < kFrames; ++f) {
                for (int ch = 0; ch < m_channels; ++ch) {
                    out[ch].push_back(buffer[f * m_channels + ch]);
                }
            }
        }
        return out;
    }

    static std::function<void(CSAMPLE*, int)> sines(double left, double right, double amplitude) {
        return [=](CSAMPLE* pOut, int frame) {
            for (int f = 0; f < kFrames; ++f) {
                const double t = (frame + f) / kRate;
                pOut[2 * f] = static_cast<CSAMPLE>(amplitude * std::sin(2 * M_PI * left * t));
                pOut[2 * f + 1] = static_cast<CSAMPLE>(amplitude * std::sin(2 * M_PI * right * t));
            }
        };
    }

    std::unique_ptr<EngineSpatialOutput> m_pOutput;
    std::optional<AudioOutput> m_out;
    int m_channels = 0;
    int m_callbacks = 0;
};

TEST_F(SpatialBusTest, ByDefaultAStemIsTheOriginalStereoOnTheFrontPair) {
    // Default: pan 0, width 60Ãƒâ€šÃ‚Â°, which on the 6-speaker ring is exactly the
    // front pair at Ãƒâ€šÃ‚Â±30Ãƒâ€šÃ‚Â°.
    const auto input = sines(1000, 1500, 0.5);
    const auto out = run(10, 0, input);
    std::vector<CSAMPLE> expected(2 * kFrames);
    // From the second callback: the spatial mix fades in over the first.
    for (int c = 1; c < 9; ++c) {
        input(expected.data(), c * kFrames);
        for (int f = 0; f < kFrames; ++f) {
            const std::size_t i = c * kFrames + f + kLookahead;
            ASSERT_NEAR(out[0][i], expected[2 * f], 1e-6);
            ASSERT_NEAR(out[1][i], expected[2 * f + 1], 1e-6);
        }
    }
    for (int ch = 2; ch < 6; ++ch) {
        EXPECT_EQ(rmsOf(out[ch]), 0) << "speaker " << ch + 1;
    }
}

TEST_F(SpatialBusTest, FrontIsExactWhereverTheStemWas) {
    set("[Channel1_Stem4]", "spatial_azimuth", 150);
    set("[Channel1_Stem4]", "spatial_width", 0.8);
    set("[Channel1_Stem4]", "spatial_spread", 0.4);
    set("[Channel1_Stem4]", "spatial_front", 1);
    // Glides for 250 ms, then exact.
    const auto out = run(40, 2, sines(700, 900, 0.5));
    const std::size_t settled = 30 * kFrames;
    EXPECT_NEAR(rmsOf(out[0], settled), 0.5 / M_SQRT2, 1e-3);
    EXPECT_NEAR(rmsOf(out[1], settled), 0.5 / M_SQRT2, 1e-3);
    for (int ch = 2; ch < 6; ++ch) {
        EXPECT_LT(rmsOf(out[ch], settled), 1e-6) << "speaker " << ch + 1;
    }
}

TEST_F(SpatialBusTest, PanningAPointSourceKeepsItsLevel) {
    // Width 0 at 90Ãƒâ€šÃ‚Â°: speaker 3 alone carries L + R, trimmed 3 dB.
    set("[Channel1_Stem1]", "spatial_azimuth", 90);
    set("[Channel1_Stem1]", "spatial_width", 0);
    const auto out = run(5, 0, sines(440, 440, 0.5));
    // (After the first callback, where the spatial mix fades in at start.)
    EXPECT_NEAR(rmsOf(out[2], kFrames), M_SQRT1_2 * 1.0 / M_SQRT2, 3e-3);
    for (int ch : {0, 1, 3, 4, 5}) {
        EXPECT_LT(rmsOf(out[ch]), 1e-6) << "speaker " << ch + 1;
    }
}

TEST_F(SpatialBusTest, ATwelveSpeakerRingWithLf) {
    connect(12, true);
    // Speaker 7 of the 12-speaker ring sits at 165 degrees.
    set("[Channel1_Stem1]", "spatial_azimuth", 165);
    set("[Channel1_Stem1]", "spatial_width", 0);
    const auto out = run(5, 0, sines(440, 440, 0.5));
    ASSERT_EQ(out.size(), 13u);
    EXPECT_NEAR(rmsOf(out[6], kFrames), M_SQRT1_2 * 1.0 / M_SQRT2, 3e-3);
    for (int ch = 0; ch < 12; ++ch) {
        if (ch != 6) {
            EXPECT_LT(rmsOf(out[ch]), 1e-6) << "speaker " << ch + 1;
        }
    }
    // The LF feed is the channel after the last speaker (40 Hz, under the
    // crossover).
    const auto low = run(5, 0, sines(40, 40, 0.5));
    EXPECT_GT(rmsOf(low[12], kFrames), 0.1);
}

TEST_F(SpatialBusTest, AThreeSpeakerRingPansAllAround) {
    connect(3, false);
    set("[Channel1_Stem1]", "spatial_width", 0);
    // Half way between speaker 2 (60 degrees) and speaker 3 (180): both equally.
    set("[Channel1_Stem1]", "spatial_azimuth", 120);
    const auto out = run(5, 0, sines(440, 440, 0.5));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_NEAR(rmsOf(out[1], kFrames), rmsOf(out[2], kFrames), 1e-3);
    EXPECT_LT(rmsOf(out[0]), 1e-6);
}

TEST_F(SpatialBusTest, TheLfFeedDoesNotDependOnPosition) {
    connect(6, true);
    set("[Spatial]", "lf_crossover_hz", 100);
    const auto atFront = run(60, 1, sines(50, 50, 0.5));
    set("[Channel1_Stem3]", "spatial_azimuth", 150);
    set("[Channel1_Stem3]", "spatial_spread", 0.7);
    const auto atBack = run(60, 1, sines(50, 50, 0.5));
    const std::size_t settled = 30 * kFrames;
    const double front = rmsOf(atFront[6], settled);
    EXPECT_NEAR(front, 0.5 / M_SQRT2, 0.03) << "50 Hz passes the 100 Hz low-pass";
    EXPECT_NEAR(rmsOf(atBack[6], settled), front, front * 0.01);
    // And the mains are high-passed at the crossover.
    double mains = 0;
    for (int ch = 0; ch < 6; ++ch) {
        mains += rmsOf(atFront[ch], settled);
    }
    EXPECT_LT(mains, 0.1);
}

TEST_F(SpatialBusTest, SquareAutopanSnapsCornerToCornerOnTheBeat) {
    connect(4, false); // speakers at -45, 45, 135, -135: the corners
    set("[SpatialAutopan1]", "shape", static_cast<double>(Shape::Corners));
    set("[SpatialAutopan1]", "wave", static_cast<double>(Wave::Square));
    set("[SpatialAutopan1]", "sync", 1);
    set("[SpatialAutopan1]", "beats", 1);
    set("[Channel1_Stem1]", "spatial_autopan", 1);
    set("[Channel1_Stem1]", "spatial_width", 0);
    const auto out = run(8 * 50, 0, sines(300, 300, 0.5));
    for (int beat = 1; beat < 8; ++beat) {
        // Skip the 4 ms gain smoothing (plus lookahead) after each snap.
        const std::size_t begin = beat * kFramesPerBeat + kLookahead + 1000;
        const std::size_t end = (beat + 1) * kFramesPerBeat;
        const int expected = beat % 4;
        for (int ch = 0; ch < 4; ++ch) {
            const double level = rmsOf(out[ch], begin, end);
            if (ch == expected) {
                EXPECT_GT(level, 0.3) << "beat " << beat;
            } else {
                EXPECT_LT(level, 1e-3) << "beat " << beat << " speaker " << ch + 1;
            }
        }
    }
}

TEST_F(SpatialBusTest, TheLimiterHoldsTheCeiling) {
    set("[Spatial]", "limiter_enabled", 1);
    set("[Spatial]", "limiter_ceiling_db", -1);
    const auto out = run(20, 0, sines(200, 300, 3.0));
    const double ceiling = std::pow(10.0, -1 / 20.0);
    for (int ch = 0; ch < 6; ++ch) {
        for (double x : out[ch]) {
            ASSERT_LE(std::abs(x), ceiling + 1e-6);
        }
    }
    EXPECT_LT(ControlObject::get(ConfigKey("[Spatial]", "limiter_reduction_db")), -6);
}

TEST_F(SpatialBusTest, SpeakerTrimsAndDelaysAlignTheRoom) {
    set("[Spatial]", "speaker_2_delay_ms", 10);
    set("[Spatial]", "speaker_2_gain_db", -6);
    // A click on both channels at frame 1000.
    const auto out = run(5, 0, [](CSAMPLE* pOut, int frame) {
        for (int f = 0; f < kFrames; ++f) {
            const float v = frame + f == 1000 ? 0.5f : 0.0f;
            pOut[2 * f] = v;
            pOut[2 * f + 1] = v;
        }
    });
    EXPECT_NEAR(out[0][1000 + kLookahead], 0.5, 1e-6);
    EXPECT_NEAR(out[1][1000 + kLookahead + 480], 0.5 * std::pow(10.0, -6 / 20.0), 1e-6);
}

TEST_F(SpatialBusTest, ATestSignalMutesTheProgram) {
    set("[Spatial]", "test_channel", 3);
    const auto out = run(10, 0, sines(500, 500, 0.5));
    EXPECT_LT(rmsOf(out[0], 2 * kFrames), 1e-9) << "program off while testing";
    EXPECT_GT(rmsOf(out[3], 3 * kFrames), 0.01);
}

// ---------------------------------------------------------------------------
// Spatial FX unit.

class SpatialFxTest : public MixxxTest {
  protected:
    void SetUp() override {
        const auto azimuths = SpeakerLayout::defaultAzimuths(4);
        m_layout.setSpeakers(4, azimuths.data());
        for (auto& channel : m_speakers) {
            channel.assign(kFrames, 0);
        }
    }

    /// Runs the delay over `callbacks` callbacks fed by `send` (mono, both
    /// sides); collects each speaker.
    std::vector<std::vector<double>> runDelay(
            SpatialDelay& delay, int callbacks, const std::function<float(int)>& send) {
        std::vector<std::vector<double>> out(4);
        std::vector<float> in(kFrames);
        std::vector<float> lf(kFrames);
        std::vector<float> toReverb(kFrames);
        for (int c = 0; c < callbacks; ++c) {
            const auto tempo = tempoAfter(c + 1);
            m_clock.advance(kFrames, kRate, tempo.beat_length->seconds,
                    *tempo.beat_fraction_buffer_end, 0);
            for (int f = 0; f < kFrames; ++f) {
                in[f] = send(c * kFrames + f);
            }
            std::array<float*, kMaxSpeakers> speakers{};
            for (int i = 0; i < 4; ++i) {
                std::fill(m_speakers[i].begin(), m_speakers[i].end(), 0.0f);
                speakers[i] = m_speakers[i].data();
            }
            delay.process(in.data(), in.data(), kFrames, kRate, m_clock, m_layout,
                    speakers, lf.data(), toReverb.data());
            for (int i = 0; i < 4; ++i) {
                out[i].insert(out[i].end(), m_speakers[i].begin(), m_speakers[i].end());
            }
        }
        return out;
    }

    /// A 1 ms 2 kHz burst at `frame`.
    static std::function<float(int)> burstAt(int frame) {
        return [frame](int f) {
            const int t = f - frame;
            return t >= 0 && t < 48
                    ? static_cast<float>(0.3 * std::sin(2 * M_PI * 2000 * t / kRate))
                    : 0.0f;
        };
    }

    /// Which speaker carries the most energy in [begin, end), and how much.
    static std::pair<int, double> loudest(const std::vector<std::vector<double>>& out,
            std::size_t begin,
            std::size_t end) {
        int best = -1;
        double bestLevel = 0;
        for (int i = 0; i < static_cast<int>(out.size()); ++i) {
            const double level = rmsOf(out[i], begin, end);
            if (level > bestLevel) {
                best = i;
                bestLevel = level;
            }
        }
        return {best, bestLevel};
    }

    void configureDelay(double beats) {
        set("[SpatialDelay]", "enabled", 1);
        set("[SpatialDelay]", "level", 1);
        set("[SpatialDelay]", "sync", 1);
        set("[SpatialDelay]", "beats", beats);
        set("[SpatialDelay]", "triplet", 0);
        set("[SpatialDelay]", "feedback", 0.5);
        set("[SpatialDelay]", "tone", 1);
        set("[SpatialDelay]", "lowcut", 20);
        set("[SpatialDelay]", "shape", static_cast<double>(Shape::Corners));
        set("[SpatialDelay]", "direction", 0);
        set("[SpatialDelay]", "poly", 0);
        set("[SpatialDelay]", "gate", 0);
    }

    SpeakerLayout m_layout;
    SpatialClock m_clock;
    std::array<std::vector<float>, 4> m_speakers;
};

TEST_F(SpatialFxTest, EchoesArriveOnTheBeatAndStepAroundTheShape) {
    SpatialDelay delay;
    configureDelay(1);
    // A burst a quarter beat in; echo k lands k beats later on corner k
    // (echo 1 on the first corner).
    const int start = kFramesPerBeat / 4;
    const auto out = runDelay(delay, 5 * 50 + 10, burstAt(start));
    double previous = 0;
    for (int k = 1; k <= 4; ++k) {
        const std::size_t at = start + k * kFramesPerBeat;
        const auto [speaker, level] = loudest(out, at - 10, at + 200);
        EXPECT_EQ(speaker, (k - 1) % 4) << "echo " << k;
        EXPECT_LT(rmsOf(out[speaker], at - 400, at - 10), level * 0.01) << "echo " << k << " early";
        if (k > 1) {
            EXPECT_NEAR(level / previous, 0.5, 0.05) << "feedback";
        }
        previous = level;
    }
}

TEST(SpatialFxMetaTest, MacrosAreNeutralInTheMiddleAndReachTheEnds) {
    FxMeta meta;
    EXPECT_DOUBLE_EQ(meta.delayFeedback(0.55), 0.55);
    EXPECT_DOUBLE_EQ(meta.reverbDecay(9), 9);
    EXPECT_DOUBLE_EQ(meta.reverbDamping(0.45), 0.45);
    meta.echo = 1;
    meta.space = 1;
    meta.tone = 1;
    EXPECT_DOUBLE_EQ(meta.delayFeedback(0.55), 0.95);
    EXPECT_DOUBLE_EQ(meta.delayToReverb(0.3), 1);
    EXPECT_NEAR(meta.reverbDecay(9), 30, 1e-9);
    EXPECT_DOUBLE_EQ(meta.reverbSize(0.8), 1);
    EXPECT_DOUBLE_EQ(meta.delayTone(0.6), 1) << "bright";
    EXPECT_DOUBLE_EQ(meta.reverbDamping(0.45), 0) << "bright";
    meta.echo = 0;
    meta.space = 0;
    meta.tone = 0;
    EXPECT_DOUBLE_EQ(meta.delayFeedback(0.55), 0);
    EXPECT_NEAR(meta.reverbDecay(9), 0.5, 1e-9);
    EXPECT_DOUBLE_EQ(meta.reverbDamping(0.45), 1) << "dark";
    meta.space = 0.75;
    EXPECT_NEAR(meta.reverbDecay(9), 9 * std::sqrt(30.0 / 9), 1e-9) << "half way, on a ratio scale";
}

TEST_F(SpatialFxTest, TheEchoMacroAtZeroLeavesOneEcho) {
    SpatialDelay delay;
    configureDelay(1);
    FxMeta meta;
    meta.echo = 0; // feedback 0.5 pushed to 0
    std::vector<float> in(kFrames);
    std::vector<float> lf(kFrames);
    std::vector<float> toReverb(kFrames);
    std::vector<double> sum;
    const auto burst = burstAt(kFramesPerBeat / 4);
    for (int c = 0; c < 3 * 50; ++c) {
        const auto tempo = tempoAfter(c + 1);
        m_clock.advance(kFrames, kRate, tempo.beat_length->seconds,
                *tempo.beat_fraction_buffer_end, 0);
        for (int f = 0; f < kFrames; ++f) {
            in[f] = burst(c * kFrames + f);
        }
        std::array<float*, kMaxSpeakers> speakers{};
        for (int i = 0; i < 4; ++i) {
            std::fill(m_speakers[i].begin(), m_speakers[i].end(), 0.0f);
            speakers[i] = m_speakers[i].data();
        }
        delay.process(in.data(), in.data(), kFrames, kRate, m_clock, m_layout,
                speakers, lf.data(), toReverb.data(), meta);
        for (int f = 0; f < kFrames; ++f) {
            double x = 0;
            for (int i = 0; i < 4; ++i) {
                x += std::abs(m_speakers[i][f]);
            }
            sum.push_back(x);
        }
    }
    const std::size_t first = kFramesPerBeat / 4 + kFramesPerBeat;
    EXPECT_GT(rmsOf(sum, first, first + 48), 0.05) << "echo 1";
    EXPECT_LT(rmsOf(sum, first + kFramesPerBeat - 10, first + kFramesPerBeat + 100), 1e-6)
            << "no echo 2";
}

TEST_F(SpatialFxTest, ReEnablingTheDelayForgetsOldEchoes) {
    SpatialDelay delay;
    configureDelay(1);
    set("[SpatialDelay]", "feedback", 0.9);
    runDelay(delay, 60, burstAt(1000)); // echoes ringing
    set("[SpatialDelay]", "enabled", 0);
    runDelay(delay, 10, [](int) { return 0.0f; }); // fades out
    set("[SpatialDelay]", "enabled", 1);
    const auto out = runDelay(delay, 200, [](int) { return 0.0f; });
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(rmsOf(out[i]), 0) << "speaker " << i + 1;
    }
}

TEST_F(SpatialFxTest, PolyrhythmPlacesEchoesAgainstTheShape) {
    SpatialDelay delay;
    configureDelay(0.5);
    set("[SpatialDelay]", "poly", 1);
    set("[SpatialDelay]", "poly_beats", 1);
    // Echoes every half beat; the placement moves once a beat.
    const int start = kFramesPerBeat / 4;
    const auto out = runDelay(delay, 3 * 50, burstAt(start));
    std::vector<int> speakers;
    for (int k = 1; k <= 4; ++k) {
        const std::size_t at = start + k * kFramesPerBeat / 2;
        speakers.push_back(loudest(out, at - 10, at + 200).first);
    }
    // Echo k on point floor((k - 1) x 1/2 beat / 1 beat).
    EXPECT_EQ(speakers, (std::vector<int>{0, 0, 1, 1}));
}

TEST_F(SpatialFxTest, AnEchoStaysOnItsPointAcrossTheBeat) {
    SpatialDelay delay;
    configureDelay(1);
    set("[SpatialDelay]", "feedback", 0);
    // A 0.3-beat tone starting just before a beat: its echo straddles the
    // next beat, yet must stay on one speaker from start to end.
    const int start = static_cast<int>(0.85 * kFramesPerBeat);
    const int length = static_cast<int>(0.3 * kFramesPerBeat);
    const auto out = runDelay(delay, 3 * 50, [=](int f) {
        return f >= start && f < start + length
                ? static_cast<float>(0.3 * std::sin(2 * M_PI * 500 * f / kRate))
                : 0.0f;
    });
    const std::size_t from = start + kFramesPerBeat + 200;
    const std::size_t to = start + kFramesPerBeat + length - 200;
    EXPECT_GT(rmsOf(out[0], from, to), 0.15);
    for (int i = 1; i < 4; ++i) {
        EXPECT_LT(rmsOf(out[i], from, to), 1e-6) << "speaker " << i + 1;
    }
}

TEST(SpatialDelayPatternTest, EchoesMapToPointsAndCycles) {
    // Without poly: echo k on point k - 1, a cycle per trip round the shape.
    EXPECT_EQ(SpatialDelay::pointOfEcho(0, 4, 0.75, 0), 0);
    EXPECT_EQ(SpatialDelay::pointOfEcho(5, 4, 0.75, 0), 1);
    EXPECT_EQ(SpatialDelay::echoesPerCycle(5, 0.75, 0), 5);
    // Dotted 1/8 echoes over a 1-beat, 4-point shape: points 0 0 1 2 3 3 4..
    std::vector<int> points;
    for (int tap = 0; tap < 6; ++tap) {
        points.push_back(SpatialDelay::pointOfEcho(tap, 4, 0.75, 1));
    }
    EXPECT_EQ(points, (std::vector<int>{0, 0, 1, 2, 3, 3}));
    // 16 echoes x 0.75 = 12 beats = 3 trips round 4 points.
    EXPECT_EQ(SpatialDelay::echoesPerCycle(4, 0.75, 1), 16);
}

TEST_F(SpatialFxTest, TheGateChopsTheEchoesOnTheGrid) {
    SpatialDelay delay;
    configureDelay(1);
    set("[SpatialDelay]", "feedback", 0);
    set("[SpatialDelay]", "gate", 1);
    set("[SpatialDelay]", "gate_beats", 0.25);
    set("[SpatialDelay]", "gate_duty", 0.5);
    set("[SpatialDelay]", "gate_smooth", 0);
    const auto out = runDelay(delay, 3 * 50, [](int f) {
        return static_cast<float>(0.5 * std::sin(2 * M_PI * 1000 * f / kRate));
    });
    // Sum the speakers, then compare the open and closed halves of each
    // 1/16 in the second beat (the first beat of echoes).
    const int sixteenth = kFramesPerBeat / 4;
    for (int g = 4; g < 8; ++g) {
        double open = 0;
        double closed = 0;
        for (int i = 0; i < 4; ++i) {
            open += rmsOf(out[i], g * sixteenth + 200, g * sixteenth + sixteenth / 2 - 50);
            closed += rmsOf(out[i], g * sixteenth + sixteenth / 2 + 200, (g + 1) * sixteenth - 50);
        }
        EXPECT_GT(open, 0.2) << "sixteenth " << g;
        EXPECT_LT(closed, open * 0.01) << "sixteenth " << g;
    }
}

TEST_F(SpatialFxTest, TheReverbTailSurroundsTheRoomAndDecays) {
    SpatialReverb reverb;
    set("[SpatialReverb]", "enabled", 1);
    set("[SpatialReverb]", "level", 1);
    set("[SpatialReverb]", "decay", 2);
    set("[SpatialReverb]", "predelay_ms", 20);
    std::vector<float> in(kFrames);
    std::vector<float> lf(kFrames);
    std::vector<std::vector<double>> out(4);
    uint32_t noise = 1;
    for (int c = 0; c < 400; ++c) { // 4 s: 0.2 s of noise, then the tail
        for (int f = 0; f < kFrames; ++f) {
            noise = noise * 1664525u + 1013904223u;
            in[f] = c < 20 ? static_cast<float>(noise / 4294967296.0 - 0.5) : 0.0f;
        }
        std::array<float*, kMaxSpeakers> speakers{};
        for (int i = 0; i < 4; ++i) {
            std::fill(m_speakers[i].begin(), m_speakers[i].end(), 0.0f);
            speakers[i] = m_speakers[i].data();
        }
        reverb.process(in.data(), kFrames, kRate, m_layout, speakers, lf.data());
        for (int i = 0; i < 4; ++i) {
            out[i].insert(out[i].end(), m_speakers[i].begin(), m_speakers[i].end());
        }
    }
    const auto window = [&](int i, double from, double to) {
        return rmsOf(out[i], static_cast<std::size_t>(from * kRate),
                static_cast<std::size_t>(to * kRate));
    };
    for (int i = 0; i < 4; ++i) {
        const double early = window(i, 0.3, 0.5);
        const double later = window(i, 1.3, 1.5);
        EXPECT_GT(early, 1e-3) << "speaker " << i + 1;
        // 2 s RT60: 60 dB per 2 s, so ~30 dB down a second later (damping
        // takes the highs faster).
        const double dropDb = 20 * std::log10(early / later);
        EXPECT_GT(dropDb, 20) << "speaker " << i + 1;
        EXPECT_LT(dropDb, 45) << "speaker " << i + 1;
    }
    // Decorrelated: speakers 1 and 2 carry different signals.
    double cross = 0;
    double a = 0;
    double b = 0;
    for (std::size_t n = static_cast<std::size_t>(0.25 * kRate);
            n < static_cast<std::size_t>(0.6 * kRate);
            ++n) {
        cross += out[0][n] * out[1][n];
        a += out[0][n] * out[0][n];
        b += out[1][n] * out[1][n];
    }
    EXPECT_LT(std::abs(cross) / std::sqrt(a * b), 0.3);
}

TEST_F(SpatialFxTest, TheReverbFillsTwelveSpeakersAtTheSameLevel) {
    set("[SpatialReverb]", "enabled", 1);
    set("[SpatialReverb]", "level", 1);
    set("[SpatialReverb]", "decay", 2);
    set("[SpatialReverb]", "predelay_ms", 20);
    // The same burst into a 6- and a 12-speaker ring.
    const auto render = [](int count) {
        SpatialReverb reverb;
        SpeakerLayout layout;
        const auto azimuths = SpeakerLayout::defaultAzimuths(count);
        layout.setSpeakers(count, azimuths.data());
        std::vector<float> in(kFrames);
        std::vector<float> lf(kFrames);
        std::array<std::vector<float>, kMaxSpeakers> buffers;
        std::vector<std::vector<double>> out(count);
        uint32_t noise = 1;
        for (int c = 0; c < 100; ++c) { // 0.2 s of noise, then the tail
            for (int f = 0; f < kFrames; ++f) {
                noise = noise * 1664525u + 1013904223u;
                in[f] = c < 20 ? static_cast<float>(noise / 4294967296.0 - 0.5) : 0.0f;
            }
            std::array<float*, kMaxSpeakers> speakers{};
            for (int i = 0; i < count; ++i) {
                buffers[i].assign(kFrames, 0.0f);
                speakers[i] = buffers[i].data();
            }
            reverb.process(in.data(), kFrames, kRate, layout, speakers, lf.data());
            for (int i = 0; i < count; ++i) {
                out[i].insert(out[i].end(), buffers[i].begin(), buffers[i].end());
            }
        }
        return out;
    };
    const auto begin = static_cast<std::size_t>(0.25 * kRate);
    const auto end = static_cast<std::size_t>(0.6 * kRate);
    const auto totalPower = [&](const std::vector<std::vector<double>>& out) {
        double power = 0;
        for (const auto& channel : out) {
            const double level = rmsOf(channel, begin, end);
            power += level * level;
        }
        return power;
    };
    const auto six = render(6);
    const auto twelve = render(12);
    // Same total power, so the same wet/dry balance on either ring.
    EXPECT_NEAR(10 * std::log10(totalPower(twelve) / totalPower(six)), 0, 1.5);
    // Every speaker carries the tail, and every pair is decorrelated.
    for (int i = 0; i < 12; ++i) {
        EXPECT_GT(rmsOf(twelve[i], begin, end), 1e-3) << "speaker " << i + 1;
        for (int j = i + 1; j < 12; ++j) {
            double cross = 0;
            double a = 0;
            double b = 0;
            for (std::size_t n = begin; n < end; ++n) {
                cross += twelve[i][n] * twelve[j][n];
                a += twelve[i][n] * twelve[i][n];
                b += twelve[j][n] * twelve[j][n];
            }
            EXPECT_LT(std::abs(cross) / std::sqrt(a * b), 0.3)
                    << "speakers " << i + 1 << " and " << j + 1;
        }
    }
}

TEST_F(SpatialFxTest, ShimmerAndFreezeStayBounded) {
    SpatialReverb reverb;
    set("[SpatialReverb]", "enabled", 1);
    set("[SpatialReverb]", "level", 1);
    set("[SpatialReverb]", "decay", 30);
    set("[SpatialReverb]", "shimmer", 1);
    set("[SpatialReverb]", "modulation", 1);
    std::vector<float> in(kFrames);
    std::vector<float> lf(kFrames);
    uint32_t noise = 7;
    double peak = 0;
    for (int c = 0; c < 1500; ++c) { // 15 s of noise, then 5 s frozen
        if (c == 1000) {
            set("[SpatialReverb]", "freeze", 1);
        }
        for (int f = 0; f < kFrames; ++f) {
            noise = noise * 1664525u + 1013904223u;
            in[f] = static_cast<float>(noise / 4294967296.0 - 0.5);
        }
        std::array<float*, kMaxSpeakers> speakers{};
        for (int i = 0; i < 4; ++i) {
            std::fill(m_speakers[i].begin(), m_speakers[i].end(), 0.0f);
            speakers[i] = m_speakers[i].data();
        }
        reverb.process(in.data(), kFrames, kRate, m_layout, speakers, lf.data());
        for (int i = 0; i < 4; ++i) {
            for (float x : m_speakers[i]) {
                ASSERT_TRUE(std::isfinite(x));
                peak = std::max(peak, static_cast<double>(std::abs(x)));
            }
        }
    }
    EXPECT_LT(peak, 4.0);
    EXPECT_GT(peak, 0.01);
}

TEST_F(SpatialFxTest, AutopanTripletsAndRetrigger) {
    SpatialAutopan autopan(1);
    set("[SpatialAutopan1]", "sync", 1);
    set("[SpatialAutopan1]", "beats", 1);
    set("[SpatialAutopan1]", "triplet", 1);
    set("[SpatialAutopan1]", "wave", static_cast<double>(Wave::Square));
    set("[SpatialAutopan1]", "shape", static_cast<double>(Shape::Orbit));
    // 2 beats = 3 triplet steps.
    for (int c = 1; c <= 100; ++c) {
        const auto tempo = tempoAfter(c);
        m_clock.advance(kFrames, kRate, tempo.beat_length->seconds,
                *tempo.beat_fraction_buffer_end, 0);
    }
    autopan.prepare(m_clock);
    EXPECT_NEAR(autopan.motion().stepsEnd, 3, 1e-9);
    // Retrigger restarts from the nearest beat.
    set("[SpatialAutopan1]", "retrigger", 1);
    set("[SpatialAutopan1]", "retrigger", 0);
    autopan.prepare(m_clock);
    EXPECT_NEAR(autopan.motion().stepsEnd, 0, 1e-9);
}

// ---------------------------------------------------------------------------
// Headphone monitoring.

/// Renders one speaker's feed (a 2 kHz tone, or a click) through the
/// binaural model; returns the left and right ears.
std::array<std::vector<double>, 2> binauralOf(int speaker, bool click, double room = 0) {
    SpatialBinaural binaural;
    SpeakerLayout layout;
    const auto azimuths = SpeakerLayout::defaultAzimuths(6);
    layout.setSpeakers(6, azimuths.data());
    std::array<std::vector<float>, kMaxSpeakers> feeds;
    for (auto& feed : feeds) {
        feed.assign(kFrames, 0.0f);
    }
    std::array<const float*, kMaxSpeakers> pointers{};
    for (int i = 0; i < kMaxSpeakers; ++i) {
        pointers[i] = feeds[i].data();
    }
    std::array<std::vector<double>, 2> ears;
    std::vector<float> out(2 * kFrames);
    for (int c = 0; c < 20; ++c) {
        for (int f = 0; f < kFrames; ++f) {
            const int n = c * kFrames + f;
            feeds[speaker][f] = click ? (n == 1000 ? 1.0f : 0.0f)
                                      : static_cast<float>(0.5 * std::sin(2 * M_PI * 2000 * n / kRate));
        }
        binaural.process(pointers, layout, kFrames, kRate, room, false, out.data());
        for (int f = 0; f < kFrames; ++f) {
            ears[0].push_back(out[2 * f]);
            ears[1].push_back(out[2 * f + 1]);
        }
    }
    return ears;
}

std::size_t peakIndex(const std::vector<double>& x) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (std::abs(x[i]) > std::abs(x[best])) {
            best = i;
        }
    }
    return best;
}

TEST(SpatialBinauralTest, ASourceOnTheRightReachesTheRightEarFirstAndLouder) {
    // Speaker 3 is at 90°: right side.
    const auto tone = binauralOf(2, false);
    const double left = rmsOf(tone[0], 2 * kFrames);
    const double right = rmsOf(tone[1], 2 * kFrames);
    // A real head gives about 10 dB at 2 kHz from the side.
    EXPECT_GT(20 * std::log10(right / left), 8) << "head shadow at 2 kHz";
    const auto click = binauralOf(2, true);
    const auto delay = static_cast<double>(peakIndex(click[0])) -
            static_cast<double>(peakIndex(click[1]));
    // Woodworth: (a / c) (1 + pi / 2) = 0.66 ms, about 31 samples.
    EXPECT_NEAR(delay, 31, 3) << "interaural delay";
}

TEST(SpatialBinauralTest, TheFrontPairIsBalanced) {
    // Speakers 1 and 2 (±30°) mirror each other between the ears.
    const auto left = binauralOf(0, false);
    const auto right = binauralOf(1, false);
    EXPECT_NEAR(rmsOf(left[0], 2 * kFrames), rmsOf(right[1], 2 * kFrames), 1e-6);
    EXPECT_NEAR(rmsOf(left[1], 2 * kFrames), rmsOf(right[0], 2 * kFrames), 1e-6);
}

TEST(SpatialBinauralTest, TheHd280CorrectionPeakHasItsGain) {
    EqBiquad peak;
    peak.set(EqBiquad::Type::Peak, 3946, 5.9, 1.82, kRate);
    double sumIn = 0;
    double sumOut = 0;
    for (int n = 0; n < 48000; ++n) {
        const double x = std::sin(2 * M_PI * 3946 * n / kRate);
        const double y = peak.process(x);
        if (n > 4800) {
            sumIn += x * x;
            sumOut += y * y;
        }
    }
    EXPECT_NEAR(10 * std::log10(sumOut / sumIn), 5.9, 0.05);
}

TEST_F(SpatialBusTest, HeadphonesHearTheSpatialMixWithoutASpatialOutput) {
    m_pOutput->onOutputDisconnected(*m_out);
    set("[Spatial]", "headphone_spatial", 1);
    ASSERT_TRUE(m_pOutput->isRendering());
    std::vector<CSAMPLE> stereo(2 * kFrames);
    double energy = 0;
    for (int c = 0; c < 10; ++c) {
        sines(500, 700, 0.5)(stereo.data(), c * kFrames);
        m_pOutput->beginCallback(kFrames, mixxx::audio::SampleRate(48000), nullptr, 120);
        m_pOutput->addSource(0, 0, stereo.data());
        m_pOutput->process(nullptr, kFrames, mixxx::audio::SampleRate(48000));
        const CSAMPLE* pBinaural = m_pOutput->binaural();
        ASSERT_NE(pBinaural, nullptr);
        for (int i = 0; i < 2 * kFrames; ++i) {
            energy += pBinaural[i] * pBinaural[i];
        }
    }
    EXPECT_GT(energy, 1);
    set("[Spatial]", "headphone_spatial", 0);
    EXPECT_FALSE(m_pOutput->isRendering());
}

} // namespace
