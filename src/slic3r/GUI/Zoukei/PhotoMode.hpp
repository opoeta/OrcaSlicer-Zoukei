// Orca Zoukei: Photo Mode for the G-code preview, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// A studio render of the preview to PNG: no bed, no UI, a soft cast shadow, a transparent or solid
// background (the shadow stays in the transparency), at 1080p / 1440p / 2160p, saved to a file or copied.
#ifndef slic3r_GUI_Zoukei_PhotoMode_hpp_
#define slic3r_GUI_Zoukei_PhotoMode_hpp_

namespace Slic3r {
namespace GUI {

class GLCanvas3D;
class ImGuiWrapper;

namespace Zoukei {

struct PhotoOptions
{
    // Height of the picture in pixels, the width follows the aspect of the preview canvas.
    int   height_px{ 1080 };
    // Transparent background (the cast shadow is kept as partial transparency) or the solid colour below.
    bool  transparent{ true };
    float background[3]{ 1.0f, 1.0f, 1.0f };
    // Cast and self shadows from the key light.
    bool  shadows{ true };
    // Frame the print in the picture, else keep the framing of the preview.
    bool  frame_print{ true };
    // Direction the key light comes from, degrees. The shadow follows it.
    float light_azimuth{ -123.7f };
    float light_elevation{ 54.2f };
};

// The options the panel edits, kept for the session and in the app config.
PhotoOptions& photo_options();

bool photo_panel_open();
void set_photo_panel_open(bool open);

// The "Photo mode" window of the G-code preview. Call once per frame after the legend.
void render_photo_panel(ImGuiWrapper& imgui, GLCanvas3D& canvas, int canvas_width, int canvas_height, float scale);

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_PhotoMode_hpp_
