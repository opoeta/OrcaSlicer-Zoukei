#ifndef slic3r_FillSandwich_hpp_
#define slic3r_FillSandwich_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: the Sandwich sub-layer generator ("FASE 2" of
// Layer::make_fills in NeotkoCM's Fill.cpp).

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
bool sandwich_fill_surface(Layer &layer, const LayerRegion &layerm, Fill &filler, const FillParams &params, const Surface &surface);

} // namespace Slic3r

#endif // slic3r_FillSandwich_hpp_
