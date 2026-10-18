// Support Zones / Stumps - ported from OrcaFS-NeotkoCM (Neotko). See SupportZoneEngine.hpp.

#include "SupportZoneEngine.hpp"
#include "SupportZoneProbe.hpp"

#include "../../BoundingBox.hpp"
#include "../../ClipperUtils.hpp"
#include "../../Layer.hpp"
#include "../../Model.hpp"
#include "../../Print.hpp"
#include "../../TriangleMesh.hpp"
#include "../../TriangleMeshSlicer.hpp"

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <string>

namespace Slic3r {
namespace SupportZones {

// How many layers above the bottom of a zone still count as "the landing" for land_only zones.
static constexpr int ZONE_LANDING_LAYERS = 3;

static bool opt_bool(const ModelVolume &v, const char *key)
{
    const auto *o = dynamic_cast<const ConfigOptionBool*>(v.config.option(key));
    return o != nullptr && o->value;
}

bool is_zone_volume(const ModelVolume &v)
{
    return v.type() == ModelVolumeType::SUPPORT_ENFORCER && v.config.has("neotko_zone_lean_deg");
}

bool is_cut_volume(const ModelVolume &v)
{
    return v.type() == ModelVolumeType::SUPPORT_BLOCKER && opt_bool(v, "neotko_support_cut");
}

bool object_has_zones(const ModelObject &mo)
{
    for (const ModelVolume *v : mo.volumes)
        if (is_zone_volume(*v))
            return true;
    return false;
}

// Slices one volume at the object layers, in the frame the layers were sliced in (same as
// PrintObject::slice_support_volumes()).
static std::vector<ExPolygons> slice_one_volume(const PrintObject &object, const ModelVolume &volume, const std::vector<float> &zs)
{
    std::vector<ExPolygons> layers;
    if (zs.empty())
        return layers;
    indexed_triangle_set its = volume.mesh().its;
    if (its.indices.empty())
        return layers;
    MeshSlicingParamsEx params;
    params.trafo = object.trafo_sliced() * volume.get_matrix();
    if (params.trafo.rotation().determinant() < 0.)
        its_flip_triangles(its);
    const Print *print = object.print();
    auto throw_if_canceled = [print]() { if (print != nullptr && print->canceled()) throw CanceledException(); };
    layers = slice_mesh_ex(its, zs, params, throw_if_canceled);
    throw_if_canceled();
    return layers;
}

// Area weighted centroid of a polygon set, in scaled coordinates.
static Vec2d centroid_of(const Polygons &polys)
{
    double area_total = 0.;
    Vec2d  acc        = Vec2d::Zero();
    for (const Polygon &poly : polys) {
        if (poly.size() < 3)
            continue;
        const double a = std::abs(poly.area());
        if (a <= 0.)
            continue;
        const Point c = poly.centroid();
        acc        += Vec2d(double(c.x()), double(c.y())) * a;
        area_total += a;
    }
    return (area_total > 0.) ? Vec2d(acc / area_total) : Vec2d::Zero();
}

static Point to_point(const Vec2d &v)
{
    return Point(coord_t(std::lround(v.x())), coord_t(std::lround(v.y())));
}

std::vector<ZoneSlices> slice_zones(const PrintObject &object)
{
    std::vector<ZoneSlices> zones;
    const ModelObject *mo = object.model_object();
    if (mo == nullptr || ! object_has_zones(*mo))
        return zones;

    const std::vector<float> zs = zs_from_layers(object.layers());
    size_t priority = 0;
    for (const ModelVolume *v : mo->volumes) {
        if (! is_zone_volume(*v))
            continue;
        std::vector<ExPolygons> slices2 = slice_one_volume(object, *v, zs);
        ZoneSlices zone;
        zone.priority     = priority ++;
        zone.model_volume = v;
        zone.solid        = opt_bool(*v, "neotko_zone_solid");
        zone.land_only    = opt_bool(*v, "neotko_zone_land_only");
        zone.roof_only    = opt_bool(*v, "neotko_zone_roof_only");
        // 0 means "not said": the default lean rules. Resolved here so the engine never sees a 0.
        zone.lean_deg = SUPPORT_ZONE_DEFAULT_LEAN_DEG;
        const double a = v->config.opt_float("neotko_zone_lean_deg");
        if (a > 0.)
            zone.lean_deg = std::min(a, SUPPORT_ZONE_MAX_LEAN_DEG);
        zone.slices.reserve(zs.size());
        for (ExPolygons &src : slices2)
            zone.slices.emplace_back(to_polygons(std::move(src)));
        zone.slices.resize(zs.size(), Polygons());

        // Where the block REALLY ends at the top, computed once. A plain diff against the layer above
        // lies as soon as the block leans (every layer of a skewed prism leaves a rim), so the layer
        // above is translated by the block's own shift between the two layers first.
        const size_t n = zone.slices.size();
        zone.roof_layer.assign(n, char(0));
        for (size_t i = 0; i < n; ++ i) {
            if (zone.slices[i].empty())
                continue;
            if (i + 1 >= n || zone.slices[i + 1].empty()) {
                zone.roof_layer[i] = char(1);
                continue;
            }
            Polygons    above = zone.slices[i + 1];
            const Point shift = to_point(centroid_of(zone.slices[i]) - centroid_of(zone.slices[i + 1]));
            for (Polygon &p : above)
                p.translate(shift);
            const Polygons band    = diff(zone.slices[i], above);
            const double   a_layer = std::abs(area(zone.slices[i]));
            // A tiny residue is the rounding of the translation, not a roof.
            if (! band.empty() && a_layer > 0. && std::abs(area(band)) > 0.02 * a_layer)
                zone.roof_layer[i] = char(1);
        }
        zones.emplace_back(std::move(zone));
    }
    return zones;
}

std::vector<Polygons> slice_cutters(const PrintObject &object)
{
    std::vector<Polygons> out;
    const ModelObject *mo = object.model_object();
    if (mo == nullptr || std::none_of(mo->volumes.begin(), mo->volumes.end(), [](const ModelVolume *v) { return is_cut_volume(*v); }))
        return out;
    const std::vector<float> zs = zs_from_layers(object.layers());
    out.assign(zs.size(), Polygons());
    for (const ModelVolume *v : mo->volumes) {
        if (! is_cut_volume(*v))
            continue;
        std::vector<ExPolygons> sl = slice_one_volume(object, *v, zs);
        for (size_t i = 0; i < sl.size() && i < out.size(); ++ i)
            polygons_append(out[i], to_polygons(std::move(sl[i])));
    }
    for (Polygons &p : out)
        if (! p.empty())
            p = union_(p);
    return out;
}

std::vector<Polygons> roof_only_seeds(const PrintObject &object, std::vector<Polygons> &enforcers_layers, float roof_expand)
{
    std::vector<Polygons> roofs;
    const ModelObject *mo = object.model_object();
    if (mo == nullptr)
        return roofs;
    bool any = false;
    for (const ModelVolume *v : mo->volumes)
        if (is_zone_volume(*v) && opt_bool(*v, "neotko_zone_roof_only")) {
            any = true;
            break;
        }
    if (! any)
        return roofs;

    const std::vector<ZoneSlices> zones = slice_zones(object);
    for (const ZoneSlices &z : zones) {
        if (! z.roof_only)
            continue;
        roofs.resize(std::max(roofs.size(), z.slices.size()), Polygons());
        for (size_t i = 0; i < z.slices.size(); ++ i) {
            if (z.slices[i].empty() || i >= z.roof_layer.size() || z.roof_layer[i] == 0)
                continue;
            Polygons top = (i + 1 < z.slices.size() && ! z.slices[i + 1].empty()) ? diff(z.slices[i], z.slices[i + 1]) : z.slices[i];
            if (top.empty())
                continue;
            // Half an extrusion wider: a perfectly vertical roof would otherwise give a zero-area band.
            top = expand(top, roof_expand, jtSquare, 0.); // = SUPPORT_SURFACES_OFFSET_PARAMETERS
            polygons_append(roofs[i], std::move(top));
        }
        // The zone stops seeding the classic way along its whole height.
        for (size_t i = 0; i < enforcers_layers.size() && i < z.slices.size(); ++ i)
            if (! enforcers_layers[i].empty() && ! z.slices[i].empty())
                enforcers_layers[i] = diff(enforcers_layers[i], z.slices[i]);
    }
    return roofs;
}

// The plane split between stumps: each stump keeps the points closer to it than to any other (its
// Voronoi cell). A ring painted around a hole is ONE island; split by islands it would go entirely
// to one side, split by the plane it goes half to each stump.
static Polygons stump_cell(const std::vector<Vec2d> &centres, size_t i, const BoundingBox &bb)
{
    Polygons cell { bb.polygon() };
    if (centres.size() < 2)
        return cell;
    const double R = 2. * (Vec2d(double(bb.max.x() - bb.min.x()), double(bb.max.y() - bb.min.y())).norm() + 1.);
    for (size_t j = 0; j < centres.size() && ! cell.empty(); ++ j) {
        if (j == i)
            continue;
        Vec2d d = centres[i] - centres[j];
        const double dl = d.norm();
        if (dl < 1e-9)
            continue;
        d /= dl;
        const Vec2d m = 0.5 * (centres[i] + centres[j]);
        const Vec2d v(- d.y(), d.x());
        Polygon half({ to_point(m - R * v), to_point(m + R * v), to_point(m + R * v + 2. * R * d), to_point(m - R * v + 2. * R * d) });
        if (! half.is_counter_clockwise())
            half.reverse();
        cell = intersection(cell, Polygons{ half });
    }
    return cell;
}

struct CorridorShift
{
    ExPolygon above;  // the corridor island at layer i+1
    Vec2d     v;      // centroid(partner at layer i) - centroid(above)
};

// Pairs the islands of a forked corridor: partner = the island below with the largest overlap, or
// the nearest centroid when nothing overlaps. Ties break by index (union_ex order).
static std::vector<CorridorShift> pair_components(const Polygons &corridor_above, const Polygons &corridor_here)
{
    std::vector<CorridorShift> out;
    const ExPolygons comps_above = union_ex(corridor_above);
    const ExPolygons comps_here  = union_ex(corridor_here);
    if (comps_above.empty() || comps_here.empty())
        return out;
    std::vector<Vec2d> centroid_here(comps_here.size());
    for (size_t j = 0; j < comps_here.size(); ++ j)
        centroid_here[j] = centroid_of(to_polygons(comps_here[j]));
    out.reserve(comps_above.size());
    for (const ExPolygon &above : comps_above) {
        const Polygons above_polys = to_polygons(above);
        const Vec2d    c_above     = centroid_of(above_polys);
        size_t         best        = size_t(-1);
        double         best_score  = 0.;
        for (size_t j = 0; j < comps_here.size(); ++ j) {
            const double a = area(intersection(above_polys, to_polygons(comps_here[j])));
            if (a > best_score) {
                best_score = a;
                best       = j;
            }
        }
        if (best == size_t(-1)) {
            double best_dist = std::numeric_limits<double>::max();
            for (size_t j = 0; j < comps_here.size(); ++ j) {
                const double d = (centroid_here[j] - c_above).squaredNorm();
                if (d < best_dist) {
                    best_dist = d;
                    best      = j;
                }
            }
        }
        out.push_back({ above, centroid_here[best] - c_above });
    }
    return out;
}

ZoneDescent::ZoneDescent(const PrintObject &object, double support_line_width_mm, double support_closing_radius_mm,
                         double gap_xy_mm, bool buildplate_only)
    : m_buildplate_only(buildplate_only), m_gap_xy_mm(gap_xy_mm)
{
    std::vector<ZoneSlices> zones = slice_zones(object);
    if (zones.empty())
        return;

    // Zones with the same recipe (same config, editor keys ignored) are one family: they merge
    // instead of partitioning the shared band. Different recipes stay exclusive.
    static const std::set<std::string> editor_keys {
        "neotko_support_zone_gesture", "neotko_zone_lean_deg", "neotko_zone_roof_only",
        "neotko_zone_solid", "neotko_zone_land_only" };
    auto same_recipe = [](const ModelVolume *a, const ModelVolume *b) {
        if (a == nullptr || b == nullptr)
            return false;
        for (const t_config_option_key &k : a->config.get().diff(b->config.get()))
            if (editor_keys.find(k) == editor_keys.end())
                return false;
        return true;
    };

    m_zones.resize(zones.size());
    for (size_t zi = 0; zi < zones.size(); ++ zi) {
        Zone &z = m_zones[zi];
        z.s = std::move(zones[zi]);
        bool found = false;
        for (size_t j = 0; j < zi; ++ j)
            if (same_recipe(z.s.model_volume, m_zones[j].s.model_volume)) {
                z.family = m_zones[j].family;
                found    = true;
                break;
            }
        if (! found)
            z.family = m_n_families ++;

        const std::vector<Polygons> &zs = z.s.slices;
        for (size_t i = 0; i < zs.size(); ++ i)
            if (! zs[i].empty()) {
                z.bottom_layer = int(i);
                break;
            }
        for (int i = int(zs.size()) - 1; i >= 0; -- i)
            if (! zs[i].empty()) {
                z.top_layer = i;
                break;
            }
        if (z.bottom_layer < 0 || z.top_layer <= z.bottom_layer)
            continue;
        // The bottom band, in one piece: from the lowest layer with block up to the first empty one.
        int t = z.bottom_layer;
        while (t + 1 < int(zs.size()) && ! zs[t + 1].empty())
            ++ t;
        z.stump_top = t;
        // No gap = the classic lofted pillar, untouched. A gap = a block tree (head + stumps): the
        // mode is recognised by the geometry, no extra key.
        if (t >= z.top_layer)
            continue;
        z.guided = true;
        for (const ExPolygon &isl : union_ex(zs[z.bottom_layer])) {
            Stump st;
            st.poly = to_polygons(isl);
            st.c    = centroid_of(st.poly);
            z.stumps.emplace_back(std::move(st));
        }
    }

    // The regulariser grows the general copy of a zone contact by up to the closing radius; that halo
    // belongs to the zone, or it falls straight down as a thin ghost column next to the ramp.
    m_zone_halo      = float(scaled<double>(support_closing_radius_mm) + scaled<double>(support_line_width_mm));
    // Two extrusions wide, said as a radius: below that there is no column to print.
    m_min_half_width = scaled<double>(support_line_width_mm);

    BOOST_LOG_TRIVIAL(debug) << "[SupportZones] object=" << (object.model_object() ? object.model_object()->name : std::string("?"))
                             << " zones=" << m_zones.size() << " families=" << m_n_families;
}

bool ZoneDescent::seeds_section(const Zone &z, size_t layer_id) const
{
    if (layer_id >= z.s.slices.size() || z.s.slices[layer_id].empty())
        return false;
    return z.s.solid || (z.s.roof_only && layer_id < z.s.roof_layer.size() && z.s.roof_layer[layer_id] != 0);
}

void ZoneDescent::descend(int layer_id, const Layer &layer, double dz_mm, Polygons &overhangs_projection, Polygons &enforcers_projection)
{
    (void)layer;
    for (Zone &z : m_zones) {
        Polygons &projection = z.projection;
        if (projection.empty())
            continue;
        const std::vector<Polygons> &zslices = z.s.slices;
        if (size_t(layer_id) + 1 >= zslices.size())
            continue;
        const Polygons &corridor_here  = zslices[layer_id];
        const Polygons &corridor_above = zslices[layer_id + 1];
        // The cap of THIS layer and THIS zone, from the real dz of the step.
        const double corridor_d_max = scaled<double>(corridor_d_max_mm(dz_mm, z.s.lean_deg));

        if (corridor_here.empty()) {
            // Between the head and the stumps of a block tree the column is steered by the stumps.
            // Otherwise the block ended: Straighten, the column keeps descending vertically.
            if (! z.guided || z.stumps.empty() || layer_id <= z.bottom_layer)
                continue;
            std::vector<Vec2d> centres;
            centres.reserve(z.stumps.size());
            for (const Stump &st : z.stumps)
                centres.push_back(st.c);
            BoundingBox bb = get_extents(projection);
            for (const Vec2d &c : centres)
                bb.merge(to_point(c));
            bb.offset(scaled<coord_t>(10.));

            Polygons moved;
            for (size_t si = 0; si < centres.size(); ++ si) {
                Polygons part = (centres.size() == 1) ? projection : intersection(projection, stump_cell(centres, si, bb));
                if (part.empty())
                    continue;
                // The shift is measured from the centroid of THIS piece, the half going to this stump.
                const Vec2d  c     = centroid_of(part);
                Vec2d        v     = centres[si] - c;
                const double v_raw = v.norm();
                if (v_raw > corridor_d_max)
                    v *= corridor_d_max / v_raw;
                const Point tr = to_point(v);
                if (tr != Point(0, 0))
                    for (Polygon &poly : part)
                        poly.translate(tr);
                // The same budget is shared: first spent on reaching the stump, what is left on
                // closing onto it, so the taper comes out at the lean that was asked for.
                const double d_shrink = corridor_d_max - std::min(v_raw, corridor_d_max);
                if (d_shrink > SCALED_EPSILON) {
                    const Polygons &stump = z.stumps[si].poly;
                    Polygons        s     = offset(part, - float(d_shrink));
                    // Once the column stands on its stump, the stump is given back every layer: the
                    // cone converges to the stump shape and stays there.
                    if (! intersection(part, stump).empty())
                        s = union_(s, stump);
                    // Never below two extrusions: an area that cannot converge stops narrowing.
                    if (! s.empty() && ! offset(s, - float(m_min_half_width)).empty())
                        part = std::move(s);
                }
                polygons_append(moved, std::move(part));
            }
            projection = moved.empty() ? Polygons() : union_(moved);
            continue;
        }

        if (z.guided && layer_id == z.stump_top && ! z.stumps.empty()) {
            // At the top of the stumps everything that does not stand on a stump goes to the general
            // stream and falls plumb, trimmed by the object layer by layer like any overhang.
            Polygons all_stumps;
            for (const Stump &st : z.stumps)
                polygons_append(all_stumps, st.poly);
            const Polygons overflow = diff(projection, all_stumps);
            if (! overflow.empty()) {
                BOOST_LOG_TRIVIAL(debug) << "[SupportZones] layer=" << layer_id << " zone=" << z.s.priority
                                         << " area outside the stumps falls plumb: "
                                         << unscale<double>(unscale<double>(area(overflow))) << " mm2";
                projection = intersection(projection, all_stumps);
                if (m_buildplate_only)
                    polygons_append(enforcers_projection, overflow);
                polygons_append(overhangs_projection, overflow);
            }
        }
        if (z.guided && layer_id < z.stump_top) {
            // Below the top of the stumps, the same, layer by layer.
            const Polygons overflow = diff(projection, corridor_here);
            if (! overflow.empty()) {
                projection = intersection(projection, corridor_here);
                if (m_buildplate_only)
                    polygons_append(enforcers_projection, overflow);
                polygons_append(overhangs_projection, overflow);
            }
        }
        if (corridor_above.empty())
            continue;

        const ExPolygons comps_above = union_ex(corridor_above);
        const ExPolygons comps_here  = union_ex(corridor_here);
        if (comps_above.size() == 1 && comps_here.size() == 1) {
            // Single island: translate the whole column by the corridor's centroid shift, capped.
            Vec2d        v     = centroid_of(corridor_here) - centroid_of(corridor_above);
            const double v_raw = v.norm();
            if (v_raw > corridor_d_max)
                v *= corridor_d_max / v_raw;
            const Point tr = to_point(v);
            if (tr != Point(0, 0))
                for (Polygon &poly : projection)
                    poly.translate(tr);
            // The intersection is the safety net, not the engine.
            projection = intersection(projection, corridor_here);
        } else {
            // Forked corridor: one delta per island, applied to the piece of the column inside it.
            const std::vector<CorridorShift> shifts = pair_components(corridor_above, corridor_here);
            Polygons rest = std::move(projection);
            Polygons shifted;
            for (const CorridorShift &sh : shifts) {
                if (rest.empty())
                    break;
                const Polygons island = to_polygons(sh.above);
                Polygons       part   = intersection(rest, island);
                if (part.empty())
                    continue;
                rest = diff(rest, island);
                Vec2d        v     = sh.v;
                const double v_raw = v.norm();
                if (v_raw > corridor_d_max)
                    v *= corridor_d_max / v_raw;
                const Point tr = to_point(v);
                if (tr != Point(0, 0))
                    for (Polygon &poly : part)
                        poly.translate(tr);
                polygons_append(shifted, std::move(part));
            }
            projection = shifted.empty() ? Polygons() : intersection(union_(shifted), corridor_here);
        }
    }
}

void ZoneDescent::route_contacts(size_t idx_src, Polygons &polygons_new, Polygons &enforcers_new)
{
    if (m_zones.empty() || enforcers_new.empty() || idx_src == size_t(-1))
        return;
    Polygons rest = std::move(enforcers_new);
    enforcers_new.clear();
    size_t best_zone = size_t(-1);
    double best_area = 0.;

    // Walk by family: inside a family every zone sees the same `rest` (siblings may share a band, their
    // columns merge later); when a family closes, what it claimed is removed from `rest` at once.
    std::vector<size_t> order(m_zones.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) { return m_zones[a].family < m_zones[b].family; });

