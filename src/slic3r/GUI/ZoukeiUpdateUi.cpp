#include "ZoukeiUpdateUi.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/ZoukeiUpdate.hpp"

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/scrolwin.h>
#include <wx/utils.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

namespace Slic3r { namespace GUI { namespace ZoukeiUpdateUi {

namespace ZU = Slic3r::ZoukeiUpdate;

namespace {

std::atomic<bool> s_check_running{false};
bool              s_dialog_open = false; // UI thread only

wxString format_bytes(uint64_t b)
{
    if (b >= 1024ull * 1024 * 1024)
        return wxString::Format("%.2f GB", double(b) / (1024.0 * 1024.0 * 1024.0));
    if (b >= 1024 * 1024)
        return wxString::Format("%.1f MB", double(b) / (1024.0 * 1024.0));
    return wxString::Format("%.0f KB", double(b) / 1024.0);
}

// Automated tests only: ORCA_ZOUKEI_UPDATE_AUTO=<mode> together with the loopback ORCA_ZOUKEI_UPDATE_URL.
bool test_mode_is(const char* mode)
{
    const char* a = std::getenv("ORCA_ZOUKEI_UPDATE_AUTO");
    return a && std::string(a) == mode && ZU::test_mode();
}

ZU::UpdaterTexts updater_texts()
{
    ZU::UpdaterTexts t;
    t.title  = into_u8(_L("Orca Zoukei update"));
    t.failed = into_u8(_L("The update could not be applied. The previous version was restored. Log:"));
    return t;
}

class ZoukeiUpdateDialog : public DPIDialog
{
public:
    ZoukeiUpdateDialog(wxWindow* parent, const ZU::ReleaseInfo& rel)
        : DPIDialog(parent, wxID_ANY, _L("Orca Zoukei update"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
        , m_rel(rel)
    {
        SetBackgroundColour(*wxWHITE); // like the other update dialogs: white page, dark text, in both themes
        auto* main = new wxBoxSizer(wxVERTICAL);

        // Orca's own Label/Button widgets (custom drawn, readable in both themes) like the other update dialogs.
        m_title = new Label(this, Label::Head_14,
                            wxString::Format(_L("Version %s is available (you have %s)."), from_u8(rel.version), from_u8(ZU::current_version())), LB_AUTO_WRAP);
        m_title->SetMinSize(FromDIP(wxSize(540, -1)));
        m_title->SetMaxSize(FromDIP(wxSize(540, -1)));
        main->Add(m_title, 0, wxEXPAND | wxALL, FromDIP(12));

        m_notes_label = new Label(this, Label::Body_13, _L("Release notes:"));
        main->Add(m_notes_label, 0, wxLEFT | wxRIGHT, FromDIP(12));
        wxString notes = from_u8(rel.notes);
        notes.Replace("\r\n", "\n");
        m_notes_win = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(560, 260)), wxVSCROLL | wxBORDER_SIMPLE);
        m_notes_win->SetScrollRate(5, FromDIP(20));
        m_notes_win->SetBackgroundColour(wxColour(0xF8, 0xF8, 0xF8));
        auto* notes_sizer = new wxBoxSizer(wxVERTICAL);
        m_notes = new Label(m_notes_win, Label::Body_13, notes, LB_AUTO_WRAP);
        m_notes->SetMinSize(FromDIP(wxSize(520, -1)));
        m_notes->SetMaxSize(FromDIP(wxSize(520, -1)));
        notes_sizer->Add(m_notes, 0, wxALL, FromDIP(8));
        m_notes_win->SetSizer(notes_sizer);
        main->Add(m_notes_win, 1, wxEXPAND | wxALL, FromDIP(12));

        m_status = new Label(this, Label::Body_13, wxEmptyString, LB_AUTO_WRAP);
        m_status->SetMinSize(FromDIP(wxSize(540, -1)));
        m_status->SetMaxSize(FromDIP(wxSize(540, -1)));
        m_status->Hide();
        main->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        m_gauge = new wxGauge(this, wxID_ANY, 1000, wxDefaultPosition, FromDIP(wxSize(-1, 16)));
        m_gauge->Hide();
        main->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_btn_skip    = new Button(this, _L("Skip this version"));
        m_btn_page    = new Button(this, _L("Open release page"));
        m_btn_page->Hide();
        m_btn_later   = new Button(this, _L("Later"));
        m_btn_update  = new Button(this, _L("Update"));
        m_btn_restart = new Button(this, _L("Restart and update"));
        m_btn_restart->Hide();
        for (Button* b : {m_btn_skip, m_btn_page, m_btn_later})
            b->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        for (Button* b : {m_btn_update, m_btn_restart})
            b->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        buttons->Add(m_btn_skip, 0, wxRIGHT, FromDIP(8));
        buttons->AddStretchSpacer();
        buttons->Add(m_btn_page, 0, wxRIGHT, FromDIP(8));
        buttons->Add(m_btn_later, 0, wxRIGHT, FromDIP(8));
        buttons->Add(m_btn_update, 0);
        buttons->Add(m_btn_restart, 0);
        main->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
        SetSizerAndFit(main);
        SetMinSize(GetSize());
        Centre(wxBOTH);

        m_btn_update->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_update(); });
        m_btn_later->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_and_close(wxID_CANCEL); });
        m_btn_skip->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxGetApp().app_config->set("zoukei_update_skip", m_rel.version);
            cancel_and_close(wxID_NO);
        });
        m_btn_restart->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        m_btn_page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (!m_rel.page_url.empty())
                wxLaunchDefaultBrowser(from_u8(m_rel.page_url));
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { cancel_and_close(wxID_CANCEL); });
        wxGetApp().UpdateDlgDarkUI(this);
        // UpdateDlgDarkUI may have set light text for the dark theme; the page stays white (as in ReleaseNote.cpp), so force dark text.
        for (Label* w : {m_title, m_notes_label, m_status, m_notes})
            w->SetForegroundColour(wxColour(0x26, 0x2E, 0x30));
        m_notes->SetBackgroundColour(wxColour(0xF8, 0xF8, 0xF8));
        if (test_mode_is("dialog"))
            CallAfter([this]() { start_update(); });
    }

    ~ZoukeiUpdateDialog() override
    {
        m_cancel = true;
        if (m_worker.joinable())
            m_worker.join();
    }

    const ZU::Prepared& prepared() const { return m_prepared; }

    void on_dpi_changed(const wxRect&) override
    {
        Layout();
        Fit();
    }

