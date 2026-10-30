#include "DeviceConsole.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Zoukei/BackgroundWorkers.hpp"

#include "slic3r/Utils/DeviceActivityLog.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/notifmsg.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/tglbtn.h>

namespace Slic3r { namespace GUI { namespace DeviceConsole {

namespace DA = Slic3r::DeviceActivity;
using json   = nlohmann::json;

namespace {

std::string jstr(const json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}
long long jint(const json& j, const char* key, long long def)
{
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<long long>() : def;
}

std::mutex            s_klipper_mutex;
std::set<std::string> s_klipper_busy;

wxWindow* top_window()
{
    return wxGetApp().GetTopWindow();
}

void copy_text(const std::string& text)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
        wxTheClipboard->Close();
    }
}

// Returns the chosen path, or empty.
std::string export_text(wxWindow* parent, const std::string& host, const std::string& text)
{
    char stamp[32] = "";
    const std::time_t t = std::time(nullptr);
    if (const std::tm* tm = std::localtime(&t))
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
    const wxString name = wxString::FromUTF8("console-" + host + "-" + stamp + ".txt");
    wxFileDialog   dlg(parent, _L("Export the console"), wxEmptyString, name, "Text (*.txt)|*.txt", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return {};
    const std::string path = dlg.GetPath().ToUTF8().data();
    boost::nowide::ofstream f(path, std::ios::binary);
    if (!f)
        return {};
    f << text;
    return f ? path : std::string();
}

// ---- native window ---------------------------------------------------------------------------------------------------

class ConsoleList : public wxListCtrl
{
public:
    using TextFn = std::function<wxString(long item, long column)>;
    using AttrFn = std::function<wxItemAttr*(long item)>;
    ConsoleList(wxWindow* parent, TextFn text, AttrFn attr)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxBORDER_SIMPLE)
        , m_text(std::move(text))
        , m_attr(std::move(attr))
    {}
    wxString     OnGetItemText(long item, long column) const override { return m_text(item, column); }
    wxItemAttr*  OnGetItemAttr(long item) const override { return m_attr(item); }

private:
    TextFn m_text;
    AttrFn m_attr;
};

class ConsoleDialog : public wxDialog
{
public:
    ConsoleDialog(wxWindow* parent, const std::string& host, const wxString& title)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxSize(980, 620), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX)
        , m_host(host)
        , m_timer(this)
    {
        const bool dark = GUI_App::dark_mode();
        m_attr_default.SetTextColour(dark ? wxColour(0xE6, 0xE6, 0xE6) : wxColour(0x26, 0x2E, 0x30));
        m_attr_error.SetTextColour(dark ? wxColour(0xFF, 0x6B, 0x6B) : wxColour(0xC6, 0x28, 0x28));
        m_attr_sync.SetTextColour(dark ? wxColour(0x4D, 0xD6, 0x8A) : wxColour(0x00, 0x8A, 0x3A));
        m_attr_out.SetTextColour(dark ? wxColour(0x6F, 0xB4, 0xFF) : wxColour(0x1F, 0x5F, 0xBF));
        m_attr_agent.SetTextColour(dark ? wxColour(0xA8, 0xB0, 0xB4) : wxColour(0x5B, 0x65, 0x6A));
        m_attr_klipper.SetTextColour(dark ? wxColour(0xFF, 0xB8, 0x4D) : wxColour(0xA8, 0x62, 0x00));
        m_attr_alert.SetTextColour(dark ? wxColour(0xFF, 0x6B, 0x6B) : wxColour(0xC6, 0x1A, 0x1A));
        m_attr_alert.SetFont(wxGetApp().bold_font());

        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* filters = new wxBoxSizer(wxHORIZONTAL);
        for (unsigned i = 0; i < unsigned(DA::Source::Count); ++i) {
            const DA::Source s = DA::Source(i);
            auto* cb           = new wxCheckBox(this, wxID_ANY, wxString::FromUTF8(DA::source_name(s)));
            cb->SetValue(true);
            cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { rebuild_view(); });
            m_source_boxes[i] = cb;
            filters->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        }
        filters->AddStretchSpacer(1);
        m_search = new wxSearchCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(220), -1));
        m_search->ShowCancelButton(true);
        m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { rebuild_view(); });
        filters->Add(m_search, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(filters, 0, wxEXPAND | wxALL, FromDIP(8));

        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        m_pause = new wxToggleButton(this, wxID_ANY, _L("Pause"));
        actions->Add(m_pause, 0, wxRIGHT, FromDIP(6));
        auto* clear = new wxButton(this, wxID_ANY, _L("Clear"));
        clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            DA::clear(m_host);
            m_entries.clear();
            m_since = 0;
            rebuild_view();
        });
        actions->Add(clear, 0, wxRIGHT, FromDIP(6));
        auto* copy = new wxButton(this, wxID_ANY, _L("Copy all"));
        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { copy_text(DA::to_text(m_host, mask(), filter_text())); });
        actions->Add(copy, 0, wxRIGHT, FromDIP(6));
        auto* exp = new wxButton(this, wxID_ANY, _L("Export .txt"));
        exp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_text(this, m_host, DA::to_text(m_host, mask(), filter_text())); });
        actions->Add(exp, 0, wxRIGHT, FromDIP(14));
        m_show_all = new wxCheckBox(this, wxID_ANY, _L("Show all traffic"));
        m_show_all->SetValue(DA::show_all());
        m_show_all->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { DA::set_show_all(m_show_all->GetValue()); });
        actions->Add(m_show_all, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
        m_klipper = new wxCheckBox(this, wxID_ANY, _L("Read klippy.log (Klipper)"));
        actions->Add(m_klipper, 0, wxALIGN_CENTER_VERTICAL);
        actions->AddStretchSpacer(1);
        m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        actions->Add(m_status, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        m_list = new ConsoleList(
            this,
            [this](long item, long column) -> wxString {
                if (item < 0 || item >= long(m_view.size()))
                    return wxEmptyString;
                const DA::Entry& e = m_entries[m_view[size_t(item)]];
                switch (column) {
                case 0: return time_text(e.t_ms);
                case 1: return wxString::FromUTF8(DA::source_name(e.source));
                default: {
                    wxString t = wxString::FromUTF8(e.text);
                    t.Replace("\n", " | ");
                    return t;
                }
                }
            },
            [this](long item) -> wxItemAttr* {
                if (item < 0 || item >= long(m_view.size()))
                    return nullptr;
                if (m_entries[m_view[size_t(item)]].alert)
                    return &m_attr_alert;
                switch (m_entries[m_view[size_t(item)]].source) {
                case DA::Source::Error: return &m_attr_error;
                case DA::Source::Sync: return &m_attr_sync;
                case DA::Source::WsOut: return &m_attr_out;
                case DA::Source::Agent: return &m_attr_agent;
                case DA::Source::Klipper: return &m_attr_klipper;
                default: return &m_attr_default;
                }
            });
        m_list->AppendColumn(_L("Time"), wxLIST_FORMAT_LEFT, FromDIP(100));
        m_list->AppendColumn(_L("Source"), wxLIST_FORMAT_LEFT, FromDIP(70));
        m_list->AppendColumn(_L("Activity"), wxLIST_FORMAT_LEFT, FromDIP(740));
        m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& ev) { show_details(ev.GetIndex()); });
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        SetSizer(root);

        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); }, m_timer.GetId());
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
            m_timer.Stop();
            Hide();
        });
        wxGetApp().UpdateDlgDarkUI(this);
        m_list->SetBackgroundColour(dark ? wxColour(0x1E, 0x23, 0x25) : wxColour(0xFF, 0xFF, 0xFF));
        m_list->SetForegroundColour(dark ? wxColour(0xE6, 0xE6, 0xE6) : wxColour(0x26, 0x2E, 0x30));
        tick();
        m_timer.Start(600);
    }

    void raise()
    {
        if (!IsShown()) {
            tick();
            m_timer.Start(600);
        }
        Show();
        Raise();
    }

