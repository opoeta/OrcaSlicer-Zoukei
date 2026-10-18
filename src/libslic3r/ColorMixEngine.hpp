#ifndef SLIC3R_COLOR_MIX_ENGINE_HPP
#define SLIC3R_COLOR_MIX_ENGINE_HPP

// "Mistura de cores" engine: predicts the colour a mixed filament prints and picks, for a set of
// target colours, the physical filaments or mixes that reproduce them best.
//
// Ideas taken from (reimplemented, AGPL-3.0 sources credited in docs/zoukei/color-mixing.md):
//  - Bambu Studio 2.8 "Decompose Color": recipes of 2-3 physical filaments with integer ratios,
//    measured blend table (ColorDecomposeRecipe) - here an explicit optical model instead.
//  - FullSpectrum (ratdoux, Snapmaker Orca): a recipe is a repeating layer pattern (A-A-B ...),
//    "match" mode searching the pattern that best reproduces a target, semi-translucent filaments.
//  - Creality Print 7.3: sub-layer stacking and evenly spread (dithered) layer cadences.
//
// Pure standard C++ (no libslic3r dependencies) so it can be unit tested standalone.

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace ColorMix {

// sRGB, components in [0, 1].
struct RGB { double r{0}, g{0}, b{0}; };
// OKLab (L in [0,1]) or CIELab (L in [0,100]); which one is said by the function using it.
struct Lab { double L{0}, a{0}, b{0}; };

RGB    rgb_from_u8(int r, int g, int b);
void   rgb_to_u8(const RGB &c, int &r, int &g, int &b);
// "#RRGGBB" / "RRGGBB" / "#RRGGBBAA"; returns false on malformed input.
bool   rgb_from_hex(const std::string &hex, RGB &out);
std::string rgb_to_hex(const RGB &c);

double srgb_to_linear(double v);
double linear_to_srgb(double v);
Lab    oklab_from_rgb(const RGB &c);
RGB    rgb_from_oklab(const Lab &lab);
Lab    cielab_from_rgb(const RGB &c); // D65
// CIEDE2000 colour difference (Sharma, Wu, Dalal 2005), kL = kC = kH = 1.
double ciede2000(const Lab &lab1, const Lab &lab2);
double delta_e(const RGB &a, const RGB &b); // CIEDE2000 of two sRGB colours
// Euclidean OKLab distance scaled by 100 (comparable in magnitude to Delta E).
double delta_e_ok(const RGB &a, const RGB &b);

// How the components of a mix are laid out when printed. Every method here is realised by the
// existing mixed-filament slicing path (ToolOrdering::resolve_mixed_filaments):
//  - AlternatingLayers: whole layers in a repeating cadence (enable_mixed_color_sublayer off).
//  - SubLayers: each layer split into one thinner sub-layer per component, heights by ratio
//    (enable_mixed_color_sublayer on); finer mixing, one tool change per component per layer.
// In-layer line interleaving / 2-D dithering is not implemented by any of the reference slicers
// (Bambu, FullSpectrum, Creality Print 7.3); see docs/zoukei/color-mixing.md.
enum class Method {
    Auto,              // pick from the printer: see choose_method()
    AlternatingLayers,
    SubLayers
};

// Printer capabilities the method choice depends on.
struct PrinterKind {
    int  nozzles{1};               // physical extruders / tool heads (Snapmaker U1: 4)
    bool single_extruder_mm{true}; // one nozzle fed by AMS / CFS / IFS / MMU: every switch purges
};
// Toolchangers / multi-nozzle printers swap tools without purging, so sub-layers (more switches,
// finer mix) are cheap there; one nozzle with a feeder pays a purge per switch, so whole layers.
Method choose_method(const PrinterKind &printer);
const char *method_key(Method m); // "auto", "layers", "sublayers"
Method method_from_key(const std::string &key);

// Colour prediction model.
// Checked against the 486 measured blends of Bambu's standard_color_recipes.json (PLA Basic
// CMYW/RYBW, 2-3 components): mean CIEDE2000 13.4 for Pigment vs 24.3 Optical (opaque) and 16.6
// Optical (fully translucent) with the nominal filament colours; 6.0 vs 9.4-12.9 once each base
// colour is fitted to the data. Pigment is therefore the default.
enum class Model {
    Pigment, // degree-4 polynomial pigment mixer (Mixbox approximation used by Bambu/FullSpectrum/Orca)
    Optical  // stripes averaged in linear light + translucency filtering (see predict())
};
const char *model_key(Model m); // "optical", "pigment"
Model model_from_key(const std::string &key);

// A physical filament as seen by the colour model.
struct Filament {
    RGB         color;
    // HueForge-style transmission distance in mm (a layer this thick lets 10 % of what is behind it
    // through). <= 0: unknown, use `opacity` instead.
    double      td_mm{0.0};
    // Opacity of one stripe of the mix when td_mm is unknown, in [0, 1]. 1 = opaque.
    double      opacity{1.0};
    std::string name;
    // Filament type (PLA, PETG ...). The slicer refuses a mix whose components differ in type, so the
    // solver only pairs filaments of the same type; empty = unknown, pairs with anything.
    std::string type;
};

// Default opacity for a filament from its type/name ("translucent", "transparent", "clear",
// "natural", "silk" ...) and its alpha when the colour carries one.
double default_opacity(const std::string &type, const std::string &name, int alpha = 255);

// A recipe: physical filament indices (0-based into the palette) with integer weights. The
// weights also describe the repeating pattern: {2,1} prints A A B A A B ...
struct Recipe {
    std::vector<int> components;
    std::vector<int> weights;

    bool        is_physical() const { return components.size() == 1; }
    int         total() const;
    // Canonical form: components ascending, zero weights dropped, weights divided by their GCD.
    Recipe      normalized() const;
    bool        operator==(const Recipe &o) const;
    // Pattern letters, spread evenly ("AAB", "ABAC"): A = components[0], B = components[1] ...
    std::string pattern() const;
    // Ratios as percentages summing to 100 (largest-remainder rounding), as Orca stores them.
    std::vector<int> percentages() const;
};

// Optional measured blends: returns true and the measured colour when (colours, weights) has
// one (Orca's lookup_measured_blend_color() over standard_color_recipes.json).
using MeasuredLookup = std::function<bool(const std::vector<RGB> &colors, const std::vector<int> &weights, RGB &out)>;

struct ModelParams {
    Model          model{Model::Pigment};
    Method         method{Method::AlternatingLayers}; // Auto is treated as AlternatingLayers
    double         layer_height{0.2};                 // mm
    // Pigment: remove the regression's endpoint residuals (FullSpectrum 0.9.14). Off by default:
    // on Bambu's measured blends it scores mean dE00 13.8 against 13.4 without it.
    bool           anchor_endpoints{false};
    MeasuredLookup measured;
};

// Effective opacity of a stripe `thickness_mm` thick (TD when known, else Filament::opacity).
double stripe_opacity(const Filament &f, double thickness_mm);

// Predicted perceived colour of a recipe: the measured blend when ModelParams::measured knows it,
// else the chosen model. Pigment: pairwise polynomial accumulation like blend_color_multi(), each
// weight scaled by the stripe opacity (FullSpectrum's TD weighting). Optical: stripes are averaged by area in linear light
// (juxtaposition, exact for opaque stripes seen from viewing distance); a translucent stripe
// lets the mix behind it through and filters it with its own colour (subtractive part):
//   B         = sum_i w_i * lin(c_i)
//   visible_i = a_i * lin(c_i) + (1 - a_i) * lin(c_i) (x) B
//   mix       = sum_i w_i * visible_i
RGB predict(const std::vector<Filament> &palette, const Recipe &recipe, const ModelParams &p);

// Weighted k-means in OKLab (k-means++ seeding with a fixed seed, so results are deterministic).
// Returns, per input colour, the index of its cluster; `centers` and `center_weights` receive
// the cluster colours and summed weights. k is clamped to the number of distinct inputs.
std::vector<int> kmeans_oklab(const std::vector<RGB> &colors, const std::vector<double> &weights, int k,
                              std::vector<RGB> &centers, std::vector<double> &center_weights,
                              int iterations = 30);

struct SolverOptions {
    int    max_colors{8};        // k-means target count (0 = keep every input colour)
    int    max_mixes{4};         // new mixed filaments the solution may create
    int    max_components{2};    // 2 = pairs, 3 = also triples
    int    pattern_length{4};    // weights are steps of 1/pattern_length (4 -> 25 %, 8 -> 12.5 %)
    bool   only_physical{false}; // no mixes at all
    // A mix replaces the nearest physical filament only when it is at least this much closer,
    // and a 3-component mix replaces the best pair only when it is triple_gain closer.
    double min_gain{1.5};
    double triple_gain{0.5};
    ModelParams model;
    // Mixes that already exist in the project: reusing them costs no new slot.
    std::vector<Recipe> existing_mixes;
};

struct Candidate {
    Recipe recipe;
    RGB    predicted;
    Lab    lab; // CIELab of predicted
    bool   existing{false};
};

struct Assignment {
    RGB    target;
    double weight{1.0};
    int    candidate{-1}; // index into Solution::candidates
    double delta_e{0.0};
};

struct Solution {
    std::vector<Candidate>  candidates;     // every recipe considered
    std::vector<Assignment> clusters;       // one per target cluster
    std::vector<int>        source_cluster; // per input colour: its cluster
    std::vector<int>        new_mixes;      // candidate indices of mixes to create
    double mean_delta_e{0}; // weighted by cluster weight
    double max_delta_e{0};
    double physical_mean_delta_e{0}; // same targets, physical filaments only (baseline)
};

// Every recipe the options allow (physical singles first), with predictions.
std::vector<Candidate> enumerate_candidates(const std::vector<Filament> &palette, const SolverOptions &opt);

// Clusters `targets` (weighted) to opt.max_colors colours and chooses a recipe for each.
Solution solve(const std::vector<RGB> &targets, const std::vector<double> &weights,
               const std::vector<Filament> &palette, const SolverOptions &opt);

// Re-evaluates a solution after the user changed the candidate of some clusters: refreshes
// delta_e, the mix list and the statistics.
void refresh(Solution &sol, const SolverOptions &opt);

} // namespace ColorMix
} // namespace Slic3r

#endif // SLIC3R_COLOR_MIX_ENGINE_HPP
