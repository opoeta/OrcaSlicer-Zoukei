// Layer Slayer calibrations, native (stage 2 of the plugin port).
//
// Each test of the Layer Slayer plugin (D:/Orca_Plugins/orca-calibration) is built here as ModelObjects: the
// plugin's meshes and stroke-font labels, the settings it wrote per object / per part, the plate layout, and what
// the plugin did in G-code is done with the slicer's own means:
//   - per object: the PrintObjectCalibConfig overrides (object_z_offset, object_initial_layer_temperature,
//     object_initial_layer_speed, object_retraction_length / _speed) and ordinary object / part settings;
//   - per height: height range modifiers (VFA, max volumetric speed) and per-layer custom G-code (temperature,
//     pressure advance, fan power, pauses).
// No G-code post-processing and no name tags.
#ifndef slic3r_GUI_Zoukei_LayerSlayer_hpp_
#define slic3r_GUI_Zoukei_LayerSlayer_hpp_

#include "libslic3r/TriangleMesh.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {
class Plater;
namespace LayerSlayer {

enum class Kind { Float, Int, Bool, Select, Floats };

struct Option
{
    std::string value;
    const char *label; // English, translated with _()
};

struct Param
{
    std::string         key;
    Kind                kind = Kind::Float;
    double              def  = 0.;
    std::vector<double> def_list;
    std::string         def_str;
    double              min  = -1e30;
    double              max  = 1e30;
    double              step = 0.;
    std::string         unit;
    std::vector<Option> options;
};

struct Calib
{
    std::string        id;
    std::string        group;
    std::vector<Param> params;
};

struct Values
{
    std::map<std::string, double>              num;
    std::map<std::string, std::vector<double>> list;
    std::map<std::string, std::string>         str;

    double                     n(const std::string &key) const;
    const std::vector<double> &l(const std::string &key) const;
    const std::string         &s(const std::string &key) const;
};

// The 22 tests, in the plugin's order (grouped, and inside a group in the order they are best run).
const std::vector<Calib> &all();
const Calib              *find(const std::string &id);
const std::vector<std::string> &groups();

// English texts (pass through _() / _u8L to show them).
const char *name(const std::string &id);
const char *description(const std::string &id);
const char *how_to_read(const std::string &id);
const char *group_name(const std::string &group);
const char *param_label(const std::string &key);
// Image file under resources/images (without extension), e.g. "calib_z_offset".
std::string icon_name(const std::string &id);

// The pad modifiers (%) of a flow_rate test with these values (the chosen pass decides start / step / count).
std::vector<double> flow_rate_modifiers(const Values &values);

Values defaults(const Calib &calib);
// Parses `raw` (what the user typed, or a JSON value turned into text) into `values`.
// Returns an empty string when it is valid, else an English error.
std::string parse(const Param &param, const std::string &raw, Values &values);
std::string format_value(const Param &param, const Values &values);

// What the build needs to know about the printer and the profiles.
struct Env
{
    double      bed_x0 = 0., bed_y0 = 0., bed_w = 256., bed_d = 256.;
    double      layer_height = 0.2, first_layer = 0.2, nozzle = 0.4;
    std::string flavor;           // gcode_flavor, serialized
    double      flow_ratio  = 1.; // filament_flow_ratio of the first filament
    double      line_width  = 0.; // outer wall line width in mm (resolved)
    double      max_volumetric = 0.;
    int         filaments   = 1;
    std::string printer;
    double cx() const { return bed_x0 + bed_w / 2.; }
    double cy() const { return bed_y0 + bed_d / 2.; }
};
Env read_env(Plater *plater);

using Settings = std::vector<std::pair<std::string, std::string>>;

struct Part
{
    std::string           name;
    indexed_triangle_set  mesh;
    Settings              settings;
};

struct Range
{
    double   lo = 0., hi = 0.;
    Settings settings;
};

struct Obj
{
    std::string        name;
    std::vector<Part>  parts;
    Settings           settings;
    Settings           note;   // shown in the legend only
    std::vector<Range> ranges; // height range modifiers
    double             x = 0., y = 0.;
    // footprint after normalize(): width (x) and depth (y), height
    double w = 0., d = 0., h = 0.;
};

enum class CodeType { Custom, Pause, ColorChange };

struct LayerCode
{
    double      z = 0.;
    CodeType    type = CodeType::Custom;
    std::string text;      // custom G-code, or the pause message
    int         extruder = 1;
};

struct LegendRow
{
    std::string name;
    std::string text;
};

struct Plate
{
    std::string            title;
    std::vector<Obj>       objects;
    std::vector<LayerCode> gcodes;
    Settings               print_edits, filament_edits, printer_edits;
    bool                   by_object = false;
    std::vector<LegendRow> extra_legend;
    std::vector<std::string> warnings; // English

    std::vector<LegendRow> legend() const;
};

struct Error : std::runtime_error
{
    explicit Error(const std::string &what) : std::runtime_error(what) {}
};

// Builds the plate (no GUI involved). Throws Error with an English message when the values cannot be built.
Plate build(const Calib &calib, const Values &values, const Env &env);

// Replaces the current project with the calibration plate. skip_confirm: do not ask about unsaved changes (the
// caller has handled them). Returns false with `error` (translated) when nothing was created; `out` receives
// the plate that was loaded.
bool run(Plater *plater, const std::string &id, const Values &values, bool skip_confirm, std::string &error,
         Plate *out = nullptr);

}}} // namespace Slic3r::GUI::LayerSlayer

#endif