private:
    static wxString time_text(long long ms)
    {
        const std::time_t t = std::time_t(ms / 1000);
        char              buf[32];
        if (const std::tm* tm = std::localtime(&t))
            std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm->tm_hour, tm->tm_min, tm->tm_sec, int(ms % 1000));
        else
            buf[0] = 0;
        return wxString(buf);
    }

    unsigned mask() const
    {
        unsigned m = 0;
        for (unsigned i = 0; i < unsigned(DA::Source::Count); ++i)
            if (m_source_boxes[i]->GetValue())
                m |= 1u << i;
        return m;
    }
    std::string filter_text() const { return std::string(m_search->GetValue().ToUTF8().data()); }

    bool matches(const DA::Entry& e, unsigned m, const std::string& needle_lower) const
    {
        if (!(m & (1u << unsigned(e.source))))
            return false;
        if (needle_lower.empty())
            return true;
        std::string t = e.text;
        for (char& c : t)
            c = char(std::tolower((unsigned char) c));
        return t.find(needle_lower) != std::string::npos;
    }
    std::string needle() const
    {
        std::string n = filter_text();
        for (char& c : n)
            c = char(std::tolower((unsigned char) c));
        return n;
    }

    void rebuild_view()
    {
        const unsigned    m = mask();
        const std::string n = needle();
        m_view.clear();
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (matches(m_entries[i], m, n))
                m_view.push_back(i);
        m_list->SetItemCount(long(m_view.size()));
        m_list->Refresh();
        if (!m_view.empty())
            m_list->EnsureVisible(long(m_view.size()) - 1);
        update_status();
    }

    void update_status()
    {
        m_status->SetLabel(wxString::Format(_L("%d of %d entries"), int(m_view.size()), int(m_entries.size())) +
                           (m_pause->GetValue() ? " - " + _L("paused") : wxString()));
    }

    void tick()
    {
        m_show_all->SetValue(DA::show_all());
        if (m_klipper->GetValue() && ++m_klipper_ticks % 5 == 1) {
            const std::string host = m_host;
            std::thread([host] {
                const Zoukei::WorkerToken token;
                if (!token)
                    return;
                {
                    std::lock_guard<std::mutex> lock(s_klipper_mutex);
                    if (!s_klipper_busy.insert(host).second)
                        return;
                }
                DA::klipper_poll(host);
                std::lock_guard<std::mutex> lock(s_klipper_mutex);
                s_klipper_busy.erase(host);
            }).detach();
        }
        if (m_pause->GetValue()) {
            update_status();
            return;
        }
        std::vector<DA::Entry> fresh = DA::entries(m_host, m_since);
        if (fresh.empty())
            return;
        // Auto-scroll only while the user is at the bottom: scrolling up stops it.
        const long count     = m_list->GetItemCount();
        const bool at_bottom = count == 0 || m_list->GetTopItem() + m_list->GetCountPerPage() >= count - 1;
        const unsigned    m = mask();
        const std::string n = needle();
        for (DA::Entry& e : fresh) {
            m_since = std::max(m_since, e.seq);
            m_entries.push_back(std::move(e));
            if (matches(m_entries.back(), m, n))
                m_view.push_back(m_entries.size() - 1);
        }
        if (m_entries.size() > DA::CAPACITY + 200) {
            m_entries.erase(m_entries.begin(), m_entries.begin() + long(m_entries.size() - DA::CAPACITY));
            m_view.clear();
            for (size_t i = 0; i < m_entries.size(); ++i)
                if (matches(m_entries[i], m, n))
                    m_view.push_back(i);
        }
        m_list->SetItemCount(long(m_view.size()));
        m_list->Refresh();
        if (at_bottom && !m_view.empty())
            m_list->EnsureVisible(long(m_view.size()) - 1);
        update_status();
    }

    void show_details(long item)
    {
        if (item < 0 || item >= long(m_view.size()))
            return;
        const DA::Entry& e = m_entries[m_view[size_t(item)]];
        wxDialog         dlg(this, wxID_ANY, _L("Entry"), wxDefaultPosition, wxSize(760, 420), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto*            sizer = new wxBoxSizer(wxVERTICAL);
        std::string      body  = e.text;
        if (!e.raw.empty()) {
            body += "\n\n";
            auto j = nlohmann::json::parse(e.raw, nullptr, false);
            body += j.is_discarded() ? e.raw : j.dump(2);
        }
        auto* text = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(body), wxDefaultPosition, wxDefaultSize,
                                    wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        sizer->Add(text, 1, wxEXPAND | wxALL, FromDIP(8));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* copy    = new wxButton(&dlg, wxID_ANY, _L("Copy"));
        copy->Bind(wxEVT_BUTTON, [body](wxCommandEvent&) { copy_text(body); });
        buttons->Add(copy, 0, wxRIGHT, FromDIP(6));
        buttons->Add(new wxButton(&dlg, wxID_OK, _L("Close")));
        sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        dlg.SetSizer(sizer);
        wxGetApp().UpdateDlgDarkUI(&dlg);
        dlg.ShowModal();
    }

    std::string                 m_host;
    wxTimer                     m_timer;
    std::vector<DA::Entry>      m_entries;
    std::vector<size_t>         m_view;
    std::uint64_t               m_since{0};
    int                         m_klipper_ticks{0};
    wxCheckBox*                 m_source_boxes[unsigned(DA::Source::Count)]{};
    wxSearchCtrl*               m_search{nullptr};
    wxToggleButton*             m_pause{nullptr};
    wxCheckBox*                 m_show_all{nullptr};
    wxCheckBox*                 m_klipper{nullptr};
    wxStaticText*               m_status{nullptr};
    ConsoleList*                m_list{nullptr};
    mutable wxItemAttr          m_attr_default, m_attr_error, m_attr_sync, m_attr_out, m_attr_agent, m_attr_klipper, m_attr_alert;
};

