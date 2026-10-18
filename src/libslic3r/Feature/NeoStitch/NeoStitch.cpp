// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoStitch Interlock. See NeoStitch.hpp.
#include <algorithm>
#include <cmath>

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "NeoStitch.hpp"

using namespace Slic3r;

namespace Slic3r::Feature::NeoStitch {

NeoStitchConfig config_from_object(const PrintObjectConfig &oc)
{
    NeoStitchConfig c;
    c.target         = oc.neostitch.value;
    c.depth_mm       = oc.neostitch_depth.value;
    c.flat_length_mm = oc.neostitch_flat_length.value;
    c.ramp_length_mm = oc.neostitch_ramp_length.value;
    c.period_mm      = oc.neostitch_period.value;
    c.flow_pct       = oc.neostitch_flow.value;
    c.skip_layers    = oc.neostitch_skip_layers.value;
    c.fill_margin_mm = oc.neostitch_fill_margin.value;
    return c;
}

namespace {

// Which inset index the target resolves to for a wall stack. false = no such wall: the wall count is never forced.
bool resolve_target_inset_idx(NeoStitchTarget target, int total_loops, size_t &out_inset_idx)
{
    switch (target) {
    case NeoStitchTarget::Outermost:
        out_inset_idx = 0;
        return true;
    case NeoStitchTarget::SecondWall:
        if (total_loops < 1) return false;
        out_inset_idx = 1;
        return true;
    case NeoStitchTarget::ThirdWall:
        if (total_loops < 2) return false;
        out_inset_idx = 2;
        return true;
    case NeoStitchTarget::Innermost:
        if (total_loops < 1) return false;
        out_inset_idx = size_t(total_loops);
        return true;
    case NeoStitchTarget::Disabled:
    default:
        return false;
    }
}

// Mutual exclusion with fuzzy skin / bump mapping on the SAME wall (a region may fuzz the skin and stitch the
// second wall at once: that is the intended use, not a conflict).
bool neostitch_conflicts(const PerimeterGenerator &pg, size_t inset_idx, bool is_contour)
{
    for (const auto &region : pg.regions_by_fuzzify)
        if (Slic3r::Feature::FuzzySkin::should_fuzzify(region.first, pg.layer_id, inset_idx, is_contour))
            return true;
    return pg.wall_bump_table != nullptr;
}

// The period count is FIXED per object from the perimeter of the bounding rectangle, rounded so the wave closes
// without a seam. It must never be derived per loop or per layer: that breaks the vertical registration.
int compute_num_periods(const BoundingBoxf3 &bounds, double period_mm)
{
    const double approx_perimeter_mm = 2.0 * (bounds.size().x() + bounds.size().y());
    return std::max(4, int(std::lround(approx_perimeter_mm / std::max(period_mm, 1e-3))));
}

// World stable angular coordinate around the object centre, in [0, 1).
double cylindrical_u(const Vec3d &point_mm, const BoundingBoxf3 &bounds)
{
    const Vec3d  center = bounds.center();
    const double theta  = std::atan2(point_mm.y() - center.y(), point_mm.x() - center.x());
    return theta / (2.0 * M_PI) + 0.5;
}

// The notch / fill envelope in [-1, 1]: negative = notch depth fraction, positive = fill fraction, 0 = neutral.
// One period is split in two halves, [0, 0.5) notch and [0.5, 1) fill; odd layers shift it by half a period.
double neostitch_signal(double u_canonical, int num_periods, int layer_parity, const NeoStitchConfig &cfg, double period_mm_eff)
{
    const double u        = u_canonical * double(num_periods) + double(layer_parity) * 0.5;
    const double tau      = u - std::floor(u);
    const bool   is_notch = tau < 0.5;
    const double r        = is_notch ? (tau / 0.5) : ((tau - 0.5) / 0.5);

    const double half_region_mm = period_mm_eff / 2.0;
    // The fill event is shorter than the notch it plugs (fill_margin per side) so the bead is contained in the gap.
    const double notch_event_mm = cfg.flat_length_mm + 2.0 * cfg.ramp_length_mm;
    const double fill_event_mm  = std::max(notch_event_mm - 2.0 * cfg.fill_margin_mm, 0.0);
    const double event_mm       = is_notch ? notch_event_mm : fill_event_mm;
    const double half_event_mm  = event_mm / 2.0;

    const double pos_mm           = r * half_region_mm;
    const double dist_from_center = std::abs(pos_mm - half_region_mm / 2.0);

    double envelope = 0.0;
    if (dist_from_center <= half_event_mm) {
        const double d = half_event_mm - dist_from_center;
        envelope = (d < cfg.ramp_length_mm) ? std::clamp(d / std::max(cfg.ramp_length_mm, 1e-6), 0.0, 1.0) : 1.0;
    }
    return is_notch ? -envelope : envelope;
}

constexpr double kMinExtrusionWidthMm = 0.05;
constexpr double kMaxWidthMultiplier  = 2.0;

// Points are inserted along each segment at a fixed spacing so the ramps have vertices to bend, then displaced
// (notch) or widened (fill) along the segment's own perpendicular.
void neostitch_walk_extrusion_line(Arachne::ExtrusionJunctions &ext_lines, const PerimeterGenerator &pg, const NeoStitchConfig &cfg,
                                   int num_periods, double period_mm_eff, const Polygons &lower_polys)
{
    // Sample spacing in scaled units (NOT mm: raw mm made the inner loop run ~1e6 times per mm of wall).
    const double sample_distance_mm      = std::max(cfg.ramp_length_mm / 4.0, 0.2);
    const double min_dist_between_points = scale_(sample_distance_mm);
    double       dist_left_over          = min_dist_between_points / 2.0;

    const int layer_parity = pg.layer_id % 2;

    auto *p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());

