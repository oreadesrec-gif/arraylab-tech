#pragma once
#include <algorithm>
#include <cmath>

// Geometry of the recording: arrays, microphones, panning. Plain C++, no JUCE.
namespace arraylab
{
constexpr int kMaxGroups = 8;
constexpr int kNumRefs = 4;         // a microphone is measured against main L, C, R and their sum
constexpr int kFineBands = 111;     // phase is resolved per 1/12 octave, 31 Hz .. 18 kHz
constexpr int kMaxPeaks = 6;        // candidate delays kept per measurement
constexpr double kPi = 3.14159265358979323846;

inline double bandFrequency (int band) { return 31.25 * std::pow (2.0, band / 12.0); }
inline double bandPosition (double hz)  { return 12.0 * std::log2 (std::max (hz, 1.0e-3) / 31.25); }

enum class Role { spot = 0, mainL, mainR, mainC, mainPair, outrigger, ambience };
enum class Pattern { omni = 0, wideCardioid, cardioid, superCardioid, hyperCardioid, figure8 };
enum class ArrayType { ab = 0, ortf, nos, xy, blumlein, decca, custom };
/** Which main microphone a spot is lined up with. */
enum class AlignTo { nearest = 0, left, centre, right, average, sum };

inline bool isMain (Role r) { return r == Role::mainL || r == Role::mainR || r == Role::mainC || r == Role::mainPair; }
inline double speedOfSound (double tempC) { return 331.3 + 0.606 * tempC; }

struct Vec2 { double x = 0.0, y = 0.0; };

/** A main array (pair or tree). Azimuths are in degrees, clockwise from "forward" (+y). */
struct ArraySettings
{
    ArrayType type = ArrayType::ortf;
    Pattern pattern = Pattern::cardioid;
    double spacingCm = 17.0;        // distance between L and R
    double angleDeg = 110.0;        // included angle between the L and R axes
    double centreForwardCm = 0.0;   // Decca: how far the centre mic sits ahead of the L-R line
    double x = 0.0, y = 0.0, height = 3.0, rotationDeg = 0.0;
};

inline void applyPreset (ArraySettings& a, ArrayType t)
{
    a.type = t;
    switch (t)
    {
        case ArrayType::ab:       a.pattern = Pattern::omni;     a.spacingCm = 40.0;  a.angleDeg = 0.0;   a.centreForwardCm = 0.0;   break;
        case ArrayType::ortf:     a.pattern = Pattern::cardioid; a.spacingCm = 17.0;  a.angleDeg = 110.0; a.centreForwardCm = 0.0;   break;
        case ArrayType::nos:      a.pattern = Pattern::cardioid; a.spacingCm = 30.0;  a.angleDeg = 90.0;  a.centreForwardCm = 0.0;   break;
        case ArrayType::xy:       a.pattern = Pattern::cardioid; a.spacingCm = 0.0;   a.angleDeg = 90.0;  a.centreForwardCm = 0.0;   break;
        case ArrayType::blumlein: a.pattern = Pattern::figure8;  a.spacingCm = 0.0;   a.angleDeg = 90.0;  a.centreForwardCm = 0.0;   break;
        case ArrayType::decca:    a.pattern = Pattern::omni;     a.spacingCm = 200.0; a.angleDeg = 0.0;   a.centreForwardCm = 150.0; break;
        case ArrayType::custom:   break;
    }
}

inline double patternGain (Pattern p, double offAxisRadians)
{
    double a = 1.0;
    switch (p)
    {
        case Pattern::omni:          a = 1.0;  break;
        case Pattern::wideCardioid:  a = 0.7;  break;
        case Pattern::cardioid:      a = 0.5;  break;
        case Pattern::superCardioid: a = 0.37; break;
        case Pattern::hyperCardioid: a = 0.25; break;
        case Pattern::figure8:       a = 0.0;  break;
    }
    return a + (1.0 - a) * std::cos (offAxisRadians);
}

inline Vec2 direction (double azimuthDeg)
{
    const double r = azimuthDeg * kPi / 180.0;
    return { std::sin (r), std::cos (r) };
}

/** Positions and axes of the array's microphones. Index 0 = L, 1 = C, 2 = R. */
struct ArrayGeometry
{
    Vec2 pos[3];
    double axisDeg[3] {};
    Vec2 forward, right;
};

inline ArrayGeometry arrayGeometry (const ArraySettings& a)
{
    ArrayGeometry g;
    g.forward = direction (a.rotationDeg);
    g.right = direction (a.rotationDeg + 90.0);
    const double half = a.spacingCm * 0.005, fwd = a.centreForwardCm * 0.01;
    g.pos[0] = { a.x - g.right.x * half, a.y - g.right.y * half };
    g.pos[2] = { a.x + g.right.x * half, a.y + g.right.y * half };
    g.pos[1] = { a.x + g.forward.x * fwd, a.y + g.forward.y * fwd };
    g.axisDeg[0] = a.rotationDeg - a.angleDeg * 0.5;
    g.axisDeg[2] = a.rotationDeg + a.angleDeg * 0.5;
    g.axisDeg[1] = a.rotationDeg;
    return g;
}

inline double distance (Vec2 a, Vec2 b) { return std::hypot (a.x - b.x, a.y - b.y); }

/** Where a source sits, given its horizontal distance to the L and R microphones.
    Of the two mirror solutions, the one in front of the array is returned. */
inline Vec2 locate (const ArrayGeometry& g, double rL, double rR)
{
    const Vec2 l = g.pos[0], r = g.pos[2];
    const double d = distance (l, r);
    const double mean = 0.5 * (rL + rR);
    const Vec2 centre { 0.5 * (l.x + r.x), 0.5 * (l.y + r.y) };

    if (d < 0.02) // coincident pair: only the distance is known
        return { centre.x + g.forward.x * mean, centre.y + g.forward.y * mean };

    const double a = (rL * rL - rR * rR + d * d) / (2.0 * d);
    const double h2 = rL * rL - a * a;
    if (h2 <= 0.0) // inconsistent radii: fall back to the bearing given by the time difference
    {
        const double s = std::clamp ((rL - rR) / d, -1.0, 1.0);
        const double c = std::sqrt (1.0 - s * s);
        return { centre.x + (g.right.x * s + g.forward.x * c) * mean,
                 centre.y + (g.right.y * s + g.forward.y * c) * mean };
    }
    const double h = std::sqrt (h2);
    return { l.x + g.right.x * a + g.forward.x * h, l.y + g.right.y * a + g.forward.y * h };
}

/** Level the main pair would give a source at `p`: each main microphone's
    directivity towards the source over its distance, normalised by the pan law
    (0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB at centre). `amount` 0 = centre, 1 = natural, up to 2. */
inline void panGains (const ArrayGeometry& g, Pattern pattern, Vec2 p, double rL, double rR,
                      int law, double amount, float& gainL, float& gainR)
{
    const auto raw = [&] (int i, double r)
    {
        const Vec2 axis = direction (g.axisDeg[i]);
        const double dx = p.x - g.pos[i].x, dy = p.y - g.pos[i].y;
        const double len = std::hypot (dx, dy);
        const double cosT = len > 1e-6 ? (dx * axis.x + dy * axis.y) / len : 1.0;
        const double theta = std::acos (std::clamp (cosT, -1.0, 1.0));
        return std::max (std::abs (patternGain (pattern, theta)), 0.05) / std::max (r, 0.2);
    };
    const double l = raw (0, rL), r = raw (2, rR);
    const double e = law == 2 ? 1.0 : law == 1 ? 1.5 : 2.0;
    const double norm = std::pow (std::pow (l, e) + std::pow (r, e), 1.0 / e);
    const double centre = std::pow (2.0, -1.0 / e);
    gainL = (float) std::max (0.0, centre + amount * (l / norm - centre));
    gainR = (float) std::max (0.0, centre + amount * (r / norm - centre));
}

/** An ordinary panner: pan from -1 (left) to +1 (right), law 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB at centre. */
inline void panLawGains (double pan, int law, float& gainL, float& gainR)
{
    const double x = 0.5 * (std::clamp (pan, -1.0, 1.0) + 1.0);
    const double cl = std::cos (x * kPi * 0.5), cr = std::sin (x * kPi * 0.5);
    const double l = law == 2 ? 1.0 - x : law == 1 ? std::sqrt (cl * (1.0 - x)) : cl;
    const double r = law == 2 ? x       : law == 1 ? std::sqrt (cr * x)         : cr;
    gainL = (float) l;
    gainR = (float) r;
}

/** Pan position for a source at `p`, from its direction as seen from the main array:
    straight ahead is centre (0), 45 degrees to one side or more is fully that side (-1 or +1). */
inline double naturalPan (const ArrayGeometry& g, Vec2 p)
{
    const Vec2 centre { 0.5 * (g.pos[0].x + g.pos[2].x), 0.5 * (g.pos[0].y + g.pos[2].y) };
    const double dx = p.x - centre.x, dy = p.y - centre.y;
    const double along = dx * g.forward.x + dy * g.forward.y, across = dx * g.right.x + dy * g.right.y;
    if (std::hypot (along, across) < 1.0e-6)
        return 0.0;
    return std::clamp (std::atan2 (across, std::max (along, 0.0)) / (kPi / 4.0), -1.0, 1.0);
}
}
