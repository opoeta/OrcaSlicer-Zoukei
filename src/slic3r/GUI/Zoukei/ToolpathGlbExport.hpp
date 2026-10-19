// Orca Zoukei: export of the sliced G-code preview toolpaths as GLB (glTF 2.0 binary) plus an optional
// self-contained three.js web viewer (resources/web/zoukeiViewer/viewer.html).
//
// The beads are built as tube meshes (4 or 8 sided, one ring per toolpath vertex), merged per extrusion
// role and colour, with the positions quantised to 16 bit (KHR_mesh_quantization). One glTF node per
// role; print stats, filament colours and layer heights go into scene.extras.zoukei.
#ifndef slic3r_GUI_Zoukei_ToolpathGlbExport_hpp_
#define slic3r_GUI_Zoukei_ToolpathGlbExport_hpp_

#include <cstdint>
#include <string>

class wxWindow;

namespace Slic3r {
namespace GUI {
namespace Zoukei {

enum class GlbDetail { Preview, Standard, Full };
enum class GlbHtml { None, Linked, Embedded }; // Linked: viewer .html next to the .glb, loads it by name

struct GlbExportOptions
{
    bool      travel    = false;
    GlbDetail detail    = GlbDetail::Standard;
    GlbHtml   html      = GlbHtml::None;
    bool      overwrite = false; // replace existing .glb / .html
};

struct GlbExportResult
{
    bool        ok = false;
    std::string error_code; // not_sliced | exists | no_directory | bad_extension | write_failed | no_template | empty
    std::string error;
    std::string glb_path;
    std::string html_path;
    uint64_t    glb_bytes   = 0;
    uint64_t    html_bytes  = 0;
    uint64_t    triangles   = 0;
    uint32_t    roles       = 0;
    uint32_t    materials   = 0;
};

const char* glb_detail_name(GlbDetail d);
bool        glb_detail_from_string(const std::string& s, GlbDetail& out);
bool        glb_html_from_string(const std::string& s, GlbHtml& out);

// The path of the viewer .html that goes with a .glb path (same folder and stem).
std::string glb_html_path_for(const std::string& glb_path_utf8);

// Exports the toolpaths shown in the Preview of the plater. Must run on the UI thread. Writes only
// glb_path_utf8 (and its .html twin when requested); refuses to replace existing files unless opt.overwrite.
GlbExportResult export_toolpaths_glb(const std::string& glb_path_utf8, const GlbExportOptions& opt);

// File > Export > "Export toolpaths as GLB / 3D web viewer...": options dialog, save dialog, export, message.
void export_toolpaths_glb_dialog(wxWindow* parent);

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_ToolpathGlbExport_hpp_
