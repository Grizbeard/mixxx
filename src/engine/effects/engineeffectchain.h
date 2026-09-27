#pragma once

#include <QList>
#include <QString>

#include "audio/types.h"
#include "engine/channelhandle.h"
#include "engine/effects/engineeffectsdelay.h"
#include "engine/effects/message.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class EngineEffect;

/// EngineEffectChain is the audio thread counterpart of EffectChain.
/// The lifetime of EngineEffectChain corresponds to the lifetime of
/// its EffectChain counterpart; EngineEffectChains are neither created
/// nor destroyed apart from Mixxx startup and shutdown.
///
/// EngineEffectChain processes a list of EngineEffects in series.
/// EngineEffectChain manages the input channel routing switches,
/// the mix knob, and the chain enable switch.
class EngineEffectChain final : public EffectsRequestHandler {
  public:
    /// called from main thread
    EngineEffectChain(const QString& group,
            const QSet<ChannelHandleAndGroup>& registeredInputChannels,
            const QSet<ChannelHandleAndGroup>& registeredOutputChannels);
    /// called from main thread
    ~EngineEffectChain();

    /// called from audio thread
    bool processEffectsRequest(
            const EffectsRequest& message,
            EffectsResponsePipe* pResponsePipe) override;

    /// called from audio thread
    bool process(const ChannelHandle& inputHandle,
            const ChannelHandle& outputHandle,
            CSAMPLE* pIn,
            CSAMPLE* pOut,
            const std::size_t numSamples,
            const mixxx::audio::SampleRate sampleRate,
            const GroupFeatureState& groupFeatures,
            bool fadeout);

  private:
    struct ChannelStatus {
        ChannelStatus()
                : oldMixKnob(0),
                  enableState(EffectEnableState::Disabled),
                  wetOnlyOutputting(false),
                  wetOnlyEndedOnInput(false) {
        }
        CSAMPLE oldMixKnob;
        EffectEnableState enableState;
        // Wet-only mode only: whether the last callback put effect output on
        // this channel, and whether that output was fading to the effect's
        // input (an effect that does not ramp from dry itself) rather than to
        // silence. Together they let the output fade instead of stepping when
        // the effects stop.
        bool wetOnlyOutputting;
        bool wetOnlyEndedOnInput;
    };

    QString debugString() const {
        return QString("EngineEffectChain(%1)").arg(m_group);
    }

    bool updateParameters(const EffectsRequest& message);
    bool processWetOnly(ChannelStatus* pChannelStatus,
            EffectEnableState effectiveChainEnableState,
            const ChannelHandle& inputHandle,
            const ChannelHandle& outputHandle,
            const CSAMPLE* pIn,
            CSAMPLE* pOut,
            const std::size_t numSamples,
            const mixxx::audio::SampleRate sampleRate,
            const GroupFeatureState& groupFeatures);
    bool addEffect(EngineEffect* pEffect, int iIndex);
    bool removeEffect(EngineEffect* pEffect, int iIndex);
    bool enableForInputChannel(ChannelHandle inputHandle);
    bool disableForInputChannel(ChannelHandle inputHandle);

    QString m_group;
    bool m_enableState;
    EffectChainMixMode::Type m_mixMode;
    CSAMPLE m_dMix;
    bool m_wetOnly;
    QList<EngineEffect*> m_effects;
    mixxx::SampleBuffer m_buffer1;
    mixxx::SampleBuffer m_buffer2;
    // Wet-only mode: the input scaled by the mix (send) knob.
    mixxx::SampleBuffer m_sendBuffer;
    ChannelHandleMap<ChannelStatus> m_outputChannelMap;
    ChannelHandleMap<ChannelHandleMap<ChannelStatus>> m_chainStatusForChannelMatrix;
    EngineEffectsDelay m_effectsDelay;

    DISALLOW_COPY_AND_ASSIGN(EngineEffectChain);
};
