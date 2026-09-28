#include <gtest/gtest.h>

#include <QCoreApplication>
#include <memory>

#include "control/controlproxy.h"
#include "effects/backends/builtin/remixeffect.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectbuttonparameterslot.h"
#include "effects/effectchain.h"
#include "effects/effectknobparameterslot.h"
#include "effects/effectparameter.h"
#include "effects/effectslot.h"
#include "effects/effectsmanager.h"
#include "effects/presets/effectchainpreset.h"
#include "engine/channelhandle.h"
#include "test/mixxxtest.h"

namespace {

// Knob positions with the default parameter order: Remix, Side Boost, then
// the crossovers, low before high.
constexpr int kLowKnobOfLowAndMid = 2;
constexpr int kHighKnobOfMid = 3;
constexpr int kHighKnobOfHigh = 2;

class RemixCrossoverLinkTest : public MixxxTest {
  protected:
    void SetUp() override {
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager =
                std::make_shared<EffectsManager>(config(), pChannelHandleFactory);
        const QString mainOutputGroup = QStringLiteral("[MasterOutput]");
        m_pEffectsManager->registerInputChannel(ChannelHandleAndGroup(
                pChannelHandleFactory->getOrCreateHandle(mainOutputGroup), mainOutputGroup));
        m_pEffectsManager->setup();
    }

    void TearDown() override {
        for (int unitIndex = 0; unitIndex < kNumStandardEffectUnits; ++unitIndex) {
            auto pEmptyPreset = EffectChainPresetPointer::create();
            pEmptyPreset->setName(QString());
            m_pEffectsManager->getStandardEffectChain(unitIndex)->loadChainPreset(pEmptyPreset);
        }
        m_pEffectsManager.reset();
    }

    EffectSlotPointer load(int unitIndex, int slotIndex, const QString& effectId) {
        EffectSlotPointer pSlot = m_pEffectsManager->getStandardEffectChain(unitIndex)
                                          ->getEffectSlot(slotIndex);
        pSlot->loadEffectWithDefaults(m_pEffectsManager->getBackendManager()->getManifest(
                effectId, EffectBackendType::BuiltIn));
        // Let the link settle a newly loaded effect's crossovers.
        QCoreApplication::processEvents();
        return pSlot;
    }

    static ControlProxy knob(const EffectSlotPointer& pSlot, int knobIndex) {
        return ControlProxy(pSlot->getGroup(),
                EffectKnobParameterSlot::formatItemPrefix(knobIndex));
    }

    static ControlProxy slopeButton(const EffectSlotPointer& pSlot) {
        return ControlProxy(pSlot->getGroup(), EffectButtonParameterSlot::formatItemPrefix(0));
    }

    std::shared_ptr<EffectsManager> m_pEffectsManager;
};

TEST_F(RemixCrossoverLinkTest, TurningOneCrossoverTurnsItsPartner) {
    auto pLow = load(0, 0, RemixLowEffect::getId());
    auto pMid = load(0, 1, RemixMidEffect::getId());
    auto pHigh = load(0, 2, RemixHighEffect::getId());

    knob(pLow, kLowKnobOfLowAndMid).set(400);
    EXPECT_DOUBLE_EQ(400, knob(pMid, kLowKnobOfLowAndMid).get());

    knob(pHigh, kHighKnobOfHigh).set(4000);
    EXPECT_DOUBLE_EQ(4000, knob(pMid, kHighKnobOfMid).get());

    // And back the other way, from Mid's side.
    knob(pMid, kLowKnobOfLowAndMid).set(150);
    EXPECT_DOUBLE_EQ(150, knob(pLow, kLowKnobOfLowAndMid).get());
    knob(pMid, kHighKnobOfMid).set(6000);
    EXPECT_DOUBLE_EQ(6000, knob(pHigh, kHighKnobOfHigh).get());
    // The low crossover is not the high one.
    EXPECT_DOUBLE_EQ(150, knob(pMid, kLowKnobOfLowAndMid).get());
}

TEST_F(RemixCrossoverLinkTest, ANewcomerTakesOverTheUnitsCrossover) {
    auto pLow = load(0, 0, RemixLowEffect::getId());
    knob(pLow, kLowKnobOfLowAndMid).set(500);

    auto pMid = load(0, 1, RemixMidEffect::getId());
    EXPECT_DOUBLE_EQ(500, knob(pMid, kLowKnobOfLowAndMid).get());
    EXPECT_DOUBLE_EQ(500, knob(pLow, kLowKnobOfLowAndMid).get());
}

TEST_F(RemixCrossoverLinkTest, AHiddenCrossoverStillFollows) {
    auto pLow = load(0, 0, RemixLowEffect::getId());
    auto pMid = load(0, 1, RemixMidEffect::getId());
    EffectParameterPointer pMidLow;
    for (const auto& pParameter :
            pMid->getLoadedParameters().value(EffectParameterType::Knob)) {
        if (pParameter->manifest()->id() == remix::kLowCrossoverId) {
            pMidLow = pParameter;
        }
    }
    ASSERT_TRUE(pMidLow);
    pMid->hideParameter(pMidLow);

    knob(pLow, kLowKnobOfLowAndMid).set(600);
    EXPECT_DOUBLE_EQ(600, pMidLow->getValue());
}

TEST_F(RemixCrossoverLinkTest, FlippingOneSlopeFlipsThemAll) {
    auto pLow = load(0, 0, RemixLowEffect::getId());
    auto pMid = load(0, 1, RemixMidEffect::getId());
    auto pHigh = load(0, 2, RemixHighEffect::getId());
    EXPECT_DOUBLE_EQ(0, slopeButton(pMid).get());

    slopeButton(pMid).set(1);
    EXPECT_DOUBLE_EQ(1, slopeButton(pLow).get());
    EXPECT_DOUBLE_EQ(1, slopeButton(pHigh).get());

    slopeButton(pLow).set(0);
    EXPECT_DOUBLE_EQ(0, slopeButton(pMid).get());
    EXPECT_DOUBLE_EQ(0, slopeButton(pHigh).get());
}

TEST_F(RemixCrossoverLinkTest, UnitsAreIndependent) {
    auto pLow1 = load(0, 0, RemixLowEffect::getId());
    auto pMid2 = load(1, 1, RemixMidEffect::getId());
    const double before = knob(pMid2, kLowKnobOfLowAndMid).get();

    knob(pLow1, kLowKnobOfLowAndMid).set(700);
    EXPECT_DOUBLE_EQ(before, knob(pMid2, kLowKnobOfLowAndMid).get());
}

} // namespace
