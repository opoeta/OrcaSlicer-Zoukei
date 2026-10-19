// Flow-rate calibration, chained like Creality Print: pass 1 (coarse) -> the user picks the best sample -> pass 2
// is generated centred on it -> the user picks the best pass 2 sample -> the final flow ratio is written to the
// filament preset. Shared by Orca's flow rate tests (Plater::calib_flowrate), the Layer Slayer flow_rate test and
// the Remote API (POST /calibrations/flow_rate/apply).
//
// Maths (all multiplicative on the filament's flow ratio at the time the test was made, "current"):
//   percent tests (pass 1, pass 2, Layer Slayer): sample ratio = base * (1 + mod / 100); final = current * ratio
//     base = 1 for a first pass, = 1 + (pass 1 winner mod) / 100 for a second pass (the Creality Print formula)
//   linear tests (Orca YOLO): final = current + mod
#ifndef slic3r_GUI_Zoukei_FlowCalibChain_hpp_
#define slic3r_GUI_Zoukei_FlowCalibChain_hpp_

#include <string>
#include <vector>

class wxWindow;

namespace Slic3r { namespace GUI { namespace FlowCalib {

struct Test
{
    bool                valid        = false;
    bool                linear       = false; // true: mods are flow ratio deltas (YOLO); false: percent
    int                 pass         = 1;
    double              current_flow = 1.;    // the filament's flow ratio when the test was generated
    double              base         = 1.;    // percent tests: multiplier of the pass this one is centred on
    std::string         name;                 // shown in the dialogs
    std::vector<double> mods;                 // the samples' modifiers, ascending
    std::string         pattern;              // "archimedean" or "monotonic" (only for the tile drawing)
};

// The flow ratio the filament should get when sample `mod` of `test` is the best.
double final_ratio(const Test &test, double mod);

// "+5", "-0.01", "0"
std::string format_mod(double mod, bool linear);

// Pass 1 modifiers of Orca's flow test (-20 ... +20 step 5, the names of the plate's objects).
std::vector<double> orca_pass1_mods();

// Modifier written in a test object's name: "flowrate_m1.5" -> -1.5. ok=false when it cannot be read.
double modifier_from_object_name(const std::string &name, bool &ok);

// ---- the filament -----------------------------------------------------------------------------------------
// The edited filament preset's flow ratio (first slot). False with `err` (English) when there is no filament.
bool current_filament_flow(double &value, std::string &name, std::string &err);
// Writes `ratio` to the edited filament preset through the Tab (marks it modified, like typing in the field).
bool apply_filament_flow(double ratio, double &old_value, std::string &name, std::string &err);

// ---- what the last test was ---------------------------------------------------------------------------------
void        remember(const Test &test);
const Test &last();

// ---- dialogs -------------------------------------------------------------------------------------------------
enum class Action { Cancel, Generate, UseCurrent, Save };
struct Choice
{
    Action action = Action::Cancel;
    double mod    = 0.;
};

// First step of pass 2: shows the pass 1 samples as pictures; the user picks the best one and decides what to do
// (generate pass 2 around it, save it as the result, or skip and use the filament's current flow).
Choice pick_first_pass(wxWindow *parent, const Test &pass1);

// Asks which sample of `test` was the best, shows "old -> new" and writes the flow ratio to the filament; offers
// to save the preset. Returns true when the filament was changed.
bool save_result(wxWindow *parent, const Test &test);

// Menu: save the result of the last flow test of this session.
void save_last_result(wxWindow *parent);

// Confirm "old -> new", write it, offer to save the preset. Used by the dialogs above.
bool confirm_and_apply(wxWindow *parent, double ratio);

}}} // namespace Slic3r::GUI::FlowCalib

#endif
