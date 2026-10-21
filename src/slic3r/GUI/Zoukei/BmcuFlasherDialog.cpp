// BMCU firmware update dialog. See BmcuFlasherDialog.hpp.
// Firmware choices, download checks, bootloader instructions and the flash sequence follow BMCU-Flasher
// (https://github.com/jarczakpawel/BMCU-Flasher, MIT, Copyright (c) 2026 Pawel Jarczak); the UI is Orca's own.
#include "BmcuFlasherDialog.hpp"

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/LabeledStaticBox.hpp"
#include "slic3r/Utils/BmcuFlasher.hpp"
#include "slic3r/Utils/BmcuSerialPort.hpp"
#include "slic3r/Utils/Http.hpp"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

namespace {

namespace B = Slic3r::Bmcu;

constexpr const char *kCfg = "bmcu_flasher";

struct Palette { wxColour bg, fg, muted, accent, error, warn; };
Palette palette()
{
    if (wxGetApp().dark_mode())
        return {wxColour("#2D2D31"), wxColour("#EFEFF0"), wxColour("#A0A0A6"), wxColour("#00A693"), wxColour("#FF6B6B"), wxColour("#F0B04A")};
    return {wxColour("#FFFFFF"), wxColour("#363636"), wxColour("#7C7C7C"), wxColour("#009688"), wxColour("#E14747"), wxColour("#B26A00")};
}

// Blocking GET on the calling (worker) thread.
bool http_get(const std::string &url, size_t limit, std::string &body, std::string &err, const std::function<void(size_t, size_t)> &prog = {})
{
    unsigned status = 0;
    bool ok = false;
    try {
        auto http = Http::get(url);
        http.timeout_connect(10)
            .timeout_max(60)
            .size_limit(limit)
            .header("User-Agent", "OrcaSlicer-Zoukei-BMCU")
            .header("Accept", "application/vnd.github+json, text/plain, */*")
            .on_complete([&](std::string b, unsigned st) { body = std::move(b); status = st; ok = true; })
            .on_error([&](std::string b, std::string e, unsigned st) {
                status = st;
                err = e.empty() ? ("HTTP " + std::to_string(st)) : e;
                (void) b;
            });
        if (prog)
            http.on_progress([&](Http::Progress p, bool &) { prog(p.dlnow, p.dltotal); });
        http.perform_sync();
    } catch (const std::exception &e) {
        err = e.what();
        ok = false;
    }
    if (ok && status >= 400) {
        ok = false;
        err = "HTTP " + std::to_string(status);
    }
    return ok;
}

boost::filesystem::path cache_dir() { return boost::filesystem::path(data_dir()) / "bmcu_firmware"; }

bool read_binary(const boost::filesystem::path &p, B::Bytes &out, size_t limit)
{
    boost::nowide::ifstream f(p.string(), std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return out.size() <= limit;
}

wxString force_label(const std::string &force)
{
    if (force == "standard(A1)") return _L("Standard (recommended)");
    if (force == "soft_load(A1)") return _L("Soft load");
    if (force == "high_force_load(P1S)") return _L("High force (P1S)");
    return from_u8(force);
}

wxString force_hint(const std::string &force)
{
    if (force == "standard(A1)") return _L("Normal load pressure. The right choice for most A1 / A1 mini setups.");
    if (force == "soft_load(A1)")
        return _L("Weaker pressure while loading. Only if you hear grinding while loading (weak lever spring); it may reject filament more often.");
    if (force == "high_force_load(P1S)") return _L("Stronger load and print pressure (long paths such as P1S). Use only if you really need more force.");
    return wxEmptyString;
}

wxString slot_label(const std::string &slot)
{
    if (slot == "AMS_AUTO") return _L("AMS_AUTO (recommended)");
    return from_u8(slot);
}

int force_rank(const std::string &f)
{
    if (f == "standard(A1)") return 0;
    if (f == "soft_load(A1)") return 1;
    if (f == "high_force_load(P1S)") return 2;
    return 3;
}

struct Source
{
    enum Kind { Release, Branch, Local } kind = Branch;
    std::string ref;
    wxString    label;
};

struct Catalog
{
    bool                    loading = false;
    bool                    ready   = false;
    std::string             error;
    std::string             version; // x.x.x.x
    B::Manifest             manifest;
    std::vector<B::Variant> variants;
};

class BmcuFlasherDialog;

// Lets worker threads post to the dialog only while it exists.
struct Bridge
{
    std::mutex         mtx;
    BmcuFlasherDialog *dlg = nullptr;
    template<class F> void post(F &&fn);
};

class BmcuFlasherDialog : public DPIDialog
{
public:
    BmcuFlasherDialog(wxWindow *parent, const std::string &installed, std::function<std::string()> busy);
    ~BmcuFlasherDialog() override;
    void on_dpi_changed(const wxRect &) override { Layout(); Refresh(); }

    // ---- called on the UI thread from workers
    void on_releases(std::vector<B::Release> rel, std::string err);
    void on_catalog(std::string ref, Catalog cat);
    void on_download_progress(int pct);
    void on_download_done(B::Bytes fw, std::string sha, std::string err, std::string rel_path);
    void on_flash_log(B::LogLevel lvl, std::string msg);
    void on_flash_progress(int pct);
    void on_flash_stage(B::Stage s);
    void on_flash_done(bool ok, bool cancelled, std::string err);

private:
    void build();
    void rebuild_sources();
    void on_source_changed();
    void load_catalog(const std::string &ref);
    void fill_options();
    void on_option_changed();
    std::string selected_rel_path() const;
    const Catalog *current_catalog() const;
    void refresh_ports();
    void on_mode_changed();
    void on_flash_clicked();
    void confirm_and_flash(B::Bytes fw, const wxString &what, const wxString &check_line);
    void start_flash(B::Bytes fw);
    void set_busy_ui(bool busy);
    void update_state();
    void log_line(const wxString &line);
    void set_status(const wxString &s, const wxColour &c);
    void save_prefs();
    void on_close(wxCloseEvent &e);
    B::Mode mode() const { return m_mode->GetSelection() == 1 ? B::Mode::Ttl : B::Mode::Usb; }

    std::shared_ptr<Bridge>         m_bridge = std::make_shared<Bridge>();
    std::string                     m_installed;
    std::function<std::string()>    m_busy;
    std::vector<B::Release>         m_releases;
    bool                            m_releases_loaded = false;
    std::vector<Source>             m_sources;
    std::map<std::string, Catalog>  m_catalogs;
    std::string                     m_main_version;
    std::string                     m_local_path;

    std::vector<std::string>        m_forces, m_slots, m_retracts;
    std::vector<B::PortInfo>        m_ports;

    std::thread                     m_worker;
    std::atomic<bool>               m_cancel{false};
    bool                            m_downloading = false;
    bool                            m_flashing = false;
    B::Stage                        m_stage = B::Stage::Connect;
    bool                            m_flash_ok = false;

    // widgets
    wxStaticText *m_head_ver   = nullptr;
    ComboBox     *m_source     = nullptr;
    wxStaticText *m_source_info = nullptr;
    wxPanel      *m_variant_panel = nullptr;
    ComboBox     *m_force      = nullptr;
    wxStaticText *m_force_hint = nullptr;
    ComboBox     *m_slot       = nullptr;
    ComboBox     *m_retract    = nullptr;
    wxCheckBox   *m_autoload   = nullptr;
    wxCheckBox   *m_rgb        = nullptr;
    wxPanel      *m_local_panel = nullptr;
    wxStaticText *m_local_label = nullptr;
    ComboBox     *m_mode       = nullptr;
    ComboBox     *m_port       = nullptr;
    wxStaticText *m_port_info  = nullptr;
    wxStaticText *m_steps      = nullptr;
    wxGauge      *m_gauge      = nullptr;
    wxStaticText *m_status     = nullptr;
    wxTextCtrl   *m_log        = nullptr;
    Button       *m_btn_flash  = nullptr;
    Button       *m_btn_close  = nullptr;
    Button       *m_btn_refresh = nullptr;
    Button       *m_btn_browse = nullptr;
    Button       *m_btn_copy   = nullptr;
};

template<class F> void Bridge::post(F &&fn)
{
    std::lock_guard<std::mutex> lk(mtx);
    if (dlg) {
        BmcuFlasherDialog *d = dlg;
        d->CallAfter([d, fn = std::forward<F>(fn)]() mutable { fn(*d); });
    }
}

// ---------------------------------------------------------------------------------------------------------------
BmcuFlasherDialog::BmcuFlasherDialog(wxWindow *parent, const std::string &installed, std::function<std::string()> busy)
    : DPIDialog(parent, wxID_ANY, _L("BMCU firmware update"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_installed(B::normalize_version(installed))
    , m_busy(std::move(busy))
{
    m_bridge->dlg = this;
    build();
    rebuild_sources();
    refresh_ports();
    on_mode_changed();

    // releases list + the main branch version, in the background
    auto bridge = m_bridge;
    std::thread([bridge]() {
        std::string body, err;
        std::vector<B::Release> rel;
        if (http_get(B::releases_api_url(), 2 * 1024 * 1024, body, err))
            rel = B::parse_releases(body);
        if (rel.empty() && err.empty())
            err = "no releases in the GitHub answer";
        bridge->post([rel = std::move(rel), err](BmcuFlasherDialog &d) mutable { d.on_releases(std::move(rel), err); });
    }).detach();

    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    Fit();
    SetMinSize(FromDIP(wxSize(760, 640)));
    CenterOnParent();
}

BmcuFlasherDialog::~BmcuFlasherDialog()
{
    {
        std::lock_guard<std::mutex> lk(m_bridge->mtx);
        m_bridge->dlg = nullptr;
    }
    if (m_worker.joinable())
        m_worker.join();
}

void BmcuFlasherDialog::build()
{
    const Palette pal = palette();
    SetBackgroundColour(pal.bg);
    SetForegroundColour(pal.fg);
    SetFont(Label::Body_14);
    Bind(wxEVT_CLOSE_WINDOW, &BmcuFlasherDialog::on_close, this);

    auto *top = new wxBoxSizer(wxVERTICAL);
    const int gap = FromDIP(8);

    // header
    auto *title = new wxStaticText(this, wxID_ANY, _L("Update the BMCU firmware"));
    title->SetFont(Label::Head_18);
    top->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
    m_head_ver = new wxStaticText(this, wxID_ANY, "");
    m_head_ver->SetFont(Label::Body_13);
    m_head_ver->SetForegroundColour(pal.muted);
    top->Add(m_head_ver, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14) / 2 + 2);
    auto *warn = new wxStaticText(this, wxID_ANY,
                                  _L("Turn the printer off and unplug the BMCU from the printer before connecting it to this computer. "
                                     "Never flash while the BMCU is connected to the printer."));
    warn->SetFont(Label::Body_13);
    warn->SetForegroundColour(pal.warn);
    warn->Wrap(FromDIP(720));
    top->Add(warn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14) / 2 + 2);

    // firmware
    auto *fw_box   = new LabeledStaticBox(this, _L("Firmware"));
    auto *fw_sizer = new wxStaticBoxSizer(fw_box, wxVERTICAL);
    auto *grid = new wxFlexGridSizer(0, 2, gap, FromDIP(10));
    grid->AddGrowableCol(1);
    auto label = [&](wxWindow *parent, const wxString &t) {
        auto *l = new wxStaticText(parent, wxID_ANY, t);
        l->SetFont(Label::Body_14);
        return l;
    };
    m_source = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(420, -1)), 0, nullptr, wxCB_READONLY);
    grid->Add(label(this, _L("Version")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_source, 1, wxEXPAND);
    grid->AddSpacer(0);
    m_source_info = new wxStaticText(this, wxID_ANY, "");
    m_source_info->SetFont(Label::Body_12);
    m_source_info->SetForegroundColour(pal.muted);
    grid->Add(m_source_info, 1, wxEXPAND);
    fw_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));

    m_variant_panel = new wxPanel(this);
    m_variant_panel->SetBackgroundColour(pal.bg);
    auto *vg = new wxFlexGridSizer(0, 2, gap, FromDIP(10));
    vg->AddGrowableCol(1);
    m_force   = new ComboBox(m_variant_panel, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(300, -1)), 0, nullptr, wxCB_READONLY);
    m_slot    = new ComboBox(m_variant_panel, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(300, -1)), 0, nullptr, wxCB_READONLY);
    m_retract = new ComboBox(m_variant_panel, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(300, -1)), 0, nullptr, wxCB_READONLY);
    m_autoload = new wxCheckBox(m_variant_panel, wxID_ANY, _L("Automatic filament load (AUTOLOAD)"));
    m_rgb      = new wxCheckBox(m_variant_panel, wxID_ANY, _L("Filament color on the BMCU LEDs (RGB)"));
    {
        const AppConfig *cfg = wxGetApp().app_config;
        m_autoload->SetValue(cfg->get(kCfg, "autoload").empty() || cfg->get_bool(kCfg, "autoload"));
        m_rgb->SetValue(cfg->get(kCfg, "rgb").empty() || cfg->get_bool(kCfg, "rgb"));
    }
    m_force_hint = new wxStaticText(m_variant_panel, wxID_ANY, "");
    m_force_hint->SetFont(Label::Body_12);
    m_force_hint->SetForegroundColour(pal.muted);
    vg->Add(label(m_variant_panel, _L("Load force")), 0, wxALIGN_CENTER_VERTICAL);
    vg->Add(m_force, 0);
    vg->AddSpacer(0);
    vg->Add(m_force_hint, 1, wxEXPAND);
    vg->Add(label(m_variant_panel, _L("BMCU slot")), 0, wxALIGN_CENTER_VERTICAL);
    vg->Add(m_slot, 0);
    vg->Add(label(m_variant_panel, _L("Retraction")), 0, wxALIGN_CENTER_VERTICAL);
    vg->Add(m_retract, 0);
    vg->AddSpacer(0);
    auto *rh = new wxStaticText(m_variant_panel, wxID_ANY, _L("A1 / A1 mini: 9.5 cm. Long PTFE paths (P1S and similar): usually 60-90 cm."));
    rh->SetFont(Label::Body_12);
    rh->SetForegroundColour(pal.muted);
    vg->Add(rh, 1, wxEXPAND);
    vg->AddSpacer(0);
    vg->Add(m_autoload, 0);
    vg->AddSpacer(0);
    vg->Add(m_rgb, 0);
    m_variant_panel->SetSizer(vg);
    fw_sizer->Add(m_variant_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    m_local_panel = new wxPanel(this);
    m_local_panel->SetBackgroundColour(pal.bg);
    auto *lp = new wxBoxSizer(wxHORIZONTAL);
    m_btn_browse = new Button(m_local_panel, _L("Choose .bin file..."));
    m_btn_browse->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    m_local_label = new wxStaticText(m_local_panel, wxID_ANY, _L("No file chosen. A local file is not checked against the GitHub manifest."));
    m_local_label->SetFont(Label::Body_12);
    m_local_label->SetForegroundColour(pal.muted);
    lp->Add(m_btn_browse, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    lp->Add(m_local_label, 1, wxALIGN_CENTER_VERTICAL);
    m_local_panel->SetSizer(lp);
    fw_sizer->Add(m_local_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    top->Add(fw_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // connection
    auto *cn_box   = new LabeledStaticBox(this, _L("Connection"));
    auto *cn_sizer = new wxStaticBoxSizer(cn_box, wxVERTICAL);
    auto *cg = new wxFlexGridSizer(0, 2, gap, FromDIP(10));
    cg->AddGrowableCol(1);
    m_mode = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(420, -1)), 0, nullptr, wxCB_READONLY);
    m_mode->Append(_L("USB: BMCU with CH340 on the board (automatic boot mode)"));
    m_mode->Append(_L("TTL: external USB-serial adapter (BOOT button by hand)"));
    m_mode->SetSelection(wxGetApp().app_config->get(kCfg, "mode") == "ttl" ? 1 : 0);
    auto *port_row = new wxBoxSizer(wxHORIZONTAL);
    m_port = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(330, -1)), 0, nullptr, wxCB_READONLY);
    m_btn_refresh = new Button(this, _L("Refresh"));
    m_btn_refresh->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    port_row->Add(m_port, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    port_row->Add(m_btn_refresh, 0, wxALIGN_CENTER_VERTICAL);
    m_port_info = new wxStaticText(this, wxID_ANY, "");
    m_port_info->SetFont(Label::Body_12);
    m_port_info->SetForegroundColour(pal.muted);
    cg->Add(label(this, _L("Mode")), 0, wxALIGN_CENTER_VERTICAL);
    cg->Add(m_mode, 1, wxEXPAND);
    cg->Add(label(this, _L("Port")), 0, wxALIGN_CENTER_VERTICAL);
    cg->Add(port_row, 1, wxEXPAND);
    cg->AddSpacer(0);
    cg->Add(m_port_info, 1, wxEXPAND);
    cn_sizer->Add(cg, 0, wxEXPAND | wxALL, FromDIP(8));
    m_steps = new wxStaticText(this, wxID_ANY, "");
    m_steps->SetFont(Label::Body_13);
    cn_sizer->Add(m_steps, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    top->Add(cn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // progress + log
    auto *pr_box   = new LabeledStaticBox(this, _L("Progress"));
    auto *pr_sizer = new wxStaticBoxSizer(pr_box, wxVERTICAL);
    m_status = new wxStaticText(this, wxID_ANY, _L("Ready."));
    m_status->SetFont(Label::Head_14);
    pr_sizer->Add(m_status, 0, wxEXPAND | wxALL, FromDIP(6));
    m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(-1, 14)));
    pr_sizer->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
    m_log = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(-1, 150)), wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    m_log->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
    m_log->SetBackgroundColour(pal.bg);
    m_log->SetForegroundColour(pal.fg);
    pr_sizer->Add(m_log, 1, wxEXPAND | wxALL, FromDIP(6));
    top->Add(pr_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // buttons
    auto *bottom = new wxBoxSizer(wxHORIZONTAL);
    m_btn_copy = new Button(this, _L("Copy log"));
    m_btn_copy->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    auto *credit = new wxStaticText(this, wxID_ANY, _L("Flashing protocol from BMCU-Flasher by Pawel Jarczak (MIT)."));
    credit->SetFont(Label::Body_12);
    credit->SetForegroundColour(pal.muted);
    m_btn_close = new Button(this, _L("Close"));
    m_btn_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_btn_flash = new Button(this, _L("Flash firmware"));
    m_btn_flash->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    bottom->Add(m_btn_copy, 0, wxALL, FromDIP(6));
    bottom->Add(credit, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    bottom->Add(m_btn_close, 0, wxALL, FromDIP(6));
    bottom->Add(m_btn_flash, 0, wxALL, FromDIP(6));
    top->Add(bottom, 0, wxEXPAND | wxALL, FromDIP(8));

    SetSizer(top);

    m_source->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { on_source_changed(); });
    for (ComboBox *c : {m_force, m_slot, m_retract})
        c->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { on_option_changed(); });
    m_autoload->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { on_option_changed(); });
    m_rgb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { on_option_changed(); });
    m_mode->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { on_mode_changed(); });
    m_port->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { update_state(); });
    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh_ports(); });
    m_btn_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxFileDialog fd(this, _L("Choose a BMCU firmware file"), "", "", "BMCU firmware (*.bin)|*.bin", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (fd.ShowModal() != wxID_OK)
            return;
        m_local_path = into_u8(fd.GetPath());
        update_state();
    });
    m_btn_copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(m_log->GetValue()));
            wxTheClipboard->Close();
        }
    });
    m_btn_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (m_flashing) {
            // before the erase this asks the worker to stop; afterwards the button is disabled
            m_cancel = true;
            set_status(_L("Stopping before the erase..."), palette().warn);
            return;
        }
        Close();
    });
    m_btn_flash->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_flash_clicked(); });

    wxString head = m_installed.empty() ? _L("Installed version: unknown")
                                        : wxString::Format(_L("Installed version: %s"), from_u8(m_installed));
    m_head_ver->SetLabel(head);
}