private:
    void cancel_and_close(int code)
    {
        m_cancel = true;
        if (m_worker.joinable())
            m_worker.join();
        EndModal(code);
    }

    void set_progress(const std::string& phase, uint64_t done, uint64_t total)
    {
        double frac = total ? double(done) / double(total) : 0.0;
        if (phase == "verify")
            m_status->SetLabel(_L("Checking the installed files..."));
        else if (phase == "download")
            m_status->SetLabel(wxString::Format(_L("Downloading %s of %s..."), format_bytes(done), format_bytes(total)));
        else
            m_status->SetLabel(_L("Extracting and verifying files..."));
        m_gauge->SetValue(int(frac * 1000));
    }

    void start_update()
    {
        const boost::filesystem::path install = ZU::install_dir();
        if (!ZU::install_dir_writable(install)) {
            show_error(wxString::Format(_L("Orca Zoukei cannot write to its own folder (%s). Download the new version from the release page and replace the files manually."),
                                        from_u8(install.string())));
            return;
        }
        m_notes_win->Hide();
        m_notes_label->Hide();
        m_btn_update->Disable();
        m_btn_skip->Disable();
        m_btn_later->SetLabel(_L("Cancel"));
        m_status->SetLabel(_L("Checking the installed files..."));
        m_status->Show();
        m_gauge->SetValue(0);
        m_gauge->Show();
        Layout();

        const ZU::UpdaterTexts texts = updater_texts();
        m_cancel                     = false;
        m_worker                     = std::thread([this, install, texts]() {
            ZU::Prepared p;
            std::string  err;
            bool         ok       = false;
            int          last_pct = -1;
            std::string  last_phase;
            try {
                ok = ZU::prepare(
                    m_rel, install,
                    [&](const std::string& phase, uint64_t done, uint64_t total) {
                        int pct = total ? int(done * 1000 / total) : 0;
                        if (phase != last_phase || pct != last_pct) {
                            last_phase = phase;
                            last_pct   = pct;
                            CallAfter([this, phase, done, total]() { set_progress(phase, done, total); });
                        }
                        return !m_cancel.load();
                    },
                    texts, p, err);
            } catch (const std::exception& e) {
                err = e.what();
            }
            CallAfter([this, ok, p, err]() { finished(ok, p, err); });
        });
    }

    void finished(bool ok, const ZU::Prepared& p, const std::string& err)
    {
        if (m_worker.joinable())
            m_worker.join();
        if (!ok) {
            if (m_cancel)
                return;
            BOOST_LOG_TRIVIAL(warning) << "Orca Zoukei update failed: " << err;
            show_error(wxString::Format(_L("Update failed: %s"), from_u8(err)));
            if (test_mode_is("dialog"))
                CallAfter([this]() { EndModal(wxID_CANCEL); });
            return;
        }
        m_prepared = p;
        m_gauge->Hide();
        m_status->SetLabel(wxString::Format(_L("The update is ready (%s). Orca Zoukei will close and open again with the new version."), from_u8(p.version)));
        m_btn_update->Hide();
        m_btn_skip->Hide();
        m_btn_later->SetLabel(_L("Later"));
        m_btn_restart->Show();
        Layout();
        Fit();
        if (test_mode_is("dialog"))
            CallAfter([this]() { EndModal(wxID_OK); });
    }

    void show_error(const wxString& msg)
    {
        m_gauge->Hide();
        m_status->SetLabel(msg);
        m_status->Show();
        m_btn_update->Hide();
        m_btn_skip->Hide();
        m_btn_page->Show();
        m_btn_later->Enable();
        m_btn_later->SetLabel(_L("Close"));
        Layout();
        Fit();
    }

    ZU::ReleaseInfo   m_rel;
    ZU::Prepared      m_prepared;
    Label*            m_title{nullptr};
    Label*            m_notes_label{nullptr};
    Label*            m_status{nullptr};
    Label*            m_notes{nullptr};
    wxScrolledWindow* m_notes_win{nullptr};
    wxGauge*          m_gauge{nullptr};
    Button*           m_btn_update{nullptr};
    Button*           m_btn_later{nullptr};
    Button*           m_btn_skip{nullptr};
    Button*           m_btn_page{nullptr};
    Button*           m_btn_restart{nullptr};
    std::thread       m_worker;
    std::atomic<bool> m_cancel{false};
};

