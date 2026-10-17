///|/ Ported from PrusaSlicer 2.9 (src/libslic3r/GCode/SeamPainting.cpp)
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "libslic3r/GCode/Seams29/SeamPainting.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r::Seams::ModelInfo {
Painting::Painting(const Transform3d &obj_transform, const ModelVolumePtrs &volumes) {
    for (const ModelVolume *mv : volumes) {
        // ORCA: as the classic SeamPlacer, painting is read from model parts and negative volumes only;
        // painting left on modifiers after a type change is ignored.
        if (!mv->is_model_part() && !mv->is_negative_volume())
            continue;
        if (mv->is_seam_painted()) {
            auto model_transformation = obj_transform * mv->get_matrix();

            indexed_triangle_set enforcers = mv->seam_facets.get_facets(*mv, EnforcerBlockerType::ENFORCER);
            // ORCA: keep the winding of mirrored volumes.
            its_transform(enforcers, model_transformation, true);
            its_merge(this->enforcers, enforcers);

            indexed_triangle_set blockers = mv->seam_facets.get_facets(*mv, EnforcerBlockerType::BLOCKER);
            its_transform(blockers, model_transformation, true);
            its_merge(this->blockers, blockers);
        }
    }

    this->enforcers_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(
        this->enforcers.vertices, this->enforcers.indices
    );
    this->blockers_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(
        this->blockers.vertices, this->blockers.indices
    );
}

bool Painting::is_enforced(const Vec3f &position, float radius) const {
    if (enforcers.empty()) {
        return false;
    }
    float radius_sqr = radius * radius;
    return AABBTreeIndirect::is_any_triangle_in_radius(
        enforcers.vertices, enforcers.indices, enforcers_tree, position, radius_sqr
    );
}

bool Painting::is_blocked(const Vec3f &position, float radius) const {
    if (blockers.empty()) {
        return false;
    }
    float radius_sqr = radius * radius;
    return AABBTreeIndirect::is_any_triangle_in_radius(
        blockers.vertices, blockers.indices, blockers_tree, position, radius_sqr
    );
}
} // namespace Slic3r::Seams::ModelInfo
