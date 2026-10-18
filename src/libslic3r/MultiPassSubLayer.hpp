#ifndef slic3r_MultiPassSubLayer_hpp_
#define slic3r_MultiPassSubLayer_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0 (MultiPassSubLayer, Print.hpp in NeotkoCM).
//
// One virtual sub-layer of the Sandwich (MultiPass / ColorStitch / PathBlend). A top surface whose
// region carries a Sandwich is not filled with the layer: Layer::make_fills() moves it into N
// single-filament sub-layers, each printed at its own nozzle Z inside the layer height, after the
// rest of the layer. ToolOrdering schedules them (MultiPassScheduler), the wipe tower plans their
// tool changes (NeoTower) and the G-code generator emits them in that same order.
//
// Differences to NeotkoCM: the records live on the Layer (Layer::sandwich_sublayers) instead of a
// per-object vector, so layers shared between identical objects carry them for free, and
// print_z is the real nozzle Z (no separate scheduling Z: our sub-layers never become LayerTools).

#include "libslic3r.h"
#include "ExtrusionEntityCollection.hpp"
#include "SurfacePassKind.hpp"

namespace Slic3r {

class PrintRegion;

struct MultiPassSubLayer
{
    // Nozzle Z of this sub-layer (top of its band; equals the layer print_z for the last band).
    coordf_t                  print_z    = 0.;
    // Extrusion height of the band.
    float                     height     = 0.f;
    // 0-based physical filament printing this sub-layer.
    int                       tool_id    = 0;
    // Position in its stacking chain (pass 0 prints before pass 1). A ColorStitch pass emits one
    // sub-layer per filament bucket, so this is a running counter, not the pass index.
    int                       pass_idx   = 0;
    // Chain identity inside the layer: disjoint surfaces get different keys so the scheduler may
    // group their work by filament (MultiPassScheduler::make_chain_key).
    int                       island_key = 0;
    SurfacePassKind           effect     = SurfacePassKind::Solid;
    // Region the surface belongs to: speeds and accelerations of the fills come from its config.
    const PrintRegion        *region     = nullptr;
    // Fill paths, already scaled to the band height (mm3_per_mm and height).
    ExtrusionEntityCollection fills;
};

} // namespace Slic3r

#endif // slic3r_MultiPassSubLayer_hpp_