void BmcuFlasherDialog::rebuild_sources()
{
    const std::string prev = m_source->GetSelection() >= 0 && size_t(m_source->GetSelection()) < m_sources.size()
                                 ? m_sources[m_source->GetSelection()].ref : wxGetApp().app_config->get(kCfg, "source");
    m_sources.clear();
    const B::Release *stable = nullptr;
    for (const auto &r : m_releases)
        if (!r.prerelease) { stable = &r; break; }
    if (stable)
        m_sources.push_back({Source::Release, stable->tag, wxString::Format(_L("Latest release %s (recommended)"), from_u8(stable->tag))});
    m_sources.push_back({Source::Branch, "main", _L("main branch (what BMCU-Flasher downloads)")});
    for (const auto &r : m_releases) {
        if (stable && r.tag == stable->tag)
            continue;
        wxString l = from_u8(r.tag);
        if (r.published_at.size() >= 10)
            l += " (" + from_u8(r.published_at.substr(0, 10)) + ")";
        if (r.prerelease)
            l += " - " + _L("pre-release");
        m_sources.push_back({Source::Release, r.tag, l});
    }
    m_sources.push_back({Source::Local, "local", _L("Local .bin file")});

    m_source->Clear();
    int sel = 0;
    for (size_t i = 0; i < m_sources.size(); ++i) {
        m_source->Append(m_sources[i].label);
        if (m_sources[i].ref == prev)
            sel = int(i);
    }
    m_source->SetSelection(sel);
    on_source_changed();
}

