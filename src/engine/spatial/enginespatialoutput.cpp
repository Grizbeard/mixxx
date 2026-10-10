#include "engine/spatial/enginespatialoutput.h"

#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/spatial/spatialautopan.h"
#include "engine/spatial/spatialcontrols.h"
#include "engine/spatial/spatialdelay.h"
#include "engine/spatial/spatialreverb.h"
#include "util/defs.h"

using namespace mixxx::spatial;

namespace {

const QString kGroup = QStringLiteral("[Spatial]");

// Paul Kellet's pink filter has an RMS of ~1.747 on uniform white noise in
// [-1, 1]; this brings it to an RMS of 1, so test_level_db reads as RMS.
constexpr double kPinkNormalisation = 1.0 / 1.747;
constexpr double kLimiterLookaheadSeconds = 0.0015;
constexpr double kLimiterReleaseSeconds = 0.15;
constexpr int kLimiterWindow = 1024; // > lookahead at 192 kHz, power of two

CSAMPLE_GAIN dbToGain(double db) {
    return static_cast<CSAMPLE_GAIN>(std::pow(10.0, db / 20.0));
}

QString speakerKey(int speaker, const char* suffix) {
    return QStringLiteral("speaker_%1_%2").arg(speaker + 1).arg(suffix);
}

} // namespace

QString EngineSpatialOutput::sourceGroup(int deck, int slot) {
    static constexpr int kStemOfSlot[kSlots] = {1, 2, 4};
    return QStringLiteral("[Channel%1_Stem%2]").arg(deck + 1).arg(kStemOfSlot[slot]);
}

EngineSpatialOutput::EngineSpatialOutput()
        : m_buffer(kMaxEngineFrames * kMaxChannels),
          m_pChannelCount(std::make_unique<ControlObject>(ConfigKey(kGroup, "channel_count"))),
          m_pChannelBase(std::make_unique<ControlObject>(ConfigKey(kGroup, "channel_base"))),
          m_pSpeakerCount(makePot(kGroup, "speaker_count", kMinSpeakers, kMaxSpeakers, kMinSpeakers, true)),
          m_pLayoutCustom(makeToggle(kGroup, "layout_custom", 0, true)),
          m_pLfEnabled(std::make_unique<ControlPushButton>(
                  ConfigKey(kGroup, "lf_enabled"), true, 1.0)),
          m_pLfCrossover(makePot(kGroup, "lf_crossover_hz", 40, 200, 100, true)),
          m_pMainsHighpass(makeToggle(kGroup, "mains_highpass", 1, true)),
          m_pLfGainDb(makePot(kGroup, "lf_gain_db", -24, 12, 0, true)),
          m_pLfDelayMs(makePot(kGroup, "lf_delay_ms", 0, kMaxAlignmentSeconds * 1000, 0, true)),
          m_pLfInvert(makeToggle(kGroup, "lf_invert", 0, true)),
          m_pLimiterEnabled(makeToggle(kGroup, "limiter_enabled", 1, true)),
          m_pLimiterCeilingDb(makePot(kGroup, "limiter_ceiling_db", -12, 0, -0.3, true)),
          m_pLimiterReduction(makeReadOnly(kGroup, "limiter_reduction_db")),
          m_pBeat(makeReadOnly(kGroup, "beat")),
          m_pTestChannel(std::make_unique<ControlObject>(
                  ConfigKey(kGroup, "test_channel"), true, false, false, -1.0)),
          m_pTestSignal(std::make_unique<ControlPushButton>(ConfigKey(kGroup, "test_signal"))),
          m_pTestFrequency(makePot(kGroup, "test_frequency", 20, 20000, 1000)),
          m_pTestLevelDb(makePot(kGroup, "test_level_db", -60, -6, -30, true)),
          m_pMainPassthrough(std::make_unique<ControlPushButton>(
                  ConfigKey(kGroup, "main_passthrough"), true, 0.0)),
          m_pDelay(std::make_unique<SpatialDelay>()),
          m_pReverb(std::make_unique<SpatialReverb>()),
          m_rendered(kMaxDecks * kSlots, false),
          m_lf(kMaxEngineFrames),
          m_sendLeft(kMaxEngineFrames),
          m_sendRight(kMaxEngineFrames),
          m_reverbIn(kMaxEngineFrames) {
    m_pChannelCount->setReadOnly();
    m_pChannelBase->setReadOnly();
    m_pLfEnabled->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pTestSignal->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pMainPassthrough->setButtonMode(mixxx::control::ButtonMode::Toggle);

    const auto defaults = SpeakerLayout::defaultAzimuths(kMaxSpeakers);
    for (int i = 0; i < kMaxSpeakers; ++i) {
        m_pSpeakerAzimuth[i] = makePot(kGroup, speakerKey(i, "azimuth"), -180, 180, defaults[i], true);
        m_pSpeakerGainDb[i] = makePot(kGroup, speakerKey(i, "gain_db"), -24, 6, 0, true);
        m_pSpeakerDelayMs[i] = makePot(
                kGroup, speakerKey(i, "delay_ms"), 0, kMaxAlignmentSeconds * 1000, 0, true);
        m_bus[i].resize(kMaxEngineFrames);
    }
    for (int c = 0; c < kMaxChannels; ++c) {
        m_pVuMeter[c] = makeReadOnly(kGroup, QStringLiteral("vu_meter_%1").arg(c + 1));
    }
    for (int deck = 0; deck < kMaxDecks; ++deck) {
        for (int slot = 0; slot < kSlots; ++slot) {
            m_sources.push_back(std::make_unique<SpatialSource>(sourceGroup(deck, slot)));
        }
    }
    for (int unit = 0; unit < kAutopanUnits; ++unit) {
        m_autopans[unit] = std::make_unique<SpatialAutopan>(unit + 1);
        m_autopanPointers[unit] = m_autopans[unit].get();
    }
    for (auto& line : m_alignment) {
        line.allocate(static_cast<int>(kMaxAlignmentSeconds * SpatialDelay::kMaxSampleRate) + 4);
    }
    m_limiter.delay.assign(kLimiterWindow * kMaxChannels, 0.0f);
    m_limiter.windowValue.assign(kLimiterWindow, 1.0f);
    m_limiter.windowIndex.assign(kLimiterWindow, 0);
    m_buffer.clear();
}

