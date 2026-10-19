// src/slic3r/GUI/Zoukei/CrealityCloudUi.hpp
//
// Orca Zoukei: windows of the Creality Cloud integration (logic in CrealityCloud.hpp):
//   - login window: the official Creality account page in a web view; Orca only catches the redirect with the
//     one-time code (the password is typed on Creality's page and never read, logged or stored by Orca)
//   - "Send filaments to the Creality Cloud" window: pick user filament presets and the printer to link them to,
//     confirm, then a per-preset report
//   - the account row of Preferences and the account bar of the Creality device tab
// Login flow ported from Creality Print (AGPL-3.0, github.com/CrealityOfficial/CrealityPrint: LoginDialog.cpp,
// HttpServer.cpp).
#ifndef slic3r_GUI_Zoukei_CrealityCloudUi_hpp_
#define slic3r_GUI_Zoukei_CrealityCloudUi_hpp_

#include <string>

class wxWindow;
class wxBoxSizer;

namespace Slic3r { namespace GUI { namespace CrCloudUi {

// Modal login window. True when an account is logged in afterwards.
bool show_login_dialog(wxWindow* parent);

// Modal upload window; `preselect` = name of a user filament preset to tick (from the filament menu).
void show_upload_dialog(wxWindow* parent, const std::string& preselect = std::string());

// Appends the account controls (region, status, log in / log out) to a Preferences row.
void build_preferences_row(wxWindow* parent, wxBoxSizer* row);

// A slim bar for the top of the Creality device tab: account status and "Send filaments to the Creality Cloud".
wxWindow* create_device_bar(wxWindow* parent);

}}} // namespace Slic3r::GUI::CrCloudUi

#endif
