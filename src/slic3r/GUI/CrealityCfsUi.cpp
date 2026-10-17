#include "CrealityCfsUi.hpp"

#include "CrealityDevicePanel.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsClient.hpp"
#include "slic3r/Utils/FlashforgePrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include "FilamentBitmapUtils.hpp"

#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
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
Watcher*                           s_watcher = nullptr;

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

std::shared_ptr<FlashforgePrinterAgent> flashforge_agent()
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (b == nullptr || CrealityDevicePanel::is_creality_printer(*b) || !is_flashforge_printer_selected())
        return nullptr;
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent != nullptr ? std::dynamic_pointer_cast<FlashforgePrinterAgent>(agent->get_printer_agent()) : nullptr;
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

// The slots to choose from: the IFS of a Flashforge printer (box 1 = IFS1..IFS4, box 0 = external spool), else the CFS.
std::vector<Entry> current_entries()
{
    if (auto ff = flashforge_agent()) {
        std::vector<Entry> out;
        const auto         slots = ff->ifs_slots();
        if (slots.empty() && ff->mode() == FlashforgePrinterAgent::Mode::unresolved)
            return out;
        out.push_back({0, 0, "Ext", wxColour(), std::string(), false});
        for (const auto& s : slots)
            out.push_back({1, s.port - 1, wxString::Format("IFS%d", s.port), s.color.empty() ? wxColour() : wxColour(s.color), s.material, s.has_filament});
        return slots.empty() ? std::vector<Entry>() : out;
    }
    return entries(snapshot());
}

wxColour bg_col() { return StateColor::darkModeColorFor(*wxWHITE); }
wxColour fg_col() { return StateColor::darkModeColorFor(wxColour("#363636")); }
wxColour edge_col() { return StateColor::darkModeColorFor(wxColour("#CECECE")); }

wxColour text_on(const wxColour& c) { return (c.Red() * 299 + c.Green() * 587 + c.Blue() * 114) / 1000 > 150 ? *wxBLACK : *wxWHITE; }

