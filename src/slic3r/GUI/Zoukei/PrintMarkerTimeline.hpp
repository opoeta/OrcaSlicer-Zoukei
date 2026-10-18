// Orca Zoukei: compact timeline under the slice preview. One bar for the whole print time with a marker at each
// pause / colour change / tool change / custom G-code and the time it happens. Hidden when the sliced plate has
// no such stops or when "Show pause and color change markers" is off in Preferences.
#ifndef slic3r_GUI_Zoukei_PrintMarkerTimeline_hpp_
#define slic3r_GUI_Zoukei_PrintMarkerTimeline_hpp_

#include <wx/panel.h>

#include "PrintMarkers.hpp"

namespace Slic3r {

struct GCodeProcessorResult;

namespace GUI {
namespace Zoukei {

class PrintMarkerTimeline : public wxPanel
{
public:
    explicit PrintMarkerTimeline(wxWindow* parent);
    ~PrintMarkerTimeline() override;

    // Rebuilds the markers from a sliced plate (nullptr or an empty result hides the bar).
    void set_result(const GCodeProcessorResult* result);

    // Re-applies the Preferences switch to every timeline.
    static void refresh_all();

private:
    void update_visibility();
    void on_paint(wxPaintEvent&);
    void on_motion(wxMouseEvent& evt);
    void on_leave(wxMouseEvent&);
    int  marker_at(const wxPoint& pt) const;
    wxRect track_rect() const;

    PrintMarkers::MarkerSet m_set;
    int                     m_hover{-1};
    const GCodeProcessorResult* m_result{nullptr}; // identity only, never dereferenced outside set_result
    size_t                  m_sig{0};
};

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_PrintMarkerTimeline_hpp_
