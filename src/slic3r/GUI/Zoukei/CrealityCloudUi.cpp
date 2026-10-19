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

        auto* note = new wxStaticText(this, wxID_ANY,
                                      _L("Sign in on Creality's own page below. OrcaSlicer does not read or store your password: "
                                         "it only keeps the session the page hands back, in the system's secure credential store."));
        note->Wrap(FromDIP(600));
        root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_status = new wxStaticText(this, wxID_ANY, CrCloud::is_mock() ? _L("Test mode: the login page comes from the local mock server.") : wxString());
        root->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_web = WebView::CreateWebView(this, wxEmptyString);
        if (m_web != nullptr) {
            root->Add(m_web, 1, wxEXPAND | wxALL, FromDIP(10));
            m_web->Bind(wxEVT_WEBVIEW_NAVIGATING, &LoginDialog::on_navigating, this);
            m_web->Bind(wxEVT_WEBVIEW_NEWWINDOW, &LoginDialog::on_new_window, this);
        } else {
            root->AddStretchSpacer(1);
            m_status->SetLabel(_L("The web view could not be created, so the Creality login page cannot be shown."));
        }

        auto* bottom = new wxBoxSizer(wxHORIZONTAL);
        bottom->AddStretchSpacer(1);
        auto* cancel = new Button(this, _L("Cancel"));
        cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        bottom->Add(cancel, 0);
        root->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        SetSizer(root);
        SetSize(FromDIP(wxSize(640, 780)));
        SetMinSize(FromDIP(wxSize(520, 600)));
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);

        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        m_region->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            CrCloud::set_region(m_region->GetSelection() == 1 ? CrCloud::Region::China : CrCloud::Region::Global);
            load();
        });
        load();
    }

private:
    void load()
    {
        if (m_web == nullptr)
            return;
        const auto ep = CrealityCloud::endpoints(CrCloud::region());
        WebView::LoadUrl(m_web, wxString::FromUTF8(CrealityCloud::login_page_url(ep, CrCloud::client_info().lang)));
    }

    void on_navigating(wxWebViewEvent& evt)
    {
        const std::string url = std::string(evt.GetURL().ToUTF8().data());
        if (!CrealityCloud::is_callback_url(url))
            return; // the account pages (and third-party sign-in they open) stay in this window
        evt.Veto();  // nothing listens on the callback port: the code is taken from the address
        handle_callback(url);
    }

    void on_new_window(wxWebViewEvent& evt)
    {
        const std::string url = std::string(evt.GetURL().ToUTF8().data());
        if (CrealityCloud::is_callback_url(url)) {
            handle_callback(url);
            return;
        }
        // Keep pop-ups (third-party sign-in) inside the login window: a system browser could not hand the code back.
        if (m_web != nullptr && !url.empty())
            m_web->LoadURL(evt.GetURL());
    }

    void handle_callback(const std::string& url)
    {
        if (m_exchanging)
            return;
        const std::string code = CrealityCloud::callback_code(url);
        if (code.empty()) {
            m_status->SetLabel(_L("The Creality page did not complete the login. Try again."));
            load();
            return;
        }
        m_exchanging = true;
        m_status->SetLabel(_L("Signing in..."));
        m_region->Enable(false);
        const CrCloud::Region            region = CrCloud::region();
        const CrealityCloud::ClientInfo  info   = CrCloud::client_info();
        wxWeakRef<LoginDialog>           weak(this);
        std::thread([weak, region, info, code]() {
            CrealityCloud::Session    s;
            const CrealityCloud::CallResult r = CrealityCloud::exchange_login_code(region, info, code, s);
            wxGetApp().CallAfter([weak, r, s]() {
                if (!weak)
                    return;
                weak->on_exchanged(r, s);
            });
        }).detach();
    }

    void on_exchanged(const CrealityCloud::CallResult& r, const CrealityCloud::Session& s)
    {
        m_exchanging = false;
        m_region->Enable(true);
        if (!r.ok) {
            m_status->SetLabel(wxString::Format(_L("Login failed: %s"), wxString::FromUTF8(r.message)));
            load();
            return;
        }
        if (!CrCloud::store_session(s))
            MessageDialog(this,
                          _L("The system's secure credential store is not available, so this login is only kept until OrcaSlicer is closed."),
                          _L("Creality Cloud"), wxOK | wxICON_WARNING)
                .ShowModal();
        EndModal(wxID_OK);
    }

    wxChoice*     m_region{nullptr};
    wxStaticText* m_status{nullptr};
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
        m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(); });
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
            wxString state = !e.uploaded ? _L("not sent yet") : e.changed ? _L("changed since sent") : _L("sent");
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
        m_login->Enable(!m_busy);
        m_close->Enable(!m_busy);
    }

    void send()
    {
        const std::vector<std::string> names = checked_names();
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
        m_busy = true;
        update_buttons();
        m_status->SetLabel(wxString::Format(_L("Sending %d preset(s)..."), int(ups.size())));
        m_report->Clear();
        wxWeakRef<UploadDialog> weak(this);
        CrCloud::upload_async(std::move(ups), [weak](const std::vector<CrCloud::UploadReport>& reports) {
            if (weak)
                weak->on_done(reports);
        });
    }

    void on_done(const std::vector<CrCloud::UploadReport>& reports)
    {
        m_busy   = false;
        int  ok  = 0;
        bool exp = false;
        for (const CrCloud::UploadReport& r : reports) {
            if (r.ok) {
                ++ok;
                m_report->AppendText(wxString::Format("%s: %s\n", wxString::FromUTF8(r.name), r.created ? _L("created") : _L("updated")));
            } else {
                m_report->AppendText(wxString::Format("%s: %s %s\n", wxString::FromUTF8(r.name), _L("error:"), wxString::FromUTF8(r.message)));
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
    Button*                             m_close{nullptr};
    std::vector<CrCloud::FilamentEntry> m_entries;
    std::set<std::string>               m_checked;
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
