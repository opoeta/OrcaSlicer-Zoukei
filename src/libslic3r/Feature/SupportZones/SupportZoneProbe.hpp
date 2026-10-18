// Support Zones / Stumps - ported from OrcaFS-NeotkoCM (Neotko), NEOTKO_SUPPORTZONES_TAG s284..s343.
//
// What a support zone is actually going to catch, plus the lean constants shared by the support
// generator (SupportZoneEngine) and the Support Zones gizmo. Both sides must use the same numbers,
// otherwise the editor would draw links the engine cannot follow.
//
// The probe answers "where is there DOWNWARD-FACING surface inside the block", by casting one
// vertical ray per grid cell against the object parts and against the block itself. It is a
// render / warning filter only: what needs support is still decided by detect_overhangs().

#ifndef slic3r_SupportZoneProbe_hpp_
#define slic3r_SupportZoneProbe_hpp_

#include <algorithm>
#include <cmath>
#include <vector>

#include "../../Point.hpp"

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace SupportZones {

// One sampled point of downward-facing object surface. Coordinates are in the frame given by
// `orient` applied to the object-local frame (orientation of the instance, no translation).
struct LitSample
{
    Vec3f pos;
    // Facet normal of the surface that was hit; its Z is always < 0 here.
    Vec3f normal;
};

struct ZoneProbe
{
    std::vector<LitSample> lit;
    float  grid_step_mm { 0.f };
    size_t cells_inside_zone { 0 };
    // A zone that catches no downward-facing surface will not produce a single gram of support.
    bool sterile() const { return lit.empty(); }
};

// Probes one enforcer volume against the object's model parts. `grid_step_mm` is meant to be
// support_base_pattern_spacing; it is clamped against the zone size. Builds an AABB tree per call:
// callers that ask per frame must cache.
ZoneProbe probe_zone(const ModelObject &object, const ModelVolume &enforcer, float grid_step_mm,
                     const Transform3d &orient = Transform3d::Identity());

// The same question over the whole object: downward-facing surface already caught by a zone
// (covered) and surface past `max_normal_z` that no zone catches (uncovered). Render only.
struct CoverageProbe
{
    std::vector<LitSample> covered;
    std::vector<LitSample> uncovered;
    float  grid_step_mm { 0.f };
    size_t cells { 0 };
};

CoverageProbe probe_object_coverage(const ModelObject &object, float grid_step_mm, float max_normal_z,
                                    const Transform3d &orient = Transform3d::Identity());

// Every SUPPORT_ENFORCER volume of the object that catches nothing, in list order.
std::vector<const ModelVolume*> sterile_zones(const ModelObject &object, float grid_step_mm,
                                              const Transform3d &orient = Transform3d::Identity());

// Physical sanity cap of the lean: below k * line width of overlap a layer rests on too little of
// the one below. The editor shows it as a recommendation; it does not block anything.
constexpr double SUPPORT_CORRIDOR_K = 0.75;

// The lean a zone is born with when nobody says otherwise, in degrees.
constexpr double SUPPORT_ZONE_DEFAULT_LEAN_DEG = 45.;

// Hard cap of the lean slider.
constexpr double SUPPORT_ZONE_MAX_LEAN_DEG = 75.;

// How far a column may move sideways in ONE layer, in mm: dz * tan(lean). `dz` must be the REAL
// print_z difference between the two layers, never the nominal layer height (adaptive layers).
inline double corridor_d_max_mm(double dz_mm, double lean_deg)
{
    if (dz_mm <= 0.)
        return 0.;
    const double a = std::clamp(lean_deg, 0., SUPPORT_ZONE_MAX_LEAN_DEG);
    constexpr double deg2rad = 0.01745329251994329576923690768489;
    return dz_mm * std::tan(a * deg2rad);
}

// The recommended per-layer step, k * line width.
inline double corridor_step_mm(double support_line_width_mm)
{
    return SUPPORT_CORRIDOR_K * support_line_width_mm;
}

// The recommended maximum lean for a layer height, in degrees. Under adaptive layer height call it
// with the THICKEST layer: the cap is per layer, so the thick layer is the one that rules.
inline double corridor_max_angle_deg(double layer_height_mm, double support_line_width_mm)
{
    if (layer_height_mm <= 0. || support_line_width_mm <= 0.)
        return 0.;
    constexpr double rad2deg = 57.295779513082320876798154814105;
    return std::atan(corridor_step_mm(support_line_width_mm) / layer_height_mm) * rad2deg;
}

// Total lateral reach gained while descending `height_mm`, in mm.
double corridor_reach_mm(double height_mm, double layer_height_mm, double support_line_width_mm);

} // namespace SupportZones
} // namespace Slic3r

#endif // slic3r_SupportZoneProbe_hpp_