    size_t   cur_family = size_t(-1);
    Polygons rest_family;
    Polygons claimed_family;
    for (size_t oi = 0; oi <= order.size(); ++ oi) {
        if (oi == order.size() || (cur_family != size_t(-1) && m_zones[order[oi]].family != cur_family)) {
            if (! claimed_family.empty())
                rest = diff(rest, claimed_family);
            claimed_family.clear();
            if (oi == order.size())
                break;
        }
        const size_t zi = order[oi];
        Zone        &z  = m_zones[zi];
        if (z.family != cur_family) {
            cur_family  = z.family;
            rest_family = rest;
        }
        const std::vector<Polygons> &zslices = z.s.slices;
        if (idx_src >= zslices.size() || zslices[idx_src].empty())
            continue;
        Polygons part;
        if (! rest_family.empty()) {
            part = intersection(rest_family, zslices[idx_src]);
            if (! part.empty())
                polygons_append(claimed_family, part);
        }
        if (! part.empty()) {
            const double a = area(part);
            if (a > best_area) {
                best_area = a;
                best_zone = zi;
            }
        }
        if (! polygons_new.empty()) {
            // The halo of this zone stops at the EXCLUSIVE footprint of a zone of another family.
            Polygons halo = expand(part.empty() ? zslices[idx_src] : part, m_zone_halo);
            for (size_t oj = 0; oj < m_zones.size() && ! halo.empty(); ++ oj) {
                if (oj == zi || m_zones[oj].family == z.family)
                    continue;
                const std::vector<Polygons> &oslices = m_zones[oj].s.slices;
                if (idx_src < oslices.size() && ! oslices[idx_src].empty())
                    halo = diff(halo, diff(oslices[idx_src], zslices[idx_src]));
            }
            if (! halo.empty()) {
                Polygons ghost = intersection(polygons_new, halo);
                if (! ghost.empty()) {
                    polygons_new = diff(polygons_new, halo);
                    polygons_append(part, std::move(ghost));
                }
            }
        }
        if (! part.empty())
            polygons_append(z.projection, std::move(part));
    }
    if (! rest.empty()) {
        if (best_zone != size_t(-1))
            // A fringe outside every block belongs to the zone that claimed the most of this contact.
            polygons_append(m_zones[best_zone].projection, std::move(rest));
        else
            // No zone claimed anything (a plain enforcer or the painted brush): unchanged behaviour.
            polygons_append(enforcers_new, std::move(rest));
    }
}

bool ZoneDescent::layer_has_work(int layer_id) const
{
    for (const Zone &z : m_zones)
        if (! z.projection.empty() || seeds_section(z, size_t(layer_id)))
            return true;
    return false;
}

void ZoneDescent::begin_layer(int layer_id, const Layer &layer)
{
    for (Zone &z : m_zones) {
        z.projection_raw = z.projection.empty() ? Polygons() : union_(z.projection);
        z.projection.clear();
        // The block IS the column on the layers where it seeds its section (every layer for a solid
        // zone, the roof layers for a roof-only zone). The object is subtracted WITH the support XY
        // gap here: the section arrives raw, and project_support_to_grid() only trims by epsilon.
        if (! seeds_section(z, size_t(layer_id)))
            continue;
        Polygons sect = diff(z.s.slices[layer_id], offset(layer.lslices, float(scale_(m_gap_xy_mm))));
        if (sect.empty())
            continue;
        polygons_append(z.projection_raw, std::move(sect));
        z.projection_raw = union_(z.projection_raw);
    }
}

bool ZoneDescent::has_raw() const
{
    for (const Zone &z : m_zones)
        if (! z.projection_raw.empty())
            return true;
    return false;
}

Polygons ZoneDescent::bottom_contact_source(int layer_id, const Polygons &base) const
{
    // The zone contacts left the general stream, so the bottom contact detector sees the union: the
    // split is about where the column descends, not about what it may land on. A land_only zone only
    // lands in its last layers, never on the shelves it crosses.
    Polygons out = base;
    for (const Zone &z : m_zones) {
        if (z.projection_raw.empty())
            continue;
        if (z.s.land_only && z.bottom_layer >= 0 && layer_id > z.bottom_layer + ZONE_LANDING_LAYERS)
            continue;
        polygons_append(out, z.projection_raw);
    }
    return union_(out);
}

void ZoneDescent::project_and_merge(int layer_id, const Layer &layer, const ProjectFn &project, Polygons &layer_support_area)
{
    (void)layer_id;
    std::vector<Polygons> zones_layer_area(m_zones.size(), Polygons());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_zones.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t zi = range.begin(); zi < range.end(); ++ zi) {
            Zone &z = m_zones[zi];
            if (z.projection_raw.empty())
                continue;
            std::tie(zones_layer_area[zi], z.projection) = project(layer, z.projection_raw);
            // The carry can never be wider than what fed it: the grid quantisation rounds outward on
            // every layer, and a corridor that shifts the column by a fraction of a cell would ratchet
            // it wider and wider. Only the carry is clipped; the printed area keeps its grid shape.
            if (! z.projection.empty())
                z.projection = intersection(z.projection, z.projection_raw);
        }
    });

    // Zones of different recipes get out of each other's way; only the PRINTED area is clipped,
    // never the carry, so no column erodes its neighbour all the way down.
    std::vector<Polygons> claimed_by_family(m_n_families, Polygons());
    for (size_t zi = 0; zi < m_zones.size(); ++ zi) {
        Polygons &za = zones_layer_area[zi];
        if (za.empty())
            continue;
        const size_t fam = m_zones[zi].family;
        Polygons claimed_so_far;
        for (size_t f = 0; f < m_n_families; ++ f)
            if (f != fam && ! claimed_by_family[f].empty())
                polygons_append(claimed_so_far, claimed_by_family[f]);
        if (! claimed_so_far.empty()) {
            za = diff(za, union_(claimed_so_far));
            if (za.empty())
                continue;
        }
        polygons_append(claimed_by_family[fam], za);
        claimed_by_family[fam] = union_(claimed_by_family[fam]);
        if (layer_support_area.empty())
            layer_support_area = za;
        else
            layer_support_area = union_(layer_support_area, za);
    }
}

} // namespace SupportZones
} // namespace Slic3r
