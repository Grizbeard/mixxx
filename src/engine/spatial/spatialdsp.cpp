#include "engine/spatial/spatialdsp.h"

#include <algorithm>

namespace mixxx::spatial {

namespace {
constexpr double kButterworthQ = 0.70710678118654752;
} // namespace

void Biquad::setLowPass(double frequency, double sampleRate) {
    const double w0 = 2 * kPi * frequency / sampleRate;
    const double alpha = std::sin(w0) / (2 * kButterworthQ);
    const double cosw = std::cos(w0);
    const double a0 = 1 + alpha;
    b0 = (1 - cosw) / 2 / a0;
    b1 = (1 - cosw) / a0;
    b2 = b0;
    a1 = -2 * cosw / a0;
    a2 = (1 - alpha) / a0;
}

void Biquad::setHighPass(double frequency, double sampleRate) {
    const double w0 = 2 * kPi * frequency / sampleRate;
    const double alpha = std::sin(w0) / (2 * kButterworthQ);
    const double cosw = std::cos(w0);
    const double a0 = 1 + alpha;
    b0 = (1 + cosw) / 2 / a0;
    b1 = -(1 + cosw) / a0;
    b2 = b0;
    a1 = -2 * cosw / a0;
    a2 = (1 - alpha) / a0;
}

void DelayLine::allocate(int minimumLength) {
    int length = 1;
    while (length < minimumLength + 2) {
        length <<= 1;
    }
    m_buffer.assign(length, 0.0f);
    m_mask = length - 1;
    m_write = 0;
}

void DelayLine::clear() {
    std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
}

void DelayLine::clearRecent(int samples) {
    samples = std::min(samples, capacity());
    for (int i = 1; i <= samples; ++i) {
        m_buffer[(m_write - i) & m_mask] = 0.0f;
    }
}

void SpatialClock::advance(double frames,
        double sampleRate,
        std::optional<double> beatLength,
        std::optional<double> beatFraction,
        double fallbackBpm) {
    const double seconds = frames / sampleRate;
    m_beatsStart = m_beatsEnd;
    m_secondsStart = m_secondsEnd;
    m_secondsEnd += seconds;
    if (beatLength && *beatLength > 0 && beatFraction) {
        m_beatLength = *beatLength;
        // Predict, then pull onto the source's phase. A paused source holds
        // its phase, so the clock holds too; a seek re-locks next callback.
        const double predicted = m_beatsEnd + seconds / m_beatLength;
        double delta = *beatFraction - (predicted - std::floor(predicted));
        delta -= std::round(delta);
        m_beatsEnd = predicted + delta;
    } else {
        const double bpm = fallbackBpm > 0 ? fallbackBpm : 120;
        m_beatLength = 60.0 / bpm;
        m_beatsEnd += seconds / m_beatLength;
    }
}

} // namespace mixxx::spatial
