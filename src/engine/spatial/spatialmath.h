#pragma once

#include <array>
#include <cmath>

/// Geometry for the spatial renderer: the speaker ring, the panner, and the
/// motion shapes and waves shared by auto-pan and the spatial delay.
///
/// Conventions: positions are points in the unit disk seen from above, with
/// +y towards the front (the DJ faces it) and +x to the right. Azimuth is in
/// degrees, 0 = front, positive clockwise (90 = right), wrapped to -180..180.
/// The ring itself is radius 1; the centre of the room is (0, 0).
namespace mixxx::spatial {

constexpr int kMaxSpeakers = 6;
constexpr int kMinSpeakers = 4;
constexpr double kPi = 3.14159265358979323846;

struct Point {
    double x = 0;
    double y = 0;
};

inline double wrapDegrees(double degrees) {
    double wrapped = std::fmod(degrees + 180.0, 360.0);
    if (wrapped < 0) {
        wrapped += 360.0;
    }
    return wrapped - 180.0;
}

inline Point pointAtAzimuth(double degrees, double radius = 1.0) {
    const double radians = degrees * kPi / 180.0;
    return {radius * std::sin(radians), radius * std::cos(radians)};
}

inline double azimuthOf(Point p) {
    return std::atan2(p.x, p.y) * 180.0 / kPi;
}

inline double radiusOf(Point p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

inline Point rotate(Point p, double degrees) {
    const double radians = degrees * kPi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    // Clockwise rotation in our (x right, y front) frame.
    return {p.x * c + p.y * s, -p.x * s + p.y * c};
}

/// The speakers on the ring, in output order (speaker 1 first), and the
/// panner over them.
class SpeakerLayout {
  public:
    SpeakerLayout();

    /// Defaults with speakers 1 and 2 as the front pair, then clockwise:
    /// 4: FL -45, FR 45, RR 135, RL -135
    /// 5: FL -36, FR 36, R 108, rear 180, L -108
    /// 6: FL -30, FR 30, SR 90, RR 150, RL -150, SL -90
    static std::array<double, kMaxSpeakers> defaultAzimuths(int count);

    void setSpeakers(int count, const double* pAzimuths);

    int count() const {
        return m_count;
    }
    double azimuth(int speaker) const {
        return m_azimuth[speaker];
    }

    /// Centre and opening angle of the front pair (speakers 1 and 2).
    double frontAzimuth() const;
    double frontWidth() const;

    /// Power-normalised speaker gains for one point source.
    /// `spread` 0 = pairwise panning on the ring (at most two speakers),
    /// 1 = equal power on every speaker, in between a power-domain blend.
    void gains(double azimuth, double spread, float* pGains) const;

  private:
    int m_count;
    std::array<double, kMaxSpeakers> m_azimuth;
    // Ring order: speaker indices sorted by azimuth, and the azimuths.
    std::array<int, kMaxSpeakers> m_ring;
    std::array<double, kMaxSpeakers> m_ringAzimuth;
    // Per ring pair (k, k+1): inverse of the 2x2 speaker-vector matrix
    // (row-major), or a flag that the pair is too wide for vector panning.
    std::array<std::array<double, 4>, kMaxSpeakers> m_inverse;
    std::array<bool, kMaxSpeakers> m_useAngleLaw;
};

/// Motion shapes. Each is a closed path through `pointCount` points; motion
/// moves along the path from point to point.
enum class Shape {
    Orbit = 0,     // a circle on the ring, 4 points (front, right, back, left)
    LeftRight = 1, // left <-> right through the centre
    FrontBack = 2, // front <-> back through the centre
    Corners = 3,   // the 4 corners (±45°, ±135°) along straight lines
    Star5 = 4,     // 5-point hypotrochoid star
    Star7 = 5,     // 7-point hypotrochoid star
};
constexpr int kShapeCount = 6;

/// How motion travels between two points within one step.
enum class Wave {
    Square = 0,     // hold on the point, snap to the next on the step
    Sine = 1,       // ease out of a point and into the next
    Triangle = 2,   // constant speed from point to point
    Saw = 3,        // ramp A->B over the step, snap to C, ramp C->D, ...
    ReverseSaw = 4, // ramp B->A, snap to D, ramp D->C, ...
};
constexpr int kWaveCount = 5;

int shapePointCount(Shape shape);

/// The path at `position` in point units: integer positions are the points,
/// fractions lie on the path between them. Wraps to 0..pointCount.
Point shapeAt(Shape shape, double position);

/// Steps in one full cycle of the wave over the shape (the motion repeats
/// after this many steps).
int waveCycleSteps(Wave wave, int pointCount);

/// Position along the path (point units) after `steps` steps.
/// `reverse` runs the points in the opposite order.
double wavePosition(Wave wave, double steps, int pointCount, bool reverse);

/// Beat lengths offered for synced motion and echoes, in beats.
constexpr std::array<double, 9> kBeatDivisions = {
        1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 4, 8, 16};

} // namespace mixxx::spatial
