// Support Zones / Stumps - ported from OrcaFS-NeotkoCM (Neotko). See SupportZoneProbe.hpp.

#include "SupportZoneProbe.hpp"

#include "../../AABBTreeIndirect.hpp"
#include "../../Model.hpp"
#include "../../TriangleMesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace SupportZones {

// A facet counts as downward-facing below this normal Z. Render/warning filter only.
static constexpr float DOWNWARD_NORMAL_Z_MAX = -0.02f;

// Guard rails for the grid step of a single block.
static constexpr int   GRID_MIN_CELLS_PER_AXIS = 3;
static constexpr int   GRID_MAX_CELLS_PER_AXIS = 160;
// The coverage map spans the whole object, so it gets a higher ceiling.
static constexpr int   COVERAGE_MAX_CELLS_PER_AXIS = 600;

namespace {

struct MeshAndTree
{
    indexed_triangle_set     its;
    AABBTreeIndirect::Tree3f tree;
    bool empty() const { return its.indices.empty(); }
};

MeshAndTree collect(const ModelObject &object, bool model_parts, const ModelVolume *only, const Transform3d &orient)
{
    MeshAndTree out;
    for (const ModelVolume *v : object.volumes) {
        if (only != nullptr) {
            if (v != only)
                continue;
        } else if (! (model_parts && v->is_model_part()))
            continue;
        indexed_triangle_set part = v->mesh().its;
        if (part.indices.empty())
            continue;
        // The instance orientation goes in front of the volume: the +Z ray and the normal filter
        // must measure the WORLD down, not the object's.
        its_transform(part, orient * v->get_matrix(), true);
        its_merge(out.its, part);
    }
    if (! out.its.indices.empty())
        out.tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(out.its.vertices, out.its.indices);
    return out;
}

Vec3f facet_normal(const indexed_triangle_set &its, int face_id)
{
    const auto  &f = its.indices[face_id];
    const Vec3f &a = its.vertices[f(0)];
    const Vec3f &b = its.vertices[f(1)];
    const Vec3f &c = its.vertices[f(2)];
    const Vec3f  n = (b - a).cross(c - a);
    const float  l = n.norm();
    return (l > 0.f) ? Vec3f(n / l) : Vec3f(0.f, 0.f, 0.f);
}

float clamp_step(float grid_step_mm, float span, int max_cells)
{
    float step = (grid_step_mm > 0.f) ? grid_step_mm : 2.f;
    step = std::min(step, span / float(GRID_MIN_CELLS_PER_AXIS));
    step = std::max(step, span / float(max_cells));
    return step;
}

} // namespace

ZoneProbe probe_zone(const ModelObject &object, const ModelVolume &enforcer, float grid_step_mm, const Transform3d &orient)
{
    ZoneProbe probe;

    const MeshAndTree zone = collect(object, false, &enforcer, orient);
    if (zone.empty())
        return probe;
    const MeshAndTree parts = collect(object, true, nullptr, orient);
    if (parts.empty())
        return probe;

    Vec3f bb_min = zone.its.vertices.front();
    Vec3f bb_max = bb_min;
    for (const Vec3f &v : zone.its.vertices) {
        bb_min = bb_min.cwiseMin(v);
        bb_max = bb_max.cwiseMax(v);
    }
    const Vec3f size = bb_max - bb_min;
    if (size.x() <= 0.f || size.y() <= 0.f)
        return probe;

    const float step = clamp_step(grid_step_mm, std::max(size.x(), size.y()), GRID_MAX_CELLS_PER_AXIS);
    probe.grid_step_mm = step;

    // Cast from below everything so the interval parity of the zone ray is unambiguous.
    float parts_min_z = parts.its.vertices.front().z();
    for (const Vec3f &v : parts.its.vertices)
        parts_min_z = std::min(parts_min_z, v.z());
    const double ray_z0 = double(std::min(bb_min.z(), parts_min_z)) - 1.;
    // The ray-triangle test is evaluated in the ray's scalar type, so the ray is double.
    const Vec3d  dir(0., 0., 1.);

    std::vector<igl::Hit<float>> hits_zone;
    std::vector<igl::Hit<float>> hits_parts;
    // Half a cell in, so a block edge never lands exactly on a ray.
    for (float y = bb_min.y() + 0.5f * step; y < bb_max.y(); y += step) {
        for (float x = bb_min.x() + 0.5f * step; x < bb_max.x(); x += step) {
            const Vec3d origin(double(x), double(y), ray_z0);

            hits_zone.clear();
            if (! AABBTreeIndirect::intersect_ray_all_hits(zone.its.vertices, zone.its.indices, zone.tree, origin, dir, hits_zone))
                continue;
            // Entry/exit pairs. An odd tail means a grazing ray or a non watertight block: dropped.
            const size_t pairs = hits_zone.size() / 2;
            if (pairs == 0)
                continue;
            ++ probe.cells_inside_zone;

            hits_parts.clear();
            if (! AABBTreeIndirect::intersect_ray_all_hits(parts.its.vertices, parts.its.indices, parts.tree, origin, dir, hits_parts))
                continue;

            for (const igl::Hit<float> &hit : hits_parts) {
                const Vec3f n = facet_normal(parts.its, hit.id);
                if (n.z() >= DOWNWARD_NORMAL_Z_MAX)
                    continue;
                const double z = ray_z0 + double(hit.t);
                bool inside = false;
                for (size_t i = 0; i < pairs; ++ i) {
                    const double z_in  = ray_z0 + double(hits_zone[2 * i].t);
                    const double z_out = ray_z0 + double(hits_zone[2 * i + 1].t);
                    if (z >= z_in && z <= z_out) {
                        inside = true;
                        break;
                    }
                }
                if (inside)
                    probe.lit.push_back({ Vec3f(x, y, float(z)), n });
            }
        }
    }

    BOOST_LOG_TRIVIAL(debug) << "[ZONEPROBE] zone='" << enforcer.name << "' step=" << probe.grid_step_mm
                             << "mm cells_in_zone=" << probe.cells_inside_zone << " lit=" << probe.lit.size()
                             << (probe.sterile() ? " (STERILE: this zone will produce nothing)" : "");
    return probe;
}

