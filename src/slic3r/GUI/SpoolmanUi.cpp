#include "SpoolmanUi.hpp"

#include "CrealityCfsUi.hpp"
#include "FilamentBitmapUtils.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "PresetComboBoxes.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace Slic3r { namespace GUI { namespace SpoolmanUi {

using Spoolman::Spool;

namespace {

struct Endpoint
{
    std::string base, key;
};

void add_endpoint(std::vector<Endpoint>& out, const std::string& base, const std::string& key)
{
    if (base.empty())
        return;
    for (const Endpoint& e : out)
        if (e.base == base)
            return;
    out.push_back({base, key == "88888888" ? std::string() : key});
}

// The Moonraker hosts worth asking for their [spoolman] section. UI thread.
std::vector<Endpoint> moonraker_endpoints()
{
    std::vector<Endpoint> out;
    if (NetworkAgent* agent = wxGetApp().getAgent())
        if (const auto moonraker = std::dynamic_pointer_cast<MoonrakerPrinterAgent>(agent->get_printer_agent())) {
            const auto ep = moonraker->moonraker_endpoint();
            add_endpoint(out, ep.first, ep.second);
        }
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;
    const DynamicPrintConfig* cfg = nullptr;
    if (bundle->physical_printers.has_selection())
        cfg = bundle->physical_printers.get_selected_printer_config();
    if (cfg == nullptr || !cfg->has("print_host") || cfg->opt_string("print_host").empty())
        cfg = &bundle->printers.get_edited_preset().config;
    if (cfg == nullptr || !cfg->has("print_host") || !cfg->has("host_type"))
        return out;
    const PrintHostType type = cfg->opt_enum<PrintHostType>("host_type");
    std::string         host = cfg->opt_string("print_host");
    boost::trim(host);
    if ((type != htMoonraker && type != htFlashforge) || host.empty())
        return out;
    const std::string key = cfg->has("printhost_apikey") ? cfg->opt_string("printhost_apikey") : std::string();
    const bool        has_scheme = host.find("://") != std::string::npos;
    std::string       base = has_scheme ? host : "http://" + host;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    add_endpoint(out, base, key);
    if (!has_scheme && host.find(':') == std::string::npos)
        add_endpoint(out, base + ":7125", key); // Klipper's own Moonraker port
    return out;
}

std::mutex                              s_cache_mutex;
std::chrono::steady_clock::time_point  s_cache_time;
bool                                   s_cache_loaded = false;
std::atomic<bool>                      s_cache_busy{false};

ConfigOptionStrings* spool_option(bool create)
{
    PresetBundle* b = wxGetApp().preset_bundle;
    return b == nullptr ? nullptr : b->project_config.option<ConfigOptionStrings>("filament_spool_id", create);
}

wxString weight_text(double g)
{
    if (g < 0.)
        return "-";
    return wxString::Format("%.0f g", g);
}

wxColour swatch_colour(const std::string& hex)
{
    wxColour c(hex);
    return c.IsOk() ? c : wxColour(200, 200, 200);
}

// ---- the dialog ---------------------------------------------------------------------------------

class SpoolDialog : public wxDialog
{
public:
    enum { ID_UNLINK = wxID_HIGHEST + 411, ID_REFRESH };