EngineSpatialOutput::~EngineSpatialOutput() = default;

std::span<const CSAMPLE> EngineSpatialOutput::buffer(const AudioOutput& output) const {
    Q_UNUSED(output);
    return std::span<const CSAMPLE>(m_buffer.data(), m_buffer.size());
}

void EngineSpatialOutput::onOutputConnected(const AudioOutput& output) {
    const int channels = std::min<int>(output.getChannelGroup().getChannelCount(), kMaxChannels);
    m_channels.store(channels);
    m_pChannelCount->forceSet(channels);
    m_pChannelBase->forceSet(output.getChannelGroup().getChannelBase());
}

void EngineSpatialOutput::onOutputDisconnected(const AudioOutput& output) {
    Q_UNUSED(output);
    m_channels.store(0);
    m_pChannelCount->forceSet(0);
    m_buffer.clear();
}

bool EngineSpatialOutput::isRendering() const {
    return isConnected() && !m_pMainPassthrough->toBool();
}

void EngineSpatialOutput::updateLayout() {
    int count = std::clamp(static_cast<int>(std::lround(m_pSpeakerCount->get())),
            kMinSpeakers,
            kMaxSpeakers);
    const int channels = m_channels.load(std::memory_order_relaxed);
    if (channels > 0) {
        count = std::min(count, channels);
    }
    std::array<double, kMaxSpeakers> azimuths = SpeakerLayout::defaultAzimuths(count);
    if (m_pLayoutCustom->toBool()) {
        for (int i = 0; i < count; ++i) {
            azimuths[i] = m_pSpeakerAzimuth[i]->get();
        }
    }
    if (count != m_layoutCount || azimuths != m_layoutAzimuths) {
        m_layout.setSpeakers(count, azimuths.data());
        m_layoutCount = count;
        m_layoutAzimuths = azimuths;
    }
}

