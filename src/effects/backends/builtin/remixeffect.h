#pragma once

#include <QString>
#include <memory>

#include "effects/backends/effectprocessor.h"
#include "engine/filters/enginefilterlinkwitzriley4.h"
#include "engine/filters/enginefilterlinkwitzriley8.h"
#include "util/samplebuffer.h"

/// The part of the spectrum a Remix effect works on.
enum class RemixBand {
    Low,
    Mid,
    High,
};

namespace remix {

/// Parameter ids of the crossover knobs and the slope switch. A parameter keeps
/// the same id in every Remix effect that has it, and RemixCrossoverLink keeps
/// parameters with the same id at the same value across the Remix effects of
/// one effect unit, so that neighbouring bands meet at the same frequency with
/// the same slope.
inline const QString kLowCrossoverId = QStringLiteral("lowCrossover");
inline const QString kHighCrossoverId = QStringLiteral("highCrossover");
inline const QString kSlopeId = QStringLiteral("slope");

bool isRemixEffect(const QString& effectId);

} // namespace remix

/// The crossover filters of a Remix effect in one slope.
template<class LowPass, class HighPass>
class RemixFilters {
  public:
    RemixFilters(mixxx::audio::SampleRate sampleRate, double splitHz, double innerHz);

    /// splitHz is where the input is split first: the low crossover for Low,
    /// the high one for Mid and High. innerHz is only used by Mid.
    void setCrossovers(mixxx::audio::SampleRate sampleRate, double splitHz, double innerHz);
    /// Forget the filter history so processing starts cleanly from the dry signal.
    void restart();
    /// Splits pInput into the band that gets remixed and the rest, which passes
    /// through. Both come out of the same all-pass split, so they add up to
    /// the input with a flat level. The scratch buffers are working space.
    void split(RemixBand band,
            const CSAMPLE* pInput,
            CSAMPLE* pBand,
            CSAMPLE* pRest,
            CSAMPLE* pScratch1,
            CSAMPLE* pScratch2,
            SINT numSamples);

  private:
    // Low and High split the input once, at their own crossover. Mid splits it
    // at the high crossover and then splits the part below at the low crossover.
    std::unique_ptr<LowPass> m_pSplitLow;
    std::unique_ptr<HighPass> m_pSplitHigh;
    std::unique_ptr<LowPass> m_pInnerLow;
    std::unique_ptr<HighPass> m_pInnerHigh;
    // Mid only: the part above the high crossover goes through the same split
    // at the low crossover and is summed again. That gives it the phase shift
    // the other two paths picked up, so the three still add up flat.
    std::unique_ptr<LowPass> m_pAlignLow;
    std::unique_ptr<HighPass> m_pAlignHigh;
};

class RemixGroupState : public EffectState {
  public:
    explicit RemixGroupState(const mixxx::EngineParameters& engineParameters);
    ~RemixGroupState() override = default;

    // Both slopes run all the time, so switching between them is a short
    // crossfade between two settled outputs rather than a restart.
    RemixFilters<EngineFilterLinkwitzRiley4Low, EngineFilterLinkwitzRiley4High> m_slope24;
    RemixFilters<EngineFilterLinkwitzRiley8Low, EngineFilterLinkwitzRiley8High> m_slope48;

    mixxx::SampleBuffer m_band24;
    mixxx::SampleBuffer m_rest24;
    mixxx::SampleBuffer m_band48;
    mixxx::SampleBuffer m_rest48;
    mixxx::SampleBuffer m_scratch1;
    mixxx::SampleBuffer m_scratch2;

    mixxx::audio::SampleRate m_sampleRate;
    double m_splitHz;
    double m_innerHz;

    // Gains applied to the band's mid and side, eased towards their targets
    CSAMPLE_GAIN m_midGain;
    CSAMPLE_GAIN m_sideGain;
    // How much of the 48 dB/oct output is heard, eased between 0 and 1
    CSAMPLE_GAIN m_steepness;
};

/// Removes the centre (mid) of the stereo image in one band and keeps the
/// sides, so what is panned dead centre there, typically kick, bass, snare and
/// hats, drops out while wide synths and pads stay. The rest of the spectrum
/// passes unchanged, so a Low, a Mid and a High effect in one unit can each
/// switch its band in and out.
template<RemixBand band>
class RemixEffect : public EffectProcessorImpl<RemixGroupState> {
  public:
    RemixEffect() = default;
    ~RemixEffect() override = default;
    RemixEffect(const RemixEffect&) = delete;
    RemixEffect& operator=(const RemixEffect&) = delete;
    RemixEffect(RemixEffect&&) = delete;
    RemixEffect& operator=(RemixEffect&&) = delete;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            RemixGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pRemixParameter;
    EngineEffectParameterPointer m_pSideBoostParameter;
    EngineEffectParameterPointer m_pLowCrossoverParameter;
    EngineEffectParameterPointer m_pHighCrossoverParameter;
    EngineEffectParameterPointer m_pSlopeParameter;
};

extern template class RemixEffect<RemixBand::Low>;
extern template class RemixEffect<RemixBand::Mid>;
extern template class RemixEffect<RemixBand::High>;

using RemixLowEffect = RemixEffect<RemixBand::Low>;
using RemixMidEffect = RemixEffect<RemixBand::Mid>;
using RemixHighEffect = RemixEffect<RemixBand::High>;
