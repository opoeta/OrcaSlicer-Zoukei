// Orca Zoukei: RealColor view of the G-code preview, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// The preview shows the colour that comes out of the printer instead of the raw colour of each extrusion:
// beads stack and let light through by the TD of their filament, neighbouring lines blend. Mixed filaments
// (alternating layers, sub-layers) print as physical filaments, so they end up with their blended colour.
#ifndef slic3r_GUI_Zoukei_RealColorView_hpp_
#define slic3r_GUI_Zoukei_RealColorView_hpp_

#include "libvgcode/include/Types.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

class ImGuiWrapper;

namespace Zoukei {

bool realcolor_enabled();
void set_realcolor_enabled(bool enabled);

// The parameters handed to the viewer. `tool_colors` are the filament colours of the loaded G-code, in
// extruder order; each one gets its TD (the "Mix colors automatically" dialog's, else from the filament
// type and name) turned into a millimetre transmission distance.
libvgcode::RealColorParams build_realcolor_params(const std::vector<std::string>& tool_colors);

// The RealColor check box and its two sliders, and the Photo mode button, at the end of the legend.
// `realcolor_applicable` is false in the views RealColor does not change. Returns true when the RealColor
// parameters changed and the viewer must be given build_realcolor_params() again.
bool render_legend_extras(ImGuiWrapper& imgui, float scale, bool realcolor_applicable);

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_RealColorView_hpp_