    SpoolDialog(wxWindow* parent, int filament_idx, int current_id, int slot_spool_id)
        : wxDialog(parent, wxID_ANY, wxString::Format(_L("Spoolman spool for filament %d"), filament_idx + 1), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_current(current_id)
        , m_slot_spool(slot_spool_id)
    {
        m_url   = Spoolman::configured_url();
        m_slots = CfsUi::spool_slot_labels();
        auto* root = new wxBoxSizer(wxVERTICAL);

        m_status = new wxStaticText(this, wxID_ANY, wxString());
        root->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _L("Search")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        m_search = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition, wxSize(FromDIP(220), -1));
        row->Add(m_search, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        m_hide_empty = new wxCheckBox(this, wxID_ANY, _L("Hide empty spools"));
        m_hide_empty->SetValue(!wxGetApp().app_config->has("spoolman_hide_empty") || wxGetApp().app_config->get_bool("spoolman_hide_empty"));
        row->Add(m_hide_empty, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        auto* refresh = new wxButton(this, ID_REFRESH, _L("Refresh"));
        row->Add(refresh, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(row, 0, wxEXPAND | wxALL, FromDIP(10));

        m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(780), FromDIP(340)), wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_THEME);
        m_list->AppendColumn(_L("Spool"), wxLIST_FORMAT_LEFT, FromDIP(96));
        m_list->AppendColumn(_L("Vendor"), wxLIST_FORMAT_LEFT, FromDIP(110));
        m_list->AppendColumn(_L("Name"), wxLIST_FORMAT_LEFT, FromDIP(180));
        m_list->AppendColumn(_L("Material"), wxLIST_FORMAT_LEFT, FromDIP(90));
        m_list->AppendColumn(_L("Remaining"), wxLIST_FORMAT_RIGHT, FromDIP(80));
        m_list->AppendColumn(_L("Location"), wxLIST_FORMAT_LEFT, FromDIP(130));
        m_list->AppendColumn(_L("Printer slot"), wxLIST_FORMAT_LEFT, FromDIP(80));
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

        auto* bottom = new wxBoxSizer(wxHORIZONTAL);
        m_unlink = new wxButton(this, ID_UNLINK, _L("Unlink spool"));
        m_unlink->Enable(current_id > 0);
        bottom->Add(m_unlink, 0, wxALIGN_CENTER_VERTICAL);
        bottom->AddStretchSpacer(1);
        m_ok = new wxButton(this, wxID_OK, _L("Use this spool"));
        m_ok->Enable(false);
        bottom->Add(m_ok, 0, wxRIGHT, FromDIP(8));
        bottom->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0);
        root->Add(bottom, 0, wxEXPAND | wxALL, FromDIP(10));

        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(640), FromDIP(360)));
        CenterOnParent();

        m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { populate(); });
        m_hide_empty->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            wxGetApp().app_config->set("spoolman_hide_empty", m_hide_empty->GetValue() ? "true" : "false");
            populate();
        });
        refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load(); });
        m_unlink->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_unlink_requested = true;
            EndModal(wxID_OK);
        });
        m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { m_ok->Enable(selected_row() >= 0); });
        m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { m_ok->Enable(selected_row() >= 0); });
        m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            if (selected_row() >= 0)
                EndModal(wxID_OK);
        });

        if (m_url.empty()) {
            m_status->SetLabel(_L("Detecting the Spoolman server from the printer..."));
            wxWeakRef<SpoolDialog> weak(this);
            detect_url_async([weak](const std::string& url) {
                if (!weak)
                    return;
                if (url.empty()) {
                    weak->m_status->SetLabel(_L("No Spoolman server set. Enter it in Preferences > Spoolman server."));
                    return;
                }
                if (wxGetApp().app_config->get("spoolman_url").empty()) {
                    wxGetApp().app_config->set("spoolman_url", url);
                    wxGetApp().app_config->save();
                }
                weak->m_url = url;
                weak->load();
            });
        } else
            load();
    }

    bool unlink_requested() const { return m_unlink_requested; }

    bool selected_spool(Spool& out) const
    {
        const long row = selected_row();
        if (row < 0)
            return false;
        const int id = int(m_list->GetItemData(row));
        for (const Spool& s : m_all)
            if (s.id == id) {
                out = s;
                return true;
            }
        return false;
    }

