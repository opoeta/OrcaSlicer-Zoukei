// Spoolman integration of the GUI: connection (Preferences), the spool of every project filament (option filament_spool_id)
// and the "Spool" dialog. Spoolman is only ever read; nothing is created or edited there.
#ifndef slic3r_GUI_SpoolmanUi_hpp_
#define slic3r_GUI_SpoolmanUi_hpp_

#include "slic3r/Utils/Spoolman.hpp"

#include <functional>
#include <string>
#include <vector>
#include <wx/string.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI { namespace SpoolmanUi {

// ---- connection -----------------------------------------------------------------------------
// Test the server (GET /api/v1/info and /api/v1/spool) on a worker thread; `done` runs on the UI thread.
void test_connection_async(const std::string& url, std::function<void(bool ok, const wxString& message)> done);
// Ask the connected Moonraker printer (or the selected printer's Moonraker host) for its [spoolman] server; `done`
// runs on the UI thread with the normalised url ("" when there is none).
void detect_url_async(std::function<void(const std::string& url)> done);
// Preferences "spoolman_url" empty -> fill it from the connected Moonraker printer (async, silent when nothing is found).
void autofill_url();
// Reload the spool list in the background when the cache is empty or old (tooltips, quota checks); needs the server.
void refresh_cache_async(bool force = false);

// ---- spool per project filament ---------------------------------------------------------------
// Spoolman spool id of a project filament (option filament_spool_id), 0 = none.
int  project_spool_id(int filament_idx);
// Stores the id (0 = none) without touching colours or presets. Marks the project as modified.
void set_project_spool_id(int filament_idx, int spool_id);
// The spool decides the filament: colour(s) (filament_colour, filament_multi_colour, filament_colour_type), the best matching
// Orca preset and the id. Then the spool is written to the slot the filament is mapped to. UI thread.
bool apply_spool(int filament_idx, const Spoolman::Spool& spool);
// Unbinds the filament (and its slot) from its spool; colours and preset stay as they are.
void clear_spool(int filament_idx);
// "#12 Voolt3D PLA (774 g)": from the cache, "#12" alone when the spool is not known (yet).
wxString spool_summary(int spool_id);
// ---- pre-print check -------------------------------------------------------------------------
// A project filament whose linked spool has less filament left than the sliced plate needs.
struct Shortage
{
    int    filament_idx = 0;
    int    spool_id     = 0;
    double needed_g     = 0.;
    double remaining_g  = 0.;
    std::string title;      // "#12 Voolt3D PLA"
};
// Used filaments of the current plate (slice result) whose spool is short. Refreshes the spools from the server first (a few
// seconds at most, the cached values when it does not answer). UI thread; empty when nothing is sliced or no spool is linked.
std::vector<Shortage> low_spools();
// One line per short spool, "" when everything is fine.
wxString low_spools_text();
// Warns (Yes/No) when a linked spool is too small for the plate. True = go on with the print.
bool confirm_spools_enough(wxWindow* parent);

// Modal list of the Spoolman spools (swatch, material, vendor, name, remaining weight, location, slot) for a project filament.
void pick_spool(wxWindow* parent, int filament_idx);

}}} // namespace Slic3r::GUI::SpoolmanUi

#endif
