// src/slic3r/GUI/Zoukei/CrealityCloudUi.cpp
//
// Orca Zoukei: windows of the Creality Cloud integration. The login window follows Creality Print's LoginDialog /
// HttpServer "/login" flow (AGPL-3.0, github.com/CrealityOfficial/CrealityPrint): the official account page with
// redirect_uri = <website>/oauth?back_url=http://localhost:13666/login; the website sends the browser to back_url
// with ?code=..., which this window catches (vetoes) and exchanges for the session. See CrealityCloudUi.hpp.
#include "CrealityCloudUi.hpp"
#include "CrealityCloud.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/CrealityCloudClient.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <wx/webview.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>

namespace Slic3r { namespace GUI { namespace CrCloudUi {

namespace {

wxString region_label(CrCloud::Region r) { return r == CrCloud::Region::China ? _L("China (mainland)") : _L("Global"); }

wxString account_summary()
{
    if (!CrCloud::logged_in())
        return _L("Creality Cloud: not logged in");
    wxString s = wxString::Format(_L("Creality Cloud: %s (%s)"), wxString::FromUTF8(CrCloud::nickname()), region_label(CrCloud::region()));
    if (CrCloud::is_mock())
        s += " [" + _L("test server") + "]";
    return s;
}

wxWindow* top_parent(wxWindow* w)
{
    wxWindow* p = w != nullptr ? wxGetTopLevelParent(w) : nullptr;
    return p != nullptr ? p : static_cast<wxWindow*>(wxGetApp().mainframe);
}

// ---- login window -------------------------------------------------------------------------------

// Third-party sign-in pages: Google, Apple and co refuse embedded web views (Google answers "disallowed_useragent",
// pop-ups end up blank), so they always go through the system browser.
bool is_social_login_host(const std::string& url)
{
    std::string host = url;
    const size_t scheme = host.find("://");
    if (scheme != std::string::npos)
        host = host.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    host = boost::algorithm::to_lower_copy(host.substr(host.find('@') == std::string::npos ? 0 : host.find('@') + 1));
    for (const char* h : {"accounts.google.com", "google.com", "appleid.apple.com", "apple.com", "facebook.com", "github.com", "weixin.qq.com",
                          "open.weixin.qq.com", "connect.qq.com", "graph.qq.com", "discord.com", "x.com", "twitter.com", "line.me", "kakao.com"})
        if (host == h || boost::ends_with(host, std::string(".") + h))
            return true;
    return false;
}

class LoginDialog : public wxDialog
{
public:
    explicit LoginDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("Log in to the Creality Cloud"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _L("Region")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        m_region = new wxChoice(this, wxID_ANY);
        m_region->Append(region_label(CrCloud::Region::Global));
        m_region->Append(region_label(CrCloud::Region::China));
        m_region->SetSelection(CrCloud::region() == CrCloud::Region::China ? 1 : 0);
        m_region->SetToolTip(_L("Creality Cloud accounts of mainland China live on a separate server (crealitycloud.cn)."));
        row->Add(m_region, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_note = new wxStaticText(this, wxID_ANY,
                                  _L("The Creality login page opens in your web browser, where Google, Apple and e-mail sign-in all work. "
                                     "When it is done, the browser hands the session back to OrcaSlicer. OrcaSlicer never sees your password; "
                                     "it keeps the session in the system's secure credential store."));
        m_note->Wrap(FromDIP(600));
        root->Add(m_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_browser_btn = new Button(this, _L("Use my browser (Google/Apple)"));
        m_browser_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        m_browser_btn->SetToolTip(_L("Open the Creality login page in the default browser."));
        buttons->Add(m_browser_btn, 0, wxRIGHT, FromDIP(8));
        m_reopen_btn = new Button(this, _L("Open the page again"));
        m_reopen_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        buttons->Add(m_reopen_btn, 0, wxRIGHT, FromDIP(8));
        m_embedded_btn = new Button(this, _L("Log in in this window"));
        m_embedded_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        m_embedded_btn->SetToolTip(_L("Show the Creality page inside OrcaSlicer (e-mail and password; Google and Apple still open the browser)."));
        buttons->Add(m_embedded_btn, 0);
        root->Add(buttons, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_status = new wxStaticText(this, wxID_ANY, wxString());
        root->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_web_sizer = new wxBoxSizer(wxVERTICAL);
        root->Add(m_web_sizer, 1, wxEXPAND | wxALL, FromDIP(10));

        auto* bottom = new wxBoxSizer(wxHORIZONTAL);
        if (CrCloud::is_mock())
            bottom->Add(new wxStaticText(this, wxID_ANY, _L("Test mode: the login page comes from the local mock server.")), 0, wxALIGN_CENTER_VERTICAL);
        bottom->AddStretchSpacer(1);
        auto* cancel = new Button(this, _L("Cancel"));
        cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        bottom->Add(cancel, 0);
        root->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        SetSizer(root);
        SetSize(FromDIP(wxSize(660, 300)));
        SetMinSize(FromDIP(wxSize(560, 280)));
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);

        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& e) {
            CrCloud::cancel_browser_login();
            e.Skip();
        });
        m_region->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            CrCloud::set_region(m_region->GetSelection() == 1 ? CrCloud::Region::China : CrCloud::Region::Global);
            if (m_web != nullptr && m_web->IsShown())
                load_embedded();
            else
                start_browser(true);
        });
        m_browser_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_browser(true); });
        m_reopen_btn->Bind(wxEVT_BUTTON, [](wxCommandEvent&) {
            if (!CrCloud::browser_login_url().empty())
                wxLaunchDefaultBrowser(wxString::FromUTF8(CrCloud::browser_login_url()));
        });
        m_embedded_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_embedded(); });

        // The browser is the default: it is the mode every sign-in method works in.
        CallAfter([this]() { start_browser(true); });
    }

    ~LoginDialog() override { CrCloud::cancel_browser_login(); }

    int ShowModal() override
    {
        const int r = wxDialog::ShowModal();
        CrCloud::cancel_browser_login();
        return r;
    }

