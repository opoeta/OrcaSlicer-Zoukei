// Expert G-code Reprocessor rule editor - ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// See GCodeReprocessorDialog.hpp. Engine: libslic3r/GCode/ExpertGCodeReprocessor.cpp.
#include "GCodeReprocessorDialog.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

namespace {

enum RuleKind { RK_Speed = 0, RK_Flow, RK_Fan, RK_ZOffset, RK_Count };

const char *rule_key(int kind)
{
    switch (kind) {
    case RK_Speed:   return "speed_multiplier";
    case RK_Flow:    return "flow_multiplier";
    case RK_Fan:     return "fan_override";
    case RK_ZOffset: return "z_offset";
    }
    return "";
}

int rule_kind_from_key(const std::string &key)
{
    for (int k = 0; k < RK_Count; ++k)
        if (key == rule_key(k))
            return k;
    return -1;
}

wxString rule_label(int kind)
{
    switch (kind) {
    case RK_Speed:   return _L("Speed override (M220, %)");
    case RK_Flow:    return _L("Flow override (M221, %)");
    case RK_Fan:     return _L("Fan override (M106, PWM 0-255)");
    case RK_ZOffset: return _L("Z offset (SET_GCODE_OFFSET Z, mm)");
    }
    return wxString();
}

void value_range(int kind, double &mn, double &mx, double &def)
{
    switch (kind) {
    case RK_Speed:   mn = 1;    mx = 300;  def = 100; break;
    case RK_Flow:    mn = 20;   mx = 200;  def = 100; break;
    case RK_Fan:     mn = 0;    mx = 255;  def = 255; break;
    default:         mn = -0.3; mx = 0.3;  def = 0.0; break;
    }
}

// Edits one rule. Layers are shown 1-based; the JSON schema (shared with the engine) is 0-based and
// uses layer_to == -1 for "to the end of the file".
class RuleEditDialog : public wxDialog
{
public:
    RuleEditDialog(wxWindow *parent, const json &rule)
        : wxDialog(parent, wxID_ANY, _L("G-code rule"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        auto *top  = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 6, 8);
        grid->AddGrowableCol(1, 1);

        auto add_row = [&](const wxString &label, wxWindow *w) {
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(w, 1, wxEXPAND);
        };

        m_type = new wxChoice(this, wxID_ANY);
        for (int k = 0; k < RK_Count; ++k)
            m_type->Append(rule_label(k));
        add_row(_L("Rule type"), m_type);

        m_mode = new wxChoice(this, wxID_ANY);
        m_mode->Append(_L("Global (whole layer range)"));
        m_mode->Append(_L("By tool (only while the tool is active)"));
        add_row(_L("Mode"), m_mode);

        m_tool = new wxSpinCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 63, 0);
        add_row(_L("Tool (T index, 0-based)"), m_tool);

        m_from = new wxSpinCtrl(this, wxID_ANY, "1", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100000, 1);
        add_row(_L("From layer"), m_from);

        m_to_end = new wxCheckBox(this, wxID_ANY, _L("Until the end of the print"));
        add_row(wxString(), m_to_end);

        m_to = new wxSpinCtrl(this, wxID_ANY, "1", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100000, 1);
        add_row(_L("To layer"), m_to);

        m_value_int = new wxSpinCtrl(this, wxID_ANY, "100", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 300, 100);
        m_value_dbl = new wxSpinCtrlDouble(this, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, -0.3, 0.3, 0.0, 0.01);
        m_value_dbl->SetDigits(2);
        auto *value_box = new wxBoxSizer(wxVERTICAL);
        value_box->Add(m_value_int, 0, wxEXPAND);
        value_box->Add(m_value_dbl, 0, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Value")), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(value_box, 1, wxEXPAND);

        m_avoid_wt = new wxCheckBox(this, wxID_ANY, _L("Avoid wipe tower G-code"));
        add_row(wxString(), m_avoid_wt);

        top->Add(grid, 1, wxEXPAND | wxALL, 12);
        top->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
        SetSizerAndFit(top);

        // Load current values.
        int kind = RK_Speed;
        if (rule.contains("type") && rule["type"].is_string()) {
            const int k = rule_kind_from_key(rule["type"].get<std::string>());
            if (k >= 0)
                kind = k;
        }
        m_type->SetSelection(kind);
        const bool by_tool = rule.contains("mode") && rule["mode"].is_string() && rule["mode"].get<std::string>() == "by_tool";
        m_mode->SetSelection(by_tool ? 1 : 0);
        if (rule.contains("tool") && rule["tool"].is_number_integer())
            m_tool->SetValue(std::clamp(rule["tool"].get<int>(), 0, 63));
        const int from = rule.contains("layer_from") && rule["layer_from"].is_number_integer() ? rule["layer_from"].get<int>() : 0;
        const int to   = rule.contains("layer_to") && rule["layer_to"].is_number_integer() ? rule["layer_to"].get<int>() : -1;
        m_from->SetValue(std::max(0, from) + 1);
        m_to_end->SetValue(to < 0);
        m_to->SetValue(to < 0 ? std::max(0, from) + 1 : to + 1);
        update_value_range(kind);
        if (rule.contains("value") && rule["value"].is_number()) {
            if (kind == RK_ZOffset)
                m_value_dbl->SetValue(std::clamp(rule["value"].get<double>(), -0.3, 0.3));
            else
                m_value_int->SetValue(static_cast<int>(std::lround(rule["value"].get<double>())));
        }
        m_avoid_wt->SetValue(rule.value("avoid_wipetower", false));
        m_json = rule;

        m_type->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { update_value_range(m_type->GetSelection()); });
        m_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { update_enabled(); });
        m_to_end->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { update_enabled(); });
        update_enabled();
        CentreOnParent();
    }

    // Returns the edited rule, merged over the original object so unknown keys survive.
    json result() const
    {
        json r = m_json.is_object() ? m_json : json::object();
        const int kind = std::clamp(m_type->GetSelection(), 0, static_cast<int>(RK_Count) - 1);
        r["type"]    = rule_key(kind);
        r["enabled"] = true;
        const bool by_tool = m_mode->GetSelection() == 1;
        r["mode"] = by_tool ? "by_tool" : "global";
        if (by_tool)
            r["tool"] = m_tool->GetValue();
        else
            r.erase("tool");
        const int from = m_from->GetValue() - 1;
        r["layer_from"] = from;
        r["layer_to"]   = m_to_end->GetValue() ? -1 : std::max(from, m_to->GetValue() - 1);
        if (kind == RK_ZOffset)
            r["value"] = std::clamp(std::round(m_value_dbl->GetValue() * 100.0) / 100.0, -0.3, 0.3);
        else
            r["value"] = m_value_int->GetValue();
        r["avoid_wipetower"] = m_avoid_wt->GetValue();
        return r;
    }

