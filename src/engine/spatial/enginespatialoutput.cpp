#include "engine/spatial/enginespatialoutput.h"

#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "util/defs.h"

namespace {

const QString kGroup = QStringLiteral("[Spatial]");

// Paul Kellet's pink filter has an RMS of ~1.747 on uniform white noise in
// [-1, 1]; this brings it to an RMS of 1, so test_level_db reads as RMS.
constexpr double kPinkNormalisation = 1.0 / 1.747;
constexpr double kButterworthQ = 0.70710678118654752;

CSAMPLE_GAIN dbToGain(double db) {
    return static_cast<CSAMPLE_GAIN>(std::pow(10.0, db / 20.0));
}

} // namespace

void EngineSpatialOutput::Biquad::setLowPass(double frequency, double sampleRate) {
    const double w0 = 2 * M_PI * frequency / sampleRate;
    const double alpha = std::sin(w0) / (2 * kButterworthQ);
    const double cosw = std::cos(w0);
    const double a0 = 1 + alpha;
    b0 = (1 - cosw) / 2 / a0;
    b1 = (1 - cosw) / a0;
    b2 = b0;
    a1 = -2 * cosw / a0;
    a2 = (1 - alpha) / a0;
}

EngineSpatialOutput::EngineSpatialOutput()
        : m_buffer(kMaxEngineFrames * kMaxChannels),
          m_pChannelCount(std::make_unique<ControlObject>(ConfigKey(kGroup, "channel_count"))),
          m_pChannelBase(std::make_unique<ControlObject>(ConfigKey(kGroup, "channel_base"))),
          m_pSpeakerCount(std::make_unique<ControlPotmeter>(ConfigKey(kGroup, "speaker_count"),
                  kMinSpeakers,
                  kMaxSpeakers,
                  false,
                  true,
                  false,
                  true,
                  kMinSpeakers)),
          m_pLfEnabled(std::make_unique<ControlPushButton>(
                  ConfigKey(kGroup, "lf_enabled"), true, 1.0)),
          m_pLfCrossover(std::make_unique<ControlPotmeter>(
                  ConfigKey(kGroup, "lf_crossover_hz"), 40, 200, false, true, false, true, 100)),
          m_pTestChannel(std::make_unique<ControlObject>(
                  ConfigKey(kGroup, "test_channel"), true, false, false, -1.0)),
          m_pTestSignal(std::make_unique<ControlPushButton>(ConfigKey(kGroup, "test_signal"))),
          m_pTestFrequency(std::make_unique<ControlPotmeter>(
                  ConfigKey(kGroup, "test_frequency"), 20, 20000, false, true, false, false, 1000)),
          m_pTestLevelDb(std::make_unique<ControlPotmeter>(
                  ConfigKey(kGroup, "test_level_db"), -60, -6, false, true, false, true, -30)),
          m_pMainPassthrough(std::make_unique<ControlPushButton>(
                  ConfigKey(kGroup, "main_passthrough"), true, 0.0)) {
    m_pChannelCount->setReadOnly();
    m_pChannelBase->setReadOnly();
    m_pLfEnabled->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pTestSignal->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pMainPassthrough->setButtonMode(mixxx::control::ButtonMode::Toggle);
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

void EngineSpatialOutput::process(const CSAMPLE* pMain,
        std::size_t frames,
        mixxx::audio::SampleRate sampleRate) {
    const int channels = m_channels.load(std::memory_order_relaxed);
    if (channels <= 0 || frames == 0 || !sampleRate.isValid()) {
        return;
    }
    frames = std::min<std::size_t>(frames, kMaxEngineFrames);
    CSAMPLE* pOut = m_buffer.data();
    std::fill(pOut, pOut + frames * channels, CSAMPLE_ZERO);

    const int speakers = std::min(
            static_cast<int>(std::lround(m_pSpeakerCount->get())), channels);
    const int lfChannel = m_pLfEnabled->toBool() && speakers < channels ? speakers : -1;

    const double rate = sampleRate.toDouble();
    const double crossover = std::clamp(m_pLfCrossover->get(), 40.0, 200.0);
    if (rate != m_filterRate || crossover != m_filterCrossover) {
        for (Biquad* pFilter : {&m_testLowPass[0], &m_testLowPass[1],
                     &m_passLowPass[0], &m_passLowPass[1]}) {
            pFilter->setLowPass(crossover, rate);
        }
        m_filterRate = rate;
        m_filterCrossover = crossover;
    }

    // Main-mix passthrough to the front pair and LF, faded in and out over one
    // buffer so switching it never clicks.
    const CSAMPLE_GAIN passTarget = m_pMainPassthrough->toBool() ? 1.0f : 0.0f;
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
                const double lf = m_passLowPass[1].process(m_passLowPass[0].process(mono));
                pOut[f * channels + lfChannel] += static_cast<CSAMPLE>(lf) * g;
            }
        }
        m_passGain = passTarget;
    }

    // Test signal on one channel. Moving it to another channel first fades it
    // out where it is, then in on the new channel next callback.
    int target = static_cast<int>(std::lround(m_pTestChannel->get()));
    if (target < 0 || target >= channels) {
        target = -1;
    }
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
                sample = static_cast<CSAMPLE>(
                        m_testLowPass[1].process(m_testLowPass[0].process(sample)));
            }
            pOut[f * channels + m_testActiveChannel] += sample * (m_testGain + step * (f + 1));
        }
    }
    m_testGain = gainTarget;
    if (m_testGain == 0) {
        m_testActiveChannel = target;
    }
}
