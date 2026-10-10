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
#include "engine/spatial/spatialdelay.h"
#include "engine/spatial/spatialdsp.h"
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
    SpeakerLayout layout;
    const auto azimuths = SpeakerLayout::defaultAzimuths(6);
    layout.setSpeakers(6, azimuths.data());
    float gains[kMaxSpeakers];
    for (int s = 0; s < 6; ++s) {
        layout.gains(azimuths[s], 0, gains);
        for (int i = 0; i < 6; ++i) {
            EXPECT_NEAR(gains[i], i == s ? 1.0 : 0.0, 1e-6) << "speaker " << s << " gain " << i;
        }
    }
}

TEST(SpatialMathTest, PanningIsPowerNormalisedAllAround) {
    for (int count = 4; count <= 6; ++count) {
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
    SpeakerLayout layout;
    float gains[kMaxSpeakers];
    layout.gains(77, 1, gains);
    for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(gains[i], 1 / std::sqrt(6.0), 1e-6);
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
    // Saw: step 0 ramps A->B, step 1 snaps to C and ramps C->D.
    EXPECT_NEAR(wavePosition(Wave::Saw, 0.0, 4, false), 0, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 0.999, 4, false), 0.999, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 1.0, 4, false), 2, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 1.5, 4, false), 2.5, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::Saw, 2.0, 4, false), 0, 1e-12) << "4 points: 2 steps a cycle";
    EXPECT_EQ(waveCycleSteps(Wave::Saw, 4), 2);
    EXPECT_EQ(waveCycleSteps(Wave::Saw, 5), 5);
    // Reverse saw: starts at B and ramps back to A, then D back to C.
    EXPECT_NEAR(wavePosition(Wave::ReverseSaw, 0.0, 4, false), 1, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::ReverseSaw, 0.75, 4, false), 0.25, 1e-12);
    EXPECT_NEAR(wavePosition(Wave::ReverseSaw, 1.0, 4, false), 3, 1e-12);
    // Reverse direction visits the points backwards.
    EXPECT_EQ(wavePosition(Wave::Square, 1.0, 4, true), 3);
    EXPECT_EQ(wavePosition(Wave::Square, 2.0, 4, true), 2);
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
    // Default: pan 0, width 60°, which on the 6-speaker ring is exactly the
    // front pair at ±30°.
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
    // Width 0 at 90°: speaker 3 alone carries L + R, trimmed 3 dB.
    set("[Channel1_Stem1]", "spatial_azimuth", 90);
    set("[Channel1_Stem1]", "spatial_width", 0);
    const auto out = run(5, 0, sines(440, 440, 0.5));
    // (After the first callback, where the spatial mix fades in at start.)
    EXPECT_NEAR(rmsOf(out[2], kFrames), M_SQRT1_2 * 1.0 / M_SQRT2, 3e-3);
    for (int ch : {0, 1, 3, 4, 5}) {
        EXPECT_LT(rmsOf(out[ch]), 1e-6) << "speaker " << ch + 1;
    }
}

TEST_F(SpatialBusTest, TheLfFeedDoesNotDependOnPosition) {
    connect(6, true);
    set("[Spatial]", "lf_crossover_hz", 100);
    const auto atFront = run(60, 1, sines(50, 50, 0.5));
    set("[Channel1_Stem2]", "spatial_azimuth", 150);
    set("[Channel1_Stem2]", "spatial_spread", 0.7);
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
        set("[SpatialDelay]", "wave", static_cast<double>(Wave::Square));
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
    // A burst a quarter beat in; echo k lands k beats later on corner k.
    const int start = kFramesPerBeat / 4;
    const auto out = runDelay(delay, 4 * 50 + 10, burstAt(start));
    double previous = 0;
    for (int k = 1; k <= 3; ++k) {
        const std::size_t at = start + k * kFramesPerBeat;
        const auto [speaker, level] = loudest(out, at - 10, at + 200);
        EXPECT_EQ(speaker, k % 4) << "echo " << k;
        EXPECT_LT(rmsOf(out[speaker], at - 400, at - 10), level * 0.01) << "echo " << k << " early";
        if (k > 1) {
            EXPECT_NEAR(level / previous, 0.5, 0.05) << "feedback";
        }
        previous = level;
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
    EXPECT_EQ(speakers, (std::vector<int>{0, 1, 1, 2}));
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

} // namespace
