#ifndef slic3r_ColorMixDialog_hpp_
#define slic3r_ColorMixDialog_hpp_

// Review dialog of the automatic colour mixing ("Mistura de cores"): shows, for each target colour
// of an imported model (or each project filament), the physical filament or mixed filament the
// ColorMixEngine solver chose, its predicted colour and Delta E, and lets the user change the
// options or any single recipe before applying.

#include <string>
#include <utility>
#include <vector>

#include <wx/panel.h>

#include "GUI_Utils.hpp"
#include "libslic3r/ColorMixEngine.hpp"

class wxCheckBox;
class wxChoice;
class wxScrolledWindow;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxBoxSizer;
class wxFlexGridSizer;

namespace Slic3r {
namespace GUI {

// A colour to reproduce.
struct ColorMixTarget {
    std::string hex;
    double      weight{1.0};
    std::string label;
};

// A filament of the project. `index` is the caller's index (project filament or dialog index).
struct ColorMixFilament {
    int         index{-1};
    std::string hex;
    std::string name;
    std::string type;
    bool        loaded{true}; // physical and available to mix
};

// An existing mixed filament: caller indices of its components and their percentages.
struct ColorMixExisting {
    std::vector<int> components;
    std::vector<int> percentages;
};

// What a target ends up with: a single filament (`filament` >= 0) or a mix of `components`.
struct ColorMixChoice {
    int              filament{-1};
    std::vector<int> components;  // caller indices
    std::vector<int> percentages; // sums to 100
    std::string      hex;         // predicted colour
    std::string      pattern;     // layer pattern, e.g. "AAB"
    double           delta_e{0.0};
    bool is_mix() const { return filament < 0 && components.size() >= 2; }
};

class ColorMixDialog : public DPIDialog
{
public:
    // Import mode: `targets` are model colours, the loaded `filaments` are the palette.
    // Project mode: the targets are the project filaments themselves; the user ticks which ones
    // are loaded in the printer and the others are reproduced from those.
    ColorMixDialog(wxWindow                             *parent,
                   const std::vector<ColorMixTarget>    &targets,
                   const std::vector<ColorMixFilament>  &filaments,
                   const std::vector<ColorMixExisting>  &existing_mixes,
                   int                                   free_slots,
                   bool                                  project_mode);

    // One per target (import mode) or per filament (project mode), in the given order.
    const std::vector<ColorMixChoice> &choices() const { return m_choices; }
    // The method the user confirmed, never Auto.
    ColorMix::Method method() const;
    const std::vector<ColorMixFilament> &filaments() const { return m_filaments; }

    // Printer description of the selected printer preset.
    static ColorMix::PrinterKind current_printer();
    // Sets the print option realising `method` (enable_mixed_color_sublayer) when it differs.
    static void apply_method(ColorMix::Method method);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void build_ui();
    void load_options();
    void save_options() const;
    void solve();
    void rebuild_rows();
    void update_summary();
    void update_choices();
    ColorMix::SolverOptions options() const;
    std::vector<ColorMix::Filament> palette() const;
    std::string filament_label(int palette_idx) const;
    std::string recipe_label(const ColorMix::Candidate &c, double de) const;
    wxPanel *swatch(wxWindow *parent, const std::string &hex, const wxSize &size);

    std::vector<ColorMixTarget>   m_targets;
    std::vector<ColorMixFilament> m_filaments;
    std::vector<ColorMixExisting> m_existing;
    int                           m_free_slots{0};
    bool                          m_project_mode{false};
    std::vector<double>           m_td;          // per filament, 0 = auto
    std::vector<int>              m_palette_map; // palette index -> m_filaments index
    std::vector<int>              m_target_ids;  // targets actually solved -> m_targets index
    ColorMix::Solution            m_solution;
    std::vector<std::vector<int>> m_row_candidates; // per cluster, candidate indices in the choice
    std::vector<ColorMixChoice>   m_choices;

    wxSpinCtrl       *m_spin_colors{nullptr};
    wxSpinCtrl       *m_spin_mixes{nullptr};
    wxChoice         *m_choice_step{nullptr};
    wxCheckBox       *m_cb_triples{nullptr};
    wxCheckBox       *m_cb_physical{nullptr};
    wxChoice         *m_choice_method{nullptr};
    wxChoice         *m_choice_model{nullptr};
    wxStaticText     *m_printer_note{nullptr};
    wxStaticText     *m_summary{nullptr};
    wxScrolledWindow *m_rows_panel{nullptr};
    wxFlexGridSizer  *m_rows_sizer{nullptr};
    std::vector<wxCheckBox *>       m_cb_loaded;
    std::vector<wxSpinCtrlDouble *> m_spin_td;
    std::vector<wxChoice *>         m_row_choice;
    std::vector<wxPanel *>          m_row_pred;
    std::vector<wxStaticText *>     m_row_de;
};

}} // namespace Slic3r::GUI

#endif // slic3r_ColorMixDialog_hpp_
