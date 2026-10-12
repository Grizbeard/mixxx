#pragma once

#include <array>
#include <memory>

#include "engine/spatial/spatialcontrols.h"
#include "engine/spatial/spatialdsp.h"
#include "util/types.h"

namespace mixxx::spatial {

class SpatialAutopan;

/// What a source renders into: planar speaker channels, the mono LF sum and
/// the stereo FX send, each `frames` long.
struct SpatialBus {
    std::array<float*, kMaxSpeakers> speakers{};
    float* lf = nullptr;
    float* sendLeft = nullptr;
    float* sendRight = nullptr;
};

struct SpatialRenderContext {
    const SpeakerLayout* pLayout = nullptr;
    const SpatialAutopan* const* ppAutopans = nullptr;
    int autopanCount = 0;
    double sampleRate = 48000;
    SpatialBus bus;
    // Dual-band panning, split at kBandCrossoverHz. With both off a source
    // renders as one band, exactly as before.
    // Even lows: the low band is normalised by `lowNorm`, a p-norm from 1
    // (the summed pressure stays constant) to 2 (power, as the highs), so a
    // source between two speakers is not louder in the lows than one on a
    // speaker.
    bool evenLows = false;
    double lowNorm = 1.5;
    // Sharp highs: the high band pans with the intensity law.
    bool sharpHighs = false;
};

/// Where dual-band panning splits lows from highs (Hughes et al. 2018).
constexpr double kBandCrossoverHz = 700;

/// One spatial source: a stem of a deck (or a whole stereo track), with its
/// position controls, panned onto the ring.
///
/// Controls, group [ChannelN_StemM] (Drums = Stem1, INST = Stem3, which
/// carries the folded-in Bass, Vocals = Stem4; a stereo track uses Stem3):
///   spatial_azimuth   pan, degrees, 0 = front, wraps (±180)
///   spatial_width     0..1 = 0..180° between the stem's L and R
///   spatial_spread    0 = a point on the ring, 1 = every speaker
///   spatial_fx_send   post-fader send to the Spatial FX unit
///   spatial_autopan   follow an auto-pan unit; the pan rotates its shape
///   spatial_autopan_unit    1 or 2
///   spatial_autopan_offset  phase offset, fraction of the shape's cycle
///   spatial_front     glide to the front pair as the original stereo image
///   read-only: spatial_x, spatial_y (current centre), spatial_level (peak),
///   spatial_front_blend, spatial_active (1 while rendering)
class SpatialSource {
  public:
    static constexpr double kFrontGlideSeconds = 0.25;
    static constexpr double kGainSmoothingSeconds = 0.004;
    static constexpr int kSubBlock = 32;

    explicit SpatialSource(const QString& group);

    /// Adds `frames` frames of interleaved stereo to the bus.
    void render(const CSAMPLE* pStereo, int frames, const SpatialRenderContext& context);

    /// Publishes the meter and position; call once per callback for every
    /// source (an idle source decays to silence and reports inactive).
    void publish(bool active);

    const QString& group() const {
        return m_group;
    }

  private:
    void targetGains(const SpatialRenderContext& context,
            double t,
            PanLaw law,
            float* pLeft,
            float* pRight,
            Point* pCentre);

    const QString m_group;
    std::unique_ptr<ControlPotmeter> m_pAzimuth;
    std::unique_ptr<ControlPotmeter> m_pWidth;
    std::unique_ptr<ControlPotmeter> m_pSpread;
    std::unique_ptr<ControlPotmeter> m_pFxSend;
    std::unique_ptr<ControlPushButton> m_pAutopan;
    std::unique_ptr<ControlPotmeter> m_pAutopanUnit;
    std::unique_ptr<ControlPotmeter> m_pAutopanOffset;
    std::unique_ptr<ControlPushButton> m_pFront;
    std::unique_ptr<ControlObject> m_pX;
    std::unique_ptr<ControlObject> m_pY;
    std::unique_ptr<ControlObject> m_pLevel;
    std::unique_ptr<ControlObject> m_pFrontBlend;
    std::unique_ptr<ControlObject> m_pActive;

    // Engine-thread state.
    std::array<float, kMaxSpeakers> m_gainLeft{};
    std::array<float, kMaxSpeakers> m_gainRight{};
    bool m_gainsValid = false;
    // Dual band: the low band's gains minus the main gains, applied to the
    // low-passed input, so equal gains reduce exactly to one band.
    std::array<float, kMaxSpeakers> m_lowDeltaLeft{};
    std::array<float, kMaxSpeakers> m_lowDeltaRight{};
    bool m_dualBand = false;
    Biquad m_split[2];
    double m_splitRate = 0;
    float m_send = 0;
    double m_frontBlend = 0;
    float m_peak = 0;
    float m_meter = 0;
    Point m_centre;
    bool m_wasActive = false;
};

} // namespace mixxx::spatial
