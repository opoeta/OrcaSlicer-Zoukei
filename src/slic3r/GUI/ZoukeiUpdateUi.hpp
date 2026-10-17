#ifndef slic3r_GUI_ZoukeiUpdateUi_hpp_
#define slic3r_GUI_ZoukeiUpdateUi_hpp_

// Orca Zoukei self-update: Help > Check for Updates, the startup check and the update dialog.
// The engine is in slic3r/Utils/ZoukeiUpdate.hpp; see docs/zoukei/updates.md.

namespace Slic3r { namespace GUI { namespace ZoukeiUpdateUi {

// Help menu / Speed Dial: always reports the outcome (new version, up to date, or the error).
void check_from_menu();
// At start: only when Preferences "zoukei_update_check" is on, not in stealth mode and not checked in the last 24 h.
// Silent unless there is a new version that was not skipped.
void check_on_startup();
// Automated tests only; does nothing unless ORCA_ZOUKEI_UPDATE_AUTO=1 and ORCA_ZOUKEI_UPDATE_URL is a loopback address.
void run_test_hook();

}}} // namespace Slic3r::GUI::ZoukeiUpdateUi

#endif
