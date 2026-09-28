#include "effects/backends/builtin/remixeffect.h"

#include <algorithm>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/math.h"

namespace {

const QString kRemixLowId = QStringLiteral("org.mixxx.effects.remixlow");
const QString kRemixMidId = QStringLiteral("org.mixxx.effects.remixmid");
const QString kRemixHighId = QStringLiteral("org.mixxx.effects.remixhigh");

// The two knob ranges do not overlap, so Remix Mid's band can never turn
// inside out. The defaults are the deck EQ's default crossovers.
constexpr double kLowCrossoverMinHz = 40;
constexpr double kLowCrossoverDefaultHz = 250;
constexpr double kLowCrossoverMaxHz = 1000;
constexpr double kHighCrossoverMinHz = 1000;
constexpr double kHighCrossoverDefaultHz = 2500;
constexpr double kHighCrossoverMaxHz = 16000;

constexpr double kMaxSideBoostDb = 12;

// How long the band's gains take to travel from the original to fully
// remixed. It softens the switch-on and any jump of the Remix knob; the
// switch-off is the engine's one-buffer crossfade to dry.
constexpr double kFadeSeconds = 0.05;

// How long a flip of the Slope switch takes to cross over to the other slope.
constexpr double kSlopeFadeSeconds = 0.02;

// Keep a corner well below Nyquist whatever the sample rate.
double limitCorner(double hz, mixxx::audio::SampleRate sampleRate) {
    return std::min(hz, 0.45 * static_cast<double>(sampleRate.value()));
}

CSAMPLE_GAIN approach(CSAMPLE_GAIN current, CSAMPLE_GAIN target, CSAMPLE_GAIN step) {
    if (current < target) {
        return std::min(current + step, target);
    }
    return std::max(current - step, target);
}

void addRemixParameters(EffectManifestPointer pManifest) {
    EffectManifestParameterPointer remix = pManifest->addParameter();
    remix->setId("remix");
    remix->setName(QObject::tr("Remix"));
    remix->setShortName(QObject::tr("Remix"));
    remix->setDescription(QObject::tr(
            "How much of the centre of the stereo image is removed in this band\n"
            "Fully left: the original\n"
            "Fully right: the centre is gone and only the sides remain"));
    remix->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    remix->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    remix->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    remix->setRange(0.0, 1.0, 1.0);

    EffectManifestParameterPointer sideBoost = pManifest->addParameter();
    sideBoost->setId("sideBoost");
    sideBoost->setName(QObject::tr("Side Boost"));
    sideBoost->setShortName(QObject::tr("Side"));
    sideBoost->setDescription(QObject::tr(
            "Raises the sides of this band as its centre is removed, to make up "
            "for the lost level. It applies in full when Remix is fully right."));
    sideBoost->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    sideBoost->setUnitsHint(EffectManifestParameter::UnitsHint::Decibel);
    sideBoost->setDefaultLinkType(EffectManifestParameter::LinkType::None);
    sideBoost->setRange(0.0, 0.0, kMaxSideBoostDb);
}

void addLowCrossoverParameter(EffectManifestPointer pManifest) {
    EffectManifestParameterPointer crossover = pManifest->addParameter();
    crossover->setId(remix::kLowCrossoverId);
    crossover->setName(QObject::tr("Low Crossover"));
    crossover->setShortName(QObject::tr("Low Fr."));
    crossover->setDescription(QObject::tr(
            "Where the low band ends and the mid band starts\n"
            "Remix Low and Remix Mid in the same effect unit share this knob."));
    crossover->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    crossover->setUnitsHint(EffectManifestParameter::UnitsHint::Hertz);
    crossover->setDefaultLinkType(EffectManifestParameter::LinkType::None);
    crossover->setRange(kLowCrossoverMinHz, kLowCrossoverDefaultHz, kLowCrossoverMaxHz);
}

void addHighCrossoverParameter(EffectManifestPointer pManifest) {
    EffectManifestParameterPointer crossover = pManifest->addParameter();
    crossover->setId(remix::kHighCrossoverId);
    crossover->setName(QObject::tr("High Crossover"));
    crossover->setShortName(QObject::tr("High Fr."));
    crossover->setDescription(QObject::tr(
            "Where the mid band ends and the high band starts\n"
            "Remix Mid and Remix High in the same effect unit share this knob."));
    crossover->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    crossover->setUnitsHint(EffectManifestParameter::UnitsHint::Hertz);
    crossover->setDefaultLinkType(EffectManifestParameter::LinkType::None);
    crossover->setRange(kHighCrossoverMinHz, kHighCrossoverDefaultHz, kHighCrossoverMaxHz);
}

void addSlopeParameter(EffectManifestPointer pManifest) {
    EffectManifestParameterPointer slope = pManifest->addParameter();
    slope->setId(remix::kSlopeId);
    slope->setName(QObject::tr("Slope"));
    slope->setShortName(QObject::tr("Slope"));
    slope->setDescription(QObject::tr(
            "How steeply the band's edges fall off\n"
            "24 dB/oct: softer edges that blend into the neighbouring bands\n"
            "48 dB/oct: sharper edges, so less of the neighbouring bands is touched\n"
            "All the Remix effects in the same effect unit switch together."));
    slope->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    slope->setRange(0, 0, 1);
    slope->appendStep(qMakePair(QObject::tr("24 dB/oct"), 0));
    slope->appendStep(qMakePair(QObject::tr("48 dB/oct"), 1));
}

} // anonymous namespace