void BmcuFlasherDialog::on_releases(std::vector<B::Release> rel, std::string err)
{
    m_releases_loaded = true;
    m_releases = std::move(rel);
    if (!err.empty() && m_releases.empty())
        log_line(wxString::Format(_L("Could not list the GitHub releases (%s); only the main branch is offered."), from_u8(err)));
    rebuild_sources();
}

void BmcuFlasherDialog::on_source_changed()
{
    const int i = m_source->GetSelection();
    const bool local = i >= 0 && size_t(i) < m_sources.size() && m_sources[i].kind == Source::Local;
    m_variant_panel->Show(!local);
    m_local_panel->Show(local);
    if (!local && i >= 0 && size_t(i) < m_sources.size())
        load_catalog(m_sources[i].ref);
    fill_options();
    Layout();
}

void BmcuFlasherDialog::load_catalog(const std::string &ref)
{
    Catalog &c = m_catalogs[ref];
    if (c.loading || c.ready)
        return;
    c.loading = true;
    c.error.clear();
    auto bridge = m_bridge;
    std::thread([bridge, ref]() {
        Catalog cat;
        std::string body, err;
        if (http_get(B::version_url(ref) + "?ts=" + std::to_string(time(nullptr)), 512, body, err)) {
            cat.version = B::normalize_version(body);
            if (cat.version.empty())
                err = "invalid version file";
        }
        if (err.empty()) {
            body.clear();
            if (http_get(B::manifest_url(ref), 4 * 1024 * 1024, body, err)) {
                try {
                    cat.manifest = B::parse_manifest(body);
                    cat.variants = B::variants_from_manifest(cat.manifest);
                    if (cat.variants.empty())
                        err = "no firmware in the manifest";
                } catch (const std::exception &e) {
                    err = e.what();
                }
            } else if (err == "HTTP 404")
                err = "this version has no firmware manifest (too old for direct flashing)";
        }
        cat.error = err;
        cat.ready = err.empty();
        bridge->post([ref, cat = std::move(cat)](BmcuFlasherDialog &d) mutable { d.on_catalog(ref, std::move(cat)); });
    }).detach();
}

