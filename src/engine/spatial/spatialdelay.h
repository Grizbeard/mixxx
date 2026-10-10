#pragma once

#include <array>
#include <memory>

#include "engine/spatial/spatialcontrols.h"
#include "engine/spatial/spatialdsp.h"

namespace mixxx::spatial {

/// The Spatial FX unit's delay: echoes placed around the room on the points
/// of a shape, each echo fixed on its point for its whole length, with an
/// optional trance gate on the echoes.
///
/// Echo 1 plays from the shape's first point, echo 2 from the second, and so
/// on around the shape. With polyrhythm the placement moves one point per
/// `poly_beats` of echo time instead of one per echo, so echo k sits on
/// point floor((k - 1) x echo time / poly_beats): the pattern runs against
/// the bar, but each echo still stays put.
///
/// How: one feedback line holds a whole cycle of the pattern (M echoes, M x
/// the echo time), and a tap per echo of the cycle reads it at k x the echo
/// time, so every echo has its own point. Tone and low cut darken each
/// repeat as a recirculating delay would.
///
/// Controls, group [SpatialDelay] (all persisted unless noted):
///   enabled       on/off (faded)
///   level         return level 0..1
///   sync          1 = echo time in beats, 0 = time_ms
///   beats         echo time in beats (1/16..4); triplet x 2/3
///   triplet
///   time_ms       echo time when not synced (10..4000)
///   feedback      0..0.97
///   tone          0..1: low-pass per repeat, dark (1 kHz) .. open (20 kHz)
///   lowcut        high-pass per repeat, Hz (20..1000)
///   shape, direction, size   the points the echoes land on
///   poly          placement moves per poly_beats instead of per echo
///   poly_beats, poly_triplet
///   gate          trance gate on the echo return
///   gate_beats    gate period in beats (1/32..1)
///   gate_duty     open fraction of each period (0.05..0.95)
///   gate_smooth   gate edge time 0..1 (0.5..20 ms)
///   to_reverb     echoes sent on to the reverb 0..1
///   read-only: point_level_1..8, the echo level on each point
class SpatialDelay {
  public:
    static constexpr int kMaxTaps = 64;
    static constexpr int kMaxPoints = 8;
    static constexpr double kMaxCycleSeconds = 32.0;
    static constexpr double kMaxSampleRate = 96000;
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

    /// The point (0-based) echo `tap` + 1 lands on, for `points` points; echo
    /// time and poly step in the same unit (beats or seconds), 0 = no poly.
    static int pointOfEcho(int tap, int points, double echoTime, double polyStep);
    /// Echoes in one full cycle of the placement pattern (at most kMaxTaps).
    static int echoesPerCycle(int points, double echoTime, double polyStep);

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
    std::array<std::unique_ptr<ControlObject>, kMaxPoints> m_pPointLevel;

    DelayLine m_cycle; // the input plus a whole recirculating cycle
    double m_echoSamples = 0; // echo time, slewed
    std::array<OnePole, kMaxTaps> m_tapTone;
    std::array<OnePole, kMaxTaps> m_tapLowCut;
    OnePole m_loopTone;
    OnePole m_loopLowCut;
    // Per point: speaker gains now and at the end of the callback.
    std::array<std::array<float, kMaxSpeakers>, kMaxPoints> m_pointGains{};
    bool m_gainsValid = false;
    std::array<float, kMaxPoints> m_pointPeak{};
    std::array<float, kMaxPoints> m_pointMeter{};
    float m_enabledGain = 0;
    float m_gateGain = 1;
    bool m_idle = true;
};

} // namespace mixxx::spatial