void EngineSpatialOutput::updateFilters(double sampleRate) {
    const double crossover = std::clamp(m_pLfCrossover->get(), 40.0, 200.0);
    if (sampleRate == m_filterRate && crossover == m_filterCrossover) {
        return;
    }
    for (auto& filter : m_highPass) {
        filter.setHighPass(crossover, sampleRate);
    }
    m_lowPass.setLowPass(crossover, sampleRate);
    m_testLowPass.setLowPass(crossover, sampleRate);
    m_passLowPass.setLowPass(crossover, sampleRate);
    m_filterRate = sampleRate;
    m_filterCrossover = crossover;
}

void EngineSpatialOutput::beginCallback(std::size_t frames,
        mixxx::audio::SampleRate sampleRate,
        const GroupFeatureState* pTempo,
        double fallbackBpm) {
    if (!sampleRate.isValid()) {
        return;
    }
    m_begun = true;
    m_frames = std::min<std::size_t>(frames, kMaxEngineFrames);
    m_sampleRate = sampleRate.toDouble();
    for (auto& channel : m_bus) {
        std::fill(channel.begin(), channel.begin() + m_frames, 0.0f);
    }
    for (auto* pChannel : {&m_lf, &m_sendLeft, &m_sendRight, &m_reverbIn}) {
        std::fill(pChannel->begin(), pChannel->begin() + m_frames, 0.0f);
    }
    std::fill(m_rendered.begin(), m_rendered.end(), false);
    updateLayout();

    std::optional<double> beatLength;
    std::optional<double> beatFraction;
    if (pTempo && pTempo->beat_length && pTempo->beat_fraction_buffer_end) {
        beatLength = pTempo->beat_length->seconds;
        beatFraction = pTempo->beat_fraction_buffer_end;
    }
    m_clock.advance(static_cast<double>(m_frames),
            m_sampleRate,
            beatLength,
            beatFraction,
            fallbackBpm);
    for (auto& pAutopan : m_autopans) {
        pAutopan->prepare(m_clock);
    }
    m_pBeat->forceSet(m_clock.beatsEnd());
}

void EngineSpatialOutput::addSource(int deck, int slot, const CSAMPLE* pStereo) {
    if (!m_begun || deck < 0 || deck >= kMaxDecks || slot < 0 || slot >= kSlots) {
        return;
    }
    SpatialRenderContext context;
    context.pLayout = &m_layout;
    context.ppAutopans = m_autopanPointers.data();
    context.autopanCount = kAutopanUnits;
    context.sampleRate = m_sampleRate;
    for (int i = 0; i < kMaxSpeakers; ++i) {
        context.bus.speakers[i] = m_bus[i].data();
    }
    context.bus.lf = m_lf.data();
    context.bus.sendLeft = m_sendLeft.data();
    context.bus.sendRight = m_sendRight.data();
    const int index = deck * kSlots + slot;
    m_sources[index]->render(pStereo, static_cast<int>(m_frames), context);
    m_rendered[index] = true;
}

void EngineSpatialOutput::addFront(const CSAMPLE* pStereo) {
    if (!m_begun) {
        return;
    }
    for (std::size_t f = 0; f < m_frames; ++f) {
        const CSAMPLE left = pStereo[2 * f];
        const CSAMPLE right = pStereo[2 * f + 1];
        m_bus[0][f] += left;
        m_bus[1][f] += right;
        m_lf[f] += 0.5f * (left + right);
    }
}