bool remix::isRemixEffect(const QString& effectId) {
    return effectId == kRemixLowId || effectId == kRemixMidId || effectId == kRemixHighId;
}

template<class LowPass, class HighPass>
RemixFilters<LowPass, HighPass>::RemixFilters(
        mixxx::audio::SampleRate sampleRate, double splitHz, double innerHz)
        : m_pSplitLow(std::make_unique<LowPass>(sampleRate, splitHz)),
          m_pSplitHigh(std::make_unique<HighPass>(sampleRate, splitHz)),
          m_pInnerLow(std::make_unique<LowPass>(sampleRate, innerHz)),
          m_pInnerHigh(std::make_unique<HighPass>(sampleRate, innerHz)),
          m_pAlignLow(std::make_unique<LowPass>(sampleRate, innerHz)),
          m_pAlignHigh(std::make_unique<HighPass>(sampleRate, innerHz)) {
    // On a fresh start each high pass fades in from the dry signal and each
    // low pass from silence, so a split starts out summing to the dry signal.
    m_pSplitHigh->setStartFromDry(true);
    m_pInnerHigh->setStartFromDry(true);
    m_pAlignHigh->setStartFromDry(true);
}

template<class LowPass, class HighPass>
void RemixFilters<LowPass, HighPass>::setCrossovers(
        mixxx::audio::SampleRate sampleRate, double splitHz, double innerHz) {
    const double split = limitCorner(splitHz, sampleRate);
    const double inner = limitCorner(innerHz, sampleRate);
    m_pSplitLow->setFrequencyCorners(sampleRate, split);
    m_pSplitHigh->setFrequencyCorners(sampleRate, split);
    m_pInnerLow->setFrequencyCorners(sampleRate, inner);
    m_pInnerHigh->setFrequencyCorners(sampleRate, inner);
    m_pAlignLow->setFrequencyCorners(sampleRate, inner);
    m_pAlignHigh->setFrequencyCorners(sampleRate, inner);
}

template<class LowPass, class HighPass>
void RemixFilters<LowPass, HighPass>::restart() {
    m_pSplitLow->pauseFilter();
    m_pSplitHigh->pauseFilter();
    m_pInnerLow->pauseFilter();
    m_pInnerHigh->pauseFilter();
    m_pAlignLow->pauseFilter();
    m_pAlignHigh->pauseFilter();
}

template<class LowPass, class HighPass>
void RemixFilters<LowPass, HighPass>::split(RemixBand band,
        const CSAMPLE* pInput,
        CSAMPLE* pBand,
        CSAMPLE* pRest,
        CSAMPLE* pScratch1,
        CSAMPLE* pScratch2,
        SINT numSamples) {
    switch (band) {
    case RemixBand::Low:
        m_pSplitLow->process(pInput, pBand, numSamples);
        m_pSplitHigh->process(pInput, pRest, numSamples);
        break;
    case RemixBand::High:
        m_pSplitLow->process(pInput, pRest, numSamples);
        m_pSplitHigh->process(pInput, pBand, numSamples);
        break;
    case RemixBand::Mid: {
        CSAMPLE* pBelow = pScratch1;
        CSAMPLE* pAbove = pRest;
        m_pSplitLow->process(pInput, pBelow, numSamples);
        m_pSplitHigh->process(pInput, pAbove, numSamples);
        m_pInnerHigh->process(pBelow, pBand, numSamples);
        m_pInnerLow->process(pBelow, pBelow, numSamples);
        m_pAlignLow->process(pAbove, pScratch2, numSamples);
        m_pAlignHigh->process(pAbove, pAbove, numSamples);
        for (SINT i = 0; i < numSamples; ++i) {
            pRest[i] += pBelow[i] + pScratch2[i];
        }
        break;
    }
    }
}