    // The lower-slices support test runs once per notch / fill EVENT (on the neutral -> active transition), not per
    // sample: an unsupported event is forced neutral, which resolves bottom surfaces, overhangs and bridges at once.
    bool prev_sample_active = false;
    bool event_supported    = true;
    for (auto &p1 : ext_lines) {
        if (p0->p == p1.p) {
            // Connect endpoints. p0 is deliberately not advanced here: a duplicate point run collapses onto the
            // real point that last anchored p0.
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            continue;
        }

        Vec2d  p0p1      = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points) {
            Point       pa       = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            const Vec2d perp_dir = perp(p0p1).cast<double>().normalized();
            const Vec3d point_mm(unscale_(pa.x()), unscale_(pa.y()), pg.slice_z);

            const double u = cylindrical_u(point_mm, pg.neostitch_bounds);
            double       s = neostitch_signal(u, num_periods, layer_parity, cfg, period_mm_eff);

            const bool sample_active = (s != 0.0);
            if (sample_active && !prev_sample_active)
                event_supported = Slic3r::contains(lower_polys, pa);
            prev_sample_active = sample_active;
            if (sample_active && !event_supported)
                s = 0.0;

            double  center_shift_mm = 0.0;
            coord_t new_w           = p1.w;
            if (s < 0.0) {
                // Notch: inward displacement (+perp is outward for this winding), width untouched.
                center_shift_mm = s * cfg.depth_mm;
            } else if (s > 0.0) {
                // Fill: the width grows to plug the notch of the SAME physical position one layer below. At full flow
                // the added cross section equals the removed one (width delta = depth). The OUTER edge stays put,
                // all the extra width goes inward.
                const double w_mm               = unscale_(p1.w);
                const double width_delta_mm_raw = cfg.depth_mm * (cfg.flow_pct / 100.0) * s;
                const double max_rad_mm         = w_mm * kMaxWidthMultiplier;
                const double rad_mm             = std::clamp(w_mm + width_delta_mm_raw, kMinExtrusionWidthMm, std::max(max_rad_mm, kMinExtrusionWidthMm));
                center_shift_mm = -(rad_mm - w_mm) / 2.0;
                new_w           = coord_t(scale_(rad_mm));
            }

            out.emplace_back(pa + (perp_dir * scale_(center_shift_mm)).cast<coord_t>(), new_w, p1.perimeter_index);
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
        --point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) {
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }

    if (out.size() >= 3)
        ext_lines = std::move(out);
}

} // anonymous namespace

bool apply_neostitch(Arachne::ExtrusionLine *extrusion, const PerimeterGenerator &perimeter_generator, bool is_contour, int total_loops)
{
    if (perimeter_generator.object_config->neostitch.value == NeoStitchTarget::Disabled)
        return false;
    NeoStitchConfig cfg = config_from_object(*perimeter_generator.object_config);
    if (cfg.depth_mm <= 0.0)
        cfg.depth_mm = double(perimeter_generator.perimeter_flow.width()); // 0 = auto: the inner wall line width

    if (!extrusion->is_closed || extrusion->is_odd)
        return false; // gap fill / odd centre beads are never touched

    size_t target_inset_idx = 0;
    if (!resolve_target_inset_idx(cfg.target, total_loops, target_inset_idx))
        return false;
    if (extrusion->inset_idx != target_inset_idx)
        return false;

    if (neostitch_conflicts(perimeter_generator, extrusion->inset_idx, is_contour))
        return false;

    if (perimeter_generator.lower_slices == nullptr)
        return false; // first layer: never notch straight into the bed
    if (perimeter_generator.layer_id < perimeter_generator.object_config->raft_layers.value + 1 + cfg.skip_layers)
        return false;
    if (perimeter_generator.upper_slices == nullptr)
        return false; // topmost layer: a notch nothing is printed over is never filled

    const BoundingBoxf3 &bounds = perimeter_generator.neostitch_bounds;
    if (!bounds.defined)
        return false;
    const int    num_periods         = compute_num_periods(bounds, cfg.period_mm);
    const double approx_perimeter_mm = 2.0 * (bounds.size().x() + bounds.size().y());
    const double period_mm_eff       = approx_perimeter_mm / double(num_periods);

    // A loop too short for even one full period is skipped (the real loop length, not the rectangle above).
    const double loop_length_mm = unscale_(double(extrusion->getLength()));
    if (loop_length_mm < 2.0 * cfg.period_mm)
        return false;

    // The support test needs the lower slices even when overhang detection (which grows them) is off.
    Polygons lower_polys = perimeter_generator.lower_slices_polygons();
    if (lower_polys.empty())
        lower_polys = to_polygons(*perimeter_generator.lower_slices);
    neostitch_walk_extrusion_line(extrusion->junctions, perimeter_generator, cfg, num_periods, period_mm_eff, lower_polys);
    return true;
}

} // namespace Slic3r::Feature::NeoStitch
