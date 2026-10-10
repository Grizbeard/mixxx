#include "engine/spatial/spatialdelay.h"

#include <algorithm>

namespace mixxx::spatial {

namespace {

constexpr int kSubBlock = 32;
constexpr double kEnableFadeSeconds = 0.02;
constexpr double kTimeSlewSeconds = 0.08;
constexpr double kPlacementSmoothingSeconds = 0.003;

/// Soft limit for the feedback path: linear up to full scale, then a smooth
/// knee towards 1.5, so runaway feedback saturates instead of exploding.
inline float softClip(float x) {
    const float magnitude = std::abs(x);
    if (magnitude <= 1.0f) {
        return x;
    }
    const float over = magnitude - 1.0f;
    return std::copysign(1.0f + over / (1.0f + 2.0f * over), x);
}

} // namespace

SpatialDelay::SpatialDelay()
        : m_pEnabled(makeToggle(kGroup, "enabled", 1, true)),
          m_pLevel(makePot(kGroup, "level", 0, 1, 0.8, true)),
          m_pSync(makeToggle(kGroup, "sync", 1, true)),
          m_pBeats(makePot(kGroup, "beats", 1.0 / 16, 4, 0.75, true)),
          m_pTriplet(makeToggle(kGroup, "triplet", 0, true)),
          m_pTimeMs(makePot(kGroup, "time_ms", 10, 4000, 375, true)),
          m_pFeedback(makePot(kGroup, "feedback", 0, 0.97, 0.55, true)),
          m_pTone(makePot(kGroup, "tone", 0, 1, 0.6, true)),
          m_pLowCut(makePot(kGroup, "lowcut", 20, 1000, 150, true)),
          m_pShape(makeToggle(kGroup, "shape", 0, true, kShapeCount)),
          m_pWave(makeToggle(kGroup, "wave", 0, true, kWaveCount)),
          m_pDirection(makeToggle(kGroup, "direction", 0, true)),
          m_pSize(makePot(kGroup, "size", 0, 1, 1, true)),
          m_pPoly(makeToggle(kGroup, "poly", 0, true)),
          m_pPolyBeats(makePot(kGroup, "poly_beats", 1.0 / 16, 16, 1, true)),
          m_pPolyTriplet(makeToggle(kGroup, "poly_triplet", 0, true)),
          m_pGate(makeToggle(kGroup, "gate", 0, true)),
          m_pGateBeats(makePot(kGroup, "gate_beats", 1.0 / 32, 1, 0.25, true)),
          m_pGateDuty(makePot(kGroup, "gate_duty", 0.05, 0.95, 0.5, true)),
          m_pGateSmooth(makePot(kGroup, "gate_smooth", 0, 1, 0.25, true)),
          m_pToReverb(makePot(kGroup, "to_reverb", 0, 1, 0.3, true)),
          m_pPosition(makeReadOnly(kGroup, "position")) {
    m_line.allocate(static_cast<int>(kMaxSeconds * kMaxSampleRate));
}

void SpatialDelay::process(const float* pSendLeft,
        const float* pSendRight,
        int frames,
        double sampleRate,
        const SpatialClock& clock,
        const SpeakerLayout& layout,
        const std::array<float*, kMaxSpeakers>& speakers,
        float* pLf,
        float* pToReverb) {
    const float enabledTarget = m_pEnabled->toBool() ? 1.0f : 0.0f;
    if (enabledTarget == 0 && m_enabledGain == 0) {
        m_idle = true;
        return;
    }
    const int speakerCount = layout.count();

    // Echo time.
    const double tripletScale = m_pTriplet->toBool() ? 2.0 / 3.0 : 1.0;
    const double echoBeats = std::max(1.0 / 64, m_pBeats->get() * tripletScale);
    const bool sync = m_pSync->toBool();
    const double echoSeconds = sync ? echoBeats * clock.beatLength()
                                    : std::max(0.01, m_pTimeMs->get() / 1000.0);
    const double targetDelay = std::clamp(echoSeconds * sampleRate,
            1.0,
            static_cast<double>(m_line.capacity() - 4));
    if (m_idle) {
        // Re-enabled: forget the old echoes in the span about to be read
        // (not the whole 8 s line, which would be too slow for the callback).
        m_line.clearRecent(static_cast<int>(std::max(targetDelay, m_delaySamples)) + 8);
        m_toneFilter.y = 0;
        m_lowCutFilter.y = 0;
        m_idle = false;
    }
    if (m_delaySamples <= 0) {
        m_delaySamples = targetDelay;
    }
    const double slew = smoothingCoefficient(kTimeSlewSeconds, 1, sampleRate);

    // Placement: once per echo, or at the polyrhythm rate.
    MotionState motion;
    motion.shape = static_cast<Shape>(toIndex(m_pShape->get(), 0, kShapeCount - 1));
    motion.wave = static_cast<Wave>(toIndex(m_pWave->get(), 0, kWaveCount - 1));
    motion.reverse = m_pDirection->toBool();
    motion.size = std::clamp(m_pSize->get(), 0.0, 1.0);
    if (m_pPoly->toBool()) {
        const double stepBeats = std::max(1.0 / 64,
                m_pPolyBeats->get() * (m_pPolyTriplet->toBool() ? 2.0 / 3.0 : 1.0));
        motion.stepsStart = clock.beatsStart() / stepBeats;
        motion.stepsEnd = clock.beatsEnd() / stepBeats;
    } else if (sync) {
        motion.stepsStart = clock.beatsStart() / echoBeats;
        motion.stepsEnd = clock.beatsEnd() / echoBeats;
    } else {
        motion.stepsStart = clock.secondsStart() / echoSeconds;
        motion.stepsEnd = clock.secondsEnd() / echoSeconds;
    }
    m_pPosition->forceSet(wavePosition(
            motion.wave, motion.stepsEnd, motion.pointCount(), motion.reverse));

    // Gate.
    const bool gate = m_pGate->toBool();
    const double gateBeats = std::max(1.0 / 64, m_pGateBeats->get());
    const double gateDuty = std::clamp(m_pGateDuty->get(), 0.05, 0.95);
    const auto gateCoefficient = static_cast<float>(smoothingCoefficient(
            0.0005 + std::clamp(m_pGateSmooth->get(), 0.0, 1.0) * 0.0195, 1, sampleRate));

    // Filters and gains.
    m_toneFilter.setCutoff(1000.0 * std::pow(20.0, std::clamp(m_pTone->get(), 0.0, 1.0)),
            sampleRate);
    m_lowCutFilter.setCutoff(std::clamp(m_pLowCut->get(), 20.0, 1000.0), sampleRate);
    const auto feedback = static_cast<float>(std::clamp(m_pFeedback->get(), 0.0, 0.97));
    const auto level = static_cast<float>(std::clamp(m_pLevel->get(), 0.0, 1.0));
    const auto toReverb = static_cast<float>(std::clamp(m_pToReverb->get(), 0.0, 1.0));
    const auto enableStep = static_cast<float>(1.0 / (kEnableFadeSeconds * sampleRate));
    const auto placementAlpha = static_cast<float>(
            smoothingCoefficient(kPlacementSmoothingSeconds, kSubBlock, sampleRate));

    std::array<float, kMaxSpeakers> target{};
    std::array<float, kMaxSpeakers> step{};
    for (int start = 0; start < frames; start += kSubBlock) {
        const int n = std::min(kSubBlock, frames - start);
        const Point p = motion.positionAt((start + 0.5 * n) / frames);
        const double radius = std::min(1.0, radiusOf(p));
        layout.gains(radius > 1e-6 ? azimuthOf(p) : 0.0, 1 - radius, target.data());
        if (!m_gainsValid) {
            m_gains = target;
            m_gainsValid = true;
        }
        for (int i = 0; i < speakerCount; ++i) {
            step[i] = (target[i] - m_gains[i]) * placementAlpha / n;
        }
        for (int j = 0; j < n; ++j) {
            const int f = start + j;
            m_delaySamples += (targetDelay - m_delaySamples) * slew;
            const float echo = m_line.read(m_delaySamples);
            const float in = 0.5f * (pSendLeft[f] + pSendRight[f]);
            const auto filtered = static_cast<float>(
                    m_lowCutFilter.highPass(m_toneFilter.lowPass(in + feedback * echo)));
            m_line.write(softClip(filtered));

            float gateTarget = 1.0f;
            if (gate) {
                const double gatePhase = clock.beatsAt((f + 0.5) / frames) / gateBeats;
                gateTarget = gatePhase - std::floor(gatePhase) < gateDuty ? 1.0f : 0.0f;
            }
            m_gateGain += (gateTarget - m_gateGain) * gateCoefficient;
            if (m_enabledGain < enabledTarget) {
                m_enabledGain = std::min(enabledTarget, m_enabledGain + enableStep);
            } else if (m_enabledGain > enabledTarget) {
                m_enabledGain = std::max(enabledTarget, m_enabledGain - enableStep);
            }

            const float wet = echo * m_enabledGain * m_gateGain;
            const float out = wet * level;
            for (int i = 0; i < speakerCount; ++i) {
                m_gains[i] += step[i];
                speakers[i][f] += out * m_gains[i];
            }
            pLf[f] += out;
            pToReverb[f] += wet * toReverb;
        }
    }
}

} // namespace mixxx::spatial