// Hands over to the updater and closes the app. The updater waits for this process to end, so it is only
// started once closing was not vetoed (unsaved project: the user may say Cancel).
void install_now(const ZU::Prepared& prepared, bool force_close)
{
    MainFrame* frame = wxGetApp().mainframe;
    if (!frame)
        return;
    if (!force_close && !frame->Close(false)) {
        BOOST_LOG_TRIVIAL(info) << "Orca Zoukei update: closing was cancelled, the update stays staged";
        MessageDialog dlg(frame, _L("The update was not installed because closing the program was cancelled. Use Help > Check for Updates to try again."),
                          _L("Orca Zoukei update"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    std::string err;
    if (!ZU::launch_updater(prepared, err)) {
        BOOST_LOG_TRIVIAL(error) << "Orca Zoukei update: " << err;
        // The app is already closing; there is nothing safe left to show besides the log.
        return;
    }
    if (force_close)
        frame->Close(true);
}

void show_offer(const ZU::ReleaseInfo& rel)
{
    if (s_dialog_open)
        return;
    s_dialog_open = true;
    ZU::Prepared prepared;
    int          rc = wxID_CANCEL;
    {
        ZoukeiUpdateDialog dlg(wxGetApp().mainframe, rel);
        rc = dlg.ShowModal();
        prepared = dlg.prepared();
    }
    s_dialog_open = false;
    if (rc == wxID_OK)
        install_now(prepared, false);
}

void info_box(const wxString& msg)
{
    InfoDialog dlg(wxGetApp().mainframe, _L("Orca Zoukei update"), msg);
    dlg.ShowModal();
}

void on_check_result(bool by_user, const ZU::CheckResult& r)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (r.status != ZU::CheckResult::Failed)
        cfg->set("zoukei_update_last_check", std::to_string(std::time(nullptr)));
    switch (r.status) {
    case ZU::CheckResult::Failed:
        if (by_user)
            info_box(wxString::Format(_L("Could not check for updates: %s"), from_u8(r.error)));
        return;
    case ZU::CheckResult::UpToDate:
        if (by_user)
            info_box(wxString::Format(_L("You already have the latest version (%s)."), from_u8(ZU::current_version())));
        return;
    case ZU::CheckResult::Available:
        if (!by_user && cfg->get("zoukei_update_skip") == r.release.version)
            return;
        show_offer(r.release);
        return;
    }
}

void run_check(bool by_user)
{
    if (s_check_running.exchange(true)) {
        if (by_user)
            info_box(_L("A check for updates is already running."));
        return;
    }
    const bool beta = wxGetApp().app_config->get_bool("zoukei_update_beta");
    std::thread([by_user, beta]() {
        ZU::CheckResult r;
        try {
            r = ZU::check(beta);
        } catch (const std::exception& e) {
            r.status = ZU::CheckResult::Failed;
            r.error  = e.what();
        }
        if (wxTheApp)
            wxTheApp->CallAfter([by_user, r]() {
                s_check_running = false;
                on_check_result(by_user, r);
            });
    }).detach();
}

} // namespace

void check_from_menu() { run_check(true); }

void check_on_startup()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg->get_bool("zoukei_update_check") || cfg->get_stealth_mode())
        return;
    const long long last = std::atoll(cfg->get("zoukei_update_last_check").c_str());
    const long long now  = (long long) std::time(nullptr);
    if (last > 0 && now >= last && now - last < 24 * 3600)
        return;
    run_check(false);
}

