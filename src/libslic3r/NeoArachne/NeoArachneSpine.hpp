// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoArachne v3 "spine" (single-line fill).
//
// Simplify3D style interior, measured on S3D G-code upstream:
//   1. Inner walls always have a fixed width and only go where they fit whole.
//   2. BEFORE each wall: if what is left fits WHOLE in one line no wider than the ceiling, no more walls go
//      there; one line is printed along the axis with width = local gap.
//   3. The minimum width is a FLOOR, not a cut: the tip of a pocket is printed at that width.
//   4. The maximum width is a CEILING: the line is never split in two.
//   5. At junctions branches are chained (a Y of 3 branches becomes 2 lines).
// Plus one of its own: a zone whose gap NEVER reaches the sliver threshold is not a pocket but a crack
// between two passes of the same wall, and is not printed.
#ifndef slic3r_NeoArachneSpine_hpp_
#define slic3r_NeoArachneSpine_hpp_

#include <cstddef>
#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"

namespace Slic3r {
class PerimeterGenerator;
namespace NeoArachne {

struct SpineParams {
    double floor_mm      = 0.2;   // floor: a narrower gap is printed at this width
    double ceiling_mm    = 1.2;   // ceiling: if the gap is wider anywhere, it is not a spine
    double min_length_mm = 0.1;   // shorter lines are dropped
    double sliver_mm     = 0.14;  // crack: zone or line whose gap never reaches this width is skipped (0 = off)
};

struct SpineStats {
    size_t components         = 0;
    size_t spine_components   = 0;
    size_t wide_components    = 0;
    size_t slivers_skipped    = 0;
    size_t polylines          = 0;
    size_t dropped_short      = 0;
    size_t tiny_loops_dropped = 0;
    size_t spurs_pruned       = 0;
    size_t junction_joins     = 0;
    double spine_len_mm       = 0.;
    double spine_area_mm2     = 0.;
    double wide_area_mm2      = 0.;
};

// Rule 2: true if no disc of diameter `ceiling_scaled` fits anywhere inside `ex`.
bool fits_in_one_line(const ExPolygon &ex, double ceiling_scaled);

// Prints the zones of `regions` that fit in one line <= ceiling as a spine (erGapFill). Returns the consumed zones;
// zones that give no line (or do not fit) are appended to `rejected` so the caller sends them to infill. Skipped
// slivers are not returned. `forced`: zones that become a spine even above the ceiling (width clipped to the
// ceiling). `out`: if not null the paths go there (the caller orders them with the walls), else to g.gap_fill.
ExPolygons run_spine(PerimeterGenerator &g, const ExPolygons &regions, const SpineParams &p, SpineStats *stats,
                     ExPolygons *rejected = nullptr, const ExPolygons *forced = nullptr,
                     ExtrusionEntitiesPtr *out = nullptr);

}} // namespace Slic3r::NeoArachne

#endif