private:
    // ---- browser mode ----
    void start_browser(bool open)
    {
        if (m_web != nullptr && m_web->IsShown()) {
            m_web->Hide();
            m_web->LoadURL("about:blank");
            SetSize(FromDIP(wxSize(660, 300)));
        }
        std::string            message;
        wxWeakRef<LoginDialog> weak(this);
        const auto st = CrCloud::start_browser_login(open, CrCloud::BROWSER_LOGIN_TIMEOUT_S, [weak](CrCloud::BrowserLoginState s, const std::string& msg) {
            if (weak)
                weak->on_browser_change(s, msg);
        }, message);
        if (st == CrCloud::BrowserLoginState::Waiting) {
            set_status(_L("Waiting for the login in your browser... Finish it there; this window closes by itself."));
            m_reopen_btn->Enable(true);
        } else {
            set_status(wxString::FromUTF8(message), true);
            m_reopen_btn->Enable(false);
        }
        Layout();
    }

    void on_browser_change(CrCloud::BrowserLoginState s, const std::string& msg)
    {
        switch (s) {
        case CrCloud::BrowserLoginState::Waiting:
            if (!msg.empty())
                set_status(wxString::Format(_L("Login failed: %s. Try again in the browser."), wxString::FromUTF8(msg)), true);
            break;
        case CrCloud::BrowserLoginState::Done: finish(); break;
        case CrCloud::BrowserLoginState::TimedOut:
            m_reopen_btn->Enable(false);
            set_status(_L("No login arrived from the browser in 5 minutes. Click \"Use my browser\" to try again."), true);
            break;
        default:
            m_reopen_btn->Enable(false);
            if (!msg.empty())
                set_status(wxString::FromUTF8(msg), true);
            break;
        }
    }

    // ---- embedded mode ----
    void show_embedded()
    {
        CrCloud::cancel_browser_login();
        m_reopen_btn->Enable(false);
        if (m_web == nullptr) {
            m_web = WebView::CreateWebView(this, wxEmptyString);
            if (m_web == nullptr) {
                set_status(_L("The web view could not be created, so the Creality login page cannot be shown."), true);
                return;
            }
            m_web_sizer->Add(m_web, 1, wxEXPAND);
            m_web->Bind(wxEVT_WEBVIEW_NAVIGATING, &LoginDialog::on_navigating, this);
            m_web->Bind(wxEVT_WEBVIEW_NEWWINDOW, &LoginDialog::on_new_window, this);
        }
        m_web->Show();
        set_status(_L("Log in with your Creality e-mail below. For Google or Apple, use your browser."));
        SetSize(FromDIP(wxSize(660, 820)));
        Layout();
        load_embedded();
    }

    void load_embedded()
    {
        if (m_web == nullptr)
            return;
        const auto ep = CrealityCloud::endpoints(CrCloud::region());
        WebView::LoadUrl(m_web, wxString::FromUTF8(CrealityCloud::login_page_url(ep, CrCloud::client_info().lang)));
    }

    void switch_to_browser_for_social()
    {
        // Same Creality page, in the system browser: the social sign-in finishes there and comes back over the listener.
        CallAfter([this]() {
            start_browser(true);
            if (CrCloud::browser_login_state() == CrCloud::BrowserLoginState::Waiting)
                set_status(_L("Google, Apple and other sign-in services do not work inside OrcaSlicer, so the login continues in your browser."));
        });
    }

    void on_navigating(wxWebViewEvent& evt)
    {
        const std::string url = std::string(evt.GetURL().ToUTF8().data());
        if (CrealityCloud::is_callback_url(url)) {
            evt.Veto(); // the code is taken from the address; nothing has to listen for the embedded mode
            handle_callback(url);
            return;
        }
        if (is_social_login_host(url)) {
            evt.Veto();
            switch_to_browser_for_social();
        }
    }

    void on_new_window(wxWebViewEvent& evt)
    {
        const std::string url = std::string(evt.GetURL().ToUTF8().data());
        if (CrealityCloud::is_callback_url(url)) {
            handle_callback(url);
            return;
        }
        // Pop-ups are the social sign-in windows: the browser handles them.
        switch_to_browser_for_social();
    }

    void handle_callback(const std::string& url)
    {
        if (m_exchanging)
            return;
        const std::string code = CrealityCloud::callback_code(url);
        if (code.empty()) {
            set_status(_L("The Creality page did not complete the login. Try again."), true);
            load_embedded();
            return;
        }
        m_exchanging = true;
        set_status(_L("Signing in..."));
        m_region->Enable(false);
        const CrCloud::Region           region = CrCloud::region();
        const CrealityCloud::ClientInfo info   = CrCloud::client_info();
        wxWeakRef<LoginDialog>          weak(this);
        std::thread([weak, region, info, code]() {
            CrealityCloud::Session          s;
            const CrealityCloud::CallResult r = CrealityCloud::exchange_login_code(region, info, code, s);
            wxGetApp().CallAfter([weak, r, s]() {
                if (weak)
                    weak->on_exchanged(r, s);
            });
        }).detach();
    }

    void on_exchanged(const CrealityCloud::CallResult& r, const CrealityCloud::Session& s)
    {
        m_exchanging = false;
        m_region->Enable(true);
        if (!r.ok) {
            set_status(wxString::Format(_L("Login failed: %s"), wxString::FromUTF8(r.message)), true);
            load_embedded();
            return;
        }
        CrCloud::store_session(s);
        finish();
    }

    void finish()
    {
        if (!CrCloud::secure_storage_available())
            MessageDialog(this,
                          _L("The system's secure credential store is not available, so this login is only kept until OrcaSlicer is closed."),
                          _L("Creality Cloud"), wxOK | wxICON_WARNING)
                .ShowModal();
        EndModal(wxID_OK);
    }

    void set_status(const wxString& text, bool warning = false)
    {
        m_status->SetLabel(text);
        m_status->SetForegroundColour(warning ? wxColour("#E14747") : wxGetApp().get_label_clr_default());
        m_status->Wrap(FromDIP(620));
        Layout();
    }

    wxChoice*     m_region{nullptr};
    wxStaticText* m_note{nullptr};
    wxStaticText* m_status{nullptr};
    Button*       m_browser_btn{nullptr};
    Button*       m_reopen_btn{nullptr};
    Button*       m_embedded_btn{nullptr};
    wxBoxSizer*   m_web_sizer{nullptr};
    wxWebView*    m_web{nullptr};
    bool          m_exchanging{false};
};

