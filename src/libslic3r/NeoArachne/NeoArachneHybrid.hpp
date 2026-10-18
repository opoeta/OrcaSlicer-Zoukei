// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoArachne "Hybrid v2" wall recipe.
//
// Classic prints ONLY the outer wall (same code path as wall_generator = Classic, so overhangs, seams,
// wave overhangs and fuzzy skin keep working on the visible surface); the interior is handed to Arachne:
// fixed-width beads (inner widening is capped) plus integrated gap fill that is tagged and printed as
// Gap infill. Per object, opt-in (neoarachne_hybrid, default off). With the key off nothing here runs.
#ifndef slic3r_NeoArachneHybrid_hpp_
#define slic3r_NeoArachneHybrid_hpp_

#include "../ExPolygon.hpp"
#include "../Polygon.hpp"

namespace Slic3r {
class PerimeterGenerator;
class PrintObjectConfig;

namespace NeoArachne {

struct HybridConfig {
    double allowed_overlap_pct = 0.0;   // how much the first Arachne bead may overlap the classic outer
    double min_bead_width_pct  = 40.0;  // % of the nozzle: thinnest bead Arachne emits
    double max_bead_width_pct  = 200.0; // % of the nozzle: ceiling of a single bead (100..200)
    double min_feature_pct     = 10.0;  // % of the nozzle: geometry thinner than this is dropped
    bool   keep_short_tails    = true;  // keep the closure tails that touch the classic outer
};

HybridConfig hybrid_config_from_object(const PrintObjectConfig &object_config);

// Runs the recipe for one layer region: replaces PerimeterGenerator::process_classic() / process_arachne().
void run_hybrid(PerimeterGenerator &g);

// Arachne over `outline_polys` (the area left inside the classic outer wall), beads appended to g.loops
// grouped by island. Returns the area Arachne leaves for the infill.
ExPolygons run_interior(PerimeterGenerator &g, const Polygons &outline_polys, int inset_count, const HybridConfig &cfg);

}} // namespace Slic3r::NeoArachne

#endif