void BmcuFlasherDialog::on_catalog(std::string ref, Catalog cat)
{
    cat.loading = false;
    if (ref == "main" && cat.ready)
        m_main_version = cat.version;
    if (!cat.error.empty())
        log_line(from_u8(ref) + ": " + from_u8(cat.error));
    m_catalogs[ref] = std::move(cat);
    fill_options();
}

const Catalog *BmcuFlasherDialog::current_catalog() const
{
    const int i = m_source->GetSelection();
    if (i < 0 || size_t(i) >= m_sources.size() || m_sources[i].kind == Source::Local)
        return nullptr;
    auto it = m_catalogs.find(m_sources[i].ref);
    return it == m_catalogs.end() ? nullptr : &it->second;
}

void BmcuFlasherDialog::fill_options()
{
    const Catalog *c = current_catalog();
    m_forces.clear();
    m_slots.clear();
    m_retracts.clear();
    if (c && c->ready) {
        std::set<std::string> f, s, r;
        for (const auto &v : c->variants) { f.insert(v.force); s.insert(v.slot); r.insert(v.retract); }
        m_forces.assign(f.begin(), f.end());
        m_slots.assign(s.begin(), s.end());
        m_retracts.assign(r.begin(), r.end());
        std::stable_sort(m_forces.begin(), m_forces.end(), [](const std::string &a, const std::string &b) { return force_rank(a) < force_rank(b); });
        std::stable_sort(m_slots.begin(), m_slots.end(), [](const std::string &a, const std::string &b) {
            return (a == "AMS_AUTO") != (b == "AMS_AUTO") ? a == "AMS_AUTO" : a < b;
        });
        std::sort(m_retracts.begin(), m_retracts.end(), [](const std::string &a, const std::string &b) { return B::retract_value(a) < B::retract_value(b); });
    }
    auto fill = [](ComboBox *cb, const std::vector<std::string> &vals, const std::function<wxString(const std::string &)> &lbl,
                   const std::string &want, const std::string &fallback) {
        const wxString keep = cb->GetSelection() >= 0 ? cb->GetString(cb->GetSelection()) : wxString();
        cb->Clear();
        int sel = -1;
        for (size_t i = 0; i < vals.size(); ++i) {
            cb->Append(lbl(vals[i]));
            if (!keep.empty() && lbl(vals[i]) == keep && sel < 0) sel = int(i); // what the user has now
        }
        if (sel < 0)
            for (size_t i = 0; i < vals.size(); ++i)
                if (vals[i] == want) sel = int(i); // last used
        if (sel < 0)
            for (size_t i = 0; i < vals.size(); ++i)
                if (vals[i] == fallback) sel = int(i);
        if (sel < 0 && !vals.empty()) sel = 0;
        if (sel >= 0) cb->SetSelection(sel);
        cb->Enable(!vals.empty());
    };
    AppConfig *cfg = wxGetApp().app_config;
    fill(m_force, m_forces, force_label, cfg->get(kCfg, "force"), "standard(A1)");
    fill(m_slot, m_slots, slot_label, cfg->get(kCfg, "slot"), "AMS_AUTO");
    fill(m_retract, m_retracts, [](const std::string &r) { return from_u8(B::retract_display(r)); }, cfg->get(kCfg, "retract"), "0.095f");
    on_option_changed();
}