private:
    void update_value_range(int kind)
    {
        double mn, mx, def;
        value_range(kind, mn, mx, def);
        const bool is_dbl = (kind == RK_ZOffset);
        if (!is_dbl) {
            const int cur = m_value_int->GetValue();
            m_value_int->SetRange(static_cast<int>(mn), static_cast<int>(mx));
            m_value_int->SetValue(std::clamp(cur, static_cast<int>(mn), static_cast<int>(mx)));
        }
        m_value_int->Show(!is_dbl);
        m_value_dbl->Show(is_dbl);
        Layout();
        Fit();
    }

    void update_enabled()
    {
        m_tool->Enable(m_mode->GetSelection() == 1);
        m_to->Enable(!m_to_end->GetValue());
    }

    json              m_json;
    wxChoice         *m_type{ nullptr };
    wxChoice         *m_mode{ nullptr };
    wxSpinCtrl       *m_tool{ nullptr };
    wxSpinCtrl       *m_from{ nullptr };
    wxSpinCtrl       *m_to{ nullptr };
    wxCheckBox       *m_to_end{ nullptr };
    wxSpinCtrl       *m_value_int{ nullptr };
    wxSpinCtrlDouble *m_value_dbl{ nullptr };
    wxCheckBox       *m_avoid_wt{ nullptr };
};

} // namespace

GCodeReprocessorDialog::GCodeReprocessorDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Expert G-code Reprocessor"), wxDefaultPosition, wxSize(720, 420),
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto *top = new wxBoxSizer(wxVERTICAL);

    auto *note = new wxStaticText(this, wxID_ANY,
        _L("Rewrites the exported G-code by layer range and, optionally, only while a given tool is active. "
           "Rules apply when the G-code is exported (no re-slice needed). Expert use only: verify the result before printing."));
    note->Wrap(680);
    top->Add(note, 0, wxEXPAND | wxALL, 10);

    m_enabled = new wxCheckBox(this, wxID_ANY, _L("Reprocessor enabled"));
    top->Add(m_enabled, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->InsertColumn(0, _L("Rule"), wxLIST_FORMAT_LEFT, 230);
    m_list->InsertColumn(1, _L("Mode"), wxLIST_FORMAT_LEFT, 90);
    m_list->InsertColumn(2, _L("Layers"), wxLIST_FORMAT_LEFT, 110);
    m_list->InsertColumn(3, _L("Value"), wxLIST_FORMAT_LEFT, 80);
    m_list->InsertColumn(4, _L("No wipe tower"), wxLIST_FORMAT_LEFT, 100);
    top->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto *btns   = new wxBoxSizer(wxHORIZONTAL);
    auto *add    = new wxButton(this, wxID_ANY, _L("Add rule..."));
    m_btn_edit   = new wxButton(this, wxID_ANY, _L("Edit..."));
    m_btn_delete = new wxButton(this, wxID_ANY, _L("Delete"));
    btns->Add(add, 0, wxRIGHT, 6);
    btns->Add(m_btn_edit, 0, wxRIGHT, 6);
    btns->Add(m_btn_delete, 0);
    top->Add(btns, 0, wxALL, 10);

    top->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    SetSizer(top);

    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_add(); });
    m_btn_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_edit(); });
    m_btn_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_delete(); });
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &) { update_buttons(); });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent &) { update_buttons(); });
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent &) { on_edit(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        if (save_to_config())
            evt.Skip(); // let the standard OK handling close the dialog
    }, wxID_OK);

    load_from_config();
    refresh_list();
    update_buttons();
    CentreOnParent();
}

