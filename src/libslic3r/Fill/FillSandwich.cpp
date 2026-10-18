// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: the Sandwich sub-layer generator, "FASE 2" of
// Layer::make_fills in NeotkoCM's Fill.cpp, reduced to the top zone in preset (config) mode.
//
// A top surface whose region carries a pass stack is not filled with the layer. Each pass of the
// stack is generated with the same filler and becomes one or more single-filament sub-layers:
//   Solid / None : one sub-layer at the top of the pass band, flow scaled to the band height
//                  (Beer-Lambert stadium model, same as Flow.cpp);
//   ColorStitch  : the lamina is split per fill line by filament (ColorStitch::split_by_tool),
//                  one sub-layer per filament bucket, all on the band plane;
//   PathBlend    : the per-scanline staircase: every fill line of the ramp filament prints at its
//                  own height (floor .. ramp end along the gradient axis, one sub-layer per height)
//                  and, in Full mode, the cap filament fills the rest of the layer at the layer Z.
#include "FillSandwich.hpp"

#include "FillBase.hpp"
#include "../ClipperUtils.hpp"
#include "../ColorStitch.hpp"
#include "../ColorStitchPaint.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../MultiPassSubLayer.hpp"
#include "../NeoTowerZ.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

// Visit every ExtrusionPath, also inside multi-paths and loops (Arachne based patterns).
void walk_paths(ExtrusionEntity *e, const std::function<void(ExtrusionPath &)> &visit)
{
    if (e == nullptr)
        return;
    if (auto *coll = dynamic_cast<ExtrusionEntityCollection *>(e)) {
        for (ExtrusionEntity *c : coll->entities)
            walk_paths(c, visit);
    } else if (auto *p = dynamic_cast<ExtrusionPath *>(e))
        visit(*p);
    else if (auto *mp = dynamic_cast<ExtrusionMultiPath *>(e)) {
        for (ExtrusionPath &sp : mp->paths)
            visit(sp);
    } else if (auto *loop = dynamic_cast<ExtrusionLoop *>(e)) {
        for (ExtrusionPath &sp : loop->paths)
            visit(sp);
    }
}

void walk_paths_const(const ExtrusionEntity *e, const std::function<void(const ExtrusionPath &)> &visit)
{
    if (e == nullptr)
        return;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        for (const ExtrusionEntity *c : coll->entities)
            walk_paths_const(c, visit);
    } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e))
        visit(*p);
    else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(e)) {
        for (const ExtrusionPath &sp : mp->paths)
            visit(sp);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(e)) {
        for (const ExtrusionPath &sp : loop->paths)
            visit(sp);
    }
}

// Scale every path of `coll` to a band of `ratio` x its height. Beer-Lambert stadium cross
// section A(W,H) = H * (W - H * (1 - pi/4)), the model Flow.cpp uses.
void scale_to_band(ExtrusionEntityCollection &coll, double ratio)
{
    const double k_mp = 1.0 - 0.25 * M_PI;
    for (ExtrusionEntity *e : coll.entities)
        walk_paths(e, [ratio, k_mp](ExtrusionPath &path) {
            const double W      = path.width;
            const double H      = path.height;
            const double H_sub  = H * ratio;
            const double A_orig = H * (W - k_mp * H);
            const double A_sub  = H_sub * (W - k_mp * H_sub);
            if (A_orig > 1e-9 && W > H + 1e-6)
                path.mm3_per_mm = path.mm3_per_mm * (A_sub / A_orig);
            else
                path.mm3_per_mm = path.mm3_per_mm * ratio;
            path.height = float(H_sub);
        });
}

// The zone a piece of a SurfaceFill can carry, false for everything else (sparse infill, bridges, ...).
bool sandwich_zone_of(const Surface &surface, const FillParams &params, SurfaceZone &zone)
{
    if (surface.is_bridge())
        return false;
    switch (params.extrusion_role) {
    case erTopSolidInfill: zone = SurfaceZone::Top; return true;
    case erBottomSurface:  zone = SurfaceZone::Bottom; return surface.is_bottom();
    case erSolidInfill:    zone = SurfaceZone::Penultimate; return surface.is_solid_infill();
    default:               return false;
    }
}

