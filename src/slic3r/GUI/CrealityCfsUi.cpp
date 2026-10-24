#include "CrealityCfsUi.hpp"
#include "slic3r/Utils/DeviceActivityLog.hpp"

#include "CrealityDevicePanel.hpp"
#include "SpoolmanUi.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/CrealityCfsClient.hpp"
#include "slic3r/Utils/FlashforgePrinterAgent.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/Spoolman.hpp"

#include "FilamentBitmapUtils.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"

#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/popupwin.h>
#include <wx/button.h>

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

// Identifier of the selected printer for the local Spoolman links: the Creality host, the Bambu dev id ("" = none / Flashforge).
std::string creality_host()
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (b == nullptr || !CrealityDevicePanel::is_creality_printer(*b))
        return {};
    const DynamicPrintConfig* c = nullptr;
    if (b->physical_printers.has_selection())
        c = b->physical_printers.get_selected_printer_config();
    if (c == nullptr || !c->has("print_host") || c->opt_string("print_host").empty())
        c = &b->printers.get_edited_preset().config;
    return c->has("print_host") ? Client::strip_host(c->opt_string("print_host")) : std::string();
}

std::shared_ptr<FlashforgePrinterAgent> flashforge_agent()
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (b == nullptr || CrealityDevicePanel::is_creality_printer(*b) || !is_flashforge_printer_selected())
        return nullptr;
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent != nullptr ? std::dynamic_pointer_cast<FlashforgePrinterAgent>(agent->get_printer_agent()) : nullptr;
}

// Snapmaker U1: the four tool heads are one unit (box 1, slots 0-3 = T1..T4) that takes the place of the CFS.
std::shared_ptr<SnapmakerPrinterAgent> snapmaker_agent()
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (b == nullptr || CrealityDevicePanel::is_creality_printer(*b) || !is_snapmaker_u1_selected())
        return nullptr;
    return SnapmakerPrinterAgent::current();
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
    int         spool_id = 0; // Spoolman spool bound to the slot (0 = none)
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

// ---- Bambu AMS: box = ams id + 1 (0 = external spool, slot = vt id - 254), label A1..D4 / HT-A / Ext.
MachineObject* bambu_obj()
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (b == nullptr || !b->is_bbl_vendor() || wxGetApp().plater() == nullptr || wxGetApp().getDeviceManager() == nullptr)
        return nullptr;
    MachineObject* o = wxGetApp().getDeviceManager()->get_selected_machine();
    if (o == nullptr || o->GetFilaSystem() == nullptr || o->GetFilaSystem()->GetAmsList().empty())
        return nullptr;
    return wxGetApp().plater()->is_same_printer_for_connected_and_selected(false) ? o : nullptr;
}

wxString ams_name(int ams, int slot)
{
    if (ams >= 0 && ams < 26)
        return wxString::Format("%c%d", 'A' + ams, slot + 1);
    if (ams >= 128 && ams < 154)
        return wxString::Format("HT-%c", 'A' + ams - 128);
    return wxString::Format("N%d", ams);
}

std::string tray_key(int ams, int slot) { return std::to_string(ams >= 254 ? ams : ams * 4 + slot); }

std::vector<Entry> bambu_entries(MachineObject* o)
{
    std::vector<Entry> out;
    auto col = [](const DevAmsTray& t) { return t.color.empty() ? wxColour() : wxColour("#" + t.color); };
    if (o->ams_support_virtual_tray)
        for (const DevAmsTray& t : o->vt_slot)
            out.push_back({0, std::max(0, atoi(t.id.c_str()) - 254), "Ext", col(t), t.m_fila_type, t.is_exists || !t.m_fila_type.empty()});
    for (const auto& a : o->GetFilaSystem()->GetAmsList()) {
        const int ams = atoi(a.first.c_str());
        for (const auto& t : a.second->GetTrays()) {
            const int slot = atoi(t.first.c_str());
            out.push_back({ams + 1, slot, ams_name(ams, slot), col(*t.second), t.second->m_fila_type, t.second->is_exists});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry& x, const Entry& y) { return x.box != y.box ? (x.box == 0 ? true : (y.box == 0 ? false : x.box < y.box)) : x.slot < y.slot; });
    return out;
}

// Column of a unit in the sidebar: 1, 2 for the first two AMS units, 3 (below, with the external spool) for the rest.
int bambu_col(const std::vector<Entry>& es, int box)
{
    if (box == 0)
        return 0;
    std::set<int> u;
    for (const Entry& e : es)
        if (e.box > 0)
            u.insert(e.box);
    const int rank = (int) std::distance(u.begin(), u.find(box));
    return rank < 2 ? rank + 1 : 3;
}

