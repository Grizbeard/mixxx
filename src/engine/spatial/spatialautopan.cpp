#include "engine/spatial/spatialautopan.h"

#include <algorithm>

namespace mixxx::spatial {

SpatialAutopan::SpatialAutopan(int unitNumber)
        : m_pShape(makeToggle(groupForUnit(unitNumber), "shape", 0, true, kShapeCount)),
          m_pWave(makeToggle(groupForUnit(unitNumber),
                  "wave",
                  static_cast<double>(Wave::Triangle),
                  true,
                  kWaveCount)),
          m_pSync(makeToggle(groupForUnit(unitNumber), "sync", 1, true)),
          m_pBeats(makePot(groupForUnit(unitNumber), "beats", 1.0 / 16, 16, 1, true)),
          m_pTriplet(makeToggle(groupForUnit(unitNumber), "triplet", 0, true)),
          m_pSeconds(makePot(groupForUnit(unitNumber), "seconds", 0.05, 60, 1, true)),
          m_pDirection(makeToggle(groupForUnit(unitNumber), "direction", 0, true)),
          m_pSize(makePot(groupForUnit(unitNumber), "size", 0, 1, 1, true)),
          m_retrigger(groupForUnit(unitNumber), "retrigger"),
          m_pPosition(makeReadOnly(groupForUnit(unitNumber), "position")),
          m_pStepLength(makeReadOnly(groupForUnit(unitNumber), "step_length")) {
}

void SpatialAutopan::prepare(const SpatialClock& clock) {
    m_motion.shape = static_cast<Shape>(toIndex(m_pShape->get(), 0, kShapeCount - 1));
    m_motion.wave = static_cast<Wave>(toIndex(m_pWave->get(), 0, kWaveCount - 1));
    m_motion.reverse = m_pDirection->toBool();
    m_motion.size = std::clamp(m_pSize->get(), 0.0, 1.0);

    if (m_retrigger.take()) {
        m_originBeats = std::round(clock.beatsEnd());
        m_originSeconds = clock.secondsEnd();
    }
    double stepSeconds;
    if (m_pSync->toBool()) {
        const double stepBeats = std::max(1.0 / 64,
                m_pBeats->get() * (m_pTriplet->toBool() ? 2.0 / 3.0 : 1.0));
        m_motion.stepsStart = (clock.beatsStart() - m_originBeats) / stepBeats;
        m_motion.stepsEnd = (clock.beatsEnd() - m_originBeats) / stepBeats;
        stepSeconds = stepBeats * clock.beatLength();
    } else {
        stepSeconds = std::max(0.01, m_pSeconds->get());
        m_motion.stepsStart = (clock.secondsStart() - m_originSeconds) / stepSeconds;
        m_motion.stepsEnd = (clock.secondsEnd() - m_originSeconds) / stepSeconds;
    }
    m_pPosition->forceSet(wavePosition(
            m_motion.wave, m_motion.stepsEnd, m_motion.pointCount(), m_motion.reverse));
    m_pStepLength->forceSet(stepSeconds);
}

} // namespace mixxx::spatial
