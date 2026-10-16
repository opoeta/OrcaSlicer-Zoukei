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

namespace Slic3r { namespace GUI { namespace CfsUi {

// Project filament index -> printer slot {box_id, slot_id}; box 0 is the external spool.
// Explicit choices persist per project file (app config); otherwise a default is derived.
std::optional<std::pair<int, int>> mapped_slot(int filament_idx);

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
};

// Combined "2 . 1A <->" box (filament no. + slot) replacing the colour box on Creality rows; click opens the slot picker.
class Badge : public wxPanel
{
public:
    Badge(wxWindow* parent, int filament_idx, wxWindow* colour_btn);
    ~Badge() override;
    void refresh_state();

private:
    void on_paint(wxPaintEvent&);
    int  m_idx;
    wxWindow* m_btn; // the original colour box, hidden while this combined box is shown
};

}}} // namespace Slic3r::GUI::CfsUi

#endif
