// Tests for the wet-only (send/return) mode of EngineEffectChain.
//
// These drive an EngineEffectChain directly, with the built-in Echo in its only
// slot, the way the engine does: requests through a message pipe, then
// process() once per callback. Echo is the case that matters: it outputs only
// its delay line and relies on the chain to add the dry signal back, which the
// Dry/Wet mode does even at a mix of 1.0.

#include <gtest/gtest.h>

#include <QString>
#include <algorithm>
#include <cmath>
#include <memory>

#include "effects/backends/effectsbackendmanager.h"
#include "engine/channelhandle.h"
#include "engine/effects/engineeffect.h"
#include "engine/effects/engineeffectchain.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/effects/message.h"
#include "engine/engine.h"
#include "test/mixxxtest.h"
#include "util/messagepipe.h"
#include "util/sample.h"
#include "util/samplebuffer.h"

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);
constexpr std::size_t kSamples = 1024; // 512 stereo frames per callback
// Echo's shortest delay without tempo information: 1/8 s.
constexpr double kDelaySeconds = 0.125;
constexpr std::size_t kDelaySamples =
        static_cast<std::size_t>(kDelaySeconds * 44100) * 2;

class EngineEffectChainWetOnlyTest : public MixxxTest {
  protected:
    EngineEffectChainWetOnlyTest()
            : m_pipes(makeTwoWayMessagePipe<EffectsRequest*, EffectsResponse>(
                      1000, 1000)),
              m_routed(m_handles.getOrCreateHandle("[BusLeft]"), "[BusLeft]"),
              m_other(m_handles.getOrCreateHandle("[Channel1]"), "[Channel1]"),
              m_output(m_handles.getOrCreateHandle("[Main]"), "[Main]"),
              m_in(kSamples),
              m_out(kSamples) {
        const QSet<ChannelHandleAndGroup> inputs{m_routed, m_other};
        const QSet<ChannelHandleAndGroup> outputs{m_output};

        m_pBackendManager = EffectsBackendManagerPointer(new EffectsBackendManager());
        EffectManifestPointer pManifest = m_pBackendManager->getManifest(
                "org.mixxx.effects.echo", EffectBackendType::BuiltIn);
        EXPECT_TRUE(pManifest);
        m_pEffect = std::make_unique<EngineEffect>(pManifest,
                m_pBackendManager,
                inputs,
                inputs,
                outputs);
        m_pChain = std::make_unique<EngineEffectChain>(
                QStringLiteral("[Test]"), inputs, outputs);

        EffectsRequest add;
        add.type = EffectsRequest::ADD_EFFECT_TO_CHAIN;
        add.AddEffectToChain.pEffect = m_pEffect.get();
        add.AddEffectToChain.iIndex = 0;
        chainRequest(add);

        EffectsRequest route;
        route.type = EffectsRequest::ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL;
        route.EnableInputChannelForChain.channelHandle = m_routed.handle();
        chainRequest(route);

        setEffectParameter(pManifest, "delay_time", kDelaySeconds);
        setEffectParameter(pManifest, "send_amount", 1.0);
        setEffectParameter(pManifest, "feedback_amount", 0.5);
        setEffectEnabled(true);
        setChain(/*enabled*/ true, /*wetOnly*/ true, /*mix*/ 1.0);
    }

    void chainRequest(const EffectsRequest& request) {
        ASSERT_TRUE(m_pChain->processEffectsRequest(request, &m_pipes.second));
    }

    void setChain(bool enabled, bool wetOnly, double mix) {
        EffectsRequest request;
        request.type = EffectsRequest::SET_EFFECT_CHAIN_PARAMETERS;
        request.SetEffectChainParameters.enabled = enabled;
        request.SetEffectChainParameters.mix_mode = EffectChainMixMode::DrySlashWet;
        request.SetEffectChainParameters.mix = mix;
        request.SetEffectChainParameters.wet_only = wetOnly;
        chainRequest(request);
    }

    void setEffectEnabled(bool enabled) {
        EffectsRequest request;
        request.type = EffectsRequest::SET_EFFECT_PARAMETERS;
        request.SetEffectParameters.enabled = enabled;
        ASSERT_TRUE(m_pEffect->processEffectsRequest(request, &m_pipes.second));
    }

    void setEffectParameter(EffectManifestPointer pManifest, const QString& id, double value) {
        const auto& parameters = pManifest->parameters();
        for (int i = 0; i < parameters.size(); ++i) {
            if (parameters.at(i)->id() == id) {
                EffectsRequest request;
                request.type = EffectsRequest::SET_PARAMETER_PARAMETERS;
                request.SetParameterParameters.iParameter = i;
                request.value = value;
                ASSERT_TRUE(m_pEffect->processEffectsRequest(request, &m_pipes.second));
                return;
            }
        }
        FAIL() << "Echo has no parameter " << id.toStdString();
    }

