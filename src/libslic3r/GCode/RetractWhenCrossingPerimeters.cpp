#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../Polyline.hpp"
#include "libslic3r/Surface.hpp"
#include <algorithm>
#include <vector>
#include "libslic3r/AABBTreeIndirect.hpp"
#include <cstddef>
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/libslic3r.h"
#include <cassert>
#include "libslic3r/Polygon.hpp"

#include "RetractWhenCrossingPerimeters.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/SurfaceCollection.hpp"

namespace Slic3r {

bool RetractWhenCrossingPerimeters::travel_inside_internal_regions(const Layer &layer, const Polyline &travel)
{
    if (m_layer != &layer) {
        // Update cache.
        m_layer = &layer;
        m_internal_islands.clear();
        m_aabbtree_internal_islands.clear();
        // Collect expolygons of internal slices.
        for (const LayerRegion *layerm : layer.regions())
            for (const Surface &surface : layerm->get_slices().surfaces)
                if (surface.is_internal())
                    m_internal_islands.emplace_back(&surface.expolygon);
        // Calculate bounding boxes of internal slices.
        std::vector<AABBTreeIndirect::BoundingBoxWrapper> bboxes;
        bboxes.reserve(m_internal_islands.size());
        for (size_t i = 0; i < m_internal_islands.size(); ++ i)
            bboxes.emplace_back(i, get_extents(*m_internal_islands[i]));
        // Build AABB tree over bounding boxes of internal slices.
        m_aabbtree_internal_islands.build_modify_input(bboxes);
    }

    BoundingBox           bbox_travel = get_extents(travel);
    AABBTree::BoundingBox bbox_travel_eigen{ bbox_travel.min, bbox_travel.max };
    int result = -1;
    bbox_travel.offset(SCALED_EPSILON);
    AABBTreeIndirect::traverse(m_aabbtree_internal_islands, 
        [&bbox_travel_eigen](const AABBTree::Node &node) {
            return bbox_travel_eigen.intersects(node.bbox);
        },
        [&travel, &bbox_travel, &result, &islands = m_internal_islands](const AABBTree::Node &node) {
            assert(node.is_leaf());
            assert(node.is_valid());
            Polygons clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*islands[node.idx], bbox_travel);
            if (diff_pl(travel, clipped).empty()) {
                // Travel path is completely inside an "internal" island. Don't retract.
                result = int(node.idx);
                // Stop traversal.
                return false;
            }
            // Continue traversal.
            return true;
        });
    return result != -1;
}

bool RetractWhenCrossingPerimeters::travel_near_perimeters_too_long(const Layer &layer, const Polyline &travel, coord_t wall_distance, coord_t max_near_wall_travel)
{
    if (wall_distance <= 0 || max_near_wall_travel <= 0 || travel.length() <= max_near_wall_travel)
        return false;

    if (m_walls_layer != &layer) {
        m_walls_layer = &layer;
        Polylines walls;
        for (const LayerRegion *layerm : layer.regions())
            layerm->perimeters.collect_polylines(walls);
        Lines lines;
        for (const Polyline &wall : walls)
            append(lines, wall.lines());
        m_walls = AABBTreeLines::LinesDistancer<Line>{ std::move(lines) };
    }
    if (m_walls.get_lines().empty())
        return false;

    // Walk the travel: small steps near a wall, far from it jump by the safety margin (the distance
    // to the walls cannot change faster than the distance walked, so no near stretch is skipped).
    const double near_step = 0.5 * double(max_near_wall_travel);
    size_t segment = 0;
    double offset  = 0.;
    Vec2d  sample  = travel.points.front().cast<double>();
    auto advance = [&](double distance) {
        while (segment + 1 < travel.points.size()) {
            const Vec2d  a   = travel.points[segment].cast<double>();
            const Vec2d  v   = travel.points[segment + 1].cast<double>() - a;
            const double len = v.norm();
            if (len > 0. && distance <= len - offset) {
                offset += distance;
                sample = a + v * (offset / len);
                return true;
            }
            distance -= len - offset;
            ++ segment;
            offset = 0.;
            sample = travel.points[segment].cast<double>();
        }
        return false;
    };

    bool   was_near = false;
    double step     = 0.;
    double run      = 0.;
    for (;;) {
        const double distance = m_walls.distance_from_lines<false>(sample.cast<coord_t>());
        const bool   is_near  = distance <= wall_distance;
        // Only samples that are both near count: separate near stretches do not add up.
        run = is_near && was_near ? run + step : 0.;
        if (run >= max_near_wall_travel)
            return true;
        step     = is_near ? near_step : std::max(near_step, distance - double(wall_distance));
        was_near = is_near;
        if (! advance(step))
            return false;
    }
}

} // namespace Slic3r