private:
    long selected_row() const { return m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); }

    void load()
    {
        if (m_url.empty())
            return;
        m_status->SetLabel(wxString::Format(_L("Loading spools from %s ..."), wxString::FromUTF8(m_url)));
        const std::string      url = m_url;
        wxWeakRef<SpoolDialog> weak(this);
        std::thread([url, weak]() {
            std::vector<Spool> spools;
            std::string        error;
            const bool         ok = Spoolman::fetch_spools(url, spools, error);
            wxTheApp->CallAfter([weak, ok, spools = std::move(spools), error]() mutable {
                if (weak)
                    weak->on_loaded(ok, std::move(spools), error);
            });
        }).detach();
    }

    void on_loaded(bool ok, std::vector<Spool> spools, const std::string& error)
    {
        if (!ok) {
            m_status->SetLabel(wxString::Format(_L("Could not read Spoolman at %s: %s"), wxString::FromUTF8(m_url), wxString::FromUTF8(error)));
            return;
        }
        std::sort(spools.begin(), spools.end(), [](const Spool& a, const Spool& b) {
            const int v = boost::ilexicographical_compare(a.vendor, b.vendor) ? -1 : boost::ilexicographical_compare(b.vendor, a.vendor) ? 1 : 0;
            if (v != 0)
                return v < 0;
            if (a.name != b.name)
                return boost::ilexicographical_compare(a.name, b.name);
            return a.id < b.id;
        });
        m_all = std::move(spools);
        m_status->SetLabel(wxString::Format(_L("%d spools on %s"), int(m_all.size()), wxString::FromUTF8(m_url)));
        populate();
    }

    bool matches(const Spool& s, const std::string& needle) const
    {
        if (needle.empty())
            return true;
        std::string hay = s.title() + " " + s.material + " " + s.location + " " + s.color;
        if (const auto it = m_slots.find(s.id); it != m_slots.end())
            hay += " " + std::string(it->second.ToUTF8().data());
        return boost::icontains(hay, needle);
    }

    void populate()
    {
        const std::string needle = std::string(m_search->GetValue().Trim().ToUTF8().data());
        const bool        hide   = m_hide_empty->GetValue();
        const int         want   = selected_id_or_default();
        m_list->Freeze();
        m_list->DeleteAllItems();
        auto* images = new wxImageList(FromDIP(28), FromDIP(14), true);
        m_list->AssignImageList(images, wxIMAGE_LIST_SMALL);
        long select_row = -1;
        for (const Spool& s : m_all) {
            const bool is_current = s.id == want;
            if (!is_current && ((hide && s.remaining_weight >= 0. && s.remaining_weight <= 0.) || !matches(s, needle)))
                continue;
            std::vector<wxColour> cols;
            if (s.is_multi())
                for (const std::string& c : s.colors)
                    cols.push_back(swatch_colour(c));
            else
                cols.push_back(swatch_colour(s.color));
            const int  img = images->Add(create_filament_bitmap(cols, wxSize(FromDIP(28), FromDIP(14)), s.gradient));
            const long row = m_list->InsertItem(m_list->GetItemCount(), wxString::Format("#%d", s.id), img);
            m_list->SetItemData(row, s.id);
            m_list->SetItem(row, 1, wxString::FromUTF8(s.vendor));
            m_list->SetItem(row, 2, wxString::FromUTF8(s.name));
            m_list->SetItem(row, 3, wxString::FromUTF8(s.material));
            m_list->SetItem(row, 4, weight_text(s.remaining_weight));
            m_list->SetItem(row, 5, wxString::FromUTF8(s.location));
            if (const auto it = m_slots.find(s.id); it != m_slots.end())
                m_list->SetItem(row, 6, it->second);
            if (is_current)
                select_row = row;
        }
        m_list->Thaw();
        if (select_row >= 0) {
            m_list->SetItemState(select_row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_list->EnsureVisible(select_row);
        }
        m_ok->Enable(selected_row() >= 0);
    }

    // Keeps the selection across a refilter; the first time the filament's spool, else the spool of its slot.
    int selected_id_or_default()
    {
        if (const long row = selected_row(); row >= 0)
            return int(m_list->GetItemData(row));
        if (!m_default_used) {
            m_default_used = true;
            return m_current > 0 ? m_current : m_slot_spool;
        }
        return 0;
    }

    int                                m_current;
    int                                m_slot_spool;
    bool                               m_default_used{false};
    bool                               m_unlink_requested{false};
    std::string                        m_url;
    std::map<int, wxString>            m_slots;
    std::vector<Spool>                 m_all;
    wxStaticText*                      m_status{nullptr};
    wxTextCtrl*                        m_search{nullptr};
    wxCheckBox*                        m_hide_empty{nullptr};
    wxListCtrl*                        m_list{nullptr};
    wxButton*                          m_unlink{nullptr};
    wxButton*                          m_ok{nullptr};
};

} // namespace

// ---- connection ---------------------------------------------------------------------------------

void test_connection_async(const std::string& raw_url, std::function<void(bool, const wxString&)> done)
{
    const std::string url = Spoolman::normalize_url(raw_url);
    std::thread([url, done]() {
        bool     ok = false;
        wxString msg;
        if (url.empty())
            msg = _L("No server address.");
        else {
            std::string version, error;
            if (!Spoolman::fetch_info(url, version, error))
                msg = wxString::Format(_L("Could not connect: %s"), wxString::FromUTF8(error));
            else {
                std::vector<Spool> spools;
                if (Spoolman::fetch_spools(url, spools, error)) {
                    ok  = true;
                    msg = wxString::Format(_L("Connected: Spoolman %s, %d spools."), wxString::FromUTF8(version), int(spools.size()));
                } else
                    msg = wxString::Format(_L("Spoolman %s answers but the spools could not be read: %s"), wxString::FromUTF8(version), wxString::FromUTF8(error));
            }
        }
        wxTheApp->CallAfter([done, ok, msg]() { done(ok, msg); });
    }).detach();
}