    /// One engine callback on `input`: returns what process() returned and
    /// leaves the output in m_out. In place, like the bus and deck paths.
    bool callback(const ChannelHandleAndGroup& input, CSAMPLE inputLevel, bool impulse) {
        m_in.fill(inputLevel);
        if (impulse) {
            m_in.data()[0] = 1.0f;
            m_in.data()[1] = 1.0f;
        }
        SampleUtil::copy(m_out.data(), m_in.data(), kSamples);
        return m_pChain->process(input.handle(),
                m_output.handle(),
                m_out.data(),
                m_out.data(),
                kSamples,
                kSampleRate,
                m_features,
                /*fadeout*/ false);
    }

    CSAMPLE peak() const {
        CSAMPLE p = 0;
        for (std::size_t i = 0; i < kSamples; ++i) {
            p = std::max(p, std::abs(m_out.data()[i]));
        }
        return p;
    }

    /// The first callback after routing ramps both the send and Echo's own
    /// enable up from zero, which would swallow an impulse at its start.
    void warmUp() {
        callback(m_routed, 0, false);
    }

    /// Runs silent callbacks until the echo of the impulse sent at the start
    /// should have arrived, and returns the loudest sample seen.
    CSAMPLE peakUntilPastFirstRepeat() {
        CSAMPLE p = 0;
        for (std::size_t done = kSamples; done < kDelaySamples + 2 * kSamples; done += kSamples) {
            callback(m_routed, 0, false);
            p = std::max(p, peak());
        }
        return p;
    }

    ChannelHandleFactory m_handles;
    std::pair<EffectsRequestPipe, EffectsResponsePipe> m_pipes;
    ChannelHandleAndGroup m_routed;
    ChannelHandleAndGroup m_other;
    ChannelHandleAndGroup m_output;
    EffectsBackendManagerPointer m_pBackendManager;
    std::unique_ptr<EngineEffect> m_pEffect;
    std::unique_ptr<EngineEffectChain> m_pChain;
    GroupFeatureState m_features;
    mixxx::SampleBuffer m_in;
    mixxx::SampleBuffer m_out;
};

// The bug wet-only exists for: Dry/Wet at 1.0 with Echo still passes the dry
// signal, because the chain adds Echo's input back before the crossfade.
TEST_F(EngineEffectChainWetOnlyTest, DryWetAtFullWetStillPassesTheDrySignal) {
    setChain(true, /*wetOnly*/ false, 1.0);
    callback(m_routed, 0.5f, false); // enabling
    callback(m_routed, 0.5f, false);
    EXPECT_NEAR(m_out.data()[kSamples / 2], 0.5f, 1e-3);
}

TEST_F(EngineEffectChainWetOnlyTest, OutputsNoDrySignal) {
    callback(m_routed, 0.5f, false);
    callback(m_routed, 0.5f, false);
    // A constant input the echo has not repeated yet: nothing on the output.
    EXPECT_LT(peak(), 1e-6);
}

TEST_F(EngineEffectChainWetOnlyTest, OutputsTheEcho) {
    warmUp();
    ASSERT_TRUE(callback(m_routed, 0, /*impulse*/ true));
    EXPECT_LT(peak(), 1e-6); // the impulse itself does not come through
    EXPECT_GT(peakUntilPastFirstRepeat(), 0.1);
}

// Post-fader: closing the send stops feeding the echo but keeps its tail.
TEST_F(EngineEffectChainWetOnlyTest, TailSurvivesTheSendClosing) {
    warmUp();
    callback(m_routed, 0, /*impulse*/ true);
    setChain(true, true, /*mix, i.e. send*/ 0.0);
    EXPECT_GT(peakUntilPastFirstRepeat(), 0.1);
}

TEST_F(EngineEffectChainWetOnlyTest, ClosedSendFeedsNothing) {
    setChain(true, true, 0.0);
    callback(m_routed, 0, false); // let the send ramp settle at 0
    callback(m_routed, 0, /*impulse*/ true);
    EXPECT_LT(peakUntilPastFirstRepeat(), 1e-6);
}

TEST_F(EngineEffectChainWetOnlyTest, SwitchedOffIsSilentNotDry) {
    callback(m_routed, 0.5f, false);
    setChain(/*enabled*/ false, true, 1.0);
    callback(m_routed, 0.5f, false); // disabling
    callback(m_routed, 0.5f, false);
    EXPECT_TRUE(callback(m_routed, 0.5f, false));
    EXPECT_LT(peak(), 1e-6);
}

TEST_F(EngineEffectChainWetOnlyTest, EmptyIsSilentNotDry) {
    callback(m_routed, 0.5f, false);
    setEffectEnabled(false);
    callback(m_routed, 0.5f, false); // the effect's disabling callback
    callback(m_routed, 0.5f, false);
    EXPECT_TRUE(callback(m_routed, 0.5f, false));
    EXPECT_LT(peak(), 1e-6);
}

TEST_F(EngineEffectChainWetOnlyTest, LeavesAnUnroutedChannelAlone) {
    EXPECT_FALSE(callback(m_other, 0.5f, false));
    EXPECT_FLOAT_EQ(m_out.data()[kSamples / 2], 0.5f);
}

} // namespace
