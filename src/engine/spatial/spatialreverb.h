#pragma once

#include <array>
#include <memory>

#include "engine/spatial/spatialcontrols.h"
#include "engine/spatial/spatialdsp.h"
#include "engine/spatial/spatialfxmeta.h"

namespace mixxx::spatial {

/// The Spatial FX unit's reverb: an 8-line feedback delay network with a
/// modulated, diffuse tail, built for long, washy trance vocal spaces. Each
/// speaker gets its own decorrelated mix of the network (orthogonal
/// Hadamard rows), so the tail surrounds the room instead of sitting
/// between the speakers. Only the wet signal is decorrelated; dry stems are
/// untouched.
///
/// - Pre-delay keeps the vocal's words clear of the wash.
/// - Low cut keeps kick and bass out of the tail.
/// - Modulation slowly detunes the lines: a lush, chorused tail, no ringing.
/// - Shimmer pitches the tail up an octave and feeds it back: the classic
///   "angelic" trance pad behind a vocal.
/// - Duck pulls the tail down while the send is loud, so it blooms in the
///   gaps between phrases.
/// - Freeze holds the current tail indefinitely.
///
/// Controls, group [SpatialReverb] (persisted):
///   enabled, level 0..1, decay seconds (0.5..30), size 0..1, predelay_ms
///   (0..500), damping 0..1, lowcut Hz (20..1000), modulation 0..1,
///   diffusion 0..1, shimmer 0..1, duck 0..1, freeze (not persisted),
///   direct 0..1: how much of the stems' FX send feeds the reverb directly
///   (the delay's to_reverb adds its echoes on top; at 0 the reverb hears
///   only the echoes)
class SpatialReverb {
  public:
    static constexpr int kLines = 8;
    static constexpr double kMaxSampleRate = 192000;
    static constexpr const char* kGroup = "[SpatialReverb]";

    SpatialReverb();

    /// The share of the stems' FX send that feeds the reverb directly.
    float directLevel() const;

    /// `pIn` is the mono input (sends + echoes), `frames` long.
    void process(const float* pIn,
            int frames,
            double sampleRate,
            const SpeakerLayout& layout,
            const std::array<float*, kMaxSpeakers>& speakers,
            float* pLf,
            const FxMeta& meta = FxMeta());

  private:
    void clear();

    std::unique_ptr<ControlPushButton> m_pEnabled;
    std::unique_ptr<ControlPotmeter> m_pLevel;
    std::unique_ptr<ControlPotmeter> m_pDecay;
    std::unique_ptr<ControlPotmeter> m_pSize;
    std::unique_ptr<ControlPotmeter> m_pPredelayMs;
    std::unique_ptr<ControlPotmeter> m_pDamping;
    std::unique_ptr<ControlPotmeter> m_pLowCut;
    std::unique_ptr<ControlPotmeter> m_pModulation;
    std::unique_ptr<ControlPotmeter> m_pDiffusion;
    std::unique_ptr<ControlPotmeter> m_pShimmer;
    std::unique_ptr<ControlPotmeter> m_pDuck;
    std::unique_ptr<ControlPushButton> m_pFreeze;
    std::unique_ptr<ControlPotmeter> m_pDirect;

    std::array<DelayLine, kLines> m_lines;
    std::array<OnePole, kLines> m_damping;
    std::array<double, kLines> m_lfoPhase{};
    std::array<DelayLine, 4> m_diffusers;
    DelayLine m_predelay;
    DelayLine m_pitchLine;
    double m_pitchPhase = 0;
    float m_shimmerSample = 0;
    OnePole m_lowCut[2];
    double m_envelope = 0;
    float m_duckGain = 1;
    float m_enabledGain = 0;
    float m_inputGain = 1;
    bool m_sleeping = true;
};

} // namespace mixxx::spatial