// ---- upload window ------------------------------------------------------------------------------

class UploadDialog : public wxDialog
{
public:
    UploadDialog(wxWindow* parent, const std::string& preselect)
        : wxDialog(parent, wxID_ANY, _L("Send filaments to the Creality Cloud"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        if (!preselect.empty())
            m_checked.insert(preselect);
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* acc = new wxBoxSizer(wxHORIZONTAL);
        m_account = new wxStaticText(this, wxID_ANY, wxString());
        acc->Add(m_account, 1, wxALIGN_CENTER_VERTICAL);
        m_login = new Button(this, _L("Log in"));
        m_login->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
        acc->Add(m_login, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        root->Add(acc, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        auto* intro = new wxStaticText(this, wxID_ANY,
                                       _L("The ticked presets go to your Creality Cloud account like in Creality Print. A printer bound to the "
                                          "same account (K2 / K2 Plus with CFS) then lists them among its materials. Each preset is sent "
                                          "complete (inherited values included) and linked to the printer below."));
        intro->Wrap(FromDIP(620));
        root->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        auto* prow = new wxBoxSizer(wxHORIZONTAL);
        prow->Add(new wxStaticText(this, wxID_ANY, _L("Link to printer")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        m_printer = new wxChoice(this, wxID_ANY);
        m_printer->SetToolTip(_L("Creality printer preset the filaments are made for (compatible_printers of the uploaded preset)."));
        const std::vector<std::string> targets = CrCloud::printer_targets();
        const std::string              def     = CrCloud::default_printer_target();
        int                            sel     = -1;
        for (size_t i = 0; i < targets.size(); ++i) {
            m_printer->Append(wxString::FromUTF8(targets[i]));
            if (targets[i] == def)
                sel = int(i);
        }
        if (sel < 0 && !def.empty()) {
            m_printer->Append(wxString::FromUTF8(def));
            sel = int(m_printer->GetCount()) - 1;
        }
        m_printer->SetSelection(std::max(sel, 0));
        prow->Add(m_printer, 1, wxALIGN_CENTER_VERTICAL);
        root->Add(prow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_list = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(620), FromDIP(260)));
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        auto* sel_row = new wxBoxSizer(wxHORIZONTAL);
        auto* all  = new Button(this, _L("Select all"));
        auto* none = new Button(this, _L("Select none"));
        auto* chg  = new Button(this, _L("Select new and changed"));
        for (Button* b : {all, none, chg}) {
            b->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
            sel_row->Add(b, 0, wxRIGHT, FromDIP(6));
        }
        root->Add(sel_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_report = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition, wxSize(-1, FromDIP(110)), wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        root->Add(m_report, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        auto* bottom = new wxBoxSizer(wxHORIZONTAL);
        m_status = new wxStaticText(this, wxID_ANY, wxString());
        bottom->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
        m_resend = new Button(this, _L("Resend failed"));
        m_resend->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        m_resend->SetToolTip(_L("Send again the presets that failed in this window (for example after the Creality rate limit)."));
        bottom->Add(m_resend, 0, wxLEFT, FromDIP(8));
        m_stop = new Button(this, _L("Stop sending"));
        m_stop->SetStyle(ButtonStyle::Alert, ButtonType::Choice);
        bottom->Add(m_stop, 0, wxLEFT, FromDIP(8));
        m_send = new Button(this, _L("Send"));
        m_send->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        bottom->Add(m_send, 0, wxLEFT, FromDIP(8));
        m_close = new Button(this, _L("Close"));
        m_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        bottom->Add(m_close, 0, wxLEFT, FromDIP(8));
        root->Add(bottom, 0, wxEXPAND | wxALL, FromDIP(10));

        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(560), FromDIP(480)));
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);

        m_login->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (CrCloud::logged_in()) {
                CrCloud::logout();
            } else {
                show_login_dialog(this);
            }
            refresh();
        });
        all->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { check_all([](const CrCloud::FilamentEntry&) { return true; }); });
        none->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { check_all([](const CrCloud::FilamentEntry&) { return false; }); });
        chg->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { check_all([](const CrCloud::FilamentEntry& e) { return !e.uploaded || e.changed; }); });
        m_list->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) { update_buttons(); });
        m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(checked_names()); });
        m_resend->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::vector<std::string> names;
            for (const CrCloud::FilamentEntry& e : m_entries)
                if (m_failed.count(e.name))
                    names.push_back(e.name);
            send(names);
        });
        m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_control)
                m_control->cancel = true;
            m_status->SetLabel(_L("Stopping after the current preset..."));
        });
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (!m_busy)
                EndModal(wxID_CLOSE);
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
            if (m_busy && e.CanVeto()) {
                e.Veto();
                return;
            }
            e.Skip();
        });
        refresh();
    }

