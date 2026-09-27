#include "effects/chains/standardeffectchain.h"

#include "control/controlpushbutton.h"
#include "effects/effectsmanager.h"
#include "mixer/playermanager.h"
#include "moc_standardeffectchain.cpp"

StandardEffectChain::StandardEffectChain(unsigned int iChainNumber,
        EffectsManager* pEffectsManager,
        EffectsMessengerPointer pEffectsMessenger)
        : EffectChain(formatEffectChainGroup(iChainNumber),
                  pEffectsManager,
                  pEffectsMessenger,
                  SignalProcessingStage::Postfader) {
    // Wet-only turns the unit from an insert into a send/return: the mix knob
    // becomes the send level into the effects, and the unit outputs only what
    // the effects produce - silence when it is off or empty - instead of
    // crossfading the processed signal against the dry one. Routed to a
    // crossfader bus or the main bus, that makes a pure effects return.
    //
    // It is a property of the unit, not of the chain preset loaded into it,
    // so it is neither saved in nor restored from presets: a preset that put a
    // return back into Dry/Wet would put the whole dry mix on its output. It
    // is persisted, so it survives a restart.
    m_pControlChainWetOnly = std::make_unique<ControlPushButton>(
            ConfigKey(group(), QStringLiteral("wet_only")), true);
    m_pControlChainWetOnly->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pControlChainWetOnly.get(),
            &ControlObject::valueChanged,
            this,
            &StandardEffectChain::sendParameterUpdate);
    // The EffectChain constructor already sent the chain's parameters to the
    // engine, before this control existed - so a wet_only restored from the
    // config would otherwise not reach the engine until some other parameter
    // changed. Setting it again does not help: an unchanged value is a no-op.
    sendParameterUpdate();

    for (int i = 0; i < kNumEffectsPerUnit; ++i) {
        addEffectSlot(formatEffectSlotGroup(iChainNumber, i));
    }

    const QSet<ChannelHandleAndGroup>& registeredChannels =
            m_pEffectsManager->registeredInputChannels();
    for (const ChannelHandleAndGroup& handle_group : registeredChannels) {
        int deckNumber;
        if (PlayerManager::isDeckGroup(handle_group.name(), &deckNumber) &&
                (iChainNumber + 1) == (unsigned)deckNumber) {
            registerInputChannel(handle_group, 1.0);
        } else {
            registerInputChannel(handle_group, 0.0);
        }
    }
}

QString StandardEffectChain::formatEffectChainGroup(const int iChainNumber) {
    // EffectRacks never did anything and there was never more than one of them,
    // but it remains in the ControlObject group name for backwards compatibility.
    return QString("[EffectRack1_EffectUnit%1]")
            .arg(QString::number(iChainNumber + 1));
}

QString StandardEffectChain::formatEffectSlotGroup(const int iChainSlotNumber,
        const int iEffectSlotNumber) {
    return QString("[EffectRack1_EffectUnit%1_Effect%2]")
            .arg(QString::number(iChainSlotNumber + 1),
                    QString::number(iEffectSlotNumber + 1));
}