void draw_chip(wxDC& dc, const wxRect& r, const wxString& txt, const wxColour& fill, bool loaded, const wxBitmap* multi = nullptr)
{
    const bool filled = loaded && fill.IsOk();
    dc.SetPen(wxPen(edge_col()));
    dc.SetBrush(filled ? wxBrush(fill) : *wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(r, wxGetApp().em_unit() / 5.0);
    if (multi != nullptr && multi->IsOk() && filled) { // coextruded sectors / gradient swatch inside the outline
        dc.DrawBitmap(*multi, r.x + 1, r.y + 1);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(r, wxGetApp().em_unit() / 3.0);
    }
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

// Mapped to a real (non-external) slot that holds no filament.
bool entry_empty(const Entry* e) { return e != nullptr && e->box != 0 && !e->loaded; }

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
            const wxString t  = m_tabs[i] == 0 ? wxString("EXT") : flashforge_agent() ? wxString("IFS") : wxString::Format("CFS%d", m_tabs[i]);
            const wxSize   ts = dc.GetTextExtent(t);
            dc.DrawText(t, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2);
        }
        const auto v = shown();
        for (size_t i = 0; i < v.size(); ++i) {
            const wxRect r = row_rect(i);
            const bool ext = v[i]->box == 0;
            draw_chip(dc, wxRect(r.x, r.y, FromDIP(48), r.height), v[i]->label, v[i]->colour, v[i]->loaded);
            dc.SetTextForeground(v[i]->loaded || ext ? fg_col() : wxColour(150, 150, 150));
            dc.DrawText(v[i]->loaded ? wxString::FromUTF8(v[i]->type.c_str()) : (ext ? wxString("-") : _L("Empty") + L" \u26A0"), r.x + FromDIP(56), r.y + FromDIP(4));
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
    const auto   es = current_entries();
    label_of(idx, es, &e);
    if (e == nullptr)
        return std::nullopt;
    return std::make_pair(e->box, e->slot);
}

bool mapped_slot_empty(int idx)
{
    const Entry* e  = nullptr;
    const auto   es = current_entries();
    label_of(idx, es, &e);
    return entry_empty(e);
}

wxString mapped_slot_label(int idx)
{
    const auto es = current_entries();
    return label_of(idx, es);
}

std::optional<int> mapped_tray(int idx)
{
    const auto slot = mapped_slot(idx);
    if (!slot)
        return std::nullopt;
    if (slot->first == 0)
        return 255;
    if (flashforge_agent())
        return slot->second; // the IFS is one unit: tray id = slot 0..3
    const auto boxes = snapshot().cfs_boxes();
    for (size_t u = 0; u < boxes.size(); ++u)
        if (boxes[u].id == slot->first)
            return int(u) * 4 + slot->second;
    return std::nullopt;
}

std::shared_ptr<FlashforgePrinterAgent> ifs_agent() { return flashforge_agent(); }

bool ifs_active() { return flashforge_agent() != nullptr && !current_entries().empty(); }

wxString unit_title(int box)
{
    if (flashforge_agent())
        return box == 1 ? wxString("IFS") : wxString();
    return wxString::Format("CFS %d", box);
}

void set_mapped_tray(int idx, int tray)
{
    if (flashforge_agent()) {
        if (tray == 254 || tray == 255)
            set_slot(idx, 0, 0);
        else if (tray >= 0 && tray <= 3)
            set_slot(idx, 1, tray);
        return;
    }
    const PrinterSnapshot snap = snapshot();
    if (tray == 254 || tray == 255) {
        for (const Entry& e : entries(snap))
            if (e.box == 0) {
                set_slot(idx, 0, e.slot);
                return;
            }
        return;
    }
    const auto boxes = snap.cfs_boxes();
    if (tray >= 0 && size_t(tray / 4) < boxes.size())
        set_slot(idx, boxes[size_t(tray / 4)].id, tray % 4);
}

// ---- Watcher -----------------------------------------------------------------------------------

bool grouping(int n, std::vector<int>& box_of)
{
    const auto es = current_entries();
    if (es.empty())
        return false;
    box_of.assign(size_t(std::max(n, 0)), -1);
    for (int i = 0; i < n; ++i) {
        const Entry* e = nullptr;
        label_of(i, es, &e);
        if (e != nullptr)
            box_of[i] = (flashforge_agent() && e->box == 1) ? (e->slot >= 2 ? 2 : 1) : e->box; // IFS: slots 1-2 left, 3-4 right
    }
    return true;
}

Watcher::Watcher(wxWindow* parent, std::function<int()> count, std::function<void()> on_change)
    : wxPanel(parent, wxID_ANY), m_timer(this), m_count(std::move(count)), m_change(std::move(on_change))
{
    Hide();
    s_watcher = this;
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { check(); });
    m_timer.Start(2000);
}

Watcher::~Watcher()
{
    if (s_watcher == this)
        s_watcher = nullptr;
}

void Watcher::check(bool force)
{
    if (m_busy) // the change callback re-lays out the sidebar; never re-enter from a timer / picker callback
        return;
    m_busy = true;
    std::vector<int> box;
    size_t sig = grouping(m_count(), box) ? box.size() + 1 : 0;
    for (int b : box)
        sig = sig * 131 + size_t(b + 2);
    if (force || sig != m_sig) {
        m_sig = sig;
        if (m_change)
            m_change();
    }
    if (sync_badges() && GetParent() != nullptr)
        GetParent()->Layout();
    m_busy = false;
}

// ---- Badge -------------------------------------------------------------------------------------

