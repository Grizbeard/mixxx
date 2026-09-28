#include "effects/backends/builtin/remixeffect.h"

#include <gtest/gtest.h>

#include <QMap>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/effects/groupfeaturestate.h"
#include "util/math.h"
#include "util/sample.h"
#include "util/samplebuffer.h"

namespace {

constexpr mixxx::audio::SampleRate kSampleRate(48000);
constexpr SINT kFrames = 512;
constexpr int kBuffers = 94; // about a second
constexpr SINT kTotalFrames = kBuffers * kFrames;
constexpr SINT kMaxMeasuredFrames = 24 * kFrames; // the last quarter second

class Stage {
  public:
    virtual ~Stage() = default;
    virtual void process(const CSAMPLE* pIn, CSAMPLE* pOut, EffectEnableState enableState) = 0;
};

/// A Remix effect with its parameters, driven the way the engine drives it.
template<class Effect>
class RemixStage : public Stage {
  public:
    RemixStage()
            : m_pManifest(Effect::getManifest()),
              m_engineParameters(kSampleRate, kFrames),
              m_state(m_engineParameters) {
        for (const auto& pManifestParameter : m_pManifest->parameters()) {
            m_parameters.insert(pManifestParameter->id(),
                    EngineEffectParameterPointer(
                            new EngineEffectParameter(pManifestParameter)));
        }
        m_effect.loadEngineEffectParameters(m_parameters);
    }

    void set(const QString& id, double value) {
        m_parameters.value(id)->setValue(value);
    }

    void process(const CSAMPLE* pIn, CSAMPLE* pOut, EffectEnableState enableState) override {
        m_effect.processChannel(&m_state, pIn, pOut, m_engineParameters, enableState, m_features);
    }

