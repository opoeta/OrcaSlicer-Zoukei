// Sidebar widgets for Creality printers: a per-filament
// slot badge/picker. Own code, inspired by Creality Print's PrinterBoxFilamentPanel / MaterialMapPanel.
#ifndef slic3r_GUI_CrealityCfsUi_hpp_
#define slic3r_GUI_CrealityCfsUi_hpp_

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <optional>
#include <utility>
#include <wx/colour.h>
#include <wx/panel.h>
#include <string>
#include <wx/window.h>
#include <wx/window.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/weakref.h>

namespace Slic3r {
class FlashforgePrinterAgent;
namespace GUI { namespace CfsUi {

// Project filament index -> printer slot {box_id, slot_id}; box 0 is the external spool.
// Explicit choices persist per project file (app config); otherwise a default is derived.
std::optional<std::pair<int, int>> mapped_slot(int filament_idx);

// The same as the print dialog's tray id: CFS unit index * 4 + slot, 255 for the external spool.
std::optional<int> mapped_tray(int filament_idx);
// The filament maps to a CFS/IFS slot that holds no filament (never true for the external spool). Label as shown on the badge.
bool     mapped_slot_empty(int filament_idx);
wxString mapped_slot_label(int filament_idx);
// Stores a choice made in the print dialog (tray id as above) in the project's filament_cfs_slot.
void set_mapped_tray(int filament_idx, int tray_id);

// Flashforge AD5X with the native agent: the IFS (one unit, box 1, slots 0-3 = IFS1..IFS4) takes the place of the CFS.
bool ifs_active();
// The Flashforge agent of the selected printer (null for any other printer).
std::shared_ptr<Slic3r::FlashforgePrinterAgent> ifs_agent();
// Header of the sidebar column of a unit: "CFS n", or "IFS" for the Flashforge unit (empty for the unused second column).
wxString unit_title(int box);

// Multi-colour filaments: all colours of filament idx (project filament_multi_colour, falls back to filament_colour)
// and whether they form a gradient (filament_colour_type "0") instead of side-by-side coextruded colours.
std::vector<wxColour> filament_colours(int idx, bool& gradient);
// Modal editor: type (solid / dual / tricolor / quadricolor / gradient), colours and metres per gradient cycle.
// On OK writes filament_gradient_length and returns the colours (first = main colour); false when cancelled.
// `official` (Bambu official filament presets): adds a button that closes the dialog and runs it instead.
bool edit_filament_colour(wxWindow* parent, int idx, std::vector<std::string>& colours, bool& gradient, const std::function<void()>& official = nullptr);

// Bambu printer with AMS data connected: the badges use its slots (box = ams id + 1, 0 = external spool, labels A1..D4 / HT-A / Ext).
bool bambu_active();
// Mapped slot as {ams id, slot id} (ams id 254/255 = external spool); empty when unmapped or the slot holds no filament.
std::optional<std::pair<int, int>> mapped_ams(int filament_idx);
void set_mapped_ams(int filament_idx, int ams, int slot);
// Local multi-colour memory per printer tray: call after the colours of a filament mapped to a slot changed ...
void remember_colours(int filament_idx);
// ... and read it back for a tray that reports no colours of its own, if its single colour is still the stored first one.
bool restore_colours(const std::string& dev_id, int ams, int slot, const std::string& tray_colour, std::vector<std::string>& cols, std::string& type);

// ---- Spoolman spools bound to printer slots ----
// Flashforge/ZMOD: the lane_data spool_id of the IFS lane (read back from the printer); Creality and Bambu keep the link
// locally (app config section "spoolman_slots", key "<printer>:<box>:<slot>"). Spoolman itself is never written.
// spool id -> label of the slot that holds it ("IFS2", "A1"), for the selected printer.
std::map<int, wxString> spool_slot_labels();
// The spool bound to the slot filament idx is mapped to (0 = none / unmapped).
int mapped_slot_spool(int filament_idx);
// Writes the spool of project filament idx to the slot it is mapped to (spool_id 0 = unbind that slot). Safe to call when the
// filament is not mapped or the printer has no slots (nothing happens).
void link_spool_to_slot(int filament_idx, int spool_id);

// Box id per filament (0 = external spool, -1 = unmapped) for the first n filaments; false when there is no CFS data.
bool grouping(int n, std::vector<int>& box_of);

// Invisible helper: polls the printer every 2 s, refreshes the badges and calls on_change when the
// CFS grouping (or its availability) changed. Also called right after a slot was picked.
class Watcher : public wxPanel
{
public:
    Watcher(wxWindow* parent, std::function<int()> count, std::function<void()> on_change);
    ~Watcher() override;
    void check(bool force = false);

private:
    wxTimer               m_timer;
    std::function<int()>  m_count;
    std::function<void()> m_change;
    size_t                m_sig{0};
    bool                  m_busy{false};
};

// Shows the combined badges and hides the original colour boxes (or the reverse) according to whether the printer
// has CFS/slot data. Returns true when any widget changed visibility (the caller then lays the panel out).
bool sync_badges();

// Combined "2 . 1A <->" box (filament no. + slot) replacing the colour box on Creality rows; click opens the slot picker.
class Badge : public wxPanel
{
public:
    Badge(wxWindow* parent, int filament_idx, wxWindow* colour_btn);
    ~Badge() override;
    bool apply(bool want);
    void retire();

private:
    void on_paint(wxPaintEvent&);
    int  m_idx;
    wxWeakRef<wxWindow> m_btn; // the original colour box, hidden while this combined box is shown
};

}}} // namespace Slic3r::GUI::CfsUi

#endif