std::map<std::string, ConsoleDialog*> s_windows; // owned by wx (top-level windows); entries removed on destroy

} // namespace

void install_alerts()
{
    DA::set_alert_handler([](const std::string& host, const std::string& text) {
        wxTheApp->CallAfter([host, text] {
            wxNotificationMessage note(_L("Printer needs attention"), wxString::FromUTF8(host) + ": " + wxString::FromUTF8(text.substr(0, 200)),
                                       wxGetApp().mainframe, wxICON_WARNING);
            note.Show(wxNotificationMessage::Timeout_Never);
            if (wxGetApp().mainframe)
                wxGetApp().mainframe->RequestUserAttention(wxUSER_ATTENTION_ERROR);
        });
    });
    DA::start_background_poll();
}

void show_native(wxWindow* parent, const std::string& host_in, const std::string& title)
{
    const std::string host = DA::key_of(host_in);
    if (host.empty())
        return;
    auto it = s_windows.find(host);
    if (it != s_windows.end() && it->second != nullptr) {
        it->second->raise();
        return;
    }
    const wxString caption = _L("Console") + " - " + wxString::FromUTF8(title.empty() ? host : title);
    auto*          dlg     = new ConsoleDialog(parent != nullptr ? parent : top_window(), host, caption);
    dlg->Bind(wxEVT_DESTROY, [host](wxWindowDestroyEvent&) { s_windows.erase(host); });
    s_windows[host] = dlg;
    dlg->raise();
}