void detect_url_async(std::function<void(const std::string&)> done)
{
    std::vector<Endpoint> endpoints = moonraker_endpoints();
    std::thread([endpoints = std::move(endpoints), done]() {
        std::string found;
        for (const Endpoint& e : endpoints) {
            found = Spoolman::discover_from_moonraker(e.base, e.key);
            if (!found.empty())
                break;
        }
        wxTheApp->CallAfter([done, found]() { done(found); });
    }).detach();
}

void autofill_url()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || !boost::trim_copy(cfg->get("spoolman_url")).empty())
        return;
    detect_url_async([](const std::string& url) {
        AppConfig* c = wxGetApp().app_config;
        if (url.empty() || c == nullptr || !boost::trim_copy(c->get("spoolman_url")).empty())
            return;
        c->set("spoolman_url", url);
        c->save();
        BOOST_LOG_TRIVIAL(info) << "Spoolman: server taken from the Moonraker printer: " << url;
    });
}

void refresh_cache_async(bool force)
{
    const std::string url = Spoolman::configured_url();
    if (url.empty() || s_cache_busy.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        if (!force && s_cache_loaded && std::chrono::steady_clock::now() - s_cache_time < std::chrono::minutes(3)) {
            s_cache_busy = false;
            return;
        }
    }
    std::thread([url]() {
        std::vector<Spool> spools;
        std::string        error;
        const bool         ok = Spoolman::fetch_spools(url, spools, error);
        {
            std::lock_guard<std::mutex> lock(s_cache_mutex);
            s_cache_time   = std::chrono::steady_clock::now(); // a failure is retried after the same pause
            s_cache_loaded = ok;
        }
        if (!ok)
            BOOST_LOG_TRIVIAL(info) << "Spoolman: spool list not loaded: " << error;
        s_cache_busy = false;
    }).detach();
}

// ---- project filament <-> spool ---------------------------------------------------------------

int project_spool_id(int idx)
{
    const auto* opt = spool_option(false);
    if (opt == nullptr || idx < 0 || size_t(idx) >= opt->values.size())
        return 0;
    const std::string& v = opt->values[idx];
    return v.empty() ? 0 : std::max(0, std::atoi(v.c_str()));
}

void set_project_spool_id(int idx, int id)
{
    auto* opt = spool_option(true);
    if (opt == nullptr || idx < 0)
        return;
    if (opt->values.size() <= size_t(idx))
        opt->values.resize(size_t(idx) + 1);
    const std::string v = id > 0 ? std::to_string(id) : std::string();
    if (opt->values[idx] == v)
        return;
    opt->values[idx] = v;
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->set_plater_dirty(true);
}

wxString spool_summary(int id)
{
    if (id <= 0)
        return wxString();
    Spool s;
    if (!Spoolman::cached_spool(id, s)) {
        refresh_cache_async();
        return wxString::Format("#%d", id);
    }
    wxString t = wxString::FromUTF8(s.title());
    if (s.remaining_weight >= 0.)
        t += " - " + weight_text(s.remaining_weight);
    return t;
}

bool apply_spool(int idx, const Spool& spool)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || idx < 0 || size_t(idx) >= bundle->filament_presets.size())
        return false;
    // 1. the Orca preset that suits the spool (vendor + material + name, else the generic one of the material)
    const std::string preset = Spoolman::best_preset(*bundle, spool);
    if (!preset.empty() && preset != bundle->filament_presets[idx] && bundle->filaments.find_preset(preset) != nullptr)
        bundle->set_filament_preset(size_t(idx), preset);
    // 2. the colour(s): coaxial -> several colours side by side, longitudinal -> gradient
    auto& pc = bundle->project_config;
    auto* colour = pc.option<ConfigOptionStrings>("filament_colour", true);
    auto* multi  = pc.option<ConfigOptionStrings>("filament_multi_colour", true);
    auto* type   = pc.option<ConfigOptionStrings>("filament_colour_type", true);
    const size_t n = std::max<size_t>(colour->values.size(), size_t(idx) + 1);
    colour->values.resize(n, "#FFFFFF");
    multi->values.resize(n);
    type->values.resize(n, "1");
    if (!spool.color.empty()) {
        colour->values[idx] = spool.color;
        if (spool.is_multi()) {
            std::string joined;
            for (const std::string& c : spool.colors)
                joined += (joined.empty() ? "" : " ") + c;
            multi->values[idx] = joined;
            type->values[idx]  = spool.gradient ? "0" : "1";
        } else {
            multi->values[idx] = spool.color;
            type->values[idx]  = "1";
        }
        if (auto* gl = pc.option<ConfigOptionFloats>("filament_gradient_length", true)) {
            gl->values.resize(n, 0.);
            gl->values[idx] = 0.; // automatic
        }
    }
    // 3. the spool itself
    set_project_spool_id(idx, spool.id);
    bundle->normalize_filament_colour_lists();
    // 4. refresh what shows filaments (same epilogue as the colour dialog / the IFS sync)
    Plater* plater = wxGetApp().plater();
    DynamicPrintConfig cfg_new;
    cfg_new.set_key_value("filament_colour", colour->clone());
    cfg_new.set_key_value("filament_multi_colour", multi->clone());
    cfg_new.set_key_value("filament_colour_type", type->clone());
    plater->update_filament_colors_in_full_config();
    wxGetApp().sidebar().update_presets(Preset::TYPE_FILAMENT);
    wxGetApp().sidebar().update_dynamic_filament_list();
    if (wxGetApp().obj_list() != nullptr)
        wxGetApp().obj_list()->update_filament_colors();
    plater->update_project_dirty_from_presets();
    bundle->export_selections(*wxGetApp().app_config);
    CfsUi::remember_colours(idx); // Bambu AMS: keep the multi-colour setup of the mapped slot
    plater->on_config_change(cfg_new);
    // 5. the slot the filament is mapped to takes the spool (Flashforge lane_data / local link)
    CfsUi::link_spool_to_slot(idx, spool.id);
    return true;
}