private:
    template<class F> void check_all(F pred)
    {
        for (unsigned i = 0; i < m_list->GetCount() && i < m_entries.size(); ++i)
            m_list->Check(i, pred(m_entries[i]));
        update_buttons();
    }

    void remember_checks()
    {
        for (unsigned i = 0; i < m_list->GetCount() && i < m_entries.size(); ++i) {
            if (m_list->IsChecked(i))
                m_checked.insert(m_entries[i].name);
            else
                m_checked.erase(m_entries[i].name);
        }
    }

    void refresh()
    {
        if (m_list->GetCount() > 0)
            remember_checks();
        m_account->SetLabel(account_summary());
        m_login->SetLabel(CrCloud::logged_in() ? _L("Log out") : _L("Log in"));
        m_entries = CrCloud::user_filaments();
        m_list->Clear();
        for (const CrCloud::FilamentEntry& e : m_entries) {
            wxString state = m_failed.count(e.name) ? _L("error - try again") :
                             !e.uploaded             ? _L("not sent yet") :
                             e.changed               ? _L("changed since sent") :
                                                       _L("sent");
            wxString kind  = wxString::FromUTF8(e.type);
            if (!e.vendor.empty())
                kind += ", " + wxString::FromUTF8(e.vendor);
            const int idx = m_list->Append(wxString::Format("%s   [%s]   - %s", wxString::FromUTF8(e.name), kind, state));
            if (m_checked.count(e.name))
                m_list->Check(idx, true);
        }
        if (m_entries.empty())
            m_status->SetLabel(_L("You have no filament presets of your own. Save a filament preset first."));
        else
            m_status->SetLabel(wxString());
        Layout();
        update_buttons();
    }

    std::vector<std::string> checked_names() const
    {
        std::vector<std::string> out;
        for (unsigned i = 0; i < m_list->GetCount() && i < m_entries.size(); ++i)
            if (m_list->IsChecked(i))
                out.push_back(m_entries[i].name);
        return out;
    }

    void update_buttons()
    {
        m_send->Enable(!m_busy && CrCloud::logged_in() && !checked_names().empty());
        m_resend->Enable(!m_busy && CrCloud::logged_in() && !m_failed.empty());
        m_resend->Show(!m_failed.empty());
        m_stop->Show(m_busy);
        m_login->Enable(!m_busy);
        m_close->Enable(!m_busy);
        Layout();
    }

    void send(const std::vector<std::string>& names)
    {
        if (names.empty() || !CrCloud::logged_in())
            return;
        const std::string target = std::string(m_printer->GetStringSelection().ToUTF8().data());
        wxString          list;
        for (size_t i = 0; i < names.size() && i < 12; ++i)
            list += "\n  - " + wxString::FromUTF8(names[i]);
        if (names.size() > 12)
            list += "\n  ...";
        const wxString msg = wxString::Format(_L("Send %d filament preset(s) to the Creality Cloud account %s, linked to %s?"), int(names.size()),
                                              wxString::FromUTF8(CrCloud::nickname()), wxString::FromUTF8(target)) +
                             list + "\n\n" + _L("Presets sent before are replaced by the current version.");
        if (MessageDialog(this, msg, _L("Creality Cloud"), wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
            return;

        remember_checks();
        std::vector<CrCloud::PreparedUpload> ups = CrCloud::prepare_uploads(names, target);
        m_busy    = true;
        m_control = std::make_shared<CrCloud::UploadControl>();
        update_buttons();
        m_status->SetLabel(wxString::Format(_L("Sending %d preset(s)..."), int(ups.size())));
        m_report->Clear();
        wxWeakRef<UploadDialog> weak(this);
        CrCloud::upload_async(
            std::move(ups),
            [weak](const std::vector<CrCloud::UploadReport>& reports) {
                if (weak)
                    weak->on_done(reports);
            },
            [weak](const CrCloud::UploadProgress& p) {
                if (weak)
                    weak->on_progress(p);
            },
            m_control);
    }

    void on_progress(const CrCloud::UploadProgress& p)
    {
        if (!m_busy || (m_control && m_control->cancel))
            return;
        wxString text = wxString::Format(_L("Sending %d of %d: %s"), p.index, p.total, wxString::FromUTF8(p.name));
        if (p.wait_s > 0 && p.rate_limit)
            text += " - " + wxString::Format(_L("waiting for the Creality Cloud limit (%d s)"), p.wait_s);
        else if (p.wait_s > 0)
            text += " - " + wxString::Format(_L("pause between presets (%d s)"), p.wait_s);
        else if (p.attempt > 0)
            text += " - " + wxString::Format(_L("attempt %d"), p.attempt + 1);
        m_status->SetLabel(text);
    }

    void on_done(const std::vector<CrCloud::UploadReport>& reports)
    {
        m_busy   = false;
        m_control.reset();
        int  ok  = 0;
        bool exp = false;
        for (const CrCloud::UploadReport& r : reports) {
            if (r.ok)
                m_failed.erase(r.name);
            else
                m_failed.insert(r.name);
            if (r.ok) {
                ++ok;
                m_report->AppendText(wxString::Format("%s: %s\n", wxString::FromUTF8(r.name), r.created ? _L("created") : _L("updated")));
            } else {
                const wxString why = r.cancelled    ? _L("not sent (stopped)") :
                                     r.rate_limited ? _L("the Creality Cloud still refuses (rate limit) after several tries; resend it later") :
                                                      wxString::FromUTF8(r.message);
                m_report->AppendText(wxString::Format("%s: %s %s\n", wxString::FromUTF8(r.name), _L("error:"), why));
            }
            exp = exp || r.token_invalid;
        }
        refresh();
        wxString status = wxString::Format(_L("%d of %d sent."), ok, int(reports.size()));
        if (exp)
            status += " " + _L("The login has expired: log in again and send the rest.");
        m_status->SetLabel(status);
    }

    wxStaticText*                       m_account{nullptr};
    Button*                             m_login{nullptr};
    wxChoice*                           m_printer{nullptr};
    wxCheckListBox*                     m_list{nullptr};
    wxTextCtrl*                         m_report{nullptr};
    wxStaticText*                       m_status{nullptr};
    Button*                             m_send{nullptr};
    Button*                             m_resend{nullptr};
    Button*                             m_stop{nullptr};
    Button*                             m_close{nullptr};
    std::vector<CrCloud::FilamentEntry> m_entries;
    std::set<std::string>               m_checked;
    std::set<std::string>               m_failed; // failed in this window: "error - try again" / Resend failed
    std::shared_ptr<CrCloud::UploadControl> m_control;
    bool                                m_busy{false};
};

// ---- device tab bar -------------------------------------------------------------------------------

class DeviceBar : public wxPanel
{
public:
    explicit DeviceBar(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_label = new wxStaticText(this, wxID_ANY, wxString());
        row->Add(m_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        m_login = new Button(this, _L("Log in"));
        m_login->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        row->Add(m_login, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        m_send = new Button(this, _L("Send filaments to the Creality Cloud"));
        m_send->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        m_send->SetToolTip(_L("Upload your filament presets to your Creality Cloud account, so the printer bound to it lists them."));
        row->Add(m_send, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        SetSizer(row);
        wxGetApp().UpdateDarkUI(this);

        m_login->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (CrCloud::logged_in()) {
                if (MessageDialog(this, _L("Log out of the Creality Cloud on this computer?"), _L("Creality Cloud"), wxYES_NO | wxICON_QUESTION).ShowModal() == wxID_YES)
                    CrCloud::logout();
            } else
                show_login_dialog(this);
            refresh();
        });
        m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            show_upload_dialog(this);
            refresh();
        });
        Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
            refresh();
            e.Skip();
        });
        refresh();
    }

    void refresh()
    {
        m_label->SetLabel(account_summary());
        m_login->SetLabel(CrCloud::logged_in() ? _L("Log out") : _L("Log in"));
        Layout();
    }