// Penultimate zone: the part of the layer under a visible top surface, i.e. the sub-top islands (the
// internal solid fill that carries a top, see PrintObject::discover_sub_top_surfaces) that the top
// surfaces of the layer above cover, grown by `grow` so the zone reaches the edge of that top.
// Reads fill_surfaces of the layer above, which make_fills never modifies.
ExPolygons penultimate_area(const Layer &layer, coord_t grow)
{
    const Layer *upper = layer.upper_layer;
    if (upper == nullptr)
        return ExPolygons();
    ExPolygons tops;
    for (const LayerRegion *lr : upper->regions())
        for (const Surface &s : lr->fill_surfaces.surfaces)
            if (s.surface_type == stTop)
                tops.push_back(s.expolygon);
    if (tops.empty())
        return ExPolygons();
    ExPolygons sub;
    for (const LayerRegion *lr : layer.regions())
        for (const Surface &s : lr->fill_surfaces.surfaces)
            if (s.surface_type == stSubTop)
                sub.push_back(s.expolygon);
    if (sub.empty())
        return ExPolygons();
    return intersection_ex(sub, offset_ex(union_ex(tops), float(grow)));
}

} // namespace

bool sandwich_split_painted(const Layer &layer, const Surface &surface, const FillParams &params, ExPolygons &expolygons,
                            std::vector<int> &recipe_of)
{
    const PrintObject *po = layer.object();
    if (po == nullptr || expolygons.empty())
        return false;
    SurfaceZone zone = SurfaceZone::Top;
    if (!sandwich_zone_of(surface, params, zone))
        return false;
    // As the Sandwich: never on the first object layer, except the Bottom zone (the face on the bed).
    if (zone != SurfaceZone::Bottom && (po->layers().empty() || po->layers().front() == &layer))
        return false;

    const ColorStitchPaintIndex *index = po->colorstitch_paint();
    // Penultimate: the object's own settings of the zone only apply under a top, so they need the cut too.
    const bool object_penu = zone == SurfaceZone::Penultimate && SurfacePassStack::config_has_zone(po->config(), zone);
    if (index == nullptr && !object_penu)
        return false;

    // Z band, slicing frame, in which the painted faces of the zone are looked for.
    //  Top         : upward facets that top out in the slab of this layer.
    //  Penultimate : the same facets, in the slab of the layer ABOVE (the top that this layer carries).
    //  Bottom      : downward facets that start between the slice plane of the layer below and this one.
    double z_lo = 0., z_hi = 0.;
    if (zone == SurfaceZone::Top) {
        z_lo = layer.slice_z - 0.5 * layer.height;
        z_hi = layer.slice_z + 0.5 * layer.height;
    } else if (zone == SurfaceZone::Penultimate) {
        if (layer.upper_layer == nullptr)
            return false;
        z_lo = layer.upper_layer->slice_z - 0.5 * layer.upper_layer->height;
        z_hi = layer.upper_layer->slice_z + 0.5 * layer.upper_layer->height;
    } else {
        z_hi = layer.slice_z;
        z_lo = layer.lower_layer != nullptr ? layer.lower_layer->slice_z : layer.slice_z - layer.height;
    }
    std::vector<std::pair<int, ExPolygons>> prints;
    if (index != nullptr)
        prints = index->footprints(z_lo, z_hi, zone);
    if (prints.empty() && !object_penu)
        return false;

    // Pieces thinner than about two lines are not worth a recipe: they stay with the rest.
    const double spacing  = std::max(0.1, double(params.flow.spacing()));
    const double min_area = 4.0 * scale_(spacing) * scale_(spacing);
    ExPolygons   under;
    if (zone == SurfaceZone::Penultimate) {
        under = penultimate_area(layer, coord_t(scale_(0.5 * spacing)));
        if (under.empty())
            return false;
    }
    ExPolygons       rest = expolygons;
    ExPolygons       out;
    std::vector<int> recipes;
    for (const std::pair<int, ExPolygons> &fp : prints) {
        if (rest.empty())
            break;
        ExPolygons piece = intersection_ex(rest, fp.second);
        if (zone == SurfaceZone::Penultimate && !piece.empty())
            piece = intersection_ex(piece, under);
        piece.erase(std::remove_if(piece.begin(), piece.end(), [min_area](const ExPolygon &e) { return e.area() < min_area; }), piece.end());
        if (piece.empty())
            continue;
        rest = diff_ex(rest, piece);
        for (ExPolygon &e : piece) {
            out.push_back(std::move(e));
            recipes.push_back(fp.first);
        }
    }
    // The part of a penultimate surface that is under a top uses the object's own zone settings; the
    // rest (and a zone the object does not set) is plain solid infill.
    ExPolygons plain;
    if (zone == SurfaceZone::Penultimate && !rest.empty()) {
        ExPolygons mine;
        if (object_penu) {
            mine = intersection_ex(rest, under);
            mine.erase(std::remove_if(mine.begin(), mine.end(), [min_area](const ExPolygon &e) { return e.area() < min_area; }), mine.end());
        }
        plain = mine.empty() ? rest : diff_ex(rest, mine);
        rest  = std::move(mine);
    }
    if (out.empty() && rest.empty())
        return false;
    const double sliver = 0.25 * scale_(spacing) * scale_(spacing);
    for (ExPolygon &e : rest)
        if (e.area() >= sliver) {
            out.push_back(std::move(e));
            recipes.push_back(0);
        }
    for (ExPolygon &e : plain)
        if (e.area() >= sliver) {
            out.push_back(std::move(e));
            recipes.push_back(-1);
        }
    expolygons = std::move(out);
    recipe_of  = std::move(recipes);
    return true;
}

