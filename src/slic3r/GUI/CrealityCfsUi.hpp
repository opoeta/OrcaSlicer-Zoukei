// Sidebar widgets for Creality printers: a per-filament
// slot badge/picker. Own code, inspired by Creality Print's PrinterBoxFilamentPanel / MaterialMapPanel.
#ifndef slic3r_GUI_CrealityCfsUi_hpp_
#define slic3r_GUI_CrealityCfsUi_hpp_

#include <cstddef>
#include <functional>
#include <vector>
#include <optional>
#include <utility>
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/weakref.h>

namespace Slic3r { namespace GUI { namespace CfsUi {

// Project filament index -> printer slot {box_id, slot_id}; box 0 is the external spool.
// Explicit choices persist per project file (app config); otherwise a default is derived.
std::optional<std::pair<int, int>> mapped_slot(int filament_idx);

// The same as the print dialog's tray id: CFS unit index * 4 + slot, 255 for the external spool.
std::optional<int> mapped_tray(int filament_idx);
// Stores a choice made in the print dialog (tray id as above) in the project's filament_cfs_slot.
void set_mapped_tray(int filament_idx, int tray_id);

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
