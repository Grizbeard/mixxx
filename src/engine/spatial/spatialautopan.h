#pragma once

#include <memory>

#include "engine/spatial/spatialcontrols.h"
#include "engine/spatial/spatialdsp.h"

namespace mixxx::spatial {

/// One auto-pan unit: a beat-locked motion that stems can follow.
///
/// Controls, group [SpatialAutopanN]:
///   shape      0 orbit, 1 left-right, 2 front-back, 3 corners,
///              4 5-point star, 5 7-point star (persisted)
///   wave       0 square, 1 sine, 2 triangle, 3 saw, 4 reverse saw (persisted)
///   sync       1 = step length in beats, 0 = in seconds (persisted)
///   beats      beats per step (point), 1/16..16 (persisted)
///   triplet    step length x 2/3 (persisted)
///   seconds    seconds per step when not synced (persisted)
///   direction  0 forward through the points, 1 reverse (persisted)
///   size       radius scale 0..1 (1 = on the ring) (persisted)
///   retrigger  restart at point 1 on the nearest beat
///   position   read-only: where the motion is, in points (0..count)
///   step_length read-only: the step length in seconds
class SpatialAutopan {
  public:
    explicit SpatialAutopan(int unitNumber);

    static QString groupForUnit(int unitNumber) {
        return QStringLiteral("[SpatialAutopan%1]").arg(unitNumber);
    }

    /// Once per callback, before any source uses the motion.
    void prepare(const SpatialClock& clock);

    const MotionState& motion() const {
        return m_motion;
    }

  private:
    std::unique_ptr<ControlPushButton> m_pShape;
    std::unique_ptr<ControlPushButton> m_pWave;
    std::unique_ptr<ControlPushButton> m_pSync;
    std::unique_ptr<ControlPotmeter> m_pBeats;
    std::unique_ptr<ControlPushButton> m_pTriplet;
    std::unique_ptr<ControlPotmeter> m_pSeconds;
    std::unique_ptr<ControlPushButton> m_pDirection;
    std::unique_ptr<ControlPotmeter> m_pSize;
    TriggerButton m_retrigger;
    std::unique_ptr<ControlObject> m_pPosition;
    std::unique_ptr<ControlObject> m_pStepLength;

    MotionState m_motion;
    double m_originBeats = 0;
    double m_originSeconds = 0;
};

} // namespace mixxx::spatial
