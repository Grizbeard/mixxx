#include "engine/spatial/spatialdelay.h"

#include <algorithm>

namespace mixxx::spatial {

namespace {

constexpr double kEnableFadeSeconds = 0.02;
constexpr double kTimeSlewSeconds = 0.08;
constexpr float kInaudibleTap = 1e-4f; // -80 dB: later echoes are skipped

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

/// n one-pole sections at f have their -3 dB point at f * this factor
/// (low-pass), or f / this factor (high-pass). One filter at the scaled
/// corner stands in for the n passes a recirculating echo would get.
double cascadeFactor(int n) {
    return std::sqrt(std::pow(2.0, 1.0 / n) - 1.0);
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
          m_pDirection(makeToggle(kGroup, "direction", 0, true)),
          m_pSize(makePot(kGroup, "size", 0, 1, 1, true)),
          m_pPoly(makeToggle(kGroup, "poly", 0, true)),
          m_pPolyBeats(makePot(kGroup, "poly_beats", 1.0 / 16, 16, 1, true)),
          m_pPolyTriplet(makeToggle(kGroup, "poly_triplet", 0, true)),
          m_pGate(makeToggle(kGroup, "gate", 0, true)),
          m_pGateBeats(makePot(kGroup, "gate_beats", 1.0 / 32, 1, 0.25, true)),
          m_pGateDuty(makePot(kGroup, "gate_duty", 0.05, 0.95, 0.5, true)),
          m_pGateSmooth(makePot(kGroup, "gate_smooth", 0, 1, 0.25, true)),
          m_pToReverb(makePot(kGroup, "to_reverb", 0, 1, 0.3, true)) {
    for (int k = 0; k < kMaxPoints; ++k) {
        m_pPointLevel[k] = makeReadOnly(kGroup, QStringLiteral("point_level_%1").arg(k + 1));
    }
    m_cycle.allocate(static_cast<int>(kMaxCycleSeconds * kMaxSampleRate));
}

int SpatialDelay::pointOfEcho(int tap, int points, double echoTime, double polyStep) {
    if (polyStep <= 0) {
        return tap % points;
    }
    return static_cast<int>(std::floor(tap * echoTime / polyStep + 1e-9)) % points;
}

int SpatialDelay::echoesPerCycle(int points, double echoTime, double polyStep) {
    if (polyStep <= 0) {
        return points;
    }
    // The pattern repeats once m echoes span a whole number of trips round
    // the shape.
    const double ratio = echoTime / polyStep;
    for (int m = 1; m <= kMaxTaps; ++m) {
        const double trips = m * ratio / points;
        if (std::abs(trips - std::round(trips)) < 1e-6 && std::round(trips) >= 1) {
            return m;
        }
    }
    return kMaxTaps;
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
        for (int k = 0; k < kMaxPoints; ++k) {
            if (m_pointMeter[k] != 0) {
                m_pointMeter[k] = 0;
                m_pPointLevel[k]->forceSet(0);
            }
        }
        return;
    }
    const int speakerCount = layout.count();

    // Echo time, and the poly step in the same unit.
    const double tripletScale = m_pTriplet->toBool() ? 2.0 / 3.0 : 1.0;
    const double echoBeats = std::max(1.0 / 64, m_pBeats->get() * tripletScale);
    const bool sync = m_pSync->toBool();
    const double echoSeconds = sync ? echoBeats * clock.beatLength()
                                    : std::max(0.01, m_pTimeMs->get() / 1000.0);
    double polyStep = 0;
    if (m_pPoly->toBool()) {
        const double polyBeats = std::max(1.0 / 64,
                m_pPolyBeats->get() * (m_pPolyTriplet->toBool() ? 2.0 / 3.0 : 1.0));
        polyStep = sync ? polyBeats : polyBeats * clock.beatLength();
    }
    const double echoTime = sync ? echoBeats : echoSeconds;

    // The placement pattern.
    const auto shape = static_cast<Shape>(toIndex(m_pShape->get(), 0, kShapeCount - 1));
    const int points = std::min(shapePointCount(shape), kMaxPoints);
    const bool reverse = m_pDirection->toBool();
    const double size = std::clamp(m_pSize->get(), 0.0, 1.0);
    const double capacity = m_cycle.capacity() - 8;
    const double targetDelay = std::clamp(echoSeconds * sampleRate, 1.0, capacity);
    int cycle = echoesPerCycle(points, echoTime, polyStep);
    cycle = std::clamp(static_cast<int>(capacity / std::max(targetDelay, m_echoSamples)), 1, cycle);

    if (m_idle) {
        // Re-enabled: forget the old echoes in the span about to be read
        // (not the whole line, which would be too slow for the callback).
        m_cycle.clearRecent(
                static_cast<int>(cycle * std::max(targetDelay, m_echoSamples)) + 8);
        for (int p = 0; p < kMaxTaps; ++p) {
            m_tapTone[p].y = 0;
            m_tapLowCut[p].y = 0;
        }
        m_loopTone.y = 0;
        m_loopLowCut.y = 0;
        m_idle = false;
    }
    if (m_echoSamples <= 0) {
        m_echoSamples = targetDelay;
    }
    const double slew = smoothingCoefficient(kTimeSlewSeconds, 1, sampleRate);

    // Echo k + 1 (tap k): level feedback^k, its point, and tone darkening as
    // if it had passed the filters k + 1 times.
    const auto feedback = static_cast<float>(std::clamp(m_pFeedback->get(), 0.0, 0.97));
    const auto cycleFeedback = static_cast<float>(std::pow(feedback, cycle));
    const double toneHz = 1000.0 * std::pow(20.0, std::clamp(m_pTone->get(), 0.0, 1.0));
    const bool toneOpen = toneHz >= 19500;
    const double lowCutHz = std::clamp(m_pLowCut->get(), 20.0, 1000.0);
    const double nyquistGuard = 0.45 * sampleRate;
    std::array<float, kMaxTaps> tapGain{};
    std::array<int, kMaxTaps> tapPoint{};
    int taps = 0;
    float gain = 1;
    for (int p = 0; p < cycle; ++p) {
        if (gain < kInaudibleTap) {
            break;
        }
        tapGain[p] = gain;
        int point = pointOfEcho(p, points, echoTime, polyStep);
        if (reverse) {
            point = (points - point) % points;
        }
        tapPoint[p] = point;
        const double factor = cascadeFactor(p + 1);
        m_tapTone[p].setCutoff(std::max(20.0, toneHz * factor), sampleRate);
        m_tapLowCut[p].setCutoff(std::min(nyquistGuard, lowCutHz / factor), sampleRate);
        taps = p + 1;
        gain *= feedback;
    }
    const double loopFactor = cascadeFactor(cycle);
    m_loopTone.setCutoff(std::max(20.0, toneHz * loopFactor), sampleRate);
    m_loopLowCut.setCutoff(std::min(nyquistGuard, lowCutHz / loopFactor), sampleRate);

    // Each point's speaker gains, ramped over the callback when they change.
    std::array<std::array<float, kMaxSpeakers>, kMaxPoints> gainStep{};
    for (int k = 0; k < points; ++k) {
        const Point at = shapeAt(shape, k);
        const Point scaled{at.x * size, at.y * size};
        const double radius = std::min(1.0, radiusOf(scaled));
        std::array<float, kMaxSpeakers> target{};
        layout.gains(radius > 1e-6 ? azimuthOf(scaled) : 0.0, 1 - radius, target.data());
        if (!m_gainsValid) {
            m_pointGains[k] = target;
        }
        for (int i = 0; i < speakerCount; ++i) {
            gainStep[k][i] = (target[i] - m_pointGains[k][i]) / frames;
        }
    }
    m_gainsValid = true;

    // Gate.
    const bool gate = m_pGate->toBool();
    const double gateBeats = std::max(1.0 / 64, m_pGateBeats->get());
    const double gateDuty = std::clamp(m_pGateDuty->get(), 0.05, 0.95);
    const auto gateCoefficient = static_cast<float>(smoothingCoefficient(
            0.0005 + std::clamp(m_pGateSmooth->get(), 0.0, 1.0) * 0.0195, 1, sampleRate));

    const auto level = static_cast<float>(std::clamp(m_pLevel->get(), 0.0, 1.0));
    const auto toReverb = static_cast<float>(std::clamp(m_pToReverb->get(), 0.0, 1.0));
    const auto enableStep = static_cast<float>(1.0 / (kEnableFadeSeconds * sampleRate));

    std::array<float, kMaxPoints> bucket{};
    for (int f = 0; f < frames; ++f) {
        m_echoSamples += (targetDelay - m_echoSamples) * slew;

        // The cycle line: input plus the whole pattern coming round again.
        auto loop = static_cast<float>(m_cycle.read(cycle * m_echoSamples));
        if (!toneOpen) {
            loop = static_cast<float>(m_loopTone.lowPass(loop));
        }
        loop = static_cast<float>(m_loopLowCut.highPass(loop));

        // Every echo of the cycle, each onto its own point.
        std::fill(bucket.begin(), bucket.begin() + points, 0.0f);
        for (int p = 0; p < taps; ++p) {
            double echo = m_cycle.read((p + 1) * m_echoSamples);
            if (!toneOpen) {
                echo = m_tapTone[p].lowPass(echo);
            }
            echo = m_tapLowCut[p].highPass(echo);
            bucket[tapPoint[p]] += static_cast<float>(echo) * tapGain[p];
        }
        const float in = 0.5f * (pSendLeft[f] + pSendRight[f]);
        m_cycle.write(softClip(in + cycleFeedback * loop));

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
        const float wetGain = m_enabledGain * m_gateGain;

        float wetSum = 0;
        for (int k = 0; k < points; ++k) {
            const float wet = bucket[k] * wetGain;
            wetSum += wet;
            m_pointPeak[k] = std::max(m_pointPeak[k], std::abs(wet));
            const float out = wet * level;
            auto& gains = m_pointGains[k];
            for (int i = 0; i < speakerCount; ++i) {
                gains[i] += gainStep[k][i];
                speakers[i][f] += out * gains[i];
            }
        }
        pLf[f] += wetSum * level;
        pToReverb[f] += wetSum * toReverb;
    }

    // Echo meters per point, for the window.
    for (int k = 0; k < kMaxPoints; ++k) {
        const float meter = std::max(k < points ? m_pointPeak[k] : 0.0f, m_pointMeter[k] * 0.9f);
        const float shown = meter < 1e-4f ? 0.0f : meter;
        if (shown != m_pointMeter[k]) {
            m_pPointLevel[k]->forceSet(shown);
        }
        m_pointMeter[k] = shown;
        m_pointPeak[k] = 0;
    }
}

} // namespace mixxx::spatial