// ---- Spoolman links kept locally (Creality, Bambu): app config section "spoolman_slots", key "<printer>:<box>:<slot>" -> spool id.
constexpr const char* SPOOL_SECTION = "spoolman_slots";

std::string spool_printer_id()
{
    if (MachineObject* o = bambu_obj())
        return "bbl-" + o->get_dev_id();
    const std::string host = creality_host();
    return host.empty() ? std::string() : "cx-" + host;
}

std::string spool_link_key(const std::string& printer, int box, int slot) { return printer + ":" + std::to_string(box) + ":" + std::to_string(slot); }

int local_spool(const std::string& printer, int box, int slot)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || printer.empty())
        return 0;
    return std::max(0, std::atoi(cfg->get(SPOOL_SECTION, spool_link_key(printer, box, slot)).c_str()));
}

// id 0 forgets the link; a spool sits in one slot of a printer only.
void set_local_spool(const std::string& printer, int box, int slot, int id)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || printer.empty())
        return;
    const std::string key = spool_link_key(printer, box, slot);
    if (id > 0 && cfg->has_section(SPOOL_SECTION)) {
        std::vector<std::string> same;
        for (const auto& kv : cfg->get_section(SPOOL_SECTION))
            if (kv.first != key && kv.first.rfind(printer + ":", 0) == 0 && std::atoi(kv.second.c_str()) == id)
                same.push_back(kv.first);
        for (const std::string& k : same)
            cfg->erase(SPOOL_SECTION, k);
    }
    if (id > 0)
        cfg->set(SPOOL_SECTION, key, std::to_string(id));
    else
        cfg->erase(SPOOL_SECTION, key);
}

wxString unit_name(int box, bool full)
{
    if (box == 0)
        return "EXT";
    const wxString n = ams_name(box - 1, 0);
    return n.StartsWith("HT") ? n : (full ? "AMS " : "") + n.Left(1);
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
            out.push_back({1, s.port - 1, wxString::Format("IFS%d", s.port), s.color.empty() ? wxColour() : wxColour(s.color), s.material, s.has_filament, s.spool_id});
        return slots.empty() ? std::vector<Entry>() : out;
    }
    if (auto u1 = snapmaker_agent()) {
        std::vector<Entry> out;
        const auto         state = u1->u1_state();
        if (!state.valid)
            return out;
        for (const auto& h : state.heads)
            out.push_back({1, h.index, wxString::Format("E%d", h.index + 1), wxColour(h.color), h.type, h.present});
        return out;
    }
    std::vector<Entry> out = bambu_obj() != nullptr ? bambu_entries(bambu_obj()) : entries(snapshot());
    if (const std::string printer = spool_printer_id(); !printer.empty() && !out.empty())
        for (Entry& e : out)
            e.spool_id = local_spool(printer, e.box, e.slot);
    return out;
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

