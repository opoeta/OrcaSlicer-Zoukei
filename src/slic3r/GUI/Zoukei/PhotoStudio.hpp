// Orca Zoukei: Photo Studio. A window of its own (a wx dialog with its own GL canvas on the application's shared
// GL context) to light, frame and export the sliced print: three point light rig, cyclorama / gradient / solid /
// transparent backgrounds, a floor with soft contact shadow and reflection, material presets, aspect ratio guides;
// PNG stills up to 8K (tiled, 4x supersampling), a batch of standard views, a caption footer and a watermark;
// turntable and virtual timelapse videos as MP4 (Windows), GIF or a PNG sequence. See StudioRender.hpp.
#ifndef slic3r_GUI_Zoukei_PhotoStudio_hpp_
#define slic3r_GUI_Zoukei_PhotoStudio_hpp_

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

// Opens the studio (one instance, modeless) or brings it to the front.
void open_photo_studio();
bool photo_studio_open();
// Remote API / diagnostics: opens the studio and returns its picture (RGBA, top row first). live = the live view
// as drawn in the window (read back from its back buffer), else an off-screen render width x height.
// Automated check hook (ZOUKEI_STUDIO_SELFTEST=<folder>), called from the preview legend; a no-op otherwise.
void studio_selftest_tick();
bool studio_api_render(int width, int height, bool live, std::vector<uint8_t>& rgba, int& out_w, int& out_h, std::string& error);

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_PhotoStudio_hpp_