CoverageProbe probe_object_coverage(const ModelObject &object, float grid_step_mm, float max_normal_z, const Transform3d &orient)
{
    CoverageProbe probe;

    const MeshAndTree parts = collect(object, true, nullptr, orient);
    if (parts.empty())
        return probe;

    // One tree PER ZONE: the inside test is an entry/exit parity, and two overlapping blocks merged
    // into one tree would interleave their pairs.
    std::vector<MeshAndTree> zones;
    for (const ModelVolume *v : object.volumes)
        if (v->is_support_enforcer()) {
            MeshAndTree z = collect(object, false, v, orient);
            if (! z.empty())
                zones.emplace_back(std::move(z));
        }

    Vec3f bb_min = parts.its.vertices.front();
    Vec3f bb_max = bb_min;
    for (const Vec3f &v : parts.its.vertices) {
        bb_min = bb_min.cwiseMin(v);
        bb_max = bb_max.cwiseMax(v);
    }
    const Vec3f size = bb_max - bb_min;
    if (size.x() <= 0.f || size.y() <= 0.f)
        return probe;

    const float step = clamp_step(grid_step_mm, std::max(size.x(), size.y()), COVERAGE_MAX_CELLS_PER_AXIS);
    probe.grid_step_mm = step;

    const double ray_z0 = double(bb_min.z()) - 1.;
    const Vec3d  dir(0., 0., 1.);

    std::vector<igl::Hit<float>> hits_parts;
    std::vector<igl::Hit<float>> hits_zone;
    for (float y = bb_min.y() + 0.5f * step; y < bb_max.y(); y += step) {
        for (float x = bb_min.x() + 0.5f * step; x < bb_max.x(); x += step) {
            const Vec3d origin(double(x), double(y), ray_z0);
            hits_parts.clear();
            if (! AABBTreeIndirect::intersect_ray_all_hits(parts.its.vertices, parts.its.indices, parts.tree, origin, dir, hits_parts))
                continue;
            ++ probe.cells;

            for (const igl::Hit<float> &hit : hits_parts) {
                const Vec3f n = facet_normal(parts.its, hit.id);
                if (n.z() >= DOWNWARD_NORMAL_Z_MAX || n.z() > max_normal_z)
                    continue;
                const double z = ray_z0 + double(hit.t);

                bool inside_any = false;
                for (const MeshAndTree &zone : zones) {
                    hits_zone.clear();
                    if (! AABBTreeIndirect::intersect_ray_all_hits(zone.its.vertices, zone.its.indices, zone.tree, origin, dir, hits_zone))
                        continue;
                    const size_t pairs = hits_zone.size() / 2;
                    for (size_t i = 0; i < pairs && ! inside_any; ++ i) {
                        const double z_in  = ray_z0 + double(hits_zone[2 * i].t);
                        const double z_out = ray_z0 + double(hits_zone[2 * i + 1].t);
                        if (z >= z_in && z <= z_out)
                            inside_any = true;
                    }
                    if (inside_any)
                        break;
                }
                (inside_any ? probe.covered : probe.uncovered).push_back({ Vec3f(x, y, float(z)), n });
            }
        }
    }
    return probe;
}

std::vector<const ModelVolume*> sterile_zones(const ModelObject &object, float grid_step_mm, const Transform3d &orient)
{
    std::vector<const ModelVolume*> sterile;
    for (const ModelVolume *v : object.volumes)
        if (v->is_support_enforcer() && probe_zone(object, *v, grid_step_mm, orient).sterile())
            sterile.push_back(v);
    return sterile;
}

double corridor_reach_mm(double height_mm, double layer_height_mm, double support_line_width_mm)
{
    if (height_mm <= 0. || layer_height_mm <= 0. || support_line_width_mm <= 0.)
        return 0.;
    // One step per layer crossed: the engine clamps per layer, so the reach is a count of steps.
    return (height_mm / layer_height_mm) * corridor_step_mm(support_line_width_mm);
}

} // namespace SupportZones
} // namespace Slic3r
