#include "CrealityCfsUi.hpp"

#include "CrealityDevicePanel.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsClient.hpp"

#include <wx/dcbuffer.h>
#include <wx/popupwin.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace Slic3r { namespace GUI { namespace CfsUi {

using namespace CrealityCfs;

namespace {

std::set<Badge*>                   s_badges;
std::map<int, std::pair<int, int>> s_map; // explicit choices for the current project
std::string                        s_map_project{"?"};

PrinterSnapshot snapshot()
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (b == nullptr || !CrealityDevicePanel::is_creality_printer(*b))
        return {};
    const DynamicPrintConfig* c = nullptr;
    if (b->physical_printers.has_selection())
        c = b->physical_printers.get_selected_printer_config();
    if (c == nullptr || !c->has("print_host") || c->opt_string("print_host").empty())
        c = &b->printers.get_edited_preset().config;
    const std::string host = c->has("print_host") ? Client::strip_host(c->opt_string("print_host")) : std::string();
    if (host.empty())
        return {};
    auto client = Client::get(host);
    return client ? client->snapshot() : PrinterSnapshot{};
}

std::string project_key()
{
    std::string name;
    if (wxGetApp().plater() != nullptr)
        name = wxGetApp().plater()->get_project_filename().ToUTF8().data();
    return std::to_string(std::hash<std::string>{}(name));
}

void load_map()
{
    const std::string key = project_key();
    if (key == s_map_project)
        return;
    s_map_project = key;
    s_map.clear();
    // "idx:box:slot," triples
    std::string text = wxGetApp().app_config->get("creality_cfs_map", key);
    std::replace(text.begin(), text.end(), ':', ' ');
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream ss(text);
    int i, b, s;
    while (ss >> i >> b >> s)
        s_map[i] = {b, s};
}

void set_slot(int idx, int box, int slot)
{
    load_map();
    s_map[idx] = {box, slot};
    std::ostringstream ss;
    for (auto& [i, p] : s_map)
        ss << i << ':' << p.first << ':' << p.second << ',';
    wxGetApp().app_config->set("creality_cfs_map", s_map_project, ss.str());
}

struct Entry
{
    int         box, slot;
    wxString    label;
    wxColour    colour;
    std::string type;
    bool        loaded;
};

// Ext first, then every CFS slot.
std::vector<Entry> entries(const PrinterSnapshot& snap)
{
    std::vector<Entry> out;
    for (const CfsBox& box : snap.boxes)
        if (box.type == BOX_TYPE_EXTERNAL) {
            for (const CfsSlot& s : box.slots)
                out.push_back({0, s.slot_id, "Ext", s.color.empty() ? wxColour() : wxColour(s.color), s.type, s.is_loaded()});
            break;
        }
    for (const CfsBox& box : snap.cfs_boxes())
        for (const CfsSlot& s : box.slots)
            out.push_back({box.id, s.slot_id, wxString::FromUTF8(s.label().c_str()), s.color.empty() ? wxColour() : wxColour(s.color),
                           s.type, s.is_loaded()});
    return out;
}

wxColour text_on(const wxColour& c) { return (c.Red() * 299 + c.Green() * 587 + c.Blue() * 114) / 1000 > 150 ? *wxBLACK : *wxWHITE; }

