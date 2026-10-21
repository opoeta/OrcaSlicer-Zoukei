// BMCU firmware update dialog (Device > Firmware, BMCU panel). Picks a firmware build from GitHub
// (jarczakpawel/BMCU-C-PJARCZAK: latest release, main branch or an older release; or a local .bin), downloads it,
// checks size / CRC32 / SHA-256 against the manifest, then writes it through the CH340 USB port of the BMCU (or an
// external USB-serial adapter in TTL mode) with the protocol ported from BMCU-Flasher (Utils/BmcuFlasher.hpp).
// Nothing is written without an explicit confirmation, and the dialog refuses while the printer prints.
#ifndef slic3r_GUI_Zoukei_BmcuFlasherDialog_hpp_
#define slic3r_GUI_Zoukei_BmcuFlasherDialog_hpp_

#include <functional>
#include <string>

class wxWindow;

namespace Slic3r { namespace GUI {

// `installed_version`: the x.x.x.x the printer reports for the BMCU ("" when unknown).
// `busy_reason`: returns a non-empty, translated reason while flashing must be refused (printer printing / updating);
// it is checked when the dialog opens and again right before writing.
void open_bmcu_flasher_dialog(wxWindow *parent, const std::string &installed_version, std::function<std::string()> busy_reason);

}} // namespace Slic3r::GUI

#endif