void register_routes(std::unordered_map<std::string, Handler>& h, Post post, Post post_async)
{
    h["device_activity"] = [post](const json& j) {
        const std::string host = jstr(j, "host");
        json out = DA::query(host, std::uint64_t(std::max<long long>(0, jint(j, "since", 0))), size_t(std::clamp<long long>(jint(j, "limit", 500), 1, 2000)),
                             DA::source_mask(jstr(j, "sources")), jstr(j, "q"), jint(j, "raw", 0) != 0);
        out["requestId"] = jstr(j, "requestId");
        post("device_activity", std::move(out));
    };
    h["device_activity_clear"]    = [](const json& j) { DA::clear(jstr(j, "host")); };
    h["device_activity_show_all"] = [](const json& j) { DA::set_show_all(j.is_object() && j.value("on", false)); };
    h["device_activity_copy"]     = [](const json& j) {
        copy_text(DA::to_text(jstr(j, "host"), DA::source_mask(jstr(j, "sources")), jstr(j, "q")));
    };
    h["device_activity_export"] = [post](const json& j) {
        const std::string host = jstr(j, "host");
        const std::string path = export_text(top_window(), DA::key_of(host), DA::to_text(host, DA::source_mask(jstr(j, "sources")), jstr(j, "q")));
        post("device_activity_export", {{"requestId", jstr(j, "requestId")}, {"ok", !path.empty()}, {"path", path}});
    };
    h["device_klipper_poll"] = [post_async](const json& j) {
        const std::string host = DA::key_of(jstr(j, "host"));
        const std::string rid  = jstr(j, "requestId");
        if (host.empty())
            return;
        std::thread([host, rid, post_async] {
            const Zoukei::WorkerToken token;
            if (!token || Zoukei::shutting_down())
                return;
            {
                std::lock_guard<std::mutex> lock(s_klipper_mutex);
                if (!s_klipper_busy.insert(host).second) {
                    post_async("device_klipper_poll", {{"requestId", rid}, {"added", 0}, {"busy", true}});
                    return;
                }
            }
            const int added = DA::klipper_poll(host);
            {
                std::lock_guard<std::mutex> lock(s_klipper_mutex);
                s_klipper_busy.erase(host);
            }
            if (!Zoukei::shutting_down())
                post_async("device_klipper_poll", {{"requestId", rid}, {"added", added < 0 ? 0 : added}, {"error", added < 0}});
        }).detach();
    };
}

}}} // namespace Slic3r::GUI::DeviceConsole