CSAMPLE EngineSpatialOutput::nextTestSample(double sampleRate) {
    if (m_pTestSignal->toBool()) {
        const double frequency = std::clamp(m_pTestFrequency->get(), 20.0, sampleRate * 0.45);
        m_sinePhase += 2 * M_PI * frequency / sampleRate;
        if (m_sinePhase > 2 * M_PI) {
            m_sinePhase -= 2 * M_PI;
        }
        // Peak sqrt(2) gives an RMS of 1, like the noise.
        return static_cast<CSAMPLE>(std::sin(m_sinePhase) * M_SQRT2);
    }
    // xorshift32 white noise in [-1, 1], then Paul Kellet's pink filter.
    m_noiseState ^= m_noiseState << 13;
    m_noiseState ^= m_noiseState >> 17;
    m_noiseState ^= m_noiseState << 5;
    const double white = m_noiseState / 2147483648.0 - 1.0;
    double* b = m_pink;
    b[0] = 0.99886 * b[0] + white * 0.0555179;
    b[1] = 0.99332 * b[1] + white * 0.0750759;
    b[2] = 0.96900 * b[2] + white * 0.1538520;
    b[3] = 0.86650 * b[3] + white * 0.3104856;
    b[4] = 0.55000 * b[4] + white * 0.5329522;
    b[5] = -0.7616 * b[5] - white * 0.0168980;
    const double pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362;
    b[6] = white * 0.115926;
    return static_cast<CSAMPLE>(pink * kPinkNormalisation);
}

void EngineSpatialOutput::renderBus(
        std::size_t frames, int speakers, int lfChannel, int channels) {
    const auto frameCount = static_cast<int>(frames);
    std::array<float*, kMaxSpeakers> bus{};
    for (int i = 0; i < kMaxSpeakers; ++i) {
        bus[i] = m_bus[i].data();
    }

    // Spatial FX unit: the delay feeds the speakers and, optionally, the
    // reverb; the reverb takes the sends too.
    m_pDelay->process(m_sendLeft.data(),
            m_sendRight.data(),
            frameCount,
            m_sampleRate,
            m_clock,
            m_layout,
            bus,
            m_lf.data(),
            m_reverbIn.data());
    for (int f = 0; f < frameCount; ++f) {
        m_reverbIn[f] += 0.5f * (m_sendLeft[f] + m_sendRight[f]);
    }
    m_pReverb->process(m_reverbIn.data(), frameCount, m_sampleRate, m_layout, bus, m_lf.data());

    // Master gain, crossover, alignment, trims.
    const bool lfEnabled = lfChannel >= 0;
    const bool highPass = lfEnabled && m_pMainsHighpass->toBool();
    const CSAMPLE_GAIN gainStep = (m_mainGain - m_mainGainOld) / frameCount;
    std::array<int, kMaxSpeakers + 1> delay{};
    for (int i = 0; i < speakers; ++i) {
        m_trimGain[i] = dbToGain(std::clamp(m_pSpeakerGainDb[i]->get(), -24.0, 6.0));
        delay[i] = std::clamp(
                static_cast<int>(std::lround(m_pSpeakerDelayMs[i]->get() / 1000.0 * m_sampleRate)),
                0,
                m_alignment[i].capacity() - 2);
    }
    const float lfSign = m_pLfInvert->toBool() ? -1.0f : 1.0f;
    m_trimGain[kMaxSpeakers] = lfSign * dbToGain(std::clamp(m_pLfGainDb->get(), -24.0, 12.0));
    delay[kMaxSpeakers] = std::clamp(
            static_cast<int>(std::lround(m_pLfDelayMs->get() / 1000.0 * m_sampleRate)),
            0,
            m_alignment[kMaxSpeakers].capacity() - 2);

    CSAMPLE* pOut = m_buffer.data();
    for (int f = 0; f < frameCount; ++f) {
        const CSAMPLE_GAIN gain = m_mainGainOld + gainStep * (f + 1);
        for (int i = 0; i < speakers; ++i) {
            double x = bus[i][f] * gain;
            if (highPass) {
                x = m_highPass[i].process(x);
            }
            m_alignment[i].write(static_cast<float>(x));
            pOut[f * channels + i] = m_alignment[i].readInteger(delay[i] + 1) * m_trimGain[i];
        }
        if (lfEnabled) {
            const double x = m_lowPass.process(m_lf[f] * gain);
            m_alignment[kMaxSpeakers].write(static_cast<float>(x));
            pOut[f * channels + lfChannel] =
                    m_alignment[kMaxSpeakers].readInteger(delay[kMaxSpeakers] + 1) *
                    m_trimGain[kMaxSpeakers];
        }
    }
    m_mainGainOld = m_mainGain;
}

