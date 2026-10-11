#include "engine/spatial/spatialreverb.h"

#include <algorithm>

namespace mixxx::spatial {

namespace {

// Line lengths at size 1 (ms): mutually prime-ish, 32-83 ms, so the echo
// density builds quickly without metallic ringing.
constexpr std::array<double, SpatialReverb::kLines> kLineMs = {
        31.7, 37.3, 43.1, 49.7, 57.1, 64.3, 73.9, 83.3};
// LFO rates (Hz) for each line's modulation.
constexpr std::array<double, SpatialReverb::kLines> kLfoHz = {
        0.13, 0.19, 0.23, 0.31, 0.37, 0.43, 0.53, 0.71};
// Input diffusers (ms).
constexpr std::array<double, 4> kDiffuserMs = {4.77, 3.59, 12.73, 9.31};
constexpr double kMaxSizeScale = 1.5;
constexpr double kMaxModulationSeconds = 0.0008;
constexpr double kMaxPredelaySeconds = 0.5;
constexpr double kPitchWindowSeconds = 0.05;
constexpr double kFadeSeconds = 0.05;

/// The 8x8 Hadamard matrix: orthogonal rows of +-1.
struct Hadamard {
    float sign[8][8];
    Hadamard() {
        for (int row = 0; row < 8; ++row) {
            for (int column = 0; column < 8; ++column) {
                int bits = row & column;
                int parity = 0;
                while (bits) {
                    parity ^= bits & 1;
                    bits >>= 1;
                }
                sign[row][column] = parity ? -1.0f : 1.0f;
            }
        }
    }
};
const Hadamard kHadamard;

} // namespace

SpatialReverb::SpatialReverb()
        : m_pEnabled(makeToggle(kGroup, "enabled", 1, true)),
          m_pLevel(makePot(kGroup, "level", 0, 1, 0.6, true)),
          m_pDecay(makePot(kGroup, "decay", 0.5, 30, 9, true)),
          m_pSize(makePot(kGroup, "size", 0, 1, 0.8, true)),
          m_pPredelayMs(makePot(kGroup, "predelay_ms", 0, 500, 60, true)),
          m_pDamping(makePot(kGroup, "damping", 0, 1, 0.45, true)),
          m_pLowCut(makePot(kGroup, "lowcut", 20, 1000, 250, true)),
          m_pModulation(makePot(kGroup, "modulation", 0, 1, 0.6, true)),
          m_pDiffusion(makePot(kGroup, "diffusion", 0, 1, 0.75, true)),
          m_pShimmer(makePot(kGroup, "shimmer", 0, 1, 0, true)),
          m_pDuck(makePot(kGroup, "duck", 0, 1, 0, true)),
          m_pFreeze(makeToggle(kGroup, "freeze")),
          m_pDirect(makePot(kGroup, "direct", 0, 1, 1, true)) {
    const double maxLine = (kLineMs.back() / 1000.0 * kMaxSizeScale + kMaxModulationSeconds * 2) *
            kMaxSampleRate;
    for (auto& line : m_lines) {
        line.allocate(static_cast<int>(maxLine) + 4);
    }
    for (std::size_t i = 0; i < m_diffusers.size(); ++i) {
        m_diffusers[i].allocate(static_cast<int>(kDiffuserMs[i] / 1000.0 * kMaxSampleRate) + 4);
    }
    m_predelay.allocate(static_cast<int>(kMaxPredelaySeconds * kMaxSampleRate) + 4);
    m_pitchLine.allocate(static_cast<int>(kPitchWindowSeconds * kMaxSampleRate) + 8);
    for (int k = 0; k < kLines; ++k) {
        m_lfoPhase[k] = k * 0.7;
    }
}

float SpatialReverb::directLevel() const {
    return static_cast<float>(std::clamp(m_pDirect->get(), 0.0, 1.0));
}

void SpatialReverb::clear() {
    for (auto& line : m_lines) {
        line.clear();
    }
    for (auto& line : m_diffusers) {
        line.clear();
    }
    m_predelay.clear();
    m_pitchLine.clear();
    for (auto& filter : m_damping) {
        filter.y = 0;
    }
    m_lowCut[0].y = 0;
    m_lowCut[1].y = 0;
    m_shimmerSample = 0;
    m_envelope = 0;
}

void SpatialReverb::process(const float* pIn,
        int frames,
        double sampleRate,
        const SpeakerLayout& layout,
        const std::array<float*, kMaxSpeakers>& speakers,
        float* pLf,
        const FxMeta& meta) {
    const bool freeze = m_pFreeze->toBool();
    const float enabledTarget = m_pEnabled->toBool() ? 1.0f : 0.0f;
    if (enabledTarget == 0 && m_enabledGain == 0) {
        if (!m_sleeping) {
            clear();
            m_sleeping = true;
        }
        return;
    }
    float inputPeak = 0;
    for (int f = 0; f < frames; ++f) {
        inputPeak = std::max(inputPeak, std::abs(pIn[f]));
    }
    if (m_sleeping && inputPeak == 0 && !freeze) {
        return;
    }
    m_sleeping = false;

    const int speakerCount = layout.count();
    const double sizeScale = 0.4 +
            (kMaxSizeScale - 0.4) * std::clamp(meta.reverbSize(m_pSize->get()), 0.0, 1.0);
    const double modulation = std::clamp(m_pModulation->get(), 0.0, 1.0);
    const double modDepth = modulation * kMaxModulationSeconds * sampleRate;
    const double decay = std::clamp(meta.reverbDecay(m_pDecay->get()), 0.5, 30.0);
    const double damping = std::clamp(meta.reverbDamping(m_pDamping->get()), 0.0, 1.0);

    std::array<double, kLines> length{};
    std::array<float, kLines> lineGain{};
    std::array<double, kLines> lfoCos{};
    std::array<double, kLines> lfoSin{};
    std::array<double, kLines> rotCos{};
    std::array<double, kLines> rotSin{};
    double gainSquares = 0;
    for (int k = 0; k < kLines; ++k) {
        length[k] = kLineMs[k] / 1000.0 * sizeScale * sampleRate;
        const double g = freeze ? 1.0 : std::pow(10.0, -3.0 * length[k] / (decay * sampleRate));
        lineGain[k] = static_cast<float>(g);
        gainSquares += g * g;
        m_damping[k].setCutoff(freeze ? 20000.0 : 20000.0 * std::pow(0.06, damping), sampleRate);
        // Each LFO as a rotating phasor: cheap per sample, renormalised here.
        lfoCos[k] = std::cos(m_lfoPhase[k]);
        lfoSin[k] = std::sin(m_lfoPhase[k]);
        const double increment = 2 * kPi * kLfoHz[k] / sampleRate;
        rotCos[k] = std::cos(increment);
        rotSin[k] = std::sin(increment);
        m_lfoPhase[k] = std::fmod(m_lfoPhase[k] + increment * frames, 2 * kPi);
    }
    // Hold the steady-state level roughly independent of the decay time.
    const auto inputNorm = static_cast<float>(
            std::sqrt(std::max(0.02, 1.0 - gainSquares / kLines)) / std::sqrt(kLines));
    const float outputNorm = 1.0f / std::sqrt(static_cast<float>(kLines));

    const double lowCut = std::clamp(m_pLowCut->get(), 20.0, 1000.0);
    m_lowCut[0].setCutoff(lowCut, sampleRate);
    m_lowCut[1].setCutoff(lowCut, sampleRate);
    const auto diffusion = static_cast<float>(0.7 * std::clamp(m_pDiffusion->get(), 0.0, 1.0));
    std::array<int, 4> diffuserLength{};
    for (std::size_t i = 0; i < diffuserLength.size(); ++i) {
        diffuserLength[i] = std::max(1, static_cast<int>(kDiffuserMs[i] / 1000.0 * sampleRate));
    }
    const int predelay = std::clamp(
            static_cast<int>(meta.reverbPredelayMs(m_pPredelayMs->get()) / 1000.0 * sampleRate),
            1,
            m_predelay.capacity() - 2);
    const auto shimmer = static_cast<float>(0.55 * std::clamp(m_pShimmer->get(), 0.0, 1.0));
    const double pitchWindow = kPitchWindowSeconds * sampleRate;
    const auto duck = static_cast<float>(std::clamp(m_pDuck->get(), 0.0, 1.0));
    const double attack = smoothingCoefficient(0.005, 1, sampleRate);
    const double release = smoothingCoefficient(0.25, 1, sampleRate);
    const auto fadeStep = static_cast<float>(1.0 / (kFadeSeconds * sampleRate));
    const float inputTarget = freeze ? 0.0f : 1.0f;
    const auto level = static_cast<float>(std::clamp(m_pLevel->get(), 0.0, 1.0) * meta.level);

    float outputPeak = 0;
    std::array<float, kLines> s{};
    for (int f = 0; f < frames; ++f) {
        if (m_inputGain < inputTarget) {
            m_inputGain = std::min(inputTarget, m_inputGain + fadeStep);
        } else if (m_inputGain > inputTarget) {
            m_inputGain = std::max(inputTarget, m_inputGain - fadeStep);
        }
        if (m_enabledGain < enabledTarget) {
            m_enabledGain = std::min(enabledTarget, m_enabledGain + fadeStep);
        } else if (m_enabledGain > enabledTarget) {
            m_enabledGain = std::max(enabledTarget, m_enabledGain - fadeStep);
        }

        const float raw = pIn[f];
        const double magnitude = std::abs(raw);
        m_envelope += (magnitude > m_envelope ? attack : release) * (magnitude - m_envelope);

        auto x = static_cast<float>(m_lowCut[1].highPass(m_lowCut[0].highPass(raw)));
        m_predelay.write(x * m_inputGain);
        // The shimmer feed stops with the input, so a frozen tail holds.
        x = m_predelay.readInteger(predelay) + m_shimmerSample * shimmer * m_inputGain;
        for (std::size_t i = 0; i < m_diffusers.size(); ++i) {
            const float delayed = m_diffusers[i].readInteger(diffuserLength[i]);
            const float w = x + diffusion * delayed;
            x = delayed - diffusion * w;
            m_diffusers[i].write(w);
        }

        float sum = 0;
        for (int k = 0; k < kLines; ++k) {
            const double c = lfoCos[k];
            lfoCos[k] = c * rotCos[k] - lfoSin[k] * rotSin[k];
            lfoSin[k] = lfoSin[k] * rotCos[k] + c * rotSin[k];
            const double delay = length[k] + modDepth * (1 + lfoSin[k]) + 1;
            s[k] = static_cast<float>(m_damping[k].lowPass(m_lines[k].read(delay))) * lineGain[k];
            sum += s[k];
        }
        // Householder feedback: lossless, every line feeds every other.
        const float mix = 2.0f / kLines * sum;
        const float injection = x * inputNorm;
        for (int k = 0; k < kLines; ++k) {
            m_lines[k].write(s[k] - mix + (k % 2 ? -injection : injection));
        }

        const float mono = sum * outputNorm;
        if (shimmer > 0) {
            // Octave up: two read heads sweeping a window at double speed,
            // crossfaded so each wraps while silent.
            m_pitchLine.write(mono);
            m_pitchPhase += 1.0 / pitchWindow;
            if (m_pitchPhase >= 1) {
                m_pitchPhase -= 1;
            }
            const double phase2 = m_pitchPhase >= 0.5 ? m_pitchPhase - 0.5 : m_pitchPhase + 0.5;
            const double w1 = 0.5 - 0.5 * std::cos(2 * kPi * m_pitchPhase);
            const double w2 = 0.5 - 0.5 * std::cos(2 * kPi * phase2);
            m_shimmerSample = static_cast<float>(
                    w1 * m_pitchLine.read(1 + (1 - m_pitchPhase) * pitchWindow) +
                    w2 * m_pitchLine.read(1 + (1 - phase2) * pitchWindow));
        } else {
            m_shimmerSample = 0;
        }

        const float duckTarget = 1.0f / (1.0f + duck * 8.0f * static_cast<float>(m_envelope));
        m_duckGain += (duckTarget - m_duckGain) * 0.002f;
        const float wet = level * m_enabledGain * m_duckGain;
        for (int i = 0; i < speakerCount; ++i) {
            // Row 0 is the mono sum; rows 1-6 are mutually decorrelated.
            const float* pRow = kHadamard.sign[i + 1];
            float out = 0;
            for (int k = 0; k < kLines; ++k) {
                out += pRow[k] * s[k];
            }
            out *= outputNorm * wet;
            speakers[i][f] += out;
            outputPeak = std::max(outputPeak, std::abs(out));
        }
        pLf[f] += 0.5f * mono * wet;
    }

    if (inputPeak == 0 && !freeze && outputPeak < 1e-6f) {
        clear();
        m_sleeping = true;
    }
}

} // namespace mixxx::spatial