Badge::Badge(wxWindow* parent, int idx, wxWindow* colour_btn) : wxPanel(parent, wxID_ANY), m_idx(idx), m_btn(colour_btn)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(60, 20)));
    SetMaxSize(FromDIP(wxSize(60, 20)));
    SetToolTip(_L("Printer slot for this filament"));
    Bind(wxEVT_PAINT, &Badge::on_paint, this);
    auto edit_colour = [this] {
        if (!m_btn)
            return;
        wxCommandEvent ev(wxEVT_BUTTON, m_btn->GetId());
        ev.SetEventObject(m_btn);
        m_btn->GetEventHandler()->ProcessEvent(ev); // works while the colour box is hidden
    };
    // Left part (index + colour) edits the colour, right part ("slot ⇄") opens the slot picker.
    auto in_colour_part = [this](const wxPoint& pt) { return pt.x < FromDIP(24); };
    Bind(wxEVT_MOTION, [this, in_colour_part](wxMouseEvent& ev) {
        const bool left = in_colour_part(ev.GetPosition());
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(left ? _L("Edit color") : _L("Swap slot"));
        ev.Skip();
    });
    Bind(wxEVT_LEFT_DOWN, [this, edit_colour, in_colour_part](wxMouseEvent& ev) {
        if (in_colour_part(ev.GetPosition())) {
            wxTheApp->CallAfter(edit_colour);
            return;
        }
        auto* p = new Picker(this, m_idx, current_entries(), [this] {
            for (Badge* b : s_badges)
                b->Refresh();
            if (s_watcher != nullptr)
                s_watcher->check();
        }, edit_colour);
        p->Position(ClientToScreen(wxPoint(0, GetSize().y)), wxSize(0, 0));
        p->Popup();
    });
    s_badges.insert(this);
    Hide();
    // The colour box belongs to the filament combo and is destroyed with it; this badge is a separate child of the
    // panel and would otherwise survive as an orphan, drawn over whatever row is later laid out at its old position.
    if (colour_btn != nullptr)
        colour_btn->Bind(wxEVT_DESTROY, [w = wxWeakRef<Badge>(this)](wxWindowDestroyEvent& ev) {
            if (w)
                w->retire();
            ev.Skip();
        });
}

Badge::~Badge() { s_badges.erase(this); }

void Badge::retire()
{
    s_badges.erase(this);
    Hide();
    wxTheApp->CallAfter([w = wxWeakRef<Badge>(this)] {
        if (w)
            w->Destroy();
    });
}

// Exactly one of {original colour box, combined badge} is visible: badge shown <=> the printer has CFS/slot data.
bool Badge::apply(bool want)
{
    bool changed = false;
    if (IsShown() != want) {
        Show(want);
        changed = true;
    }
    if (m_btn && m_btn->IsShown() == want) {
        m_btn->Show(!want);
        changed = true;
    }
    Refresh();
    return changed;
}

bool sync_badges()
{
    const bool        want = !current_entries().empty();
    bool              changed = false;
    const std::vector<Badge*> all(s_badges.begin(), s_badges.end());
    for (Badge* b : all)
        changed |= b->apply(want);
    return changed;
}

void Badge::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(bg_col());
    dc.Clear();
    const auto   es = current_entries();
    const Entry* e  = nullptr;
    const wxString t = label_of(m_idx, es, &e);
    PresetBundle*  b = wxGetApp().preset_bundle;
    const auto* cols = b->project_config.option<ConfigOptionStrings>("filament_colour");
    wxColour col = cols != nullptr && m_idx < (int) cols->values.size() ? wxColour(cols->values[m_idx]) : wxColour();
    bool gradient = false;
    const std::vector<wxColour> mc = filament_colours(m_idx, gradient);
    const wxRect rc(wxPoint(0, 0), GetClientSize());
    const bool   multi = mc.size() > 1;
    if (multi)
        col = mc[mc.size() / 2]; // text contrast
    const wxString txt = wxString::Format(L"%d \u00B7 ", m_idx + 1) + t + wxString(L" \u21C4");
    if (!multi) {
        draw_chip(dc, rc, txt, col, true);
        return;
    }
    // multi-colour: the sectors / gradient fill the whole compact badge, label on top
    wxBitmap bmp = create_filament_bitmap(mc, wxSize(rc.width - 2, rc.height - 2), gradient);
    draw_chip(dc, rc, txt, col, true, &bmp);
}

std::vector<wxColour> filament_colours(int idx, bool& gradient)
{
    gradient = false;
    std::vector<wxColour> out;
    const auto&           pc = wxGetApp().preset_bundle->project_config;
    const auto*           mc = pc.option<ConfigOptionStrings>("filament_multi_colour");
    const auto*           ty = pc.option<ConfigOptionStrings>("filament_colour_type");
    const auto*           c1 = pc.option<ConfigOptionStrings>("filament_colour");
    if (idx < 0)
        return out;
    if (mc != nullptr && idx < (int) mc->values.size()) {
        std::istringstream is(mc->values[idx]);
        for (std::string t; is >> t;) {
            wxColour c(t);
            if (c.IsOk())
                out.push_back(c);
        }
    }
    if (out.empty() && c1 != nullptr && idx < (int) c1->values.size())
        out.emplace_back(c1->values[idx]);
    if (out.size() > 4)
        out.resize(4);
    gradient = out.size() > 1 && ty != nullptr && idx < (int) ty->values.size() && ty->values[idx] == "0";
    return out;
}