template class RemixFilters<EngineFilterLinkwitzRiley4Low, EngineFilterLinkwitzRiley4High>;
template class RemixFilters<EngineFilterLinkwitzRiley8Low, EngineFilterLinkwitzRiley8High>;

RemixGroupState::RemixGroupState(const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          m_slope24(engineParameters.sampleRate(),
                  kHighCrossoverDefaultHz,
                  kLowCrossoverDefaultHz),
          m_slope48(engineParameters.sampleRate(),
                  kHighCrossoverDefaultHz,
                  kLowCrossoverDefaultHz),
          m_band24(engineParameters.samplesPerBuffer()),
          m_rest24(engineParameters.samplesPerBuffer()),
          m_band48(engineParameters.samplesPerBuffer()),
          m_rest48(engineParameters.samplesPerBuffer()),
          m_scratch1(engineParameters.samplesPerBuffer()),
          m_scratch2(engineParameters.samplesPerBuffer()),
          m_sampleRate(engineParameters.sampleRate()),
          m_splitHz(kHighCrossoverDefaultHz),
          m_innerHz(kLowCrossoverDefaultHz),
          m_midGain(1),
          m_sideGain(1),
          m_steepness(0) {
}

// static
template<RemixBand band>
QString RemixEffect<band>::getId() {
    switch (band) {
    case RemixBand::Low:
        return kRemixLowId;
    case RemixBand::Mid:
        return kRemixMidId;
    case RemixBand::High:
        break;
    }
    return kRemixHighId;
}

// static
template<RemixBand band>
EffectManifestPointer RemixEffect<band>::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    switch (band) {
    case RemixBand::Low:
        pManifest->setName(QObject::tr("Remix Low"));
        pManifest->setShortName(QObject::tr("Remix Lo"));
        pManifest->setDescription(QObject::tr(
                "Removes the centre of the stereo image below the low "
                "crossover, where kick and bass usually sit, and keeps the "
                "sides. Load Remix Mid and Remix High next to it to switch "
                "each band on its own."));
        break;
    case RemixBand::Mid:
        pManifest->setName(QObject::tr("Remix Mid"));
        pManifest->setShortName(QObject::tr("Remix Mid"));
        pManifest->setDescription(QObject::tr(
                "Removes the centre of the stereo image between the low and "
                "high crossovers, where snare, clap and vocals usually sit, and "
                "keeps the sides. Load Remix Low and Remix High next to it to "
                "switch each band on its own."));
        break;
    case RemixBand::High:
        pManifest->setName(QObject::tr("Remix High"));
        pManifest->setShortName(QObject::tr("Remix Hi"));
        pManifest->setDescription(QObject::tr(
                "Removes the centre of the stereo image above the high "
                "crossover, where hats usually sit, and keeps the sides. Load "
                "Remix Low and Remix Mid next to it to switch each band on its "
                "own."));
        break;
    }
    pManifest->setAuthor("Grizbeard");
    pManifest->setVersion("1.0");
    pManifest->setMetaknobDefault(1.0);

    addRemixParameters(pManifest);
    if (band != RemixBand::High) {
        addLowCrossoverParameter(pManifest);
    }
    if (band != RemixBand::Low) {
        addHighCrossoverParameter(pManifest);
    }
    addSlopeParameter(pManifest);
    return pManifest;
}

template<RemixBand band>
void RemixEffect<band>::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pRemixParameter = parameters.value("remix");
    m_pSideBoostParameter = parameters.value("sideBoost");
    m_pLowCrossoverParameter = parameters.value(remix::kLowCrossoverId);
    m_pHighCrossoverParameter = parameters.value(remix::kHighCrossoverId);
    m_pSlopeParameter = parameters.value(remix::kSlopeId);
}

