#pragma once

#include <array>
#include <memory>

#include "engine/spatial/spatialcontrols.h"
#include "engine/spatial/spatialdsp.h"

namespace mixxx::spatial {

/// The Spatial FX unit's delay: a feedback echo whose repeats are placed
/// around the room by a motion shape, with an optional trance gate on the
/// echoes.
///
/// Each echo emerges at the shape's current point. Without polyrhythm the
/// placement steps once per echo, so echo k comes from point k. With
/// polyrhythm the placement steps at its own rate (poly_beats), so echoes
/// land on a moving pattern against the bar.
///
/// Controls, group [SpatialDelay] (all persisted unless noted):
///   enabled       on/off (faded)
///   level         return level 0..1
///   sync          1 = echo time in beats, 0 = time_ms
///   beats         echo time in beats (1/16..4); triplet x 2/3
///   triplet
///   time_ms       echo time when not synced (10..4000)
///   feedback      0..0.97
///   tone          0..1: feedback low-pass, dark (1 kHz) .. open (20 kHz)
///   lowcut        feedback high-pass, Hz (20..1000)
///   shape, wave, direction, size   echo placement, as in auto-pan
///   poly          placement steps at poly_beats instead of once per echo
///   poly_beats, poly_triplet
///   gate          trance gate on the echo return
///   gate_beats    gate period in beats (1/32..1)
///   gate_duty     open fraction of each period (0.05..0.95)
///   gate_smooth   gate edge time 0..1 (0.5..20 ms)
///   to_reverb     echoes sent on to the reverb 0..1
///   read-only: position (placement, in points)
class SpatialDelay {
  public:
    static constexpr double kMaxSeconds = 8.0;
    static constexpr double kMaxSampleRate = 192000;
    static constexpr const char* kGroup = "[SpatialDelay]";

    SpatialDelay();

    /// Adds the echo return to the speakers and LF, and the echoes destined
    /// for the reverb to `pToReverb`.
    void process(const float* pSendLeft,
            const float* pSendRight,
            int frames,
            double sampleRate,
            const SpatialClock& clock,
            const SpeakerLayout& layout,
            const std::array<float*, kMaxSpeakers>& speakers,
            float* pLf,
            float* pToReverb);

  private:
    std::unique_ptr<ControlPushButton> m_pEnabled;
    std::unique_ptr<ControlPotmeter> m_pLevel;
    std::unique_ptr<ControlPushButton> m_pSync;
    std::unique_ptr<ControlPotmeter> m_pBeats;
    std::unique_ptr<ControlPushButton> m_pTriplet;
    std::unique_ptr<ControlPotmeter> m_pTimeMs;
    std::unique_ptr<ControlPotmeter> m_pFeedback;
    std::unique_ptr<ControlPotmeter> m_pTone;
    std::unique_ptr<ControlPotmeter> m_pLowCut;
    std::unique_ptr<ControlPushButton> m_pShape;
    std::unique_ptr<ControlPushButton> m_pWave;
    std::unique_ptr<ControlPushButton> m_pDirection;
    std::unique_ptr<ControlPotmeter> m_pSize;
    std::unique_ptr<ControlPushButton> m_pPoly;
    std::unique_ptr<ControlPotmeter> m_pPolyBeats;
    std::unique_ptr<ControlPushButton> m_pPolyTriplet;
    std::unique_ptr<ControlPushButton> m_pGate;
    std::unique_ptr<ControlPotmeter> m_pGateBeats;
    std::unique_ptr<ControlPotmeter> m_pGateDuty;
    std::unique_ptr<ControlPotmeter> m_pGateSmooth;
    std::unique_ptr<ControlPotmeter> m_pToReverb;
    std::unique_ptr<ControlObject> m_pPosition;

    DelayLine m_line;
    double m_delaySamples = 0;
    OnePole m_toneFilter;
    OnePole m_lowCutFilter;
    float m_enabledGain = 0;
    float m_gateGain = 1;
    std::array<float, kMaxSpeakers> m_gains{};
    bool m_gainsValid = false;
    bool m_idle = true;
};

} // namespace mixxx::spatial
