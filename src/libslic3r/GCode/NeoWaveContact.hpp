// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: NeoWave support, contact layer (WaveSupport "Mecanismo 2",
// NeoweaveEngine contact mode of NeotkoCM).
//
// Ripples the Z of the object's bridge infill where it rests on a support roof: an upward-only rectified sine,
// valleys at the layer Z (touching the roof), crests lifted by the amplitude, so the contact is intermittent and
// the support separates more easily. Only Z changes: the XY path, the width and the extrusion stay the same.
// Upstream gates on the bridge role alone (and so also waves bridges over air); here the wave is applied only to the
// points that lie over the support islands of the support layer right below. Opt-in (neowave_contact, default off).
#ifndef slic3r_GCode_NeoWaveContact_hpp_
#define slic3r_GCode_NeoWaveContact_hpp_

#include "../ExPolygon.hpp"
#include "../Point.hpp"

#include <functional>
#include <string>

namespace Slic3r {
class ExtrusionPath;
class FullPrintConfig;
class PrintObjectNeoConfig;
class GCodeWriter;
class Layer;

namespace NeoWaveContact {

// The support islands the bridge `path` of `layer` rests on, or nullptr when the contact wave does not apply (key off,
// zero amplitude, not a bridge, first layer, no support layer right below, or the path does not cross the roof).
const ExPolygons *roof_below(const ExtrusionPath &path, const Layer *layer, const FullPrintConfig &config);

// Emits `path` (already positioned at its first point, speed set) with the contact wave over `roof`, then restores the
// layer Z. Replaces the plain G1 / arc emission of GCode::_extrude for this path.
void extrude(std::string &gcode, GCodeWriter &writer, const ExtrusionPath &path, const ExPolygons &roof,
             const PrintObjectNeoConfig &neo, const FullPrintConfig &config, double nominal_z, double F, double e_per_mm,
             const std::function<Vec2d(const Point &)> &point_to_gcode, const std::string &comment);

}} // namespace Slic3r::NeoWaveContact

#endif