// Slot every project filament maps to, without repeats: explicit choices (project filament_cfs_slot) are reserved
// first; then each remaining filament, in order, takes the first FREE loaded slot with the same type+colour (slot
// order breaks ties); the rest fall back to their own position, then to any free CFS slot. A physical slot is thus
// never given to two filaments while an equivalent free one exists (e.g. same spool in 1B and 2B).
// positional = false stops after the type+colour match (the slot sync must not guess: see CrealityCfs::sync_targets).
std::vector<const Entry*> assign_slots(const std::vector<Entry>& es, bool positional = true)
{
    PresetBundle* b = wxGetApp().preset_bundle;
    std::vector<const Entry*> out;
    if (es.empty() || b == nullptr)
        return out;
    const size_t n = b->filament_presets.size();
    out.assign(n, nullptr);
    std::vector<bool> used(es.size(), false);
    for (size_t i = 0; i < n; ++i)
        if (const auto chosen = chosen_slot(int(i)))
            for (size_t k = 0; k < es.size(); ++k)
                if (es[k].box == chosen->first && es[k].slot == chosen->second) {
                    out[i] = &es[k];
                    used[k] = true;
                    break;
                }
    const auto* cols = b->project_config.option<ConfigOptionStrings>("filament_colour");
    for (size_t i = 0; i < n; ++i) {
        if (out[i] != nullptr || chosen_slot(int(i)))
            continue;
        std::string type;
        if (const Preset* p = b->filaments.find_preset(b->filament_presets[i]))
            if (const auto* o = p->config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
                type = o->values.front();
        const wxColour col = cols != nullptr && i < cols->values.size() ? wxColour(cols->values[i]) : wxColour();
        for (size_t k = 0; k < es.size(); ++k) {
            const Entry& x = es[k];
            if (!used[k] && x.box != 0 && x.loaded && x.type == type && x.colour.IsOk() && col.IsOk() && x.colour == col) {
                out[i] = &x;
                used[k] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < n && positional; ++i) {
        if (out[i] != nullptr || chosen_slot(int(i)))
            continue;
        size_t pos = 0;
        int    first_free = -1;
        for (size_t k = 0; k < es.size(); ++k) {
            if (es[k].box == 0)
                continue;
            if (pos++ == i && !used[k]) {
                first_free = int(k);
                break;
            }
        }
        if (first_free < 0)
            for (size_t k = 0; k < es.size(); ++k)
                if (es[k].box != 0 && !used[k]) {
                    first_free = int(k);
                    break;
                }
        if (first_free < 0) { // every slot is taken: reuse the positional one (more filaments than slots)
            pos = 0;
            for (size_t k = 0; k < es.size(); ++k)
                if (es[k].box != 0 && pos++ == i) {
                    first_free = int(k);
                    break;
                }
        }
        if (first_free >= 0) {
            out[i] = &es[size_t(first_free)];
            used[size_t(first_free)] = true;
        }
    }
    return out;
}

// Label of the slot filament `idx` maps to (see assign_slots).
wxString label_of(int idx, const std::vector<Entry>& es, const Entry** found = nullptr)
{
    if (found)
        *found = nullptr;
    if (es.empty() || wxGetApp().preset_bundle == nullptr)
        return "-";
    const Entry* e = nullptr;
    if (const auto chosen = chosen_slot(idx)) {
        for (const Entry& x : es)
            if (x.box == chosen->first && x.slot == chosen->second) {
                e = &x;
                break;
            }
    } else {
        const auto all = assign_slots(es);
        if (idx >= 0 && size_t(idx) < all.size())
            e = all[size_t(idx)];
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
            const wxString t  = m_bambu ? unit_name(m_tabs[i], false) : m_tabs[i] == 0 ? wxString("EXT") : flashforge_agent() ? wxString("IFS") : snapmaker_agent() ? wxString("U1") : wxString::Format("CFS%d", m_tabs[i]);
            const wxSize   ts = dc.GetTextExtent(t);
            dc.DrawText(t, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2);
        }
        const auto v = shown();
        for (size_t i = 0; i < v.size(); ++i) {
            const wxRect r = row_rect(i);
            const bool ext = v[i]->box == 0;
            draw_chip(dc, wxRect(r.x, r.y, FromDIP(48), r.height), v[i]->label, v[i]->colour, v[i]->loaded);
            dc.SetTextForeground(v[i]->loaded || ext ? fg_col() : wxColour(150, 150, 150));
            wxString txt = v[i]->loaded ? wxString::FromUTF8(v[i]->type.c_str()) : (ext ? wxString("-") : _L("Empty") + L" \u26A0");
            if (v[i]->spool_id > 0)
                txt += wxString::Format("  #%d", v[i]->spool_id); // Spoolman spool bound to the slot
            dc.DrawText(txt, r.x + FromDIP(56), r.y + FromDIP(4));
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
                if (m_bambu)
                    remember_colours(m_idx);
                if (const int spool = SpoolmanUi::project_spool_id(m_idx); spool > 0)
                    link_spool_to_slot(m_idx, spool); // the filament's Spoolman spool follows it to the new slot
                auto done = m_done;
                Dismiss();
                done();
                return;
            }
    }
    int                   m_idx;
    bool                  m_bambu{bambu_obj() != nullptr};
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

std::vector<std::optional<std::pair<int, int>>> matched_slots()
{
    std::vector<std::optional<std::pair<int, int>>> out;
    const auto                                      es = current_entries();
    if (es.empty())
        return out;
    for (const Entry* e : assign_slots(es, false))
        out.push_back(e != nullptr ? std::make_optional(std::make_pair(e->box, e->slot)) : std::nullopt);
    return out;
}

std::optional<std::pair<int, int>> chosen_mapped_slot(int idx) { return chosen_slot(idx); }

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
    if (flashforge_agent() || snapmaker_agent())
        return slot->second; // the IFS / the U1 heads are one unit: tray id = slot 0..3
    const auto boxes = snapshot().cfs_boxes();
    for (size_t u = 0; u < boxes.size(); ++u)
        if (boxes[u].id == slot->first)
            return int(u) * 4 + slot->second;
    return std::nullopt;
}

bool bambu_active() { return bambu_obj() != nullptr; }

std::optional<std::pair<int, int>> mapped_ams(int idx)
{
    if (bambu_obj() == nullptr || mapped_slot_empty(idx))
        return std::nullopt;
    const auto m = mapped_slot(idx);
    if (!m)
        return std::nullopt;
    return m->first == 0 ? std::make_pair(254 + m->second, 0) : std::make_pair(m->first - 1, m->second);
}

void set_mapped_ams(int idx, int ams, int slot)
{
    if (bambu_obj() == nullptr)
        return;
    set_slot(idx, ams >= 254 ? 0 : ams + 1, ams >= 254 ? ams - 254 : slot);
    remember_colours(idx);
}

// Local memory of the multi-colour setup of a slot (the printer's ams_filament_setting only takes one colour):
// app config section "ams_multicolour", key "<dev_id>:<tray>", value "<type> <c1> <c2> ..." (type "0" gradient, "1" side by side).
void remember_colours(int idx)
{
    MachineObject* o   = bambu_obj();
    AppConfig*     cfg = wxGetApp().app_config;
    const auto     m   = o != nullptr ? mapped_ams(idx) : std::nullopt;
    if (cfg == nullptr || !m)
        return;
    const std::string key = o->get_dev_id() + ":" + tray_key(m->first, m->second);
    bool grad = false;
    const auto cols = filament_colours(idx, grad);
    if (cols.size() < 2) {
        cfg->erase("ams_multicolour", key);
        return;
    }
    std::string v = grad ? "0" : "1";
    for (const wxColour& c : cols)
        v += " " + c.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    cfg->set("ams_multicolour", key, v);
}

bool restore_colours(const std::string& dev_id, int ams, int slot, const std::string& tray_colour, std::vector<std::string>& cols, std::string& type)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || tray_colour.size() < 6)
        return false;
    std::istringstream is(cfg->get("ams_multicolour", dev_id + ":" + tray_key(ams, slot)));
    is >> type;
    cols.clear();
    for (std::string t; is >> t;)
        cols.push_back(t);
    const wxColour first(cols.empty() ? "" : cols[0]), cur("#" + tray_colour.substr(0, 6));
    if (cols.size() < 2 || !first.IsOk() || !cur.IsOk() || first.GetRGB() != cur.GetRGB())
        return false;
    return true;
}

