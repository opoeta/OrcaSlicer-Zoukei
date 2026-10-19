// Layer Slayer: the dialog of the Calibration menu (stage 3 of the plugin port). Lists the 22 tests grouped like
// the plugin's page, with their art, parameters, a drawing of the plate, what each piece changes and how to read
// the print; "Create plate" replaces the project with the test (LayerSlayer::run).
#ifndef slic3r_GUI_Zoukei_LayerSlayerDialog_hpp_
#define slic3r_GUI_Zoukei_LayerSlayerDialog_hpp_

#include <string>

namespace Slic3r { namespace GUI {
class Plater;

// Opens the dialog (modal); `id` preselects a test ("" = the last one used, else the first).
void open_layer_slayer_dialog(Plater *plater, const std::string &id = std::string());

}} // namespace Slic3r::GUI

#endif