void draw_chip(wxDC& dc, const wxRect& r, const wxString& txt, const wxColour& fill, bool loaded)
{
    const bool filled = loaded && fill.IsOk();
    dc.SetPen(wxPen(wxColour(170, 170, 170)));
    dc.SetBrush(filled ? wxBrush(fill) : *wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(r, 4);
    dc.SetTextForeground(filled ? text_on(fill) : wxColour(120, 120, 120));
    const wxSize ts = dc.GetTextExtent(txt);
    dc.DrawText(txt, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2);
}

// Label of the slot filament `idx` maps to (explicit choice, else same colour+type, else the idx-th CFS slot).
wxString label_of(int idx, const std::vector<Entry>& es, const Entry** found = nullptr)
{
    if (found)
        *found = nullptr;
    PresetBundle* b = wxGetApp().preset_bundle;
    if (es.empty() || b == nullptr)
        return "-";
    const Entry* e = nullptr;
    load_map();
    if (auto it = s_map.find(idx); it != s_map.end()) {
        for (const Entry& x : es)
            if (x.box == it->second.first && x.slot == it->second.second) {
                e = &x;
                break;
            }
    } else {
        std::string type;
        if (idx < (int) b->filament_presets.size())
            if (const Preset* p = b->filaments.find_preset(b->filament_presets[idx]))
                if (const auto* o = p->config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
                    type = o->values.front();
        const auto* cols = b->project_config.option<ConfigOptionStrings>("filament_colour");
        const wxColour col = cols != nullptr && idx < (int) cols->values.size() ? wxColour(cols->values[idx]) : wxColour();
        for (const Entry& x : es)
            if (x.box != 0 && x.loaded && x.type == type && x.colour.IsOk() && col.IsOk() && x.colour == col) {
                e = &x;
                break;
            }
        if (e == nullptr) {
            int n = 0;
            for (const Entry& x : es)
                if (x.box != 0 && n++ == idx) {
                    e = &x;
                    break;
                }
        }
    }
    if (found)
        *found = e;
    return e != nullptr ? e->label : wxString("-");
}

class Picker : public wxPopupTransientWindow
{
public:
    Picker(wxWindow* parent, int idx, std::vector<Entry> es, std::function<void()> done)
        : wxPopupTransientWindow(parent, wxBORDER_SIMPLE), m_idx(idx), m_es(std::move(es)), m_done(std::move(done))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        for (const Entry& e : m_es)
            if (std::find(m_tabs.begin(), m_tabs.end(), e.box) == m_tabs.end())
                m_tabs.push_back(e.box);
        std::sort(m_tabs.begin(), m_tabs.end(), [](int a, int b) { return a == 0 ? false : (b == 0 ? true : a < b); }); // EXT last
        if (!m_tabs.empty())
            m_tab = m_tabs.front();
        SetSize(FromDIP(wxSize(190, 32 + 4 * 28 + 6)));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            paint(dc);
        });
        Bind(wxEVT_LEFT_DOWN, &Picker::on_click, this);
    }

private:
    wxRect tab_rect(size_t i) const
    {
        const int w = GetClientSize().x / (int) std::max<size_t>(1, m_tabs.size());
        return {int(i) * w, 0, w, FromDIP(28)};
    }
    std::vector<const Entry*> shown() const
    {
        std::vector<const Entry*> v;
        for (const Entry& e : m_es)
            if (e.box == m_tab)
                v.push_back(&e);
        return v;
    }
    wxRect row_rect(size_t i) const { return {FromDIP(6), FromDIP(32) + int(i) * FromDIP(28), GetClientSize().x - FromDIP(12), FromDIP(24)}; }
    void   paint(wxDC& dc)
    {
        dc.SetBackground(*wxWHITE);
        dc.Clear();
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            const wxRect r = tab_rect(i);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(m_tabs[i] == m_tab ? wxColour(0, 150, 136) : wxColour(235, 235, 235)));
            dc.DrawRectangle(r);
            dc.SetTextForeground(m_tabs[i] == m_tab ? *wxWHITE : *wxBLACK);
            const wxString t  = m_tabs[i] == 0 ? wxString("EXT") : wxString::Format("CFS%d", m_tabs[i]);
            const wxSize   ts = dc.GetTextExtent(t);
            dc.DrawText(t, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2);
        }
        const auto v = shown();
        for (size_t i = 0; i < v.size(); ++i) {
            const wxRect r = row_rect(i);
            draw_chip(dc, wxRect(r.x, r.y, FromDIP(40), r.height), v[i]->label, v[i]->colour, v[i]->loaded);
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText(v[i]->loaded ? wxString::FromUTF8(v[i]->type.c_str()) : wxString("-"), r.x + FromDIP(48), r.y + FromDIP(4));
        }
    }
    void on_click(wxMouseEvent& e)
    {
        for (size_t i = 0; i < m_tabs.size(); ++i)
            if (tab_rect(i).Contains(e.GetPosition())) {
                m_tab = m_tabs[i];
                Refresh();
                return;
            }
        const auto v = shown();
        for (size_t i = 0; i < v.size(); ++i)
            if (row_rect(i).Contains(e.GetPosition())) {
                set_slot(m_idx, v[i]->box, v[i]->slot);
                auto done = m_done;
                Dismiss();
                done();
                return;
            }
    }
    int                   m_idx;
    std::vector<Entry>    m_es;
    std::function<void()> m_done;
    std::vector<int>      m_tabs;
    int                   m_tab{0};
};

} // namespace

