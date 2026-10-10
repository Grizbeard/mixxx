#pragma once

#include <cmath>
#include <optional>
#include <vector>

#include "engine/spatial/spatialmath.h"

/// Small DSP building blocks for the spatial bus.
namespace mixxx::spatial {

/// Second-order Butterworth section (RBJ). Two in series give the
/// Linkwitz-Riley 4th order used for the LF crossover.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    void setLowPass(double frequency, double sampleRate);
    void setHighPass(double frequency, double sampleRate);
    void reset() {
        z1 = 0;
        z2 = 0;
    }
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/// Linkwitz-Riley 4th order: low-pass and high-pass outputs sum flat.
struct LinkwitzRiley4 {
    Biquad stage[2];
    void setLowPass(double frequency, double sampleRate) {
        stage[0].setLowPass(frequency, sampleRate);
        stage[1].setLowPass(frequency, sampleRate);
    }
    void setHighPass(double frequency, double sampleRate) {
        stage[0].setHighPass(frequency, sampleRate);
        stage[1].setHighPass(frequency, sampleRate);
    }
    void reset() {
        stage[0].reset();
        stage[1].reset();
    }
    double process(double x) {
        return stage[1].process(stage[0].process(x));
    }
};

/// One-pole low-pass, and the matching high-pass as input minus low-pass.
struct OnePole {
    double a = 0; // coefficient: y += a * (x - y)
    double y = 0;
    void setCutoff(double frequency, double sampleRate) {
        a = 1 - std::exp(-2 * kPi * frequency / sampleRate);
    }
    double lowPass(double x) {
        y += a * (x - y);
        return y;
    }
    double highPass(double x) {
        return x - lowPass(x);
    }
};

/// Coefficient for a one-pole smoother with time constant `seconds`,
/// applied once per `samples` samples.
inline double smoothingCoefficient(double seconds, double samples, double sampleRate) {
    if (seconds <= 0) {
        return 1;
    }
    return 1 - std::exp(-samples / (seconds * sampleRate));
}

/// A fractional delay line (power-of-two ring, linear interpolation).
class DelayLine {
  public:
    void allocate(int minimumLength);
    void clear();
    int capacity() const {
        return m_mask + 1;
    }
    void write(float x) {
        m_buffer[m_write] = x;
        m_write = (m_write + 1) & m_mask;
    }
    /// The sample written `delay` samples ago (>= 1 = the last write).
    float read(double delay) const {
        const double position = m_write - delay;
        const double floorPos = std::floor(position);
        const int i = static_cast<int>(floorPos) & m_mask;
        const auto frac = static_cast<float>(position - floorPos);
        return m_buffer[i] + (m_buffer[(i + 1) & m_mask] - m_buffer[i]) * frac;
    }
    float readInteger(int delay) const {
        return m_buffer[(m_write - delay) & m_mask];
    }

  private:
    std::vector<float> m_buffer;
    int m_mask = 0;
    int m_write = 0;
};

/// The musical clock driving motion and synced effects. Counts beats
/// continuously, locked to the tempo source's beat phase, and seconds.
class SpatialClock {
  public:
    /// Once per callback. `beatLength` (seconds, rate adjusted) and
    /// `beatFraction` (phase at the buffer end) come from the tempo source;
    /// without them the clock free-runs at `fallbackBpm`.
    void advance(double frames,
            double sampleRate,
            std::optional<double> beatLength,
            std::optional<double> beatFraction,
            double fallbackBpm);

    double beatsStart() const {
        return m_beatsStart;
    }
    double beatsEnd() const {
        return m_beatsEnd;
    }
    double secondsStart() const {
        return m_secondsStart;
    }
    double secondsEnd() const {
        return m_secondsEnd;
    }
    double beatLength() const {
        return m_beatLength;
    }
    double beatsAt(double t) const {
        return m_beatsStart + (m_beatsEnd - m_beatsStart) * t;
    }
    double secondsAt(double t) const {
        return m_secondsStart + (m_secondsEnd - m_secondsStart) * t;
    }

  private:
    double m_beatsStart = 0;
    double m_beatsEnd = 0;
    double m_secondsStart = 0;
    double m_secondsEnd = 0;
    double m_beatLength = 0.5;
};

/// Where a motion is during one callback: shape, wave and the step count at
/// the callback's start and end.
struct MotionState {
    Shape shape = Shape::Orbit;
    Wave wave = Wave::Square;
    bool reverse = false;
    double size = 1;
    double stepsStart = 0;
    double stepsEnd = 0;

    int pointCount() const {
        return shapePointCount(shape);
    }
    /// `t` 0..1 across the callback; `offset` shifts by a fraction of the
    /// motion's full cycle.
    Point positionAt(double t, double offset = 0) const {
        const double cycle = waveCycleSteps(wave, pointCount());
        const double steps = stepsStart + (stepsEnd - stepsStart) * t + offset * cycle;
        const Point p = shapeAt(shape, wavePosition(wave, steps, pointCount(), reverse));
        return {p.x * size, p.y * size};
    }
};

} // namespace mixxx::spatial