  private:
    EffectManifestPointer m_pManifest;
    mixxx::EngineParameters m_engineParameters;
    RemixGroupState m_state;
    Effect m_effect;
    QMap<QString, EngineEffectParameterPointer> m_parameters;
    GroupFeatureState m_features;
};

struct Levels {
    double midDb;
    double sideDb;
};

/// Plays a stereo sine through the stages in series for about a second and
/// returns how much the mid and side of the output changed against the input,
/// in dB, over the last quarter second. It measures whole cycles of the sine,
/// so the phase shift of the crossovers doesn't bias the levels.
Levels measure(const std::vector<Stage*>& stages, double hz, double left, double right) {
    mixxx::SampleBuffer input(kFrames * 2);
    mixxx::SampleBuffer output(kFrames * 2);
    mixxx::SampleBuffer temp(kFrames * 2);
    const double framesPerCycle = kSampleRate.value() / hz;
    const auto measuredFrames = static_cast<SINT>(std::round(
            std::floor(kMaxMeasuredFrames / framesPerCycle) * framesPerCycle));
    double inMid = 0, inSide = 0, outMid = 0, outSide = 0;
    SINT frame = 0;
    for (int buffer = 0; buffer < kBuffers; ++buffer) {
        for (SINT i = 0; i < kFrames; ++i, ++frame) {
            const double s = std::sin(2 * M_PI * hz * frame / kSampleRate.value());
            input[i * 2] = static_cast<CSAMPLE>(left * s);
            input[i * 2 + 1] = static_cast<CSAMPLE>(right * s);
        }
        const EffectEnableState enableState = buffer == 0
                ? EffectEnableState::Enabling
                : EffectEnableState::Enabled;
        const CSAMPLE* pIn = input.data();
        for (Stage* pStage : stages) {
            pStage->process(pIn, output.data(), enableState);
            SampleUtil::copy(temp.data(), output.data(), kFrames * 2);
            pIn = temp.data();
        }
        const SINT firstFrameOfBuffer = frame - kFrames;
        for (SINT i = 0; i < kFrames; ++i) {
            if (firstFrameOfBuffer + i < kTotalFrames - measuredFrames) {
                continue;
            }
            const double im = (input[i * 2] + input[i * 2 + 1]) / 2;
            const double is = (input[i * 2] - input[i * 2 + 1]) / 2;
            const double om = (output[i * 2] + output[i * 2 + 1]) / 2;
            const double os = (output[i * 2] - output[i * 2 + 1]) / 2;
            inMid += im * im;
            inSide += is * is;
            outMid += om * om;
            outSide += os * os;
        }
    }
    auto db = [](double out, double in) {
        if (in == 0) {
            return 0.0;
        }
        return 10 * std::log10(std::max(out, 1e-30) / in);
    };
    return Levels{db(outMid, inMid), db(outSide, inSide)};
}

Levels measureMono(Stage* pStage, double hz) {
    return measure({pStage}, hz, 0.5, 0.5);
}

/// Every test runs at both slopes. The shallower one leaks more of a band into
/// its neighbours, so where a test measures leakage it expects less of it.
class RemixEffectTest : public testing::TestWithParam<int> {
  protected:
    bool steep() const {
        return GetParam() != 0;
    }
    /// The expected value at 24 or at 48 dB/oct
    double bySlope(double at24, double at48) const {
        return steep() ? at48 : at24;
    }
    template<class Effect>
    void useSlope(RemixStage<Effect>* pStage) const {
        pStage->set(remix::kSlopeId, GetParam());
    }
};

TEST_P(RemixEffectTest, LowRemovesTheCentreOfTheBass) {
    RemixStage<RemixLowEffect> low;
    useSlope(&low);
    EXPECT_LT(measureMono(&low, 60).midDb, bySlope(-45, -60));
}

TEST_P(RemixEffectTest, LowLeavesTheRestAlone) {
    RemixStage<RemixLowEffect> low;
    useSlope(&low);
    EXPECT_NEAR(measureMono(&low, 1000).midDb, 0, 0.05);
    EXPECT_NEAR(measureMono(&low, 8000).midDb, 0, 0.05);
}

TEST_P(RemixEffectTest, LowKeepsTheSides) {
    RemixStage<RemixLowEffect> low;
    useSlope(&low);
    const Levels levels = measure({&low}, 60, 0.5, -0.5);
    EXPECT_NEAR(levels.sideDb, 0, 0.05);
}

TEST_P(RemixEffectTest, HardPannedBassComesBackAsSide) {
    // A sound panned hard left is half centre and half side. Only the side is left.
    RemixStage<RemixLowEffect> low;
    useSlope(&low);
    const Levels levels = measure({&low}, 60, 0.5, 0);
    EXPECT_LT(levels.midDb, bySlope(-45, -60));
    EXPECT_NEAR(levels.sideDb, 0, 0.05);
}

TEST_P(RemixEffectTest, MidRemovesTheCentreBetweenTheCrossovers) {
    RemixStage<RemixMidEffect> mid;
    useSlope(&mid);
    EXPECT_LT(measureMono(&mid, 1000).midDb, bySlope(-28, -50));
    EXPECT_NEAR(measureMono(&mid, 60).midDb, 0, 0.05);
    EXPECT_NEAR(measureMono(&mid, 12000).midDb, 0, 0.05);
}

TEST_P(RemixEffectTest, HighRemovesTheCentreOfTheTop) {
    RemixStage<RemixHighEffect> high;
    useSlope(&high);
    EXPECT_LT(measureMono(&high, 10000).midDb, bySlope(-45, -60));
    EXPECT_NEAR(measureMono(&high, 100).midDb, 0, 0.05);
}

TEST_P(RemixEffectTest, CrossoverKnobsMoveTheBand) {
    RemixStage<RemixLowEffect> low;
    useSlope(&low);
    low.set(remix::kLowCrossoverId, 100);
    EXPECT_NEAR(measureMono(&low, 400).midDb, 0, 0.05);
    low.set(remix::kLowCrossoverId, 1000);
    EXPECT_LT(measureMono(&low, 400).midDb, bySlope(-28, -40));
}

TEST_P(RemixEffectTest, RemixKnobSetsTheDepth) {
    RemixStage<RemixLowEffect> low;
    useSlope(&low);
    low.set("remix", 0.5);
    EXPECT_NEAR(measureMono(&low, 60).midDb, -6.02, bySlope(0.1, 0.05));
    low.set("remix", 0);
    EXPECT_NEAR(measureMono(&low, 60).midDb, 0, 0.05);
}

TEST_P(RemixEffectTest, SideBoostRaisesTheSides) {
    RemixStage<RemixLowEffect> low;
    useSlope(&low);
    low.set("sideBoost", 6);
    EXPECT_NEAR(measure({&low}, 60, 0.5, -0.5).sideDb, 6, 0.05);
    // It scales with the Remix knob, so the original stays the original.
    low.set("remix", 0);
    EXPECT_NEAR(measure({&low}, 60, 0.5, -0.5).sideDb, 0, 0.05);
}

TEST_P(RemixEffectTest, AllThreeAtOriginalAreFlat) {
    RemixStage<RemixLowEffect> low;
    RemixStage<RemixMidEffect> mid;
    RemixStage<RemixHighEffect> high;
    useSlope(&low);
    useSlope(&mid);
    useSlope(&high);
    low.set("remix", 0);
    mid.set("remix", 0);
    high.set("remix", 0);
    for (double hz : {40.0, 180.0, 250.0, 350.0, 1000.0, 2500.0, 5000.0, 15000.0}) {
        SCOPED_TRACE(hz);
        EXPECT_NEAR(measure({&low, &mid, &high}, hz, 0.5, 0.5).midDb, 0, 0.05);
    }
}

TEST_P(RemixEffectTest, NeighbouringBandsLeaveSomeCentreAtTheirCrossover) {
    // Effects in series can't cancel perfectly where their bands overlap:
    // at the crossover, Low leaves half the centre and Mid keeps half of that.
    // This documents the size of that residue, not a wish for it.
    RemixStage<RemixLowEffect> low;
    RemixStage<RemixMidEffect> mid;
    useSlope(&low);
    useSlope(&mid);
    EXPECT_NEAR(measure({&low, &mid}, 250, 0.5, 0.5).midDb, -12.04, 0.2);
    // An octave away it is much smaller, and more so at the steeper slope.
    EXPECT_LT(measure({&low, &mid}, 125, 0.5, 0.5).midDb, bySlope(-20, -40));
    EXPECT_LT(measure({&low, &mid}, 500, 0.5, 0.5).midDb, bySlope(-20, -40));
}

INSTANTIATE_TEST_SUITE_P(Remix,
        RemixEffectTest,
        testing::Values(0, 1),
        [](const testing::TestParamInfo<int>& info) {
            return info.param ? std::string("Slope48") : std::string("Slope24");
        });

TEST(RemixSlopeSwitchTest, FlippingTheSlopeDoesNotLetTheOriginalThrough) {
    // Fully remixed, a centred bass note is gone at either slope. Flipping the
    // switch mid-note must cross between the two, not restart from the dry
    // signal, or the note would flash back for a moment.
    RemixStage<RemixLowEffect> low;
    mixxx::SampleBuffer input(kFrames * 2);
    mixxx::SampleBuffer output(kFrames * 2);
    const double amplitude = 0.5;
    double loudestAfterFlip = 0;
    SINT frame = 0;
    for (int buffer = 0; buffer < kBuffers; ++buffer) {
        for (SINT i = 0; i < kFrames; ++i, ++frame) {
            const auto s = static_cast<CSAMPLE>(
                    amplitude * std::sin(2 * M_PI * 60 * frame / kSampleRate.value()));
            input[i * 2] = s;
            input[i * 2 + 1] = s;
        }
        if (buffer == kBuffers / 2) {
            low.set(remix::kSlopeId, 1);
        }
        low.process(input.data(),
                output.data(),
                buffer == 0 ? EffectEnableState::Enabling : EffectEnableState::Enabled);
        if (buffer < kBuffers / 2) {
            continue;
        }
        for (SINT i = 0; i < kFrames; ++i) {
            const double mid = (output[i * 2] + output[i * 2 + 1]) / 2;
            loudestAfterFlip = std::max(loudestAfterFlip, std::abs(mid));
        }
    }
    EXPECT_LT(20 * std::log10(loudestAfterFlip / amplitude), -40);
}

} // namespace