void EngineSpatialOutput::runLimiter(
        CSAMPLE* pOut, std::size_t frames, int channels, double sampleRate) {
    Limiter& lim = m_limiter;
    const bool enabled = m_pLimiterEnabled->toBool();
    const float ceiling = dbToGain(std::clamp(m_pLimiterCeilingDb->get(), -12.0, 0.0));
    const int lookahead = std::clamp(
            static_cast<int>(std::lround(kLimiterLookaheadSeconds * sampleRate)),
            1,
            kLimiterWindow - 2);
    constexpr int kMask = kLimiterWindow - 1;
    const auto attack = static_cast<float>(1 - std::exp(-5.0 / lookahead));
    const auto release = static_cast<float>(
            smoothingCoefficient(kLimiterReleaseSeconds, 1, sampleRate));
    float minGain = 1;
    for (std::size_t f = 0; f < frames; ++f) {
        CSAMPLE* pFrame = pOut + f * channels;
        float peak = 0;
        for (int c = 0; c < channels; ++c) {
            peak = std::max(peak, std::abs(pFrame[c]));
        }
        const float target = enabled && peak > ceiling ? ceiling / peak : 1.0f;
        // Sliding minimum of the target over the lookahead window.
        const auto now = static_cast<unsigned>(lim.position);
        while (lim.tail != lim.head && lim.windowValue[(lim.tail - 1) & kMask] >= target) {
            lim.tail = (lim.tail - 1) & kMask;
        }
        lim.windowValue[lim.tail] = target;
        lim.windowIndex[lim.tail] = static_cast<int>(now);
        lim.tail = (lim.tail + 1) & kMask;
        while (((now - static_cast<unsigned>(lim.windowIndex[lim.head])) & 0x3fffffffu) >
                static_cast<unsigned>(lookahead)) {
            lim.head = (lim.head + 1) & kMask;
        }
        const float windowMin = lim.windowValue[lim.head];
        lim.gain += (windowMin - lim.gain) * (windowMin < lim.gain ? attack : release);
        minGain = std::min(minGain, lim.gain);

        // Delay the audio by the lookahead so the gain arrives first.
        float* pSlot = lim.delay.data() + (lim.position & kMask) * kMaxChannels;
        float* pDelayed =
                lim.delay.data() + ((lim.position - lookahead) & kMask) * kMaxChannels;
        for (int c = 0; c < channels; ++c) {
            pSlot[c] = pFrame[c];
        }
        for (int c = 0; c < channels; ++c) {
            float y = pDelayed[c] * lim.gain;
            if (enabled) {
                y = std::clamp(y, -ceiling, ceiling);
            }
            pFrame[c] = y;
        }
        lim.position = (lim.position + 1) & 0x3fffffff;
    }
    m_pLimiterReduction->forceSet(minGain < 1 ? 20 * std::log10(minGain) : 0.0);
}

