///|/ Orca Zoukei: RealColor view, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_REALCOLOR_HPP
#define VGCODE_REALCOLOR_HPP

#include "../include/PathVertex.hpp"
#include "../include/Types.hpp"

#include <vector>

namespace libvgcode {

// Replaces the colour of every extrusion vertex in `colors` (the same float-encoded 0xRRGGBB the viewer
// uploads) by the colour that shows on the finished part. Everything is simulated from the toolpaths alone:
//  - a 2D grid of the plate keeps, per cell, the colour seen from above; each bead laid over a cell covers
//    what is below by its opacity 1 - 0.1^(height / TD) (the transmission distance TD of its filament is the
//    thickness that still lets 10 % of the light through, as OrcaFS-NeotkoCM's Beer-Lambert accumulate),
//  - a second grid keeps a running mean of the last beads of the cell, what a wall shows when seen from the side,
//  - at the end of every layer each vertex takes the mean of its neighbourhood (the eye cannot resolve
//    single lines), blending towards the side mean where the cell is on an edge or a wall.
// This is how alternating-layer and sub-layer mixed filaments end up with their blended colour.
//  - a filament without a TD is opaque: its beads keep their own colour on the face (only walls mix layers),
//  - `colors_bottom`, if given, receives the colours for a camera under the plate (first bead laid on top); it is
//    left empty when it would equal `colors`, i.e. when no filament is translucent.
void apply_realcolor(const std::vector<PathVertex>& vertices, std::vector<float>& colors, const RealColorParams& params,
                     std::vector<float>* colors_bottom = nullptr);

} // namespace libvgcode

#endif // VGCODE_REALCOLOR_HPP
