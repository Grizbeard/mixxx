#include "engine/spatial/spatialsource.h"

#include <algorithm>

#include "engine/spatial/spatialautopan.h"

namespace mixxx::spatial {

SpatialSource::SpatialSource(const QString& group)
        : m_group(group),
          m_pAzimuth(makePot(group, "spatial_azimuth", -180, 180, 0, false, true)),
          m_pWidth(makePot(group, "spatial_width", 0, 1, 1.0 / 3)),
          m_pSpread(makePot(group, "spatial_spread", 0, 1, 0)),
          m_pFxSend(makePot(group, "spatial_fx_send", 0, 1, 0)),
          m_pAutopan(makeToggle(group, "spatial_autopan")),
          m_pAutopanUnit(makePot(group, "spatial_autopan_unit", 1, 2, 1)),
          m_pAutopanOffset(makePot(group, "spatial_autopan_offset", 0, 1, 0)),
          m_pFront(makeToggle(group, "spatial_front")),
          m_pX(makeReadOnly(group, "spatial_x")),
          m_pY(makeReadOnly(group, "spatial_y", 1)),
          m_pLevel(makeReadOnly(group, "spatial_level")),
          m_pFrontBlend(makeReadOnly(group, "spatial_front_blend")),
          m_pActive(makeReadOnly(group, "spatial_active")) {
}

void SpatialSource::targetGains(const SpatialRenderContext& context,
        double t,
        float* pLeft,
        float* pRight,
        Point* pCentre) {
    const SpeakerLayout& layout = *context.pLayout;
    const int speakers = layout.count();
    const double azimuth = wrapDegrees(m_pAzimuth->get());
    const double halfWidth = std::clamp(m_pWidth->get(), 0.0, 1.0) * 90.0;
    const double manualSpread = std::clamp(m_pSpread->get(), 0.0, 1.0);

    double centreAzimuth = azimuth;
    double spread = manualSpread;
    if (m_pAutopan->toBool() && context.autopanCount > 0) {
        const int unit = toIndex(m_pAutopanUnit->get(), 1, context.autopanCount);
        const MotionState& motion = context.ppAutopans[unit - 1]->motion();
        // The manual pan rotates the shape, so "front" of the shape follows it.
        const Point p = rotate(motion.positionAt(t, m_pAutopanOffset->get()), azimuth);
        const double radius = std::min(1.0, radiusOf(p));
        if (radius > 1e-6) {
            centreAzimuth = azimuthOf(p);
        }
        spread = 1 - radius * (1 - manualSpread);
    }

    layout.gains(centreAzimuth - halfWidth, spread, pLeft);
    layout.gains(centreAzimuth + halfWidth, spread, pRight);
    // Where L and R land on the same speakers, they add coherently; trim by
    // up to 3 dB as they overlap so collapsing the width keeps the level.
    double overlap = 0;
    for (int i = 0; i < speakers; ++i) {
        overlap += pLeft[i] * pRight[i];
    }
    const auto compensation = static_cast<float>(1.0 / std::sqrt(1.0 + overlap));
    for (int i = 0; i < speakers; ++i) {
        pLeft[i] *= compensation;
        pRight[i] *= compensation;
    }

    Point centre = pointAtAzimuth(centreAzimuth, 1 - spread);
    // Front: the stem's L on speaker 1 and R on speaker 2, exactly.
    const double f = m_frontBlend * m_frontBlend * (3 - 2 * m_frontBlend);
    if (f > 0 && speakers >= 2) {
        for (int i = 0; i < speakers; ++i) {
            const double frontLeft = i == 0 ? 1.0 : 0.0;
            const double frontRight = i == 1 ? 1.0 : 0.0;
            pLeft[i] = static_cast<float>(std::sqrt(
                    (1 - f) * pLeft[i] * pLeft[i] + f * frontLeft));
            pRight[i] = static_cast<float>(std::sqrt(
                    (1 - f) * pRight[i] * pRight[i] + f * frontRight));
        }
        const Point front = pointAtAzimuth(layout.frontAzimuth());
        centre = {centre.x + (front.x - centre.x) * f, centre.y + (front.y - centre.y) * f};
    }
    *pCentre = centre;
}

void SpatialSource::render(const CSAMPLE* pStereo,
        int frames,
        const SpatialRenderContext& context) {
    if (frames <= 0) {
        return;
    }
    const int speakers = context.pLayout->count();
    const double rate = context.sampleRate;
    const double frontTarget = m_pFront->toBool() ? 1.0 : 0.0;
    const auto sendTarget = static_cast<float>(std::clamp(m_pFxSend->get(), 0.0, 1.0));
    const auto alpha = static_cast<float>(
            smoothingCoefficient(kGainSmoothingSeconds, kSubBlock, rate));
    const SpatialBus& bus = context.bus;

    std::array<float, kMaxSpeakers> targetLeft{};
    std::array<float, kMaxSpeakers> targetRight{};
    std::array<float, kMaxSpeakers> stepLeft{};
    std::array<float, kMaxSpeakers> stepRight{};
    float peak = 0;

    for (int start = 0; start < frames; start += kSubBlock) {
        const int n = std::min(kSubBlock, frames - start);
        const double glide = n / (kFrontGlideSeconds * rate);
        if (m_frontBlend < frontTarget) {
            m_frontBlend = std::min(frontTarget, m_frontBlend + glide);
        } else if (m_frontBlend > frontTarget) {
            m_frontBlend = std::max(frontTarget, m_frontBlend - glide);
        }
        targetGains(context,
                (start + n * 0.5) / frames,
                targetLeft.data(),
                targetRight.data(),
                &m_centre);
        if (!m_gainsValid) {
            m_gainLeft = targetLeft;
            m_gainRight = targetRight;
            m_gainsValid = true;
        }
        for (int i = 0; i < speakers; ++i) {
            stepLeft[i] = (targetLeft[i] - m_gainLeft[i]) * alpha / n;
            stepRight[i] = (targetRight[i] - m_gainRight[i]) * alpha / n;
        }
        const float sendStep = (sendTarget - m_send) / n;

        const CSAMPLE* pIn = pStereo + 2 * start;
        for (int j = 0; j < n; ++j) {
            const float left = pIn[2 * j];
            const float right = pIn[2 * j + 1];
            const int frame = start + j;
            for (int i = 0; i < speakers; ++i) {
                m_gainLeft[i] += stepLeft[i];
                m_gainRight[i] += stepRight[i];
                bus.speakers[i][frame] += left * m_gainLeft[i] + right * m_gainRight[i];
            }
            bus.lf[frame] += 0.5f * (left + right);
            m_send += sendStep;
            bus.sendLeft[frame] += left * m_send;
            bus.sendRight[frame] += right * m_send;
            peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
        }
    }
    m_peak = std::max(m_peak, peak);
}

void SpatialSource::publish(bool active) {
    // A peak meter with ~300 ms fall, updated per callback.
    m_meter = std::max(m_peak, m_meter * 0.9f);
    m_peak = 0;
    if (!active) {
        m_gainsValid = false;
        if (m_wasActive) {
            m_pActive->forceSet(0);
            m_wasActive = false;
        }
        if (m_meter < 1e-4f) {
            if (m_meter > 0) {
                m_meter = 0;
                m_pLevel->forceSet(0);
            }
            return;
        }
    } else if (!m_wasActive) {
        m_pActive->forceSet(1);
        m_wasActive = true;
    }
    m_pLevel->forceSet(m_meter);
    m_pX->forceSet(m_centre.x);
    m_pY->forceSet(m_centre.y);
    m_pFrontBlend->forceSet(m_frontBlend);
}

} // namespace mixxx::spatial