// Automated tests only (ORCA_ZOUKEI_UPDATE_AUTO=1 together with the loopback ORCA_ZOUKEI_UPDATE_URL): check, stage, hand over to the
// updater and quit without any dialog. Never active against the real GitHub repository.
void run_test_hook()
{
    if (test_mode_is("dialog")) { // exercises the real dialog (and takes pictures of it), then ends the process
        BOOST_LOG_TRIVIAL(info) << "Orca Zoukei update test hook: dialog mode";
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(90));
#ifdef _WIN32
            ::TerminateProcess(::GetCurrentProcess(), 4);
#else
            std::_Exit(4);
#endif
        }).detach();
        run_check(true);
        return;
    }
    if (!test_mode_is("1"))
        return;
    BOOST_LOG_TRIVIAL(info) << "Orca Zoukei update test hook: running";
    std::thread([]() {
        ZU::CheckResult r = ZU::check(true);
        ZU::Prepared    prepared;
        std::string     err;
        bool            ok = false;
        if (r.status == ZU::CheckResult::Available) {
            try {
                ok = ZU::prepare(r.release, ZU::install_dir(), nullptr, updater_texts(), prepared, err);
            } catch (const std::exception& e) {
                err = e.what();
            }
        } else {
            err = r.status == ZU::CheckResult::Failed ? r.error : "up to date";
        }
        BOOST_LOG_TRIVIAL(info) << "Orca Zoukei update test hook: prepare " << (ok ? "ok" : "failed: " + err);
        if (wxTheApp)
            wxTheApp->CallAfter([ok, prepared]() {
                // Whatever happens, do not stay open (a modal wizard on a fresh --datadir can keep Close() from finishing):
                // a watchdog ends the process, which is what the updater waits for.
                std::thread([ok]() {
                    std::this_thread::sleep_for(std::chrono::seconds(8));
#ifdef _WIN32
                    ::TerminateProcess(::GetCurrentProcess(), ok ? 0 : 3);
#else
                    std::_Exit(ok ? 0 : 3);
#endif
                }).detach();
                if (ok)
                    install_now(prepared, true);
            });
    }).detach();
}

}}} // namespace Slic3r::GUI::ZoukeiUpdateUi
