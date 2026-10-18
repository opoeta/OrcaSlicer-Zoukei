// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoArachne wall generator (NeoArachnePlan / NeoArachneInterior).
//
// Per object opt-in (neoarachne_hybrid, default off); with the key off nothing here runs. Per object (PrintObjectNeoConfig)
// the engine of the outer wall and of the inner walls is chosen (neoarachne_outer_wall / neoarachne_inner_walls):
//   outer Classic + inner Arachne*  -> "Hybrid v2" (the default): Classic prints ONLY the outer wall (same code path
//                                      as wall_generator = Classic, so overhangs, seams, wave overhangs and fuzzy
//                                      skin keep working on the visible surface); Arachne fills the interior with
//                                      capped beads plus integrated gap fill tagged as Gap infill. NeotkoEdge adds
//                                      the pinned outer bead width and the bead count hysteresis.
//   outer Classic + inner Classic   -> "v3": fixed width Classic walls placed only where they fit whole, and the
//                                      remaining pocket printed as ONE variable width line ("spine",
//                                      NeoArachneSpine) when it fits under the ceiling (neoarachne_spine); plain
//                                      Classic when the spine is off.
//   outer Arachne*                  -> stock Arachne (a variable width outer needs the Arachne interior too).
// NeoStroke (debug only upstream) is not ported.
#ifndef slic3r_NeoArachneHybrid_hpp_
#define slic3r_NeoArachneHybrid_hpp_

#include "../ExPolygon.hpp"
#include "../Polygon.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {
class PerimeterGenerator;

namespace NeoArachne {

struct HybridConfig {
    NeoArachneWallSource outer_wall  = NeoArachneWallSource::Classic;
    NeoArachneWallSource inner_walls = NeoArachneWallSource::Arachne;

    // Edge Closure (Hybrid v2 only).
    double allowed_overlap_pct = 0.0;   // how much the first Arachne bead may overlap the classic outer
    double min_bead_width_pct  = 40.0;  // % of the nozzle: thinnest bead Arachne emits
    double max_bead_width_pct  = 200.0; // % of the nozzle: ceiling of a single bead (100..200)
    double min_feature_pct     = 10.0;  // % of the nozzle: geometry thinner than this is dropped
    bool   keep_short_tails    = true;  // keep the closure tails that touch the classic outer

    // NeotkoEdge (inner walls = Arachne (NeotkoEdge) only).
    bool   pin_outer_width           = true;
    double bead_count_hysteresis_pct = 20.0;

    // v3 spine (inner walls = Classic only).
    bool   spine               = true;
    double spine_min_width_pct = 50.0;  // % of the inner wall line width: floor
    double spine_max_width_pct = 300.0; // % of the inner wall line width: ceiling
    double spine_min_length_mm = 0.1;
    double spine_sliver_pct    = 35.0;  // % of the inner wall line width: cracks below are skipped (0 = off)
};

HybridConfig hybrid_config(const PrintObjectConfig &object_config, const PrintObjectNeoConfig &neo_config);

// Runs NeoArachne for one layer region: replaces PerimeterGenerator::process_classic() / process_arachne().
void run_hybrid(PerimeterGenerator &g, const PrintObjectNeoConfig &neo_config);

// Arachne over `outline_polys` (the area left inside the classic outer wall), beads appended to g.loops
// grouped by island. Returns the area Arachne leaves for the infill.
ExPolygons run_interior(PerimeterGenerator &g, const Polygons &outline_polys, int inset_count, const HybridConfig &cfg);

}} // namespace Slic3r::NeoArachne

#endif