std::shared_ptr<FlashforgePrinterAgent> ifs_agent() { return flashforge_agent(); }

std::shared_ptr<SnapmakerPrinterAgent> u1_agent() { return snapmaker_agent(); }

std::string activity_host()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    const DynamicPrintConfig* config = nullptr;
    if (bundle->physical_printers.has_selection())
        config = bundle->physical_printers.get_selected_printer_config();
    if (config == nullptr || !config->has("print_host") || config->opt_string("print_host").empty())
        config = &bundle->printers.get_edited_preset().config;
    return config->has("print_host") ? DeviceActivity::key_of(config->opt_string("print_host")) : std::string();
}

bool u1_active() { return snapmaker_agent() != nullptr && !current_entries().empty(); }

bool ifs_active() { return flashforge_agent() != nullptr && !current_entries().empty(); }

wxString unit_title(int box)
{
    if (bambu_obj() != nullptr) {
        const auto es = current_entries();
        for (const Entry& e : es)
            if (e.box > 0 && bambu_col(es, e.box) == box)
                return unit_name(e.box, true);
        return wxString();
    }
    if (flashforge_agent())
        return box == 1 ? wxString("IFS") : wxString();
    if (snapmaker_agent())
        return box == 1 ? wxString("U1") : wxString();
    return wxString::Format("CFS %d", box);
}

