#include "engine/spatial/spatialmath.h"

#include <algorithm>
#include <numeric>

namespace mixxx::spatial {

namespace {

/// Wraps to [0, 360).
double wrap360(double degrees) {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0) {
        wrapped += 360.0;
    }
    return wrapped;
}

double wrapPosition(double position, int pointCount) {
    double wrapped = std::fmod(position, static_cast<double>(pointCount));
    if (wrapped < 0) {
        wrapped += pointCount;
    }
    return wrapped;
}

Point lerp(Point a, Point b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

/// A polyline through `points`, closed back to the first point.
Point polylineAt(const Point* pPoints, int count, double position) {
    const double wrapped = wrapPosition(position, count);
    const int k = std::min(static_cast<int>(wrapped), count - 1);
    return lerp(pPoints[k], pPoints[(k + 1) % count], wrapped - k);
}

/// A hypotrochoid with `points` outer points (R = points, rolling circle r),
/// scaled so the points lie on the ring and the path dips to `innerRadius`
/// halfway between them. Point 0 is at the front.
Point starAt(int points, int rollingRadius, double innerRadius, double position) {
    const double bigR = points;
    const double r = rollingRadius;
    const double rr = bigR - r;
    const double d = rr * (1 - innerRadius) / (1 + innerRadius);
    const double t = position * 2 * kPi * r / bigR;
    const double u = rr * std::cos(t) + d * std::cos(rr / r * t);
    const double v = rr * std::sin(t) - d * std::sin(rr / r * t);
    // Standard angle a maps to our azimuth a: x = sin a, y = cos a.
    return {v / (rr + d), u / (rr + d)};
}

} // namespace

SpeakerLayout::SpeakerLayout()
        : m_count(0),
          m_azimuth{},
          m_ring{},
          m_ringAzimuth{},
          m_inverse{},
          m_useAngleLaw{} {
    const auto defaults = defaultAzimuths(kMaxSpeakers);
    setSpeakers(kMaxSpeakers, defaults.data());
}

std::array<double, kMaxSpeakers> SpeakerLayout::defaultAzimuths(int count) {
    switch (count) {
    case 4:
        return {-45, 45, 135, -135, 0, 0};
    case 5:
        return {-36, 36, 108, 180, -108, 0};
    default:
        return {-30, 30, 90, 150, -150, -90};
    }
}

void SpeakerLayout::setSpeakers(int count, const double* pAzimuths) {
    m_count = std::clamp(count, 1, kMaxSpeakers);
    for (int i = 0; i < m_count; ++i) {
        m_azimuth[i] = wrapDegrees(pAzimuths[i]);
    }
    std::iota(m_ring.begin(), m_ring.begin() + m_count, 0);
    std::sort(m_ring.begin(), m_ring.begin() + m_count, [this](int a, int b) {
        return m_azimuth[a] < m_azimuth[b];
    });
    for (int k = 0; k < m_count; ++k) {
        m_ringAzimuth[k] = m_azimuth[m_ring[k]];
    }
    for (int k = 0; k < m_count; ++k) {
        const double a = m_ringAzimuth[k];
        const double b = m_ringAzimuth[(k + 1) % m_count];
        const double span = m_count == 1 ? 360.0 : wrap360(b - a);
        const Point la = pointAtAzimuth(a);
        const Point lb = pointAtAzimuth(b);
        const double det = la.x * lb.y - la.y * lb.x;
        // Vector panning needs the pair to open less than 180°; near that it
        // becomes ill-conditioned, so wide gaps use the sine/cosine law.
        m_useAngleLaw[k] = span >= 170.0 || std::abs(det) < 1e-6;
        if (!m_useAngleLaw[k]) {
            m_inverse[k] = {lb.y / det, -la.y / det, -lb.x / det, la.x / det};
        }
    }
}

double SpeakerLayout::frontAzimuth() const {
    if (m_count < 2) {
        return 0;
    }
    const Point a = pointAtAzimuth(m_azimuth[0]);
    const Point b = pointAtAzimuth(m_azimuth[1]);
    return azimuthOf({a.x + b.x, a.y + b.y});
}

double SpeakerLayout::frontWidth() const {
    if (m_count < 2) {
        return 0;
    }
    return std::abs(wrapDegrees(m_azimuth[1] - m_azimuth[0]));
}

void SpeakerLayout::gains(double azimuth, double spread, float* pGains) const {
    std::fill(pGains, pGains + m_count, 0.0f);
    if (m_count == 1) {
        pGains[0] = 1;
        return;
    }
    const double az = wrapDegrees(azimuth);
    // The ring pair whose arc holds the source.
    int pair = m_count - 1;
    double offset = 0;
    double span = 360;
    for (int k = 0; k < m_count; ++k) {
        const double a = m_ringAzimuth[k];
        const double s = wrap360(m_ringAzimuth[(k + 1) % m_count] - a);
        const double o = wrap360(az - a);
        if (o <= s) {
            pair = k;
            offset = o;
            span = s;
            break;
        }
    }
    double ga;
    double gb;
    if (span < 1e-9) {
        ga = 1;
        gb = 0;
    } else if (m_useAngleLaw[pair]) {
        const double t = offset / span;
        ga = std::cos(t * kPi / 2);
        gb = std::sin(t * kPi / 2);
    } else {
        const Point p = pointAtAzimuth(az);
        const auto& inv = m_inverse[pair];
        ga = std::max(0.0, p.x * inv[0] + p.y * inv[2]);
        gb = std::max(0.0, p.x * inv[1] + p.y * inv[3]);
        const double norm = std::sqrt(ga * ga + gb * gb);
        if (norm > 0) {
            ga /= norm;
            gb /= norm;
        } else {
            ga = 1;
        }
    }
    const int speakerA = m_ring[pair];
    const int speakerB = m_ring[(pair + 1) % m_count];
    const double s = std::clamp(spread, 0.0, 1.0);
    const double even = s / m_count;
    for (int i = 0; i < m_count; ++i) {
        double v = 0;
        if (i == speakerA) {
            v = ga;
        } else if (i == speakerB) {
            v = gb;
        }
        pGains[i] = static_cast<float>(std::sqrt((1 - s) * v * v + even));
    }
}

int shapePointCount(Shape shape) {
    switch (shape) {
    case Shape::Orbit:
        return 4;
    case Shape::LeftRight:
    case Shape::FrontBack:
        return 2;
    case Shape::Corners:
        return 4;
    case Shape::Star5:
        return 5;
    case Shape::Star7:
        return 7;
    }
    return 4;
}

Point shapeAt(Shape shape, double position) {
    const int count = shapePointCount(shape);
    switch (shape) {
    case Shape::Orbit:
        return pointAtAzimuth(wrapPosition(position, count) / count * 360.0);
    case Shape::LeftRight: {
        static const Point points[] = {{-1, 0}, {1, 0}};
        return polylineAt(points, 2, position);
    }
    case Shape::FrontBack: {
        static const Point points[] = {{0, 1}, {0, -1}};
        return polylineAt(points, 2, position);
    }
    case Shape::Corners: {
        static const Point points[] = {pointAtAzimuth(-45),
                pointAtAzimuth(45),
                pointAtAzimuth(135),
                pointAtAzimuth(-135)};
        return polylineAt(points, 4, position);
    }
    case Shape::Star5:
        // {5/2}: each step skips a point; dips to an edge's closest approach.
        return starAt(5, 3, std::cos(2 * kPi / 5), position);
    case Shape::Star7:
        // {7/3}: the sharper heptagram.
        return starAt(7, 4, std::cos(3 * kPi / 7), position);
    }
    return {};
}

int waveCycleSteps(Wave wave, int pointCount) {
    if (wave == Wave::Saw || wave == Wave::ReverseSaw) {
        // Each step covers two points.
        return pointCount % 2 == 0 ? pointCount / 2 : pointCount;
    }
    return pointCount;
}

double wavePosition(Wave wave, double steps, int pointCount, bool reverse) {
    const double step = std::floor(steps);
    const double phase = steps - step;
    double position = step;
    switch (wave) {
    case Wave::Square:
        position = step;
        break;
    case Wave::Sine:
        position = step + (1 - std::cos(kPi * phase)) / 2;
        break;
    case Wave::Triangle:
        position = step + phase;
        break;
    case Wave::Saw:
        position = 2 * step + phase;
        break;
    case Wave::ReverseSaw:
        position = 2 * step + 1 - phase;
        break;
    }
    if (reverse) {
        position = -position;
    }
    return wrapPosition(position, pointCount);
}

} // namespace mixxx::spatial