void BmcuFlasherDialog::on_option_changed()
{
    const int f = m_force->GetSelection();
    m_force_hint->SetLabel(f >= 0 && size_t(f) < m_forces.size() ? force_hint(m_forces[f]) : wxString());
    m_force_hint->Wrap(FromDIP(460));
    update_state();
}

std::string BmcuFlasherDialog::selected_rel_path() const
{
    const int f = m_force->GetSelection(), s = m_slot->GetSelection(), r = m_retract->GetSelection();
    if (f < 0 || s < 0 || r < 0 || size_t(f) >= m_forces.size() || size_t(s) >= m_slots.size() || size_t(r) >= m_retracts.size())
        return {};
    return B::build_rel_path(m_forces[f], m_autoload->GetValue(), m_rgb->GetValue(), m_slots[s], m_retracts[r]);
}

void BmcuFlasherDialog::refresh_ports()
{
    const wxString keep = m_port->GetSelection() >= 0 ? m_port->GetString(m_port->GetSelection()) : wxString();
    const std::string saved = wxGetApp().app_config->get(kCfg, "port");
    try {
        m_ports = B::list_ports();
    } catch (const std::exception &e) {
        m_ports.clear();
        log_line(wxString::Format(_L("Could not list the serial ports: %s"), from_u8(e.what())));
    }
    m_port->Clear();
    int sel = -1;
    for (size_t i = 0; i < m_ports.size(); ++i) {
        const auto &p = m_ports[i];
        wxString l = from_u8(p.port);
        if (!p.description.empty() && p.description != p.port)
            l += " - " + from_u8(p.description);
        if (!p.adapter.empty())
            l += " [" + from_u8(p.adapter) + "]";
        m_port->Append(l);
        if (l == keep) sel = int(i);
    }
    if (sel < 0)
        for (size_t i = 0; i < m_ports.size(); ++i)
            if (m_ports[i].port == saved) sel = int(i);
    if (sel < 0 && !m_ports.empty())
        sel = 0; // CH340 ports are listed first
    if (sel >= 0)
        m_port->SetSelection(sel);
    update_state();
}

void BmcuFlasherDialog::on_mode_changed()
{
    if (mode() == B::Mode::Usb)
        m_steps->SetLabel(_L("1. Turn the printer off and unplug the BMCU cable from the printer.\n"
                             "2. Connect the USB port of the BMCU board (CH340) to this computer.\n"
                             "3. Windows may ask for the WCH CH340/CH341 driver.\n"
                             "4. Orca switches the BMCU to boot mode by itself; no button is needed."));
    else
        m_steps->SetLabel(_L("1. Turn the printer off and unplug the BMCU from the printer. Do not connect 24 V.\n"
                             "2. Wire the adapter: RX to TX, TX to RX, 3V3 to 3V3, GND to GND.\n"
                             "3. When asked: unplug the adapter USB, hold BOOT, plug it back and release BOOT when the board LED lights up.\n"
                             "4. Halfway through Orca asks again: keep USB connected, hold BOOT, tap RESET, release BOOT."));
    m_steps->Wrap(FromDIP(700));
    update_state();
    Layout();
}

