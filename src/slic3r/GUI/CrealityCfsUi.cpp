#include "CrealityCfsUi.hpp"

#include "CrealityDevicePanel.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsClient.hpp"

#include <wx/dcbuffer.h>
#include <wx/popupwin.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace Slic3r { namespace GUI { namespace CfsUi {

using namespace CrealityCfs;

namespace {

std::set<Badge*>                   s_badges;

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

// Explicit slot choices live in the project config (option filament_cfs_slot, one "box:slot" string per
// filament, empty = automatic), so they are saved in the 3MF. Slicing never reads this option.
ConfigOptionStrings* cfs_option(bool create)
{
    PresetBundle* b = wxGetApp().preset_bundle;
    return b == nullptr ? nullptr : b->project_config.option<ConfigOptionStrings>("filament_cfs_slot", create);
}

std::optional<std::pair<int, int>> parse_slot(const std::string& text)
{
    int b = 0, s = 0;
    char sep = 0;
    std::istringstream ss(text);
    if (ss >> b >> sep >> s && sep == ':')
        return std::make_pair(b, s);
    return std::nullopt;
}

// Old versions kept the choices in the app config, keyed by a hash of the project file name.
// Move them into the project the first time it is looked at, then forget the old entry.
void migrate_legacy_map()
{
    static std::string migrated{"?"};
    std::string        name;
    if (wxGetApp().plater() != nullptr)
        name = wxGetApp().plater()->get_project_filename().ToUTF8().data();
    const std::string key = std::to_string(std::hash<std::string>{}(name));
    if (key == migrated)
        return;
    migrated = key;
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    std::string text = cfg->get("creality_cfs_map", key);
    if (text.empty())
        return;
    if (auto* opt = cfs_option(false); opt != nullptr && std::any_of(opt->values.begin(), opt->values.end(), [](const std::string& v) { return !v.empty(); }))
        return; // the project already has its own mapping
    std::replace(text.begin(), text.end(), ':', ' ');
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream ss(text);
    int i, b, s;
    ConfigOptionStrings* opt = cfs_option(true);
    while (opt != nullptr && ss >> i >> b >> s)
        if (i >= 0 && i < 64) {
            if (opt->values.size() <= size_t(i))
                opt->values.resize(size_t(i) + 1);
            opt->values[i] = std::to_string(b) + ":" + std::to_string(s);
        }
    cfg->erase("creality_cfs_map", key);
}

std::optional<std::pair<int, int>> chosen_slot(int idx)
{
    migrate_legacy_map();
    const auto* opt = cfs_option(false);
    if (opt == nullptr || idx < 0 || size_t(idx) >= opt->values.size())
        return std::nullopt;
    return parse_slot(opt->values[idx]);
}

void set_slot(int idx, int box, int slot)
{
    migrate_legacy_map();
    ConfigOptionStrings* opt = cfs_option(true);
    if (opt == nullptr || idx < 0)
        return;
    if (opt->values.size() <= size_t(idx))
        opt->values.resize(size_t(idx) + 1);
    opt->values[idx] = std::to_string(box) + ":" + std::to_string(slot);
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->set_plater_dirty(true);
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

wxColour bg_col() { return StateColor::darkModeColorFor(*wxWHITE); }
wxColour fg_col() { return StateColor::darkModeColorFor(wxColour("#363636")); }
wxColour edge_col() { return StateColor::darkModeColorFor(wxColour("#CECECE")); }

wxColour text_on(const wxColour& c) { return (c.Red() * 299 + c.Green() * 587 + c.Blue() * 114) / 1000 > 150 ? *wxBLACK : *wxWHITE; }

void draw_chip(wxDC& dc, const wxRect& r, const wxString& txt, const wxColour& fill, bool loaded)
{
    const bool filled = loaded && fill.IsOk();
    dc.SetPen(wxPen(edge_col()));
    dc.SetBrush(filled ? wxBrush(fill) : *wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(r, wxGetApp().em_unit() / 3.0);
    dc.SetTextForeground(filled ? text_on(fill) : fg_col());
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
    if (const auto chosen = chosen_slot(idx)) {
        for (const Entry& x : es)
            if (x.box == chosen->first && x.slot == chosen->second) {
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
    Picker(wxWindow* parent, int idx, std::vector<Entry> es, std::function<void()> done, std::function<void()> edit)
        : wxPopupTransientWindow(parent, wxBORDER_SIMPLE), m_idx(idx), m_es(std::move(es)), m_done(std::move(done)), m_edit(std::move(edit))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        for (const Entry& e : m_es)
            if (std::find(m_tabs.begin(), m_tabs.end(), e.box) == m_tabs.end())
                m_tabs.push_back(e.box);
        std::sort(m_tabs.begin(), m_tabs.end(), [](int a, int b) { return a == 0 ? false : (b == 0 ? true : a < b); }); // EXT last
        if (!m_tabs.empty())
            m_tab = m_tabs.front();
        SetSize(FromDIP(wxSize(190, 32 + 5 * 28 + 6)));
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
    wxRect edit_rect() const { return {FromDIP(6), FromDIP(32) + 4 * FromDIP(28) + FromDIP(2), GetClientSize().x - FromDIP(12), FromDIP(24)}; }
    void   paint(wxDC& dc)
    {
        dc.SetBackground(bg_col());
        dc.Clear();
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            const wxRect r = tab_rect(i);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(m_tabs[i] == m_tab ? wxColour(0, 150, 136) : StateColor::darkModeColorFor(wxColour("#EBEBEB"))));
            dc.DrawRectangle(r);
            dc.SetTextForeground(m_tabs[i] == m_tab ? *wxWHITE : fg_col());
            const wxString t  = m_tabs[i] == 0 ? wxString("EXT") : wxString::Format("CFS%d", m_tabs[i]);
            const wxSize   ts = dc.GetTextExtent(t);
            dc.DrawText(t, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2);
        }
        const auto v = shown();
        for (size_t i = 0; i < v.size(); ++i) {
            const wxRect r = row_rect(i);
            draw_chip(dc, wxRect(r.x, r.y, FromDIP(40), r.height), v[i]->label, v[i]->colour, v[i]->loaded);
            dc.SetTextForeground(fg_col());
            dc.DrawText(v[i]->loaded ? wxString::FromUTF8(v[i]->type.c_str()) : wxString("-"), r.x + FromDIP(48), r.y + FromDIP(4));
        }
        if (m_edit) {
            const wxRect r = edit_rect();
            dc.SetPen(wxPen(edge_col()));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(r, wxGetApp().em_unit() / 3.0);
            dc.SetTextForeground(fg_col());
            const wxString t = _L("Edit color");
            const wxSize   ts = dc.GetTextExtent(t);
            dc.DrawText(t, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2);
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
        if (m_edit && edit_rect().Contains(e.GetPosition())) {
            auto edit = m_edit;
            Dismiss();
            wxTheApp->CallAfter(edit);
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
    std::function<void()> m_done, m_edit;
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
    SetBackgroundColour(bg_col());
    SetMinSize(FromDIP(wxSize(-1, 26)));
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
    dc.SetBackground(bg_col());
    dc.Clear();
    dc.SetFont(GetFont());
    const auto es  = entries(snapshot());
    const int  cw  = FromDIP(26), ch = FromDIP(18), gap = FromDIP(4), y = (GetClientSize().y - ch) / 2;
    int        x   = FromDIP(12), cur_box = -1;
    for (const Entry& e : es) {
        if (e.box != cur_box) {
            cur_box        = e.box;
            const wxString t  = e.box == 0 ? wxString("EXT") : wxString::Format("CFS%d", e.box);
            const wxSize   ts = dc.GetTextExtent(t);
            dc.SetTextForeground(fg_col());
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

Badge::Badge(wxWindow* parent, int idx, wxWindow* colour_btn) : wxPanel(parent, wxID_ANY), m_idx(idx), m_btn(colour_btn)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(64, 20)));
    SetToolTip(_L("Printer slot for this filament"));
    Bind(wxEVT_PAINT, &Badge::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        auto* p = new Picker(this, m_idx, entries(snapshot()), [this] {
            for (Badge* b : s_badges)
                b->Refresh();
        }, [this] {
            wxCommandEvent ev(wxEVT_BUTTON, m_btn->GetId());
            ev.SetEventObject(m_btn);
            m_btn->GetEventHandler()->ProcessEvent(ev);
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
        m_btn->Show(!want);
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
    Refresh();
}

void Badge::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(bg_col());
    dc.Clear();
    const auto   es = entries(snapshot());
    const Entry* e  = nullptr;
    const wxString t = label_of(m_idx, es, &e);
    PresetBundle*  b = wxGetApp().preset_bundle;
    const auto* cols = b->project_config.option<ConfigOptionStrings>("filament_colour");
    wxColour col = cols != nullptr && m_idx < (int) cols->values.size() ? wxColour(cols->values[m_idx]) : wxColour();
    draw_chip(dc, wxRect(wxPoint(0, 0), GetClientSize()), wxString::Format("%d Â· ", m_idx + 1) + t + wxString::FromUTF8(" â"), col, true);
}

}}} // namespace Slic3r::GUI::CfsUi
