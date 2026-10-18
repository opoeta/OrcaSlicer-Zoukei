// Orca Zoukei: compact print marker timeline. See PrintMarkerTimeline.hpp.
#include "PrintMarkerTimeline.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#include <algorithm>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

namespace {
std::vector<PrintMarkerTimeline*>& instances()
{
    static std::vector<PrintMarkerTimeline*> list;
    return list;
}
} // namespace

PrintMarkerTimeline::PrintMarkerTimeline(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFont(::Label::Body_12);
    SetMinSize(wxSize(-1, FromDIP(44)));
    Bind(wxEVT_PAINT, &PrintMarkerTimeline::on_paint, this);
    Bind(wxEVT_MOTION, &PrintMarkerTimeline::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &PrintMarkerTimeline::on_leave, this);
    instances().push_back(this);
    Hide();
}

PrintMarkerTimeline::~PrintMarkerTimeline()
{
    auto& list = instances();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

void PrintMarkerTimeline::refresh_all()
{
    for (PrintMarkerTimeline* t : instances())
        t->update_visibility();
}

void PrintMarkerTimeline::set_result(const GCodeProcessorResult* result)
{
    // Rebuilding walks every move: skip it while the same result is shown again (view switches reload the preview).
    const size_t sig = result == nullptr ? 0 : result->moves.size() * 31u + result->custom_gcode_per_print_z.size() * 7u +
                                                    size_t(result->print_statistics.modes[0].time);
    if (result == m_result && sig == m_sig) {
        update_visibility();
        return;
    }
    m_result = result;
    m_sig    = sig;
    m_set    = (result != nullptr && !result->moves.empty()) ? PrintMarkers::build(*result) : PrintMarkers::MarkerSet();
    m_hover  = -1;
    update_visibility();
}

void PrintMarkerTimeline::update_visibility()
{
    const bool show = PrintMarkers::enabled() && !m_set.empty();
    if (show != IsShown()) {
        Show(show);
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
    if (show)
        Refresh();
}

wxRect PrintMarkerTimeline::track_rect() const
{
    const wxString caption = wxString::Format(_L("Stops: %d"), int(m_set.markers.size()));
    const int      pad     = FromDIP(12);
    const int      left    = pad + GetTextExtent(caption).x + FromDIP(12);
    const int      right   = GetClientSize().x - pad;
    return wxRect(left, FromDIP(9), std::max(FromDIP(20), right - left), FromDIP(8));
}

int PrintMarkerTimeline::marker_at(const wxPoint& pt) const
{
    const wxRect track = track_rect();
    int          best  = -1;
    int          dist  = FromDIP(8);
    for (size_t i = 0; i < m_set.markers.size(); ++i) {
        const int x = track.x + int(track.width * m_set.markers[i].frac);
        const int d = std::abs(pt.x - x);
        if (d <= dist && std::abs(pt.y - (track.y + track.height / 2)) <= FromDIP(12)) {
            dist = d;
            best = int(i);
        }
    }
    return best;
}

void PrintMarkerTimeline::on_motion(wxMouseEvent& evt)
{
    const int hover = marker_at(evt.GetPosition());
    if (hover != m_hover) {
        m_hover = hover;
        SetToolTip(hover >= 0 ? PrintMarkers::describe(m_set.markers[size_t(hover)]) : wxString());
        Refresh();
    }
    evt.Skip();
}

void PrintMarkerTimeline::on_leave(wxMouseEvent&)
{
    if (m_hover != -1) {
        m_hover = -1;
        Refresh();
    }
}

void PrintMarkerTimeline::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC pdc(this);
    const bool            dark = wxGetApp().dark_mode();

    pdc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour(255, 255, 255))));
    pdc.Clear();
    if (m_set.empty())
        return;

    wxGCDC dc(pdc);
    const wxColour text_col  = StateColor::darkModeColorFor(wxColour(107, 107, 107));
    const wxColour track_col = StateColor::darkModeColorFor(wxColour(222, 222, 222));
    const wxColour outline   = PrintMarkers::outline_colour(dark);

    const wxRect track = track_rect();
    const int    pad   = FromDIP(12);

    // Caption.
    dc.SetFont(::Label::Body_12);
    dc.SetTextForeground(text_col);
    const wxString caption = wxString::Format(_L("Stops: %d"), int(m_set.markers.size()));
    dc.DrawText(caption, pad, track.y + track.height / 2 - dc.GetTextExtent(caption).y / 2);

    // Track.
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(track_col));
    dc.DrawRoundedRectangle(track, track.height / 2.0);

    // Markers.
    const int r = FromDIP(5);
    dc.SetFont(::Label::Body_10);
    int label_end = track.x - FromDIP(8);
    const wxString total_text = m_set.has_time ? PrintMarkers::format_duration(m_set.total_time) : wxString();
    const int      total_w    = total_text.empty() ? 0 : dc.GetTextExtent(total_text).x;
    const int      label_y    = track.y + track.height + FromDIP(5);
    for (size_t i = 0; i < m_set.markers.size(); ++i) {
        const PrintMarkers::Marker& m = m_set.markers[i];
        const int                   x = track.x + int(track.width * m.frac);
        dc.SetPen(wxPen(outline, FromDIP(2)));
        dc.SetBrush(wxBrush(PrintMarkers::colour(m)));
        const int rr = int(i) == m_hover ? r + FromDIP(1) : r;
        dc.DrawCircle(wxPoint(x, track.y + track.height / 2), rr);

        if (m_set.has_time) {
            const wxString t  = PrintMarkers::format_duration(m.time_s);
            const wxSize   ts = dc.GetTextExtent(t);
            const int      lx = std::min(x - ts.x / 2, track.GetRight() - total_w - FromDIP(6) - ts.x);
            if (lx >= label_end + FromDIP(6)) {
                dc.SetTextForeground(text_col);
                dc.DrawText(t, lx, label_y);
                label_end = lx + ts.x;
            }
        }
    }
    if (!total_text.empty()) {
        dc.SetTextForeground(text_col);
        dc.DrawText(total_text, track.GetRight() - total_w, label_y);
    }
}

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
