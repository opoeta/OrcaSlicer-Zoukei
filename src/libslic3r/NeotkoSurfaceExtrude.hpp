// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0 ("Construct", NeotkoCM s363).
// Geometry of a flat face: the coplanar region around a facet (the flood fill the ColorStitch Painter
// and Align & Construct use), its outline projected to XY, and a solid extruded from it. No GUI here.
#ifndef slic3r_NeotkoSurfaceExtrude_hpp_
#define slic3r_NeotkoSurfaceExtrude_hpp_

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <vector>

namespace Slic3r { namespace SurfaceExtrude {

// Facets connected to `seed` whose normal stays within acos(cos_thresh) of the seed's normal.
// `normals` / `neighbors` come from its_face_normals() / its_face_neighbors().
std::vector<int> coplanar_region(const std::vector<Vec3f>   &normals,
                                 const std::vector<Vec3i32> &neighbors,
                                 int                         seed,
                                 float                       cos_thresh,
                                 size_t                      max_facets = 40000);

// The region projected onto XY after `trafo` (scaled coordinates), holes kept.
// mean_z receives the average Z of the region's vertices after `trafo`.
ExPolygons region_outline(const indexed_triangle_set &its,
                          const std::vector<int>     &region,
                          const Transform3d          &trafo,
                          double                     *mean_z = nullptr);

// The holes of `outline`, each one as a filled shape (counter-clockwise, no holes of its own).
ExPolygons outline_holes(const ExPolygons &outline);

// Closed prism of `outline` grown/shrunk by offset_mm, from z0 to z0 + height.
// Empty when the offset eats the whole shape or height <= 0.
indexed_triangle_set extrude_outline(const ExPolygons &outline,
                                     double            offset_mm,
                                     double            z0,
                                     double            height);

// A curve node: a height fraction t (0..1 of the total height) the profile passes through, with its
// offset e in mm (positive grows, negative shrinks). (ht, he) is the control point of the quadratic
// Bezier segment from this node to the next one.
struct CurveNode {
    double t  = 0.;
    double e  = 0.;
    double ht = 0.;
    double he = 0.;
};

// The curve as a polyline of (t, e), t ascending.
std::vector<Vec2d> sample_curve(const std::vector<CurveNode> &curve, int per_segment = 24);
// e at height fraction t, read off a sample_curve() polyline.
double curve_value(const std::vector<Vec2d> &samples, double t);

// Like extrude_outline(), but the outline is offset by offset_mm + curve(t) at each height.
// t = 0 is the picked face: with grow_down the solid hangs below z0 + height instead of rising
// from z0. The solid is built in thin steps of at most max_step_mm.
indexed_triangle_set extrude_outline_curved(const ExPolygons             &outline,
                                            double                        offset_mm,
                                            double                        z0,
                                            double                        height,
                                            const std::vector<CurveNode> &curve,
                                            bool                          grow_down,
                                            double                        max_step_mm = 0.025);

}} // namespace Slic3r::SurfaceExtrude

#endif // slic3r_NeotkoSurfaceExtrude_hpp_
