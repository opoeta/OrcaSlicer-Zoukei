// Sidebar widgets for Creality printers: a live CFS strip (EXT + CFS1/CFS2 chips) and a per-filament
// slot badge/picker. Own code, inspired by Creality Print's PrinterBoxFilamentPanel / MaterialMapPanel.
#ifndef slic3r_GUI_CrealityCfsUi_hpp_
#define slic3r_GUI_CrealityCfsUi_hpp_

#include <cstddef>
#include <optional>
#include <utility>
#include <wx/panel.h>
#include <wx/timer.h>

namespace Slic3r { namespace GUI { namespace CfsUi {

// Project filament index -> printer slot {box_id, slot_id}; box 0 is the external spool.
// Explicit choices persist per project file (app config); otherwise a default is derived.
std::optional<std::pair<int, int>> mapped_slot(int filament_idx);

// Live strip at the top of the Filament section; hidden unless a Creality printer with CFS data is connected.
class Strip : public wxPanel
{
public:
    explicit Strip(wxWindow* parent);

private:
    void    on_paint(wxPaintEvent&);
    void    on_tick(wxTimerEvent&);
    wxTimer m_timer;
    size_t  m_sig{0};
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
