#include "ColorMixDialog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <map>
#include <numeric>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Tab.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

namespace {

constexpr const char *kSection = "color_mix";
const int kSteps[] = {2, 3, 4, 6, 8, 10};

std::string key_safe(std::string s)
{
    for (char &c : s)
        if (!std::isalnum((unsigned char) c))
            c = '_';
    return s;
}

int hex_alpha(const std::string &hex)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 8)
        return 255;
    try {
        return std::stoi(s.substr(6, 2), nullptr, 16);
    } catch (...) {
        return 255;
    }
}

wxColour de_colour(double de)
{
    if (de < 3.0) return wxColour(0, 150, 136);
    if (de < 8.0) return wxColour(232, 163, 61);
    return wxColour(208, 27, 27);
}

} // namespace

ColorMixDialog::ColorMixDialog(wxWindow                            *parent,
                               const std::vector<ColorMixTarget>   &targets,
                               const std::vector<ColorMixFilament> &filaments,
                               const std::vector<ColorMixExisting> &existing_mixes,
                               int                                  free_slots,
                               bool                                 project_mode)
    : DPIDialog(parent, wxID_ANY, _L("Mix colors automatically"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_targets(targets)
    , m_filaments(filaments)
    , m_existing(existing_mixes)
    , m_free_slots(std::max(0, free_slots))
    , m_project_mode(project_mode)
{
    m_td.assign(m_filaments.size(), 0.0);
    const AppConfig *cfg = wxGetApp().app_config;
    for (size_t i = 0; i < m_filaments.size(); ++i) {
        const std::string v = cfg->get(kSection, "td_" + key_safe(m_filaments[i].hex) + "_" + key_safe(m_filaments[i].name));
        if (!v.empty())
            m_td[i] = std::max(0.0, std::atof(v.c_str()));
    }
    build_ui();
    load_options();
    solve();
    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    Fit();
    SetMinSize(wxSize(FromDIP(640), FromDIP(520)));
    CentreOnParent();
}

void ColorMixDialog::on_dpi_changed(const wxRect &)
{
    Layout();
    Fit();
    Refresh();
}

wxPanel *ColorMixDialog::swatch(wxWindow *parent, const std::string &hex, const wxSize &size)
{
    auto *p = new wxPanel(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_SIMPLE);
    p->SetBackgroundColour(wxColour(wxString::FromUTF8(hex.substr(0, 7))));
    p->SetToolTip(wxString::FromUTF8(hex));
    return p;
}

ColorMix::PrinterKind ColorMixDialog::current_printer()
{
    ColorMix::PrinterKind k;
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    if (auto *nd = cfg.option<ConfigOptionFloats>("nozzle_diameter"))
        k.nozzles = std::max<int>(1, int(nd->values.size()));
    if (auto *semm = cfg.option<ConfigOptionBool>("single_extruder_multi_material"))
        k.single_extruder_mm = semm->value || k.nozzles == 1;
    return k;
}

void ColorMixDialog::apply_method(ColorMix::Method method)
{
    if (method == ColorMix::Method::Auto)
        method = ColorMix::choose_method(current_printer());
    const bool          want = method == ColorMix::Method::SubLayers;
    PresetBundle       *pb   = wxGetApp().preset_bundle;
    DynamicPrintConfig  conf = pb->prints.get_edited_preset().config;
    if (!conf.has("enable_mixed_color_sublayer") || conf.opt_bool("enable_mixed_color_sublayer") == want)
        return;
    conf.set_key_value("enable_mixed_color_sublayer", new ConfigOptionBool(want));
    if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
        tab->load_config(conf);
}

void ColorMixDialog::build_ui()
{
    auto *top = new wxBoxSizer(wxVERTICAL);
    const int gap = FromDIP(8);

    auto *intro = new wxStaticText(this, wxID_ANY,
        m_project_mode ?
            _L("Tick the filaments loaded in the printer. The other project colors are reproduced with mixed filaments "
               "(alternating layers of the loaded filaments) or with the closest loaded filament.") :
            _L("The model colors are reproduced with mixed filaments (alternating layers of your loaded filaments) "
               "or with the closest loaded filament. Review the recipes before applying."));
    intro->Wrap(FromDIP(620));
    top->Add(intro, 0, wxALL, gap);

    // Filaments, with optional transmission distance.
    auto *fil_box   = new wxStaticBoxSizer(wxVERTICAL, this, m_project_mode ? _L("Project filaments") : _L("Loaded filaments"));
    // Two filaments per row: [loaded] swatch name TD [spin].
    auto *fil_grid  = new wxFlexGridSizer(0, 10, FromDIP(4), FromDIP(6));
    for (size_t i = 0; i < m_filaments.size(); ++i) {
        const ColorMixFilament &f = m_filaments[i];
        wxCheckBox *cb = nullptr;
        if (m_project_mode) {
            cb = new wxCheckBox(fil_box->GetStaticBox(), wxID_ANY, wxEmptyString);
            cb->SetValue(f.loaded);
            cb->SetToolTip(_L("Loaded in the printer"));
            cb->Bind(wxEVT_CHECKBOX, [this, i](wxCommandEvent &e) {
                m_filaments[i].loaded = e.IsChecked();
                solve();
            });
            fil_grid->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, i % 2 ? FromDIP(16) : 0);
        } else {
            fil_grid->Add(i % 2 ? FromDIP(16) : 0, 0);
        }
        m_cb_loaded.push_back(cb);
        fil_grid->Add(swatch(fil_box->GetStaticBox(), f.hex, wxSize(FromDIP(18), FromDIP(18))), 0, wxALIGN_CENTER_VERTICAL);
        auto *name = new wxStaticText(fil_box->GetStaticBox(), wxID_ANY,
                                      wxString::Format("F%d %s", f.index + 1, wxString::FromUTF8(f.name)));
        fil_grid->Add(name, 0, wxALIGN_CENTER_VERTICAL);
        auto *td = new wxSpinCtrlDouble(fil_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition,
                                        wxSize(FromDIP(80), -1), wxSP_ARROW_KEYS, 0.0, 50.0, m_td[i], 0.1);
        td->SetDigits(1);
        td->SetToolTip(_L("Transmission distance (TD, mm) as measured for HueForge: a layer this thick still lets 10% of the light "
                          "through. Lower is more opaque. 0 = estimate from the filament type."));
        td->Bind(wxEVT_SPINCTRLDOUBLE, [this, i, td](wxSpinDoubleEvent &) {
            m_td[i] = td->GetValue();
            solve();
        });
        m_spin_td.push_back(td);
        fil_grid->Add(new wxStaticText(fil_box->GetStaticBox(), wxID_ANY, "TD"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
        fil_grid->Add(td, 0, wxALIGN_CENTER_VERTICAL);
    }
    fil_box->Add(fil_grid, 0, wxALL, FromDIP(4));
    top->Add(fil_box, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);

    // Options.
    auto *opt_box  = new wxStaticBoxSizer(wxVERTICAL, this, _L("Options"));
    wxWindow *ob   = opt_box->GetStaticBox();
    auto *row1     = new wxBoxSizer(wxHORIZONTAL);
    auto  on_change = [this](wxCommandEvent &) { solve(); };
    if (!m_project_mode) {
        row1->Add(new wxStaticText(ob, wxID_ANY, _L("Target colors")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        m_spin_colors = new wxSpinCtrl(ob, wxID_ANY, "8", wxDefaultPosition, wxSize(FromDIP(64), -1), wxSP_ARROW_KEYS, 1, 32, 8);
        m_spin_colors->SetToolTip(_L("The model colors are grouped (k-means in OKLab) into at most this many target colors"));
        m_spin_colors->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { solve(); });
        row1->Add(m_spin_colors, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    }
    row1->Add(new wxStaticText(ob, wxID_ANY, _L("Max. new mixes")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    const int max_mix = std::min(16, m_free_slots);
    m_spin_mixes = new wxSpinCtrl(ob, wxID_ANY, "4", wxDefaultPosition, wxSize(FromDIP(64), -1), wxSP_ARROW_KEYS, 0, max_mix,
                                  std::min(4, max_mix));
    m_spin_mixes->SetToolTip(_L("Most mixed filaments this creates; each one uses a filament slot"));
    m_spin_mixes->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { solve(); });
    row1->Add(m_spin_mixes, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row1->Add(new wxStaticText(ob, wxID_ANY, _L("Ratio step")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_choice_step = new wxChoice(ob, wxID_ANY);
    for (int L : kSteps)
        m_choice_step->Append(wxString::Format("1/%d (%g%%)", L, std::round(1000.0 / L) / 10.0));
    m_choice_step->SetToolTip(_L("Smallest share of one filament in a mix. Finer steps give more colors and longer layer patterns."));
    m_choice_step->Bind(wxEVT_CHOICE, on_change);
    row1->Add(m_choice_step, 0, wxALIGN_CENTER_VERTICAL);
    opt_box->Add(row1, 0, wxALL, FromDIP(4));

    auto *row2 = new wxBoxSizer(wxHORIZONTAL);
    m_cb_triples = new wxCheckBox(ob, wxID_ANY, _L("Allow 3-filament mixes"));
    m_cb_triples->Bind(wxEVT_CHECKBOX, on_change);
    row2->Add(m_cb_triples, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    m_cb_physical = new wxCheckBox(ob, wxID_ANY, _L("Only physical filaments"));
    m_cb_physical->SetToolTip(_L("Do not create mixed filaments; every color goes to the closest loaded filament"));
    m_cb_physical->Bind(wxEVT_CHECKBOX, on_change);
    row2->Add(m_cb_physical, 0, wxALIGN_CENTER_VERTICAL);
    opt_box->Add(row2, 0, wxALL, FromDIP(4));

    auto *row3 = new wxBoxSizer(wxHORIZONTAL);
    const ColorMix::PrinterKind printer = current_printer();
    const ColorMix::Method      autom   = ColorMix::choose_method(printer);
    row3->Add(new wxStaticText(ob, wxID_ANY, _L("Method")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_choice_method = new wxChoice(ob, wxID_ANY);
    m_choice_method->Append(wxString::Format(_L("Automatic (%s)"),
                                             autom == ColorMix::Method::SubLayers ? _L("Sub-layers") : _L("Alternating layers")));
    m_choice_method->Append(_L("Alternating layers"));
    m_choice_method->Append(_L("Sub-layers"));
    m_choice_method->SetToolTip(_L("Alternating layers: whole layers of each filament in a repeating pattern, fewest filament changes.\n"
                                   "Sub-layers: every layer is split into thinner sub-layers, one per filament; finer mix, one change "
                                   "per filament on every layer. This sets the print option \"Mixed color sublayer\"."));
    m_choice_method->Bind(wxEVT_CHOICE, on_change);
    row3->Add(m_choice_method, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row3->Add(new wxStaticText(ob, wxID_ANY, _L("Color model")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_choice_model = new wxChoice(ob, wxID_ANY);
    m_choice_model->Append(_L("Pigment (measured blends + polynomial)"));
    m_choice_model->Append(_L("Optical (stripes + translucency)"));
    m_choice_model->Bind(wxEVT_CHOICE, on_change);
    row3->Add(m_choice_model, 0, wxALIGN_CENTER_VERTICAL);
    opt_box->Add(row3, 0, wxALL, FromDIP(4));
    top->Add(opt_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    m_printer_note = new wxStaticText(this, wxID_ANY,
        (printer.nozzles > 1 && !printer.single_extruder_mm) ?
            _L("Toolchanger / multi-nozzle printer: switching between the filaments of a mix needs no purge, so mixes are cheap "
               "and sub-layers give the finest blend.") :
            _L("Single-nozzle printer with a filament feeder (AMS / CFS / IFS / MMU): every switch between the filaments of a mix "
               "purges filament into the prime tower. Alternating layers switch at most once per layer; sub-layers switch on "
               "every layer. Expect more waste and print time with many mixes."));
    m_printer_note->SetForegroundColour((printer.nozzles > 1 && !printer.single_extruder_mm) ? wxColour(0, 150, 136) : wxColour(232, 130, 30));
    m_printer_note->Wrap(FromDIP(620));
    top->Add(m_printer_note, 0, wxALL, gap);

    // Recipes.
    m_rows_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(640), FromDIP(300)), wxVSCROLL);
    m_rows_panel->SetScrollRate(0, FromDIP(16));
    m_rows_sizer = new wxFlexGridSizer(0, 5, FromDIP(4), FromDIP(8));
    m_rows_sizer->AddGrowableCol(2, 1);
    m_rows_panel->SetSizer(m_rows_sizer);
    top->Add(m_rows_panel, 1, wxEXPAND | wxLEFT | wxRIGHT, gap);

    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
    top->Add(m_summary, 0, wxALL, gap);

    auto *buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    if (wxWindow *ok = FindWindow(wxID_OK))
        ok->SetLabel(_L("Apply"));
    top->Add(buttons, 0, wxEXPAND | wxALL, gap);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        save_options();
        e.Skip();
    }, wxID_OK);

    SetSizer(top);
}

void ColorMixDialog::load_options()
{
    const AppConfig *cfg = wxGetApp().app_config;
    auto get_int = [cfg](const char *key, int def) {
        const std::string v = cfg->get(kSection, key);
        return v.empty() ? def : std::atoi(v.c_str());
    };
    if (m_spin_colors)
        m_spin_colors->SetValue(std::clamp(get_int("max_colors", 8), 1, 32));
    m_spin_mixes->SetValue(std::clamp(get_int("max_mixes", 4), 0, m_spin_mixes->GetMax()));
    const int step = get_int("pattern_length", 4);
    int sel = 2;
    for (int i = 0; i < int(std::size(kSteps)); ++i)
        if (kSteps[i] == step)
            sel = i;
    m_choice_step->SetSelection(sel);
    m_cb_triples->SetValue(get_int("triples", 0) != 0);
    m_cb_physical->SetValue(get_int("only_physical", 0) != 0);
    const ColorMix::Method m = ColorMix::method_from_key(cfg->get(kSection, "method"));
    m_choice_method->SetSelection(m == ColorMix::Method::AlternatingLayers ? 1 : m == ColorMix::Method::SubLayers ? 2 : 0);
    m_choice_model->SetSelection(ColorMix::model_from_key(cfg->get(kSection, "model")) == ColorMix::Model::Optical ? 1 : 0);
}

void ColorMixDialog::save_options() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (m_spin_colors)
        cfg->set(kSection, "max_colors", std::to_string(m_spin_colors->GetValue()));
    cfg->set(kSection, "max_mixes", std::to_string(m_spin_mixes->GetValue()));
    cfg->set(kSection, "pattern_length", std::to_string(kSteps[std::max(0, m_choice_step->GetSelection())]));
    cfg->set(kSection, "triples", m_cb_triples->GetValue() ? "1" : "0");
    cfg->set(kSection, "only_physical", m_cb_physical->GetValue() ? "1" : "0");
    const int ms = m_choice_method->GetSelection();
    cfg->set(kSection, "method", ColorMix::method_key(ms == 1 ? ColorMix::Method::AlternatingLayers :
                                                       ms == 2 ? ColorMix::Method::SubLayers : ColorMix::Method::Auto));
    cfg->set(kSection, "model", ColorMix::model_key(m_choice_model->GetSelection() == 1 ? ColorMix::Model::Optical : ColorMix::Model::Pigment));
    for (size_t i = 0; i < m_filaments.size(); ++i) {
        const std::string key = "td_" + key_safe(m_filaments[i].hex) + "_" + key_safe(m_filaments[i].name);
        if (m_td[i] > 0.0 || cfg->has(kSection, key))
            cfg->set(kSection, key, m_td[i] > 0.0 ? wxString::Format("%.1f", m_td[i]).ToStdString() : std::string());
    }
}

ColorMix::Method ColorMixDialog::method() const
{
    const int sel = m_choice_method ? m_choice_method->GetSelection() : 0;
    if (sel == 1) return ColorMix::Method::AlternatingLayers;
    if (sel == 2) return ColorMix::Method::SubLayers;
    return ColorMix::choose_method(current_printer());
}

std::vector<ColorMix::Filament> ColorMixDialog::palette() const
{
    std::vector<ColorMix::Filament> out;
    for (int fi : m_palette_map) {
        const ColorMixFilament &src = m_filaments[size_t(fi)];
        ColorMix::Filament      f;
        ColorMix::rgb_from_hex(src.hex, f.color);
        f.td_mm   = m_td[size_t(fi)];
        f.opacity = ColorMix::default_opacity(src.type, src.name, hex_alpha(src.hex));
        f.name    = src.name;
        out.push_back(f);
    }
    return out;
}

ColorMix::SolverOptions ColorMixDialog::options() const
{
    ColorMix::SolverOptions opt;
    opt.max_colors     = m_project_mode ? 0 : (m_spin_colors ? m_spin_colors->GetValue() : 8);
    opt.max_mixes      = m_spin_mixes->GetValue();
    opt.pattern_length = kSteps[std::max(0, m_choice_step->GetSelection())];
    opt.max_components = m_cb_triples->GetValue() ? 3 : 2;
    opt.only_physical  = m_cb_physical->GetValue() || m_palette_map.size() < 2;
    opt.model.model    = m_choice_model->GetSelection() == 1 ? ColorMix::Model::Optical : ColorMix::Model::Pigment;
    opt.model.method   = method();
    if (const auto *lh = wxGetApp().preset_bundle->prints.get_edited_preset().config.option<ConfigOptionFloat>("layer_height"))
        opt.model.layer_height = lh->value;
    if (opt.model.model == ColorMix::Model::Pigment)
        opt.model.measured = [](const std::vector<ColorMix::RGB> &colors, const std::vector<int> &weights, ColorMix::RGB &out) {
            std::vector<std::string> hexes;
            for (const ColorMix::RGB &c : colors)
                hexes.push_back(ColorMix::rgb_to_hex(c));
            const std::string hex = lookup_measured_blend_color(hexes, weights);
            return !hex.empty() && ColorMix::rgb_from_hex(hex, out);
        };
    // Existing mixes whose components are all in the palette are free to reuse.
    for (const ColorMixExisting &e : m_existing) {
        ColorMix::Recipe r;
        for (size_t k = 0; k < e.components.size() && k < e.percentages.size(); ++k) {
            int pal = -1;
            for (size_t p = 0; p < m_palette_map.size(); ++p)
                if (m_filaments[size_t(m_palette_map[p])].index == e.components[k])
                    pal = int(p);
            if (pal < 0) {
                r.components.clear();
                break;
            }
            r.components.push_back(pal);
            r.weights.push_back(e.percentages[k]);
        }
        if (r.components.size() >= 2)
            opt.existing_mixes.push_back(r);
    }
    return opt;
}

void ColorMixDialog::solve()
{
    m_palette_map.clear();
    for (size_t i = 0; i < m_filaments.size(); ++i)
        if (m_filaments[i].loaded)
            m_palette_map.push_back(int(i));

    std::vector<ColorMix::RGB> colors;
    std::vector<double>        weights;
    m_target_ids.clear();
    if (m_project_mode) {
        for (size_t i = 0; i < m_filaments.size(); ++i)
            if (!m_filaments[i].loaded) {
                ColorMix::RGB c;
                ColorMix::rgb_from_hex(m_filaments[i].hex, c);
                colors.push_back(c);
                weights.push_back(1.0);
                m_target_ids.push_back(int(i));
            }
    } else {
        for (size_t i = 0; i < m_targets.size(); ++i) {
            ColorMix::RGB c;
            if (!ColorMix::rgb_from_hex(m_targets[i].hex, c))
                c = {0.5, 0.5, 0.5};
            colors.push_back(c);
            weights.push_back(std::max(1e-6, m_targets[i].weight));
            m_target_ids.push_back(int(i));
        }
    }

    m_solution = ColorMix::Solution{};
    if (!m_palette_map.empty() && !colors.empty())
        m_solution = ColorMix::solve(colors, weights, palette(), options());
    rebuild_rows();
    update_choices();
    update_summary();
}

std::string ColorMixDialog::filament_label(int palette_idx) const
{
    const ColorMixFilament &f = m_filaments[size_t(m_palette_map[size_t(palette_idx)])];
    return "F" + std::to_string(f.index + 1);
}

std::string ColorMixDialog::recipe_label(const ColorMix::Candidate &c, double de) const
{
    std::string s;
    const ColorMix::Recipe &r = c.recipe;
    if (r.is_physical()) {
        const ColorMixFilament &f = m_filaments[size_t(m_palette_map[size_t(r.components[0])])];
        s = filament_label(r.components[0]) + " " + f.name;
    } else {
        const std::vector<int> pct = r.percentages();
        for (size_t i = 0; i < r.components.size(); ++i) {
            if (i) s += " + ";
            s += filament_label(r.components[i]) + " " + std::to_string(pct[i]) + "%";
        }
        s += "  [" + r.pattern() + "]";
        if (c.existing)
            s += " *";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "   \xCE\x94" "E %.1f", de);
    return s + buf;
}

void ColorMixDialog::rebuild_rows()
{
    m_rows_panel->Freeze();
    m_rows_sizer->Clear(true);
    m_row_choice.clear();
    m_row_pred.clear();
    m_row_de.clear();
    m_row_candidates.clear();

    const wxSize sw(FromDIP(22), FromDIP(18));
    auto header = [this](const wxString &t) {
        auto *l = new wxStaticText(m_rows_panel, wxID_ANY, t);
        l->SetFont(l->GetFont().Bold());
        m_rows_sizer->Add(l, 0, wxALIGN_CENTER_VERTICAL);
    };
    header(_L("Target"));
    header(wxEmptyString);
    header(_L("Recipe (filament shares and layer pattern)"));
    header(_L("Predicted"));
    header(wxString::FromUTF8("\xCE\x94" "E"));

    for (size_t c = 0; c < m_solution.clusters.size(); ++c) {
        const ColorMix::Assignment &a = m_solution.clusters[c];
        // Target: the members of the cluster (or the project filament).
        auto *members = new wxBoxSizer(wxHORIZONTAL);
        int   shown   = 0;
        for (size_t t = 0; t < m_solution.source_cluster.size() && shown < 6; ++t) {
            if (m_solution.source_cluster[t] != int(c))
                continue;
            const int id = m_target_ids[t];
            const std::string hex = m_project_mode ? m_filaments[size_t(id)].hex : m_targets[size_t(id)].hex;
            wxPanel *p = swatch(m_rows_panel, hex, sw);
            if (m_project_mode)
                p->SetToolTip(wxString::Format("F%d %s", m_filaments[size_t(id)].index + 1, wxString::FromUTF8(m_filaments[size_t(id)].name)));
            members->Add(p, 0, wxRIGHT, FromDIP(2));
            ++shown;
        }
        m_rows_sizer->Add(members, 0, wxALIGN_CENTER_VERTICAL);
        m_rows_sizer->Add(new wxStaticText(m_rows_panel, wxID_ANY, wxString::FromUTF8("\xE2\x86\x92")), 0, wxALIGN_CENTER_VERTICAL);

        // Choices: the closest candidates to this target, the current one always included.
        const ColorMix::Lab tl = ColorMix::cielab_from_rgb(a.target);
        std::vector<std::pair<double, int>> ranked;
        for (size_t k = 0; k < m_solution.candidates.size(); ++k)
            ranked.emplace_back(ColorMix::ciede2000(tl, m_solution.candidates[k].lab), int(k));
        std::sort(ranked.begin(), ranked.end());
        std::vector<int> ids;
        for (size_t k = 0; k < ranked.size() && ids.size() < 14; ++k)
            ids.push_back(ranked[k].second);
        if (std::find(ids.begin(), ids.end(), a.candidate) == ids.end() && a.candidate >= 0)
            ids.insert(ids.begin(), a.candidate);
        // Physical filaments are always offered.
        for (size_t k = 0; k < m_solution.candidates.size(); ++k)
            if (m_solution.candidates[k].recipe.is_physical() && std::find(ids.begin(), ids.end(), int(k)) == ids.end())
                ids.push_back(int(k));
        auto *choice = new wxChoice(m_rows_panel, wxID_ANY);
        for (int k : ids)
            choice->Append(wxString::FromUTF8(recipe_label(m_solution.candidates[size_t(k)],
                                                           ColorMix::ciede2000(tl, m_solution.candidates[size_t(k)].lab))));
        const auto it = std::find(ids.begin(), ids.end(), a.candidate);
        choice->SetSelection(it == ids.end() ? wxNOT_FOUND : int(it - ids.begin()));
        choice->Bind(wxEVT_CHOICE, [this, c](wxCommandEvent &e) {
            const int sel = e.GetSelection();
            if (sel < 0 || size_t(sel) >= m_row_candidates[c].size())
                return;
            m_solution.clusters[c].candidate = m_row_candidates[c][size_t(sel)];
            ColorMix::refresh(m_solution, options());
            const ColorMix::Candidate &cand = m_solution.candidates[size_t(m_solution.clusters[c].candidate)];
            m_row_pred[c]->SetBackgroundColour(wxColour(wxString(ColorMix::rgb_to_hex(cand.predicted))));
            m_row_pred[c]->SetToolTip(wxString(ColorMix::rgb_to_hex(cand.predicted)));
            m_row_pred[c]->Refresh();
            m_row_de[c]->SetLabel(wxString::Format("%.1f", m_solution.clusters[c].delta_e));
            m_row_de[c]->SetForegroundColour(de_colour(m_solution.clusters[c].delta_e));
            update_choices();
            update_summary();
        });
        m_row_candidates.push_back(ids);
        m_row_choice.push_back(choice);
        m_rows_sizer->Add(choice, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

        const std::string pred = a.candidate >= 0 ? ColorMix::rgb_to_hex(m_solution.candidates[size_t(a.candidate)].predicted) : "#808080";
        wxPanel *pp = swatch(m_rows_panel, pred, sw);
        m_row_pred.push_back(pp);
        m_rows_sizer->Add(pp, 0, wxALIGN_CENTER_VERTICAL);
        auto *de = new wxStaticText(m_rows_panel, wxID_ANY, wxString::Format("%.1f", a.delta_e));
        de->SetForegroundColour(de_colour(a.delta_e));
        m_row_de.push_back(de);
        m_rows_sizer->Add(de, 0, wxALIGN_CENTER_VERTICAL);
    }
    if (m_solution.clusters.empty()) {
        m_rows_sizer->Add(new wxStaticText(m_rows_panel, wxID_ANY,
                                           m_palette_map.empty() ? _L("Select at least one loaded filament.") :
                                                                   _L("Nothing to mix: every color is a loaded filament.")),
                          0, wxALL, FromDIP(8));
    }
    m_rows_panel->FitInside();
    m_rows_panel->Layout();
    m_rows_panel->Thaw();
    wxGetApp().UpdateDarkUIWin(m_rows_panel);
}

void ColorMixDialog::update_choices()
{
    const size_t n = m_project_mode ? m_filaments.size() : m_targets.size();
    m_choices.assign(n, ColorMixChoice{});
    if (m_project_mode)
        for (size_t i = 0; i < m_filaments.size(); ++i)
            if (m_filaments[i].loaded) {
                m_choices[i].filament = m_filaments[i].index;
                m_choices[i].hex      = m_filaments[i].hex;
            }
    for (size_t t = 0; t < m_target_ids.size() && t < m_solution.source_cluster.size(); ++t) {
        const int cluster = m_solution.source_cluster[t];
        if (cluster < 0 || size_t(cluster) >= m_solution.clusters.size())
            continue;
        const ColorMix::Assignment &a = m_solution.clusters[size_t(cluster)];
        if (a.candidate < 0)
            continue;
        const ColorMix::Candidate &cand = m_solution.candidates[size_t(a.candidate)];
        ColorMixChoice            &ch   = m_choices[size_t(m_target_ids[t])];
        ch.hex     = ColorMix::rgb_to_hex(cand.predicted);
        ch.delta_e = a.delta_e;
        if (cand.recipe.is_physical()) {
            ch.filament = m_filaments[size_t(m_palette_map[size_t(cand.recipe.components[0])])].index;
        } else {
            for (int comp : cand.recipe.components)
                ch.components.push_back(m_filaments[size_t(m_palette_map[size_t(comp)])].index);
            ch.percentages = cand.recipe.percentages();
            ch.pattern     = cand.recipe.pattern();
        }
    }
}

void ColorMixDialog::update_summary()
{
    if (m_solution.clusters.empty()) {
        m_summary->SetLabel(wxEmptyString);
        return;
    }
    m_summary->SetLabel(wxString::Format(_L("Mean %s %.1f (closest loaded filament only: %.1f), worst %.1f, %d new mixed filaments"),
                                         wxString::FromUTF8("\xCE\x94" "E00"), m_solution.mean_delta_e,
                                         m_solution.physical_mean_delta_e, m_solution.max_delta_e,
                                         int(m_solution.new_mixes.size())));
    Layout();
}

}} // namespace Slic3r::GUI