template<RemixBand band>
void RemixEffect<band>::processChannel(
        RemixGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);
    const SINT numSamples = engineParameters.samplesPerBuffer();
    const mixxx::audio::SampleRate sampleRate = engineParameters.sampleRate();

    double splitHz = pState->m_splitHz;
    double innerHz = pState->m_innerHz;
    if (band == RemixBand::Low) {
        splitHz = m_pLowCrossoverParameter->value();
    } else {
        splitHz = m_pHighCrossoverParameter->value();
        if (band == RemixBand::Mid) {
            innerHz = m_pLowCrossoverParameter->value();
        }
    }
    if (sampleRate != pState->m_sampleRate ||
            splitHz != pState->m_splitHz ||
            innerHz != pState->m_innerHz) {
        pState->m_slope24.setCrossovers(sampleRate, splitHz, innerHz);
        pState->m_slope48.setCrossovers(sampleRate, splitHz, innerHz);
        pState->m_sampleRate = sampleRate;
        pState->m_splitHz = splitHz;
        pState->m_innerHz = innerHz;
    }

    const CSAMPLE_GAIN steepnessTarget = m_pSlopeParameter->toBool() ? 1 : 0;
    if (enableState == EffectEnableState::Enabling) {
        // The filters last ran when the effect was switched off, so their
        // history is stale. Start again from the original.
        pState->m_slope24.restart();
        pState->m_slope48.restart();
        pState->m_midGain = 1;
        pState->m_sideGain = 1;
        pState->m_steepness = steepnessTarget;
    }

    CSAMPLE_GAIN midTarget = 1;
    CSAMPLE_GAIN sideTarget = 1;
    if (enableState != EffectEnableState::Disabling) {
        const auto remix = static_cast<CSAMPLE_GAIN>(m_pRemixParameter->value());
        const auto sideBoost = static_cast<CSAMPLE_GAIN>(
                db2ratio(m_pSideBoostParameter->value()));
        midTarget = 1 - remix;
        sideTarget = 1 + (sideBoost - 1) * remix;
    }

    CSAMPLE* pBand24 = pState->m_band24.data();
    CSAMPLE* pRest24 = pState->m_rest24.data();
    CSAMPLE* pBand48 = pState->m_band48.data();
    CSAMPLE* pRest48 = pState->m_rest48.data();
    CSAMPLE* pScratch1 = pState->m_scratch1.data();
    CSAMPLE* pScratch2 = pState->m_scratch2.data();
    pState->m_slope24.split(band, pInput, pBand24, pRest24, pScratch1, pScratch2, numSamples);
    pState->m_slope48.split(band, pInput, pBand48, pRest48, pScratch1, pScratch2, numSamples);

    const double framesPerSecond = sampleRate.value();
    const auto gainStep = static_cast<CSAMPLE_GAIN>(1 / (kFadeSeconds * framesPerSecond));
    const auto slopeStep = static_cast<CSAMPLE_GAIN>(1 / (kSlopeFadeSeconds * framesPerSecond));
    CSAMPLE_GAIN midGain = pState->m_midGain;
    CSAMPLE_GAIN sideGain = pState->m_sideGain;
    CSAMPLE_GAIN steepness = pState->m_steepness;
    for (SINT i = 0; i < numSamples; i += 2) {
        midGain = approach(midGain, midTarget, gainStep);
        sideGain = approach(sideGain, sideTarget, gainStep);
        steepness = approach(steepness, steepnessTarget, slopeStep);
        const CSAMPLE bandL = pBand24[i] + steepness * (pBand48[i] - pBand24[i]);
        const CSAMPLE bandR = pBand24[i + 1] + steepness * (pBand48[i + 1] - pBand24[i + 1]);
        const CSAMPLE restL = pRest24[i] + steepness * (pRest48[i] - pRest24[i]);
        const CSAMPLE restR = pRest24[i + 1] + steepness * (pRest48[i + 1] - pRest24[i + 1]);
        const CSAMPLE mid = (bandL + bandR) * 0.5f;
        const CSAMPLE side = (bandL - bandR) * 0.5f;
        pOutput[i] = restL + midGain * mid + sideGain * side;
        pOutput[i + 1] = restR + midGain * mid - sideGain * side;
    }
    pState->m_midGain = midGain;
    pState->m_sideGain = sideGain;
    pState->m_steepness = steepness;
}

template class RemixEffect<RemixBand::Low>;
template class RemixEffect<RemixBand::Mid>;
template class RemixEffect<RemixBand::High>;