void GCodeReprocessorDialog::on_dpi_changed(const wxRect &) { Layout(); Fit(); }

void GCodeReprocessorDialog::load_from_config()
{
    m_rules.clear();
    bool enabled = true;
    const auto &cfg = wxGetApp().preset_bundle->project_config;
    if (cfg.has("expert_gcode_reprocessor_rules")) {
        const std::string raw = cfg.opt_string("expert_gcode_reprocessor_rules");
        if (!raw.empty()) {
            try {
                const json root = json::parse(raw);
                if (root.is_object()) {
                    enabled = root.value("enabled", true);
                    if (root.contains("rules") && root["rules"].is_array())
                        for (const auto &r : root["rules"])
                            if (r.is_object())
                                m_rules.push_back(r);
                }
            } catch (const std::exception &) {
                // malformed blob: start from an empty rule list
            }
        }
    }
    m_enabled->SetValue(enabled);
}

bool GCodeReprocessorDialog::save_to_config()
{
    json root;
    root["version"] = 2;
    root["enabled"] = m_enabled->GetValue();
    root["rules"]   = json::array();
    for (const auto &r : m_rules)
        root["rules"].push_back(r);
    // An empty list with the master switch on is stored as an empty string (feature fully off).
    const std::string out = (m_rules.empty()) ? std::string() : root.dump();

    auto &cfg = wxGetApp().preset_bundle->project_config;
    auto *opt = cfg.option<ConfigOptionString>("expert_gcode_reprocessor_rules", true);
    if (opt != nullptr && opt->value != out) {
        opt->value = out;
        // Push the new project config to the Print so the next export sees it. The key is in
        // Print::apply's steps_ignore list: nothing gets invalidated, no re-slice happens.
        if (wxGetApp().plater() != nullptr)
            wxGetApp().plater()->update();
    }
    return true;
}

void GCodeReprocessorDialog::refresh_list()
{
    const long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    m_list->DeleteAllItems();
    for (size_t i = 0; i < m_rules.size(); ++i) {
        const json &r = m_rules[i];
        const std::string type = (r.contains("type") && r["type"].is_string()) ? r["type"].get<std::string>() : std::string();
        const int kind = rule_kind_from_key(type);
        const long row = m_list->InsertItem(static_cast<long>(i), kind >= 0 ? rule_label(kind) : wxString::FromUTF8(type + " (unknown)"));

        const bool by_tool = r.contains("mode") && r["mode"].is_string() && r["mode"].get<std::string>() == "by_tool";
        wxString mode = by_tool ? _L("By tool") : _L("Global");
        if (by_tool && r.contains("tool") && r["tool"].is_number_integer())
            mode += wxString::Format(" T%d", r["tool"].get<int>());
        m_list->SetItem(row, 1, mode);

        const int from = (r.contains("layer_from") && r["layer_from"].is_number_integer()) ? r["layer_from"].get<int>() : 0;
        const int to   = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        m_list->SetItem(row, 2, to < 0 ? wxString::Format("%d - %s", from + 1, _L("end")) : wxString::Format("%d - %d", from + 1, to + 1));

        wxString value;
        if (r.contains("value") && r["value"].is_number())
            value = (kind == RK_ZOffset) ? wxString::Format("%.2f", r["value"].get<double>())
                                         : wxString::Format("%d", static_cast<int>(std::lround(r["value"].get<double>())));
        m_list->SetItem(row, 3, value);
        m_list->SetItem(row, 4, r.value("avoid_wipetower", false) ? _L("Yes") : wxString());
    }
    if (sel >= 0 && sel < m_list->GetItemCount())
        m_list->SetItemState(sel, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    update_buttons();
}

void GCodeReprocessorDialog::update_buttons()
{
    const bool has_sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) >= 0;
    m_btn_edit->Enable(has_sel);
    m_btn_delete->Enable(has_sel);
}

void GCodeReprocessorDialog::on_add()
{
    json fresh = { { "type", "speed_multiplier" }, { "enabled", true }, { "mode", "global" },
                   { "layer_from", 0 }, { "layer_to", -1 }, { "value", 100 }, { "avoid_wipetower", false } };
    RuleEditDialog dlg(this, fresh);
    if (dlg.ShowModal() == wxID_OK) {
        m_rules.push_back(dlg.result());
        refresh_list();
    }
}

void GCodeReprocessorDialog::on_edit()
{
    const long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= m_rules.size())
        return;
    RuleEditDialog dlg(this, m_rules[sel]);
    if (dlg.ShowModal() == wxID_OK) {
        m_rules[sel] = dlg.result();
        refresh_list();
    }
}

void GCodeReprocessorDialog::on_delete()
{
    const long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= m_rules.size())
        return;
    m_rules.erase(m_rules.begin() + sel);
    refresh_list();
}

}} // namespace Slic3r::GUI
