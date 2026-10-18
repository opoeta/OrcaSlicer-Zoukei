// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoStitch Interlock (Feature/NeoStitch).
//
// Z-stitch interlock: a mechanical layer-to-layer lock that never changes Z. On the target wall every
// extrusion line alternates NOTCH segments (the wall deflects inward) and FILL segments (the wall widens,
// outer edge anchored, to plug the notch left by the layer below). Both are placed along a world stable
// angular coordinate around the object and the phase flips by half a period on odd layers, so what was a notch
// on layer N is a fill on layer N+1: the registration is a property of the phase, nothing is searched for.
//
// Scope of this port: the notch, the fill, the "real drop" margin (fill shorter than its notch) and the
// guards (first layers, top layer, short loops, unsupported events, fuzzy skin / bump mapping on the same
// wall). Not ported: the fill speed (upstream documents it as a confirmed no-op) and the visual height tag.
// Arachne walls only (classic perimeters are untouched), as upstream.
#ifndef libslic3r_NeoStitch_hpp_
#define libslic3r_NeoStitch_hpp_

#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"

namespace Slic3r::Feature::NeoStitch {

struct NeoStitchConfig
{
    NeoStitchTarget target         = NeoStitchTarget::Disabled;
    double          depth_mm       = 0.0;   // notch depth; 0 = the wall line width
    double          flat_length_mm = 3.0;   // plateau of one notch / fill event
    double          ramp_length_mm = 1.0;   // ramp in and out of each event
    double          period_mm      = 10.0;  // nominal distance between events
    double          flow_pct       = 100.0; // % of the fill width delta that is applied
    int             skip_layers    = 3;     // layers skipped above the first one
    double          fill_margin_mm = 1.0;   // fill is this much shorter than its notch, per side
};

NeoStitchConfig config_from_object(const PrintObjectConfig &object_config);

// Applies the interlock to one Arachne wall (in place). Returns true when the wall was modified.
bool apply_neostitch(Arachne::ExtrusionLine *extrusion, const PerimeterGenerator &perimeter_generator, bool is_contour, int total_loops);

} // namespace Slic3r::Feature::NeoStitch

#endif // libslic3r_NeoStitch_hpp_