void set_mapped_tray(int idx, int tray)
{
    if (flashforge_agent() || snapmaker_agent()) {
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

// ---- Spoolman ----------------------------------------------------------------------------------

std::map<int, wxString> spool_slot_labels()
{
    std::map<int, wxString> out;
    for (const Entry& e : current_entries())
        if (e.spool_id > 0)
            out[e.spool_id] = e.label;
    return out;
}

int mapped_slot_spool(int idx)
{
    const Entry* e  = nullptr;
    const auto   es = current_entries();
    label_of(idx, es, &e);
    return e != nullptr ? e->spool_id : 0;
}

void link_spool_to_slot(int idx, int spool_id)
{
    const Entry* e  = nullptr;
    const auto   es = current_entries();
    label_of(idx, es, &e);
    if (e == nullptr)
        return;
    spool_id = std::max(0, spool_id);
    if (auto ff = flashforge_agent()) {
        if (e->box != 1 || ff->mode() != FlashforgePrinterAgent::Mode::zmod || (spool_id == 0 && e->spool_id == 0))
            return;
        PresetBundle*                     b = wxGetApp().preset_bundle;
        FlashforgePrinterAgent::SlotWrite w;
        w.port     = e->slot + 1;
        w.spool_id = spool_id;
        if (const auto* cols = b->project_config.option<ConfigOptionStrings>("filament_colour"); cols != nullptr && idx < (int) cols->values.size()) {
            const wxColour c(cols->values[idx]);
            if (c.IsOk())
                w.color = c.GetAsString(wxC2S_HTML_SYNTAX).Upper().ToStdString();
        }
        if (idx < (int) b->filament_presets.size())
            if (const Preset* p = b->filaments.find_preset(b->filament_presets[idx]))
                if (const auto* o = p->config.option<ConfigOptionStrings>("filament_type"); o != nullptr && !o->values.empty())
                    w.type = o->values.front();
        bool                        grad = false;
        const std::vector<wxColour> mc   = filament_colours(idx, grad);
        if (mc.size() >= 2) {
            for (const wxColour& c : mc)
                w.colors.push_back(c.GetAsString(wxC2S_HTML_SYNTAX).Upper().ToStdString());
            w.gradient = grad;
        }
        if (Spoolman::Spool s; spool_id > 0 && Spoolman::cached_spool(spool_id, s)) {
            w.spool_filament_id = s.filament_id;
            w.spool_vendor_id   = s.vendor_id;
            w.spool_vendor      = s.vendor;
            w.spool_name        = s.name;
        }
        ff->link_spools({w});
        return;
    }
    const std::string printer = spool_printer_id();
    if (printer.empty() || (spool_id == 0 && e->spool_id == 0))
        return;
    set_local_spool(printer, e->box, e->slot, spool_id);
    for (Badge* b : s_badges)
        b->Refresh();
}

// ---- Watcher -----------------------------------------------------------------------------------

bool grouping(int n, std::vector<int>& box_of)
{
    const auto es = current_entries();
    if (es.empty())
        return false;
    const bool bambu = bambu_obj() != nullptr;
    box_of.assign(size_t(std::max(n, 0)), -1);
    for (int i = 0; i < n; ++i) {
        const Entry* e = nullptr;
        label_of(i, es, &e);
        if (e != nullptr)
            box_of[i] = bambu ? bambu_col(es, e->box) : ((flashforge_agent() || snapmaker_agent()) && e->box == 1) ? (e->slot >= 2 ? 2 : 1) : e->box; // IFS / U1: slots 1-2 left, 3-4 right
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
        wxString tip = left ? _L("Edit color") : _L("Swap slot");
        if (const int id = SpoolmanUi::project_spool_id(m_idx); id > 0)
            tip += "\n" + _L("Spool") + ": " + SpoolmanUi::spool_summary(id);
        else if (const int sid = mapped_slot_spool(m_idx); sid > 0)
            tip += "\n" + _L("Slot spool") + ": " + SpoolmanUi::spool_summary(sid);
        tip += "\n" + _L("Right click: Spoolman spool");
        if (GetToolTipText() != tip)
            SetToolTip(tip);
        ev.Skip();
    });
    Bind(wxEVT_LEFT_DOWN, [this, edit_colour, in_colour_part](wxMouseEvent& ev) {
        if (in_colour_part(ev.GetPosition())) {
            wxTheApp->CallAfter(edit_colour);
            return;
        }
        auto* p = new Picker(this, m_idx, current_entries(), [] {
            for (Badge* b : s_badges)
                b->Refresh();
            if (s_watcher != nullptr)
                s_watcher->check();
        }, edit_colour);
        p->Position(ClientToScreen(wxPoint(0, GetSize().y)), wxSize(0, 0));
        p->Popup();
    });
    Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
        const int idx = m_idx;
        wxTheApp->CallAfter([idx] { SpoolmanUi::pick_spool(wxGetApp().plater(), idx); });
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

bool edit_filament_colour(wxWindow* parent, int idx, std::vector<std::string>& colours, bool& gradient, const std::function<void()>& official)
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
    if (official) {
        auto* ob = new wxButton(&dlg, wxID_ANY, _L("Official Bambu colors..."));
        ob->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent&) { dlg.EndModal(wxID_APPLY); });
        root->Add(ob, 0, wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(10));
    }
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
    const int rc = dlg.ShowModal();
    if (rc == wxID_APPLY && official) {
        official();
        return false;
    }
    if (rc != wxID_OK)
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