std::optional<std::pair<int, int>> mapped_slot(int idx)
{
    const Entry* e  = nullptr;
    const auto   es = entries(snapshot());
    label_of(idx, es, &e);
    if (e == nullptr)
        return std::nullopt;
    return std::make_pair(e->box, e->slot);
}

// ---- Strip -------------------------------------------------------------------------------------

Strip::Strip(wxWindow* parent) : wxPanel(parent, wxID_ANY), m_timer(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(-1, 34)));
    Bind(wxEVT_PAINT, &Strip::on_paint, this);
    Bind(wxEVT_TIMER, &Strip::on_tick, this);
    m_timer.Start(2000);
    Hide();
}

void Strip::on_tick(wxTimerEvent&)
{
    const PrinterSnapshot snap = snapshot();
    const auto            es   = entries(snap);
    const bool            want = snap.connected && !es.empty();
    size_t                sig  = want ? es.size() + 1 : 0;
    for (const Entry& e : es)
        sig = sig * 131 + std::hash<std::string>{}(e.type) + (e.colour.IsOk() ? e.colour.GetRGB() : 0) + e.box;
    if (want != IsShown()) {
        Show(want);
        if (GetParent() != nullptr) {
            GetParent()->Layout();
            GetParent()->FitInside();
        }
    }
    if (sig != m_sig) {
        m_sig = sig;
        Refresh();
    }
    for (Badge* b : s_badges)
        b->refresh_state();
}

void Strip::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(*wxWHITE);
    dc.Clear();
    dc.SetFont(GetFont());
    const auto es  = entries(snapshot());
    const int  cw  = FromDIP(30), ch = FromDIP(22), gap = FromDIP(4), y = (GetClientSize().y - ch) / 2;
    int        x   = FromDIP(12), cur_box = -1;
    for (const Entry& e : es) {
        if (e.box != cur_box) {
            cur_box        = e.box;
            const wxString t  = e.box == 0 ? wxString("EXT") : wxString::Format("CFS%d", e.box);
            const wxSize   ts = dc.GetTextExtent(t);
            dc.SetTextForeground(wxColour(90, 90, 90));
            dc.DrawText(t, x, y + (ch - ts.y) / 2);
            x += ts.x + gap * 2;
        }
        draw_chip(dc, wxRect(x, y, cw, ch), e.label == "Ext" ? wxString("") : e.label, e.colour, e.loaded);
        x += cw + gap;
        if (e.box == 0)
            x += gap * 2;
    }
}

// ---- Badge -------------------------------------------------------------------------------------

Badge::Badge(wxWindow* parent, int idx) : wxPanel(parent, wxID_ANY), m_idx(idx)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(46, 22)));
    SetToolTip("Printer slot for this filament");
    Bind(wxEVT_PAINT, &Badge::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        auto* p = new Picker(this, m_idx, entries(snapshot()), [this] {
            for (Badge* b : s_badges)
                b->Refresh();
        });
        p->Position(ClientToScreen(wxPoint(0, GetSize().y)), wxSize(0, 0));
        p->Popup();
    });
    s_badges.insert(this);
    Hide();
}

Badge::~Badge() { s_badges.erase(this); }

void Badge::refresh_state()
{
    const bool want = !entries(snapshot()).empty();
    if (want != IsShown()) {
        Show(want);
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
    Refresh();
}

void Badge::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(*wxWHITE);
    dc.Clear();
    const auto   es = entries(snapshot());
    const Entry* e  = nullptr;
    const wxString t = label_of(m_idx, es, &e);
    draw_chip(dc, wxRect(wxPoint(0, 0), GetClientSize()), t + wxString::FromUTF8(" \xE2\x87\x84"), e != nullptr ? e->colour : wxColour(),
              e != nullptr && e->loaded);
}

}}} // namespace Slic3r::GUI::CfsUi