void clear_spool(int idx)
{
    if (project_spool_id(idx) <= 0)
        return;
    set_project_spool_id(idx, 0);
    if (PresetBundle* bundle = wxGetApp().preset_bundle) {
        bundle->normalize_filament_colour_lists();
        wxGetApp().plater()->update_project_dirty_from_presets();
    }
    CfsUi::link_spool_to_slot(idx, 0);
    for (PlaterPresetComboBox* cb : wxGetApp().sidebar().combos_filament())
        if (cb != nullptr)
            cb->update();
}

std::vector<Shortage> low_spools()
{
    std::vector<Shortage> out;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    Plater*       plater = wxGetApp().plater();
    if (bundle == nullptr || plater == nullptr)
        return out;
    PartPlate* plate = plater->get_partplate_list().get_curr_plate();
    if (plate == nullptr || plate->get_slice_result() == nullptr)
        return out;
    const auto& volumes = plate->get_slice_result()->print_statistics.total_volumes_per_extruder; // mm3 per filament
    const DynamicPrintConfig full = bundle->full_config();
    const auto*              density = full.option<ConfigOptionFloats>("filament_density");
    const std::string url = Spoolman::configured_url();
    bool server_ok = !url.empty();
    for (const auto& [idx, volume] : volumes) {
        const int spool_id = project_spool_id(int(idx));
        if (spool_id <= 0 || volume <= 0.)
            continue;
        Spool spool;
        bool  have = false;
        if (server_ok) {
            std::string error;
            have      = Spoolman::fetch_spool(url, spool_id, spool, error, 3);
            server_ok = have || error.rfind("HTTP", 0) == 0; // unreachable server: do not wait for every filament
        }
        if (!have)
            have = Spoolman::cached_spool(spool_id, spool);
        if (!have || spool.remaining_weight < 0.)
            continue;
        const double d = density != nullptr && idx < density->values.size() && density->values[idx] > 0. ? density->values[idx] : 1.24;
        const double needed = volume * d / 1000.;
        if (needed > spool.remaining_weight + 0.5)
            out.push_back({int(idx), spool_id, needed, spool.remaining_weight, spool.title()});
    }
    return out;
}

wxString low_spools_text()
{
    wxString text;
    for (const Shortage& s : low_spools())
        text += wxString::Format(_L("Filament %d: spool %s has %.0f g left, the plate needs %.0f g."), s.filament_idx + 1, wxString::FromUTF8(s.title), s.remaining_g, s.needed_g) + "\n";
    text.Trim();
    return text;
}

bool confirm_spools_enough(wxWindow* parent)
{
    const wxString text = low_spools_text();
    if (text.empty())
        return true;
    MessageDialog dlg(parent, text + "\n\n" + _L("Print anyway?"), _L("Spoolman"), wxICON_WARNING | wxYES_NO);
    return dlg.ShowModal() == wxID_YES;
}

void pick_spool(wxWindow* parent, int idx)
{
    SpoolDialog dlg(parent, idx, project_spool_id(idx), CfsUi::mapped_slot_spool(idx));
    if (dlg.ShowModal() != wxID_OK)
        return;
    if (dlg.unlink_requested()) {
        clear_spool(idx);
        return;
    }
    Spool spool;
    if (dlg.selected_spool(spool))
        apply_spool(idx, spool);
}

}}} // namespace Slic3r::GUI::SpoolmanUi
