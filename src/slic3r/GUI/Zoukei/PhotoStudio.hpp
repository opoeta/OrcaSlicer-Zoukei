// Orca Zoukei: Photo Studio. A window of its own (a wx dialog with its own GL canvas on the application's shared
// GL context) to light, frame and export the sliced print: three point light rig, cyclorama / gradient / solid /
// transparent backgrounds, a floor with soft contact shadow and reflection, material presets, aspect ratio guides;
// PNG stills up to 8K (tiled, 4x supersampling), a batch of standard views, a caption footer and a watermark;
// turntable and virtual timelapse videos as MP4 (Windows), GIF or a PNG sequence. See StudioRender.hpp.
#ifndef slic3r_GUI_Zoukei_PhotoStudio_hpp_
#define slic3r_GUI_Zoukei_PhotoStudio_hpp_

namespace Slic3r {
namespace GUI {
namespace Zoukei {

// Opens the studio (one instance, modeless) or brings it to the front.
void open_photo_studio();
bool photo_studio_open();

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_PhotoStudio_hpp_