bool edit_filament_colour(wxWindow* parent, int idx, std::vector<std::string>& colours, bool& gradient)
{
    auto&                 pc   = wxGetApp().preset_bundle->project_config;
    bool                  grad = false;
    std::vector<wxColour> cur  = filament_colours(idx, grad);
    const int             n0   = std::max<int>(1, (int) cur.size());
    if (cur.empty())
        cur.emplace_back(0, 0, 0);
    while (cur.size() < 4)
        cur.push_back(cur.back());
    double len0 = 0.;
    if (const auto* gl = pc.option<ConfigOptionFloats>("filament_gradient_length"))
        if (idx < (int) gl->values.size())
            len0 = gl->values[idx];

    wxDialog dlg(parent, wxID_ANY, _L("Filament colour"));
    auto*    root = new wxBoxSizer(wxVERTICAL);
    auto*    type = new wxChoice(&dlg, wxID_ANY);
    for (const wxString& t : {_L("Solid"), _L("Dual"), _L("Tricolor"), _L("Quadricolor"), _L("Gradient")})
        type->Append(t);
    type->SetSelection(grad ? 4 : std::min(n0, 4) - 1);
    auto* ncol = new wxSpinCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxSize(dlg.FromDIP(60), -1), wxSP_ARROW_KEYS, 2, 4, std::max(2, std::min(n0, 4)));
    auto* lenv = new wxTextCtrl(&dlg, wxID_ANY, wxString::Format("%g", len0), wxDefaultPosition, wxSize(dlg.FromDIP(70), -1));
    auto* row0 = new wxBoxSizer(wxHORIZONTAL);
    row0->Add(new wxStaticText(&dlg, wxID_ANY, _L("Type")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dlg.FromDIP(8));
    row0->Add(type, 1);
    auto* row1 = new wxBoxSizer(wxHORIZONTAL);
    row1->Add(new wxStaticText(&dlg, wxID_ANY, _L("Colours")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dlg.FromDIP(8));
    row1->Add(ncol, 0);
    auto* row2 = new wxBoxSizer(wxHORIZONTAL);
    row2->Add(new wxStaticText(&dlg, wxID_ANY, _L("Meters per cycle (0 = auto)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dlg.FromDIP(8));
    row2->Add(lenv, 0);
    auto*                            row3 = new wxBoxSizer(wxHORIZONTAL);
    std::vector<wxColourPickerCtrl*> pick;
    for (int i = 0; i < 4; ++i) {
        pick.push_back(new wxColourPickerCtrl(&dlg, wxID_ANY, cur[i]));
        row3->Add(pick.back(), 0, wxRIGHT, dlg.FromDIP(6));
    }
    root->Add(row0, 0, wxEXPAND | wxALL, dlg.FromDIP(10));
    root->Add(row1, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(10));
    root->Add(row2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(10));
    root->Add(row3, 0, wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(10));
    root->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, dlg.FromDIP(10));
    auto count = [&] { return type->GetSelection() == 4 ? ncol->GetValue() : type->GetSelection() + 1; };
    auto upd   = [&] {
        const bool g = type->GetSelection() == 4;
        root->Show(row1, g, true);
        root->Show(row2, g, true);
        for (int i = 0; i < 4; ++i)
            pick[i]->Show(i < count());
        dlg.Layout();
        dlg.Fit();
    };
    type->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { upd(); });
    ncol->Bind(wxEVT_SPINCTRL, [&](wxCommandEvent&) { upd(); });
    dlg.SetSizer(root);
    upd();
    dlg.CenterOnParent();
    if (dlg.ShowModal() != wxID_OK)
        return false;
    gradient = type->GetSelection() == 4;
    colours.clear();
    for (int i = 0; i < count(); ++i)
        colours.push_back(pick[i]->GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    double   v   = 0.;
    wxString txt = lenv->GetValue();
    txt.Replace(",", ".");
    if (!gradient || !txt.ToCDouble(&v) || v < 0.)
        v = 0.;
    auto* gl = pc.option<ConfigOptionFloats>("filament_gradient_length", true);
    if (gl->values.size() <= size_t(idx))
        gl->values.resize(size_t(idx) + 1, 0.);
    gl->values[idx] = v;
    return true;
}

}}} // namespace Slic3r::GUI::CfsUi
