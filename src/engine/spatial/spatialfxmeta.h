#pragma once

#include <algorithm>
#include <cmath>
#include <memory>

class ControlPotmeter;

namespace mixxx::spatial {

/// The Spatial FX unit's macro knobs, for the Basic layout and a
/// controller: three knobs and a level fader that play the delay and
/// reverb together, like an effect unit's meta knob and mix.
///
/// At 0.5 a macro leaves the unit as its settings (or the loaded preset)
/// say; turning it pushes those settings towards their extremes:
///   SPACE  reverb decay, size and pre-delay: small room .. endless hall
///   ECHO   delay feedback and how much of it goes on into the reverb
///   TONE   dark .. bright: delay tone up, reverb damping down
///   LEVEL  the whole unit's wet level (1 = as set)
///
/// Controls, group [SpatialFX]: space, echo, tone (0..1, default 0.5),
/// level (0..1, default 1). Not persisted: they are performance controls.
struct FxMeta {
    double space = 0.5;
    double echo = 0.5;
    double tone = 0.5;
    double level = 1;

    /// Moves `base` towards `low` (macro below 0.5) or `high` (above).
    static double push(double base, double macro, double low, double high) {
        macro = std::clamp(macro, 0.0, 1.0);
        return macro >= 0.5 ? base + (high - base) * (2 * macro - 1)
                            : base - (base - low) * (1 - 2 * macro);
    }
    /// The same on a ratio scale, for times.
    static double pushRatio(double base, double macro, double low, double high) {
        base = std::clamp(base, low, high);
        macro = std::clamp(macro, 0.0, 1.0);
        return macro >= 0.5 ? base * std::pow(high / base, 2 * macro - 1)
                            : base * std::pow(low / base, 1 - 2 * macro);
    }

    double reverbDecay(double base) const {
        return pushRatio(base, space, 0.5, 30);
    }
    double reverbSize(double base) const {
        return push(base, space, 0, 1);
    }
    double reverbPredelayMs(double base) const {
        return push(base, space, 0, 200);
    }
    double delayFeedback(double base) const {
        return push(base, echo, 0, 0.95);
    }
    double delayToReverb(double base) const {
        return push(base, echo, 0, 1);
    }
    double delayTone(double base) const {
        return push(base, tone, 0, 1);
    }
    double reverbDamping(double base) const {
        return push(base, 1 - tone, 0, 1);
    }
};

/// Owns the [SpatialFX] macro controls.
class SpatialFxMetaControls {
  public:
    static constexpr const char* kGroup = "[SpatialFX]";
    SpatialFxMetaControls();
    ~SpatialFxMetaControls();
    FxMeta values() const;

  private:
    std::unique_ptr<ControlPotmeter> m_pSpace;
    std::unique_ptr<ControlPotmeter> m_pEcho;
    std::unique_ptr<ControlPotmeter> m_pTone;
    std::unique_ptr<ControlPotmeter> m_pLevel;
};

} // namespace mixxx::spatial