private:
    wxStaticText* m_label{nullptr};
    Button*       m_login{nullptr};
    Button*       m_send{nullptr};
};

} // namespace

bool show_login_dialog(wxWindow* parent)
{
    LoginDialog dlg(top_parent(parent));
    dlg.ShowModal();
    return CrCloud::logged_in();
}

void show_upload_dialog(wxWindow* parent, const std::string& preselect)
{
    UploadDialog dlg(top_parent(parent), preselect);
    dlg.ShowModal();
}

void build_preferences_row(wxWindow* parent, wxBoxSizer* row)
{
    auto* region = new wxChoice(parent, wxID_ANY);
    region->Append(region_label(CrCloud::Region::Global));
    region->Append(region_label(CrCloud::Region::China));
    region->SetSelection(CrCloud::region() == CrCloud::Region::China ? 1 : 0);
    region->SetToolTip(_L("Creality Cloud accounts of mainland China live on a separate server (crealitycloud.cn). Changing it logs out."));

    auto* btn = new Button(parent, CrCloud::logged_in() ? _L("Log out") : _L("Log in"));
    btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);

    auto* status = new wxStaticText(parent, wxID_ANY, account_summary(), wxDefaultPosition, wxSize(parent->FromDIP(300), -1), wxST_ELLIPSIZE_END);
    status->SetFont(::Label::Body_14);

    row->Add(region, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(8));
    row->Add(status, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(10));

    wxWeakRef<wxStaticText> weak_status(status);
    wxWeakRef<Button>       weak_btn(btn);
    auto refresh = [weak_status, weak_btn]() {
        if (weak_status)
            weak_status->SetLabel(account_summary());
        if (weak_btn) {
            weak_btn->SetLabel(CrCloud::logged_in() ? _L("Log out") : _L("Log in"));
            weak_btn->GetParent()->Layout();
        }
    };
    region->Bind(wxEVT_CHOICE, [region, refresh](wxCommandEvent&) {
        const CrCloud::Region r = region->GetSelection() == 1 ? CrCloud::Region::China : CrCloud::Region::Global;
        if (r == CrCloud::region())
            return;
        CrCloud::set_region(r);
        refresh();
    });
    btn->Bind(wxEVT_BUTTON, [parent, refresh](wxCommandEvent&) {
        if (CrCloud::logged_in())
            CrCloud::logout();
        else
            show_login_dialog(parent);
        refresh();
    });
}

wxWindow* create_device_bar(wxWindow* parent) { return new DeviceBar(parent); }

}}} // namespace Slic3r::GUI::CrCloudUi
