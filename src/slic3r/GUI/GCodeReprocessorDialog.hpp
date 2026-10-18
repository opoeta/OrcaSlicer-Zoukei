#ifndef slic3r_GUI_GCodeReprocessorDialog_hpp_
#define slic3r_GUI_GCodeReprocessorDialog_hpp_

// Expert G-code Reprocessor rule editor - ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// NeotkoCM edits the rules through an ImGui chart in the G-code preview legend; this fork uses a
// plain wx dialog (File > Export > "Expert G-code Reprocessor...") over the same JSON schema
// (project config key "expert_gcode_reprocessor_rules", consumed by
// libslic3r/GCode/ExpertGCodeReprocessor.cpp when the G-code is exported).

#include <vector>
#include <nlohmann/json.hpp>

#include "GUI_Utils.hpp"

class wxButton;
class wxCheckBox;
class wxListCtrl;
class wxListEvent;

namespace Slic3r { namespace GUI {

class GCodeReprocessorDialog : public DPIDialog
{
public:
    explicit GCodeReprocessorDialog(wxWindow *parent);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void load_from_config();
    bool save_to_config();
    void refresh_list();
    void update_buttons();
    void on_add();
    void on_edit();
    void on_delete();

    std::vector<nlohmann::json> m_rules; // unknown rule types are kept verbatim (forward compat)
    wxCheckBox *m_enabled{ nullptr };
    wxListCtrl *m_list{ nullptr };
    wxButton   *m_btn_edit{ nullptr };
    wxButton   *m_btn_delete{ nullptr };
};

}} // namespace Slic3r::GUI

#endif
