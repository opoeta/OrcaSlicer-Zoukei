#include "WhatsNewDialog.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Zoukei {

namespace {

constexpr const char* SEEN_KEY = "zoukei_whats_new_seen";

struct Item
{
    std::string title, text, action, action_label;
};

struct Release
{
    std::string       version, title;
    std::vector<Item> items;
};

// shortcut: the file is pt-BR only, per-language files when the app ships to other languages.
Release load_newest()
{
    Release out;
    try {
        boost::nowide::ifstream file((boost::filesystem::path(resources_dir()) / "data" / "whats_new.json").string());
        const nlohmann::json root = nlohmann::json::parse(file);
        const nlohmann::json& newest = root.at("releases").at(0);
        out.version = newest.at("version").get<std::string>();
        out.title   = newest.value("title", std::string());
        for (const nlohmann::json& it : newest.at("items"))
            out.items.push_back({ it.at("title").get<std::string>(), it.at("text").get<std::string>(), it.value("action", std::string()),
                                  it.value("action_label", std::string()) });
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "whats_new.json: " << e.what();
        return {};
    }
    return out;
}

void run_action(const std::string& action)
{
    if (action == "align_construct") {
        wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
        wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager().open_gizmo(GLGizmosManager::AlignStack);
    }
}

class WhatsNewDialog : public DPIDialog
{
public:
    explicit WhatsNewDialog(const Release& release)
        : DPIDialog(wxGetApp().mainframe, wxID_ANY, _L("What's new"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        const int gap = FromDIP(8);
        auto*     top = new wxBoxSizer(wxVERTICAL);

        auto* heading = new wxStaticText(this, wxID_ANY, from_u8(release.title.empty() ? release.version : release.title));
        heading->SetFont(heading->GetFont().Bold().Larger());
        top->Add(heading, 0, wxALL, gap * 2);

        auto* scroll = new wxScrolledWindow(this);
        scroll->SetScrollRate(0, FromDIP(12));
        auto* list = new wxBoxSizer(wxVERTICAL);
        const int wrap = FromDIP(520);
        for (const Item& item : release.items) {
            auto* title = new wxStaticText(scroll, wxID_ANY, from_u8(item.title));
            title->SetFont(title->GetFont().Bold());
            list->Add(title, 0, wxTOP | wxLEFT | wxRIGHT, gap);
            auto* text = new wxStaticText(scroll, wxID_ANY, from_u8(item.text));
            text->Wrap(wrap);
            list->Add(text, 0, wxTOP | wxLEFT | wxRIGHT, gap / 2);
            if (!item.action.empty()) {
                auto* button = new Button(scroll, from_u8(item.action_label));
                button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
                button->Bind(wxEVT_BUTTON, [this, action = item.action](wxCommandEvent&) {
                    EndModal(wxID_OK);
                    run_action(action);
                });
                list->Add(button, 0, wxTOP | wxLEFT, gap);
            }
        }
        scroll->SetSizer(list);
        scroll->SetMinSize(wxSize(wrap + FromDIP(40), FromDIP(420)));
        top->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, gap);

        auto* close = new Button(this, _L("Close"));
        close->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        top->Add(close, 0, wxALIGN_RIGHT | wxALL, gap * 2);

        SetSizerAndFit(top);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

protected:
    void on_dpi_changed(const wxRect&) override {}
};

void show(const Release& release)
{
    if (release.version.empty() || release.items.empty())
        return;
    wxGetApp().app_config->set(SEEN_KEY, release.version);
    WhatsNewDialog dlg(release);
    dlg.ShowModal();
}

} // namespace

void show_whats_new() { show(load_newest()); }

void show_whats_new_if_unseen()
{
    const Release release = load_newest();
    if (release.version != wxGetApp().app_config->get(SEEN_KEY))
        show(release);
}

}}} // namespace Slic3r::GUI::Zoukei