void BmcuFlasherDialog::update_state()
{
    const Palette pal = palette();
    // source info
    const int si = m_source->GetSelection();
    const bool local = si >= 0 && size_t(si) < m_sources.size() && m_sources[si].kind == Source::Local;
    bool fw_ok = false;
    if (local) {
        fw_ok = !m_local_path.empty();
        m_source_info->SetLabel(_L("Advanced: flashes a file from disk, for recovery or test builds."));
        if (fw_ok)
            m_local_label->SetLabel(from_u8(boost::filesystem::path(m_local_path).filename().string()) + "  -  " +
                                    _L("not checked against the GitHub manifest"));
    } else if (const Catalog *c = current_catalog()) {
        if (c->ready) {
            wxString info = wxString::Format(_L("Firmware %s (%s)"), from_u8(B::display_version(c->version)), from_u8(c->version));
            if (!m_installed.empty()) {
                const int cmp = B::compare_versions(c->version, m_installed);
                info += "  -  " + (cmp > 0 ? _L("newer than the installed one") : cmp == 0 ? _L("same as the installed one") : _L("older than the installed one"));
            }
            const std::string rel = selected_rel_path();
            if (!rel.empty() && c->manifest.count(rel)) {
                fw_ok = true;
                info += "\n" + from_u8(rel) + "  (" + wxString::Format(_L("%d bytes"), int(c->manifest.at(rel).size)) + ")";
            } else if (!rel.empty())
                info += "\n" + _L("This combination does not exist in this version.");
            m_source_info->SetLabel(info);
        } else if (!c->error.empty())
            m_source_info->SetLabel(wxString::Format(_L("Not available: %s"), from_u8(c->error)));
        else
            m_source_info->SetLabel(_L("Reading the firmware list from GitHub..."));
    } else
        m_source_info->SetLabel(_L("Reading the firmware list from GitHub..."));
    m_source_info->Wrap(FromDIP(460));

    // port info
    const int pi = m_port->GetSelection();
    bool port_ok = pi >= 0 && size_t(pi) < m_ports.size();
    if (m_ports.empty())
        m_port_info->SetLabel(mode() == B::Mode::Usb ? _L("No serial port found. Connect the BMCU USB cable and click Refresh.")
                                                    : _L("No serial port found. Connect the USB-serial adapter and click Refresh."));
    else if (port_ok && mode() == B::Mode::Usb && !m_ports[pi].is_ch340)
        m_port_info->SetLabel(_L("This port is not a CH340 (1A86:7523). The BMCU board uses a CH340; use TTL mode for other adapters."));
    else if (port_ok && m_ports[pi].is_ch340)
        m_port_info->SetLabel(_L("CH340 found: this is most likely the BMCU."));
    else
        m_port_info->SetLabel(wxEmptyString);
    m_port_info->SetForegroundColour(port_ok && (mode() == B::Mode::Ttl || m_ports[pi].is_ch340) ? pal.muted : pal.warn);

    const bool busy = m_flashing || m_downloading;
    m_btn_flash->Enable(!busy && fw_ok && port_ok);
    m_source->Enable(!busy);
    m_variant_panel->Enable(!busy);
    m_local_panel->Enable(!busy);
    m_mode->Enable(!busy);
    m_port->Enable(!busy);
    m_btn_refresh->Enable(!busy);
    if (m_flashing) {
        m_btn_close->SetLabel(_L("Stop"));
        m_btn_close->Enable(m_stage < B::Stage::Erase && !m_cancel);
    } else {
        m_btn_close->SetLabel(_L("Close"));
        m_btn_close->Enable(!m_downloading);
    }
    if (!m_flashing && !m_downloading && !m_flash_ok) {
        const Catalog *c = current_catalog();
        m_btn_flash->SetLabel(c && c->ready && !m_installed.empty() && B::compare_versions(c->version, m_installed) == 0 ? _L("Reflash firmware")
                                                                                                                     : _L("Flash firmware"));
    }
    Layout();
}

void BmcuFlasherDialog::log_line(const wxString &line)
{
    m_log->AppendText(line + "\n");
    BOOST_LOG_TRIVIAL(info) << "BMCU flasher: " << into_u8(line);
}

void BmcuFlasherDialog::set_status(const wxString &s, const wxColour &c)
{
    m_status->SetLabel(s);
    m_status->SetForegroundColour(c);
    m_status->Wrap(FromDIP(700));
    Layout();
}

void BmcuFlasherDialog::save_prefs()
{
    AppConfig *cfg = wxGetApp().app_config;
    const int si = m_source->GetSelection();
    if (si >= 0 && size_t(si) < m_sources.size())
        cfg->set(kCfg, "source", m_sources[si].ref);
    const int f = m_force->GetSelection(), s = m_slot->GetSelection(), r = m_retract->GetSelection();
    if (f >= 0 && size_t(f) < m_forces.size()) cfg->set(kCfg, "force", m_forces[f]);
    if (s >= 0 && size_t(s) < m_slots.size()) cfg->set(kCfg, "slot", m_slots[s]);
    if (r >= 0 && size_t(r) < m_retracts.size()) cfg->set(kCfg, "retract", m_retracts[r]);
    cfg->set(kCfg, "autoload", m_autoload->GetValue());
    cfg->set(kCfg, "rgb", m_rgb->GetValue());
    cfg->set(kCfg, "mode", mode() == B::Mode::Ttl ? "ttl" : "usb");
    const int pi = m_port->GetSelection();
    if (pi >= 0 && size_t(pi) < m_ports.size())
        cfg->set(kCfg, "port", m_ports[pi].port);
}

void BmcuFlasherDialog::on_flash_clicked()
{
    if (m_flashing || m_downloading)
        return;
    if (m_busy) {
        const std::string why = m_busy();
        if (!why.empty()) {
            MessageDialog(this, from_u8(why) + "\n\n" + _L("Flashing the BMCU is blocked until the printer is idle."), _L("BMCU firmware update"),
                          wxOK | wxICON_WARNING).ShowModal();
            return;
        }
    }
    save_prefs();
    m_flash_ok = false;
    m_gauge->SetValue(0);

    const int si = m_source->GetSelection();
    if (si < 0 || size_t(si) >= m_sources.size())
        return;
    if (m_sources[si].kind == Source::Local) {
        B::Bytes fw;
        if (!read_binary(boost::filesystem::path(m_local_path), fw, B::FLASH_BYTES) || fw.empty()) {
            MessageDialog(this, _L("The file cannot be read or is larger than the 64 KiB flash of the BMCU."), _L("BMCU firmware update"), wxOK | wxICON_ERROR)
                .ShowModal();
            return;
        }
        const std::string sha = B::sha256_hex(fw.data(), fw.size());
        log_line(wxString::Format("local file %s size=%d sha256=%s", from_u8(m_local_path), int(fw.size()), from_u8(sha)));
        confirm_and_flash(std::move(fw), from_u8(boost::filesystem::path(m_local_path).filename().string()),
                          wxString::Format(_L("SHA-256 %s (local file, not checked against GitHub)"), from_u8(sha.substr(0, 16)) + "..."));
        return;
    }

    const Catalog *c = current_catalog();
    const std::string rel = selected_rel_path();
    if (!c || !c->ready || !c->manifest.count(rel) || !B::is_safe_rel_path(rel))
        return;
    const B::ManifestEntry entry = c->manifest.at(rel);
    const std::string ref = m_sources[si].ref;

    // cache hit?
    const boost::filesystem::path cached = cache_dir() / (entry.sha256 + ".bin");
    B::Bytes fw;
    std::string err;
    if (boost::filesystem::exists(cached) && read_binary(cached, fw, B::FLASH_BYTES) && B::verify_firmware(fw, entry, err)) {
        log_line("online: cache hit (" + from_u8(cached.filename().string()) + ")");
        on_download_done(std::move(fw), entry.sha256, "", rel);
        return;
    }

    m_downloading = true;
    update_state();
    set_status(_L("Downloading the firmware from GitHub..."), palette().fg);
    const std::string url = B::firmware_url(ref, rel);
    log_line("online: download " + from_u8(rel));
    log_line("online: url " + from_u8(url));
    auto bridge = m_bridge;
    std::thread([bridge, url, entry, rel, cached]() {
        std::string body, err;
        B::Bytes fw;
        auto last = std::make_shared<int>(-1);
        if (http_get(url, B::FLASH_BYTES + 1024, body, err, [bridge, last, total_hint = entry.size](size_t now, size_t total) {
                const size_t t = total ? total : total_hint;
                if (t) {
                    const int pct = int(std::min<size_t>(100, now * 100 / t));
                    if (pct != *last) {
                        *last = pct;
                        bridge->post([pct](BmcuFlasherDialog &d) { d.on_download_progress(pct); });
                    }
                }
            })) {
            fw.assign(body.begin(), body.end());
            if (B::verify_firmware(fw, entry, err)) {
                err.clear();
                try {
                    boost::filesystem::create_directories(cached.parent_path());
                    const boost::filesystem::path tmp = cached.string() + ".part";
                    {
                        boost::nowide::ofstream o(tmp.string(), std::ios::binary | std::ios::trunc);
                        o.write(body.data(), std::streamsize(body.size()));
                    }
                    boost::filesystem::rename(tmp, cached);
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(warning) << "BMCU flasher: cache write failed: " << e.what();
                }
            }
        }
        bridge->post([fw = std::move(fw), sha = entry.sha256, err, rel](BmcuFlasherDialog &d) mutable {
            d.on_download_done(std::move(fw), sha, err, rel);
        });
    }).detach();
}