void EngineSpatialOutput::process(const CSAMPLE* pMain,
        std::size_t frames,
        mixxx::audio::SampleRate sampleRate) {
    if (!m_begun) {
        beginCallback(frames, sampleRate, nullptr, 0);
    }
    m_begun = false;
    for (std::size_t i = 0; i < m_sources.size(); ++i) {
        m_sources[i]->publish(m_rendered[i]);
    }
    const int channels = m_channels.load(std::memory_order_relaxed);
    if (channels <= 0 || frames == 0 || !sampleRate.isValid()) {
        return;
    }
    frames = std::min<std::size_t>(frames, kMaxEngineFrames);
    CSAMPLE* pOut = m_buffer.data();
    std::fill(pOut, pOut + frames * channels, CSAMPLE_ZERO);

    const int speakers = m_layout.count();
    const int lfChannel = m_pLfEnabled->toBool() && speakers < channels ? speakers : -1;
    const double rate = sampleRate.toDouble();
    updateFilters(rate);

    int target = static_cast<int>(std::lround(m_pTestChannel->get()));
    if (target < 0 || target >= channels) {
        target = -1;
    }
    const bool testing = target >= 0 || m_testActiveChannel >= 0;
    const bool passthrough = m_pMainPassthrough->toBool();

    // The spatial mix, faded out while a test or the passthrough plays.
    const CSAMPLE_GAIN programTarget = (!testing && !passthrough) ? 1.0f : 0.0f;
    if (programTarget > 0 || m_programGain > 0) {
        renderBus(frames, speakers, lfChannel, channels);
        runLimiter(pOut, frames, channels, rate);
        if (programTarget != m_programGain) {
            const CSAMPLE_GAIN step = (programTarget - m_programGain) / frames;
            for (std::size_t f = 0; f < frames; ++f) {
                const CSAMPLE_GAIN g = m_programGain + step * (f + 1);
                for (int c = 0; c < channels; ++c) {
                    pOut[f * channels + c] *= g;
                }
            }
        }
    } else {
        m_mainGainOld = m_mainGain;
    }
    m_programGain = programTarget;

    // Main-mix passthrough to the front pair and LF, faded in and out over one
    // buffer so switching it never clicks.
    const CSAMPLE_GAIN passTarget = passthrough ? 1.0f : 0.0f;
    if (pMain && (passTarget > 0 || m_passGain > 0)) {
        const CSAMPLE_GAIN step = (passTarget - m_passGain) / frames;
        for (std::size_t f = 0; f < frames; ++f) {
            const CSAMPLE_GAIN g = m_passGain + step * (f + 1);
            const CSAMPLE left = pMain[2 * f];
            const CSAMPLE right = pMain[2 * f + 1];
            pOut[f * channels] += left * g;
            if (speakers >= 2) {
                pOut[f * channels + 1] += right * g;
            }
            if (lfChannel >= 0) {
                const double mono = 0.5 * (left + right);
                const double lf = m_passLowPass.process(mono);
                pOut[f * channels + lfChannel] += static_cast<CSAMPLE>(lf) * g;
            }
        }
        m_passGain = passTarget;
    }

    // Test signal on one channel. Moving it to another channel first fades it
    // out where it is, then in on the new channel next callback.
    const CSAMPLE_GAIN level = dbToGain(std::clamp(m_pTestLevelDb->get(), -60.0, -6.0));
    const CSAMPLE_GAIN gainTarget =
            (target == m_testActiveChannel && target >= 0) ? level : 0.0f;
    if (m_testActiveChannel >= 0 && m_testActiveChannel < channels &&
            (gainTarget > 0 || m_testGain > 0)) {
        const CSAMPLE_GAIN step = (gainTarget - m_testGain) / frames;
        const bool bandLimit = m_testActiveChannel == lfChannel;
        for (std::size_t f = 0; f < frames; ++f) {
            CSAMPLE sample = nextTestSample(rate);
            if (bandLimit) {
                sample = static_cast<CSAMPLE>(m_testLowPass.process(sample));
            }
            pOut[f * channels + m_testActiveChannel] += sample * (m_testGain + step * (f + 1));
        }
    }
    m_testGain = gainTarget;
    if (m_testGain == 0) {
        m_testActiveChannel = target;
    }

    // Output meters.
    for (int c = 0; c < channels; ++c) {
        float peak = 0;
        for (std::size_t f = 0; f < frames; ++f) {
            peak = std::max(peak, std::abs(pOut[f * channels + c]));
        }
        const float meter = std::max(peak, m_vuPeak[c] * 0.9f);
        const float shown = meter < 1e-4f ? 0.0f : meter;
        if (shown != m_vuPeak[c]) {
            m_pVuMeter[c]->forceSet(shown);
        }
        m_vuPeak[c] = shown;
    }
}