bool sandwich_fill_surface(Layer &layer, const LayerRegion &layerm, Fill &f, const FillParams &params, const Surface &surface,
                           int painted_profile)
{
    const PrintObject *po = layer.object();
    if (po == nullptr || painted_profile < 0)
        return false;
    // ColorStitch Painter: a painted piece uses its recipe (config with the recipe keys + the stack of the zone).
    const ColorStitchPaintIndex::Recipe *painted = nullptr;
    if (painted_profile > 0) {
        painted = po->colorstitch_paint() != nullptr ? po->colorstitch_paint()->recipe(painted_profile) : nullptr;
        if (painted == nullptr)
            return false;
    }
    // The Sandwich keys are per object (PrintObjectConfig). Cheap gates first: with the feature
    // off nothing below runs.
    const PrintObjectConfig &cfg = painted != nullptr ? painted->config : po->config();
    SurfaceZone              zone = SurfaceZone::Top;
    if (!sandwich_zone_of(surface, params, zone))
        return false;
    if (painted == nullptr && !SurfacePassStack::config_has_zone(cfg, zone))
        return false;
    // NeotkoCM: never on the first object layer (no tool changes on layer 0), except the Bottom zone,
    // whose whole point is the face on the bed.
    if (zone != SurfaceZone::Bottom && (po->layers().empty() || po->layers().front() == &layer))
        return false;

    SurfacePassStack stack = painted != nullptr ? painted->stack(zone) : SurfacePassStack::resolve_zone(cfg, zone);
    if (!stack.any_effect())
        return false;

    const double H        = layer.height;
    const double bottom_z = layer.bottom_z();
    if (H <= 0.0)
        return false;

    // Filaments: 0-based; anything out of range falls back to the region's top surface filament.
    const int n_filaments  = int(po->print()->config().filament_diameter.values.size());
    const PrintRegionConfig &region_cfg = layerm.region().config();
    const int natural_raw = zone == SurfaceZone::Top ? int(layerm.region().extruder(frTopSolidInfill)) :
                            zone == SurfaceZone::Bottom ? int(region_cfg.bottom_surface_filament_id.value) :
                                                          int(layerm.region().extruder(frSolidInfill));
    const int natural_tool = std::clamp(natural_raw - 1, 0, std::max(0, n_filaments - 1));
    auto      valid_tool   = [n_filaments](int t) { return t >= 0 && t < n_filaments; };

    // PathBlend is always a single full height gradient: when present it is the whole stack. Not in the
    // Bottom zone: its ramp starts above the bed, so the first beads would print in the air.
    std::vector<SurfacePass> passes;
    if (zone != SurfaceZone::Bottom)
        for (const SurfacePass &p : stack.passes)
            if (p.kind == SurfacePassKind::PathBlend) {
                passes.assign(1, p);
                passes.front().ratio = 1.0;
                break;
            }
    if (passes.empty())
        for (const SurfacePass &p : stack.passes)
            if (p.kind == SurfacePassKind::None || p.kind == SurfacePassKind::Solid || p.kind == SurfacePassKind::ColorStitch)
                passes.push_back(p);
    if (passes.size() > size_t(SurfacePassStack::kMaxPasses))
        passes.resize(SurfacePassStack::kMaxPasses);
    if (passes.empty())
        return false;
    const int n = int(passes.size());

    // Effective ratios. A band thinner than the printable minimum is dropped and its height is
    // shared among the survivors (NeotkoCM s276: collapse instead of leaving a hole); if none
    // survives the thickest one (the last on ties) takes the whole layer. Unlike NeotkoCM the
    // survivors are always normalized to the full layer height.
    const double     kMinBandHeight = NeoTowerZ::NOMINAL_LH_MIN;
    std::vector<double> eff(size_t(n), 0.0);
    {
        double sum = 0.0;
        int    alive = 0;
        for (int i = 0; i < n; ++i) {
            const double r = std::max(0.0, passes[i].ratio);
            if (passes[i].kind == SurfacePassKind::PathBlend || H * r >= kMinBandHeight) {
                eff[i] = r;
                sum += r;
                ++alive;
            }
        }
        if (alive == 0 || sum <= 1e-9) {
            int best = n - 1;
            double best_r = -1.0;
            for (int i = 0; i < n; ++i)
                if (passes[i].ratio >= best_r) {
                    best_r = passes[i].ratio;
                    best   = i;
                }
            std::fill(eff.begin(), eff.end(), 0.0);
            eff[best] = 1.0;
        } else
            for (double &r : eff)
                r /= sum;
    }
    int last_eff = -1;
    for (int i = 0; i < n; ++i)
        if (eff[i] > 0.0)
            last_eff = i;

    // Filler state to restore.
    const float    base_angle  = f.angle;
    const bool     base_fixed  = f.fixed_angle;
    const coordf_t base_spacing = f.spacing;

    // Disjoint surfaces of the same layer form separate scheduling chains.
    const int island_key = int(layer.sandwich_sublayers.size()) + 1;
    int       global_pass = 0;
    const PrintRegion *region = &layerm.region();

    auto fill_pass = [&](float angle_rad, bool fixed, bool unconnected, ExtrusionEntityCollection &out) {
        f.angle       = angle_rad;
        f.fixed_angle = fixed;
        f.spacing     = base_spacing;
        FillParams p  = params;
        if (unconnected) {
            // One path per fill line: ColorStitch colours lines and PathBlend gives each its own height.
            p.anchor_length     = 0.f;
            p.anchor_length_max = 0.f;
        }
        Surface surf = surface;
        if (fixed)
            surf.bridge_angle = -1.;
        f.fill_surface_extrusion(&surf, p, out.entities);
    };

    auto push_sublayer = [&](coordf_t z, float h, int tool, SurfacePassKind kind, ExtrusionEntityCollection &&fills) {
        if (fills.entities.empty())
            return;
        MultiPassSubLayer sub;
        sub.print_z    = z;
        sub.height     = h;
        sub.tool_id    = tool;
        sub.pass_idx   = global_pass++;
        sub.island_key = island_key;
        sub.effect     = kind;
        sub.zone       = zone;
        sub.region     = region;
        sub.fills      = std::move(fills);
        layer.sandwich_sublayers.push_back(std::move(sub));
    };

    double cumsum = 0.0;
    for (int i = 0; i < n; ++i) {
        const SurfacePass &pass  = passes[i];
        const double       ratio = eff[i];
        if (ratio <= 0.0)
            continue;
        cumsum += ratio;
        // The last surviving band always closes at the layer Z (no rounding gap).
        const coordf_t band_top_z  = (i == last_eff) ? layer.print_z : bottom_z + cumsum * H;
        const float    band_height = float(H * ratio);

        if (pass.kind == SurfacePassKind::PathBlend) {
            PathBlendPassConfig pb = PathBlendPassConfig::from_config(cfg);
            auto blob_it = pass.pathblend.kv.find("blob");
            if (blob_it != pass.pathblend.kv.end() && !blob_it->second.empty())
                pb = PathBlendPassConfig::from_blob_json(blob_it->second);
            const int  tool_ramp = valid_tool(pb.tool_bottom) ? pb.tool_bottom : natural_tool;
            const bool want_cap  = pb.mode == PathBlendPassConfig::Mode::Full && valid_tool(pb.tool_top);
            const int  tool_cap  = want_cap ? pb.tool_top : -1;

            const double min_h   = pb_min_printable_h();
            const double floor_h = std::max(min_h, std::max(0.01, double(pb.floor_mm)));
            const double mid_pref = pb.mid_end_mm < 0.f ? (want_cap ? H - min_h : H) : double(pb.mid_end_mm);
            const double mid_end  = std::min(mid_pref, H - min_h);
            const double range    = mid_end - floor_h;

            const bool  fixed = pb.fill_angle >= 0;
            const float angle = fixed ? float(Geometry::deg2rad(double(pb.fill_angle))) : base_angle;
            ExtrusionEntityCollection lines;
            fill_pass(angle, fixed, true, lines);

            if (range <= 1e-6) {
                // Degenerate ramp (layer too thin): print the colours as flat solid bands.
                if (want_cap) {
                    ExtrusionEntityCollection lower(lines);
                    scale_to_band(lower, 0.5);
                    push_sublayer(bottom_z + 0.5 * H, float(0.5 * H), tool_ramp, SurfacePassKind::Solid, std::move(lower));
                    scale_to_band(lines, 0.5);
                    push_sublayer(layer.print_z, float(0.5 * H), tool_cap, SurfacePassKind::Solid, std::move(lines));
                } else
                    push_sublayer(layer.print_z, float(H), tool_ramp, SurfacePassKind::Solid, std::move(lines));
                continue;
            }

            // Gradient axis: the normal to the fill lines, MEASURED on the generated lines (the
            // filler adds its own rotation, so it cannot be derived from the angle), sign canonized.
            double ux = 0.0, uy = 1.0;
            {
                double s2x = 0.0, s2y = 0.0;
                for (const ExtrusionEntity *e : lines.entities)
                    walk_paths_const(e, [&s2x, &s2y](const ExtrusionPath &p) {
                        const Points3 &pts = p.polyline.points;
                        for (size_t k = 1; k < pts.size(); ++k) {
                            const double dx = double(pts[k].x() - pts[k - 1].x());
                            const double dy = double(pts[k].y() - pts[k - 1].y());
                            const double L  = std::sqrt(dx * dx + dy * dy);
                            if (L <= 0.0)
                                continue;
                            const double th = std::atan2(dy, dx);
                            s2x += L * std::cos(2.0 * th);
                            s2y += L * std::sin(2.0 * th);
                        }
                    });
                if (s2x * s2x + s2y * s2y > 0.0) {
                    const double line_ang = 0.5 * std::atan2(s2y, s2x);
                    ux = -std::sin(line_ang);
                    uy = std::cos(line_ang);
                    if (uy < 0.0 || (std::abs(uy) <= 1e-12 && ux < 0.0)) {
                        ux = -ux;
                        uy = -uy;
                    }
                }
            }
            // Lanes of the real line spacing, numbered between the extremes of the surface contour
            // (fit to surface) or folded on a physical period (span_mm > 0). Both pieces of a line
            // cut by a hole fall in the same lane and get the same height.
            const double sp = std::max(1e-3, double(params.flow.spacing()));
            auto proj_mm = [ux, uy](double x, double y) { return (x * ux + y * uy) * SCALING_FACTOR; };
            auto lane_of = [&](double pmm) -> long long {
                double pos = pmm;
                if (pb.span_mm > 1e-4f) {
                    pos = std::fmod(pos, double(pb.span_mm));
                    if (pos < 0.0)
                        pos += double(pb.span_mm);
                }
                return std::llround(pos / sp);
            };
            long long k0 = 0, k1 = 0;
            {
                bool first = true;
                for (const Point &pt : surface.expolygon.contour.points) {
                    const long long k = lane_of(proj_mm(double(pt.x()), double(pt.y())));
                    if (first) { k0 = k1 = k; first = false; }
                    else { k0 = std::min(k0, k); k1 = std::max(k1, k); }
                }
            }
            auto t_of = [&](const ExtrusionPath &p) -> double {
                if (p.polyline.points.empty())
                    return 0.5;
                double sx = 0.0, sy = 0.0;
                for (const Point3 &pt : p.polyline.points) {
                    sx += double(pt.x());
                    sy += double(pt.y());
                }
                const double cnt = double(p.polyline.points.size());
                const long long k = lane_of(proj_mm(sx / cnt, sy / cnt));
                if (pb.span_mm > 1e-4f)
                    return std::clamp(double(k) / std::max(1.0, double(pb.span_mm) / sp), 0.0, 1.0);
                if (k1 <= k0)
                    return 0.5;
                return std::clamp(double(k - k0) / double(k1 - k0), 0.0, 1.0);
            };

            // RAMP: one height per fill line h = floor + u(t) * range, nozzle at bottom_z + h, flow
            // h / H. Lines at the same height share a sub-layer; ascending t = ascending Z.
            struct ScanPath { double t; double h; ExtrusionPath *p; };
            std::vector<ScanPath> scans;
            ExtrusionEntityCollection cap;
            for (const ExtrusionEntity *e : lines.entities)
                walk_paths_const(e, [&](const ExtrusionPath &p) {
                    const double t      = t_of(p);
                    const double u      = pb.profile_u(t);
                    const double h_ramp = floor_h + u * range;
                    auto *ramp_path = static_cast<ExtrusionPath *>(p.clone());
                    ramp_path->mm3_per_mm = p.mm3_per_mm * (h_ramp / H);
                    ramp_path->height     = float(h_ramp);
                    scans.push_back({ t, h_ramp, ramp_path });
                    // CAP (Full): the same line fills the rest of the layer, H - ramp, so ramp + cap = H
                    // holds line by line. Caps thinner than the printable minimum are not emitted.
                    if (want_cap) {
                        const double h_cap = H - h_ramp;
                        if (h_cap >= min_h) {
                            auto *cap_path = static_cast<ExtrusionPath *>(p.clone());
                            cap_path->mm3_per_mm = p.mm3_per_mm * (h_cap / H);
                            cap_path->height     = float(h_cap);
                            cap.entities.push_back(cap_path);
                        }
                    }
                });
            std::stable_sort(scans.begin(), scans.end(), [](const ScanPath &a, const ScanPath &b) { return a.t < b.t; });
            constexpr double Z_QUANT = 1e-4;
            size_t s = 0;
            while (s < scans.size()) {
                size_t e = s + 1;
                while (e < scans.size() && std::abs(scans[e].h - scans[s].h) < Z_QUANT)
                    ++e;
                ExtrusionEntityCollection group;
                for (size_t k = s; k < e; ++k)
                    group.entities.push_back(scans[k].p);
                push_sublayer(bottom_z + scans[s].h, float(scans[s].h), tool_ramp, SurfacePassKind::PathBlend, std::move(group));
                s = e;
            }
            if (want_cap)
                push_sublayer(layer.print_z, float(H), tool_cap, SurfacePassKind::PathBlend, std::move(cap));
            continue;
        }

        if (pass.kind == SurfacePassKind::ColorStitch) {
            ColorStitchParams cp = ColorStitchParams::from_config(cfg);
            if (pass.colorstitch.present && !pass.colorstitch.kv.empty())
                cp.apply_kv(pass.colorstitch.kv);
            for (int &t : cp.tool)
                if (t >= 0 && !valid_tool(t))
                    t = natural_tool;
            const bool  fixed = cp.angle >= 0;
            const float angle = fixed ? float(Geometry::deg2rad(double(cp.angle))) : base_angle;
            ExtrusionEntityCollection lamina;
            fill_pass(angle, fixed, true, lamina);
            const int fallback = cp.tool[0] >= 0 ? cp.tool[0] : natural_tool;
            // Design anchored at the object origin (fill coordinates are object-local), so the
            // stripes do not move from layer to layer and travel with the part.
            auto buckets = ColorStitch::split_by_tool(lamina, cp, H, layer.id(), 0.0, 0.0, fallback);
            for (auto &b : buckets) {
                if (!valid_tool(b.first))
                    b.first = natural_tool;
                scale_to_band(b.second, ratio);
                push_sublayer(band_top_z, band_height, b.first, SurfacePassKind::ColorStitch, std::move(b.second));
            }
            continue;
        }

        // Solid or None (None = the natural filament, a plain band).
        const bool is_none    = pass.kind == SurfacePassKind::None;
        const int  solid_tool = (!is_none && valid_tool(pass.solid_tool)) ? pass.solid_tool : natural_tool;
        const bool fixed      = !is_none && pass.angle >= 0;
        // Auto angle: consecutive passes cross at 90 degrees for bonding.
        const float angle = fixed ? float(Geometry::deg2rad(double(pass.angle))) : base_angle + float(i % 2) * float(M_PI / 2.);
        ExtrusionEntityCollection band;
        fill_pass(angle, fixed, false, band);
        scale_to_band(band, ratio);
        push_sublayer(band_top_z, band_height, solid_tool, is_none ? SurfacePassKind::None : SurfacePassKind::Solid, std::move(band));
    }

    f.angle       = base_angle;
    f.fixed_angle = base_fixed;
    f.spacing     = base_spacing;
    return true;
}

} // namespace Slic3r