void BmcuFlasherDialog::on_download_progress(int pct)
{
    if (m_downloading)
        m_gauge->SetValue(pct);
}

void BmcuFlasherDialog::on_download_done(B::Bytes fw, std::string sha, std::string err, std::string rel_path)
{
    m_downloading = false;
    m_gauge->SetValue(0);
    if (!err.empty() || fw.empty()) {
        log_line("online: " + from_u8(err));
        set_status(wxString::Format(_L("Download failed: %s"), from_u8(err)), palette().error);
        update_state();
        return;
    }
    log_line(wxString::Format("online: ok size=%d sha=%s", int(fw.size()), from_u8(sha)));
    set_status(_L("Firmware downloaded and checked (size, CRC32, SHA-256)."), palette().fg);
    update_state();
    const Catalog *c = current_catalog();
    const wxString ver = c ? from_u8(B::display_version(c->version)) : wxString();
    confirm_and_flash(std::move(fw), ver + "  " + from_u8(rel_path),
                      wxString::Format(_L("Checked against the GitHub manifest: SHA-256 %s"), from_u8(sha.substr(0, 16)) + "..."));
}

void BmcuFlasherDialog::confirm_and_flash(B::Bytes fw, const wxString &what, const wxString &check_line)
{
    const int pi = m_port->GetSelection();
    if (pi < 0 || size_t(pi) >= m_ports.size())
        return;
    const wxString msg = wxString::Format(_L("Write this firmware to the BMCU?\n\n%s\n%s\nPort: %s, mode: %s\n\n"
                                             "- The BMCU must be unplugged from the printer and the printer turned off.\n"
                                             "- Do not unplug the cable while it writes (about 10-60 s).\n"
                                             "- The old firmware is erased first. If writing fails the BMCU stays without firmware until it is "
                                             "flashed again; its bootloader cannot be erased, so it can always be recovered.\n\n"
                                             "Flash the firmware now?"),
                                          what, check_line, from_u8(m_ports[pi].port), mode() == B::Mode::Usb ? wxString("USB") : wxString("TTL"));
    MessageDialog dlg(this, msg, _L("Confirm BMCU firmware update"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES) {
        log_line("flash: cancelled by the user before starting");
        set_status(_L("Not flashed."), palette().muted);
        return;
    }
    if (m_busy) {
        const std::string why = m_busy();
        if (!why.empty()) {
            MessageDialog(this, from_u8(why) + "\n\n" + _L("Flashing the BMCU is blocked until the printer is idle."), _L("BMCU firmware update"),
                          wxOK | wxICON_WARNING).ShowModal();
            return;
        }
    }
    start_flash(std::move(fw));
}

void BmcuFlasherDialog::start_flash(B::Bytes fw)
{
    const int pi = m_port->GetSelection();
    const B::PortInfo port = m_ports[pi];
    B::FlashOptions opt;
    opt.mode = mode();
    if (opt.mode == B::Mode::Ttl)
        for (const auto &a : B::known_adapters())
            if (a.vid == port.vid && a.pid == port.pid)
                opt.fast_baud = a.fast_baud;

    if (m_worker.joinable())
        m_worker.join();
    m_flashing = true;
    m_cancel   = false;
    m_stage    = B::Stage::Connect;
    m_gauge->SetValue(0);
    log_line(wxString::Format("flash: port=%s mode=%s fast_baud=%u size=%d", from_u8(port.port), opt.mode == B::Mode::Usb ? "usb" : "ttl",
                              opt.fast_baud, int(fw.size())));
    set_status(_L("Connecting to the BMCU bootloader..."), palette().fg);
    update_state();

    auto bridge = m_bridge;
    std::atomic<bool> *cancel = &m_cancel; // the dialog joins this thread in its destructor
    m_worker = std::thread([bridge, cancel, fw = std::move(fw), opt, port_name = port.port]() {
        B::FlashCallbacks cb;
        cb.log = [bridge](B::LogLevel l, const std::string &m) {
            bridge->post([l, m](BmcuFlasherDialog &d) { d.on_flash_log(l, m); });
        };
        cb.progress = [bridge](int p) { bridge->post([p](BmcuFlasherDialog &d) { d.on_flash_progress(p); }); };
        cb.stage    = [bridge](B::Stage s) { bridge->post([s](BmcuFlasherDialog &d) { d.on_flash_stage(s); }); };
        cb.cancelled = [cancel]() { return cancel->load(); };
        bool ok = false, cancelled = false;
        std::string err;
        try {
            B::flash_firmware(fw, [port_name](unsigned baud) { return B::open_serial(port_name, baud); }, opt, cb);
            ok = true;
        } catch (const B::FlashCancelled &) {
            cancelled = true;
        } catch (const std::exception &e) {
            err = e.what();
        }
        bridge->post([ok, cancelled, err](BmcuFlasherDialog &d) { d.on_flash_done(ok, cancelled, err); });
    });
}

void BmcuFlasherDialog::on_flash_log(B::LogLevel lvl, std::string msg)
{
    const char *tag = lvl == B::LogLevel::Warn ? "WARN " : lvl == B::LogLevel::Action ? "ACTION " : lvl == B::LogLevel::Error ? "ERROR " : "";
    log_line(wxString(tag) + from_u8(msg));
    if (lvl != B::LogLevel::Action)
        return;
    // the steps the user has to do now, in the user's language
    const Palette pal = palette();
    if (msg.rfind("ttl mode: enter bootloader", 0) == 0)
        set_status(_L("Enter the bootloader now: unplug the adapter USB, hold BOOT, plug it back in and release BOOT when the board LED lights up."), pal.warn);
    else if (msg.rfind("ttl: re-enter bootloader", 0) == 0)
        set_status(_L("Enter the bootloader again now: keep USB connected, hold BOOT, tap RESET, release BOOT."), pal.warn);
    else if (msg.rfind("TTL fallback", 0) == 0)
        set_status(_L("The fast transfer failed; Orca restarts at normal speed. Keep or put the BMCU in bootloader mode when asked."), pal.warn);
}

void BmcuFlasherDialog::on_flash_progress(int pct) { m_gauge->SetValue(std::clamp(pct, 0, 100)); }

void BmcuFlasherDialog::on_flash_stage(B::Stage s)
{
    m_stage = s;
    const Palette pal = palette();
    switch (s) {
    case B::Stage::Connect: set_status(_L("Connecting to the BMCU bootloader..."), pal.fg); break;
    case B::Stage::Unlock: set_status(_L("Bootloader found (CH32V203). Preparing..."), pal.fg); break;
    case B::Stage::Erase: set_status(_L("Erasing the old firmware... do not unplug the BMCU."), pal.warn); break;
    case B::Stage::Program: set_status(_L("Writing the new firmware... do not unplug the BMCU."), pal.warn); break;
    case B::Stage::Verify: set_status(_L("Verifying what was written..."), pal.warn); break;
    case B::Stage::Finish: set_status(_L("Restarting the BMCU..."), pal.fg); break;
    case B::Stage::Done: break;
    }
    update_state();
}

void BmcuFlasherDialog::on_flash_done(bool ok, bool cancelled, std::string err)
{
    if (m_worker.joinable())
        m_worker.join();
    m_flashing = false;
    const Palette pal = palette();
    if (ok) {
        m_flash_ok = true;
        m_gauge->SetValue(100);
        set_status(_L("Done: firmware written and verified."), pal.accent);
        log_line("flash: OK");
        update_state();
        m_btn_flash->SetLabel(_L("Flash again"));
        MessageDialog(this,
                      _L("The BMCU firmware was written and verified.\n\nUnplug the USB cable, connect the BMCU to the printer again and turn the "
                         "printer on. The new version shows up on this page once the printer reports it."),
                      _L("BMCU firmware update"), wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }
    if (cancelled) {
        set_status(_L("Stopped before the erase: the old firmware is still on the BMCU."), pal.muted);
        log_line("flash: stopped before erase");
        update_state();
        return;
    }
    const bool after_erase = m_stage >= B::Stage::Erase;
    log_line("flash: FAILED: " + from_u8(err));
    wxString hint;
    if (err.find("autodi failed") != std::string::npos)
        hint = _L("The BMCU did not answer on the USB port. Check that the cable goes to the BMCU board, that the printer is off and the BMCU is "
                  "unplugged from it, close other programs using this port, then try again. If it keeps failing, use TTL mode.");
    else if (err.find("identify failed (ttl)") != std::string::npos)
        hint = _L("The bootloader did not answer in time. Check the wiring (RX/TX crossed, 3V3, GND) and enter the bootloader when asked.");
    else if (err.find("cannot open") != std::string::npos)
        hint = _L("The serial port could not be opened. Close other programs that use it (another slicer, serial monitor, BMCU-Flasher) and try again.");
    else if (err.find("unexpected chip") != std::string::npos)
        hint = _L("The device on this port is not a BMCU (CH32V203). Nothing was written.");
    else
        hint = _L("Check the cable and try again.");
    if (after_erase)
        hint += "\n\n" + _L("The old firmware was already erased: the BMCU needs a successful flash before it works again. This is recoverable: "
                            "flash again (TTL mode with the BOOT button if USB does not answer).");
    else
        hint += "\n\n" + _L("Nothing was erased: the old firmware is still on the BMCU.");
    set_status(_L("Flashing failed."), pal.error);
    update_state();
    MessageDialog(this, wxString::Format(_L("Flashing failed: %s"), from_u8(err)) + "\n\n" + hint, _L("BMCU firmware update"), wxOK | wxICON_ERROR)
        .ShowModal();
}

void BmcuFlasherDialog::on_close(wxCloseEvent &e)
{
    if (m_flashing && e.CanVeto()) {
        e.Veto();
        MessageDialog(this, _L("The BMCU is being flashed. Wait until it finishes; interrupting now can leave it without firmware."),
                      _L("BMCU firmware update"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    if (m_downloading && e.CanVeto()) {
        e.Veto();
        return;
    }
    save_prefs();
    e.Skip();
}

} // namespace

void open_bmcu_flasher_dialog(wxWindow *parent, const std::string &installed_version, std::function<std::string()> busy_reason)
{
    if (busy_reason) {
        const std::string why = busy_reason();
        if (!why.empty()) {
            MessageDialog(parent, from_u8(why) + "\n\n" + _L("Flashing the BMCU is blocked until the printer is idle."), _L("BMCU firmware update"),
                          wxOK | wxICON_WARNING)
                .ShowModal();
            return;
        }
    }
    BmcuFlasherDialog dlg(parent, installed_version, std::move(busy_reason));
    dlg.ShowModal();
}

}} // namespace Slic3r::GUI
