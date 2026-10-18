#ifndef slic3r_FillSandwich_hpp_
#define slic3r_FillSandwich_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: the Sandwich sub-layer generator ("FASE 2" of
// Layer::make_fills in NeotkoCM's Fill.cpp).

#include <vector>

#include "../ExPolygon.hpp"

namespace Slic3r {

class Fill;
class Layer;
class LayerRegion;
class Surface;
struct FillParams;

// When `surface` is a top surface whose object carries a Sandwich pass stack (multipass_*,
// interlayer_colormix_*, multipass_path_gradient or neotko_surface_passes_top), fills it once per
// pass with `filler`, scales each pass to its share of the layer height and appends the result to
// layer.sandwich_sublayers. Returns true when the surface was consumed (the caller must then skip
// its normal fill). Returns false, touching nothing, for every surface without a Sandwich, so with
// the feature off Layer::make_fills behaves exactly as before.
// The filler's angle / fixed_angle / spacing are restored before returning.
// Zones (ported from OrcaFS-NeotkoCM, Neotko): the zone follows from the surface: a top surface is the
// Top zone, the internal solid fill of a piece under a top surface the Penultimate zone and a supported
// bottom surface (erBottomSurface, not a bridge) the Bottom zone. Each zone has its own stack
// (SurfacePassStack::resolve_zone, or the painted recipe) and prints the same way.
// ColorStitch Painter: `painted_profile` > 0 fills the surface with that painted recipe (a piece cut
// by sandwich_split_painted) instead of the object's own Sandwich settings; < 0 means "no Sandwich on
// this piece" (the part of a penultimate surface that is not under a top).
bool sandwich_fill_surface(Layer &layer, const LayerRegion &layerm, Fill &filler, const FillParams &params, const Surface &surface,
                           int painted_profile = 0);

// ColorStitch Painter (ported from OrcaFS-NeotkoCM, Neotko): cuts the surface pieces `expolygons` of
// one SurfaceFill (top, penultimate or bottom zone) along the painted recipe and sticker footprints of
// this layer. On success `expolygons` is replaced by the painted pieces followed by the unpainted rest
// and `recipe_of` gets one value per piece: a profile id (painted), 0 (the object's own settings of the
// zone) or -1 (no Sandwich). The penultimate zone is also cut along the area under the top surfaces of
// the layer above. Returns false and touches nothing when nothing applies, which is always the case
// for an object without ColorStitch paint, stickers and zone settings.
bool sandwich_split_painted(const Layer &layer, const Surface &surface, const FillParams &params, ExPolygons &expolygons,
                            std::vector<int> &recipe_of);

} // namespace Slic3r

#endif // slic3r_FillSandwich_hpp_
