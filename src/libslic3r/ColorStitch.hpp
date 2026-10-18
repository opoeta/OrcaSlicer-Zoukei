#ifndef slic3r_ColorStitch_hpp_
#define slic3r_ColorStitch_hpp_

// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: Surface ColorStitch, PathBlend and the Sandwich
// pass stack (ColorStitch.hpp / ColorStitch.cpp in NeotkoCM).
//
// What this port keeps, and how it maps onto the original:
//  * The Sandwich: a top surface becomes a stack of up to 3 passes, each printed as its own thin
//    sub-layer inside the layer height (SurfacePass / SurfacePassStack, same JSON as NeotkoCM).
//  * ColorStitch: the filament is chosen per fill line. The 2.4.6 "field" model is ported: the
//    design lives in millimetres on the surface and every line asks which colour the spot where
//    it lands has, so holes, line width and line count do not move it. Bresenham dither (2 and 3
//    colours, with easing), stripes in mm, custom digit patterns and textile weaves.
//  * PathBlend: the per-scanline staircase. One filament climbs as a real Z ramp inside the layer
//    (one height per fill line) and an optional second filament caps the rest.
// What is left out (it depends on NeotkoCM-only infrastructure): the painter / sticker / MMU
// coexistence paths, MixedFilament virtual digits 5-9, Neoweaving and the legacy count-based line
// distribution modes. The penultimate and bottom zones are ported in a reduced form: each zone has its
// own stack (neotko_surface_passes_penu / _bottom, the penu_zone_* / bottom_zone_* quick modes, or the
// painted recipe), is printed by the same engine as the top (FillSandwich.cpp) and goes through the
// same scheduler, tower plan and G-code emission as the top sub-layers.

#include "libslic3r.h"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "SurfaceEffectProfile.hpp"
#include "SurfacePassKind.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class PrintObjectConfig;
class DynamicPrintConfig;

// Default fixed angle of a ColorStitch / PathBlend pass. A fixed angle keeps the stripes and the
// gradient axis identical on every layer; -1 (auto) follows the per-layer rotation.
static constexpr int COLORSTITCH_DEFAULT_ANGLE_DEG = 45;

// interlayer_colormix_mode values. 0..4 match NeotkoCM; 5 (weave) is ours: NeotkoCM compiles a
// weave into a mode 0 pattern string in its dialog, here the slicer expands it at slice time.
enum ColorStitchMode : int {
    csmPattern    = 0, // custom digit pattern: '1'..'9' = filament 1..9, one digit per line
    csmBlend2     = 1, // smooth blend of tools A and B (Bresenham dither)
    csmBlend3     = 2, // smooth blend of tools A, B and C
    csmBandsLines = 3, // legacy: stripes counted in lines (converted to mm with the real spacing)
    csmBandsMM    = 4, // stripes in millimetres
    csmWeave      = 5, // textile weave of tools A and B
};

// interlayer_colormix_weave values (WeaveLibrary in NeotkoCM).
enum ColorStitchWeave : int {
    cswPlain   = 0, // 1212
    cswTwill22 = 1, // 1122, shifted one line per layer (real diagonal)
    cswTwill31 = 2, // 1112, shifted one line per layer
    cswSatin5  = 3, // 11112
};

// Parameters of one ColorStitch pass (the interlayer_colormix_* keys of the object, optionally
// overridden per pass by the stack JSON).
struct ColorStitchParams
{
    int         mode          = csmBlend2;
    std::string pattern       = "12";
    // 0-based filaments; -1 = off (C and D).
    int         tool[4]       = { 0, 1, -1, -1 };
    int         pct_a         = 50;
    int         pct_b         = 33;
    int         easing        = 0;
    double      gamma         = 1.0;
    double      overlap       = 0.6;
    bool        invert        = false;
    int         band_count[4] = { 0, 0, 0, 0 };  // legacy mode 3, read only from NeotkoCM stacks
    double      band_mm[4]    = { 8.0, 8.0, 0.0, 0.0 };
    double      grad_span_mm  = 0.0;              // 0 = fit to surface, > 0 = repeat every N mm
    int         repetitions   = 1;
    int         angle         = COLORSTITCH_DEFAULT_ANGLE_DEG;
    double      min_length_mm = 0.5;
    int         min_lines     = 3;
    int         weave         = cswPlain;

    static ColorStitchParams from_config(const PrintObjectConfig &cfg);
    // Overlays serialized interlayer_colormix_* values (a pass payload of a stack JSON). Unknown
    // keys and values that do not parse are ignored.
    void apply_kv(const std::map<std::string, std::string> &kv);
};

class ColorStitch
{
public:
    enum ColormixEasing : int {
        kColormixEasing_Linear    = 0,
        kColormixEasing_EaseIn    = 1,
        kColormixEasing_EaseOut   = 2,
        kColormixEasing_EaseInOut = 3,
        kColormixEasing_Gamma     = 4,
        kColormixEasing_HardBand  = 5,
    };

    // Easing applied to the position t in [0,1] before the dither decision.
    static double colorstitch_easing_apply(double t, int easing, double gamma = 1.0);

    // Bresenham dither, 2 colours: pct_a percent of the entries get tool_a, the rest tool_b,
    // distributed so that every window of the sequence holds the target ratio.
    static std::vector<int> build_dithered_tools_2color(int n_lines, int tool_a, int tool_b, int pct_a,
                                                        int easing = kColormixEasing_Linear, double gamma = 1.0);
    // Same, evaluated at the real position of each entry (t_asc ascending in [0,1]). With
    // t_asc = {0, 1/(n-1), ..., 1} it returns exactly what build_dithered_tools_2color returns.
    static std::vector<int> build_dithered_tools_2color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int pct_a,
                                                           int easing = kColormixEasing_Linear, double gamma = 1.0);
    // 3 colours: A at t=0, B in the middle, C at t=1. `overlap` (0..1) is how much each colour
    // bleeds into its neighbour's zone; the global ratios stay exact.
    static std::vector<int> build_dithered_tools_3color(int n_lines, int tool_a, int tool_b, int tool_c, int pct_a, int pct_b,
                                                        int easing = kColormixEasing_Linear, double gamma = 1.0, double overlap = 0.6);
    static std::vector<int> build_dithered_tools_3color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int tool_c,
                                                           int pct_a, int pct_b, int easing = kColormixEasing_Linear,
                                                           double gamma = 1.0, double overlap = 0.6);
    // Hard bands cycling through up to 4 (tool, count) pairs. Zero counts and tools < 0 are skipped.
    static std::vector<int> build_custom_bands(int n_lines, int tool_a, int cnt_a, int tool_b, int cnt_b,
                                               int tool_c, int cnt_c, int tool_d, int cnt_d);
    // Rough line count of a surface, for UI feedback.
    static int estimate_surface_line_count(double area_mm2, double line_width_mm, double overlap_fraction = 0.0,
                                           double pattern_factor = 1.0);

    // Textile weaves: digit pattern ('1' = tool A, '2' = tool B) and line shift per layer.
    static std::string weave_pattern(int weave);
    static int         weave_offset_per_layer(int weave);

    // THE engine. Splits one lamina of fill lines into one collection per filament, following `p`.
    //   layer_height : layer height, used to derive the real line spacing (lane size)
    //   layer_id     : layer index (twill weaves shift one line per layer)
    //   anchor_*_mm  : origin of the design (the object origin), so the pattern travels with the part
    //   fallback_tool: filament for lines ColorStitch cannot place (too few, too short)
    // Buckets come back in recipe order: the first colour of the recipe is printed first.
    static std::vector<std::pair<int, ExtrusionEntityCollection>> split_by_tool(const ExtrusionEntityCollection &fills,
                                                                                const ColorStitchParams         &p,
                                                                                double                           layer_height,
                                                                                size_t                           layer_id,
                                                                                double                           anchor_x_mm,
                                                                                double                           anchor_y_mm,
                                                                                int                              fallback_tool);
};

// ---------------------------------------------------------------------------------------------
// PathBlend pass configuration (the v2 "geometry" model of NeotkoCM).
//   floor_mm   : height of the ramp at t=0 (low end), >= 0.01 mm
//   mid_end_mm : height of the ramp at t=1; < 0 means auto (tallest legal ramp)
//   Full       : a flat cap in tool_top fills [ramp, layer top]
//   Half       : no cap, the area above the ramp stays empty
struct PathBlendPassConfig
{
    enum class Mode : int { Half = 0, Full = 1 };

    bool  enabled     = false;
    Mode  mode        = Mode::Full;
    float floor_mm    = 0.01f;
    float mid_end_mm  = -1.0f;
    int   tool_bottom = 0;
    int   tool_top    = 1;
    int   ease_mode   = 0;
    int   fill_angle  = COLORSTITCH_DEFAULT_ANGLE_DEG;
    // 0 = the ramp spans the surface (by lanes), > 0 = physical period in mm (no UI yet).
    float span_mm     = 0.0f;
    // Start / end zone: the ramp stays at floor until in_t and at mid_end from out_t on.
    float in_t        = 0.0f;
    float out_t       = 1.0f;

    // Legacy view derived from mode / tools (num_passes 1 = Half, 2 = Full).
    int   num_passes  = 2;
    int   tool[4]     = { 0, 1, -1, -1 };

    double profile_u(double t) const;
    void   sync_legacy_view();
    void   apply_constraints(double layer_height_mm);

    // pathblend_top blob when set, else the flat pathblend_* keys.
    static PathBlendPassConfig from_config(const PrintObjectConfig &cfg);
    std::string                to_blob_json() const;
    static PathBlendPassConfig from_blob_json(const std::string &blob);
};

// Physical floor (mm) of every bead a PathBlend pass emits. NeotkoCM measured 0.035 mm as the
// margin over the knee where thin beads start to string. ORCA_PB_MIN_H overrides it at runtime.
inline double pb_min_printable_h()
{
    static const double v = [] {
        if (const char *e = std::getenv("ORCA_PB_MIN_H")) {
            try {
                const double d = std::stod(e);
                if (d >= 0.0 && d < 0.5)
                    return d;
            } catch (...) {}
        }
        return 0.035;
    }();
    return v;
}

// One band of a PathBlend pass (ramp or cap).
struct PBBand
{
    bool  is_cap = false;
    float t_lo   = 0.f;
    float t_hi   = 0.f;
    float t_mid  = 0.f;
    float z_top  = 0.f;
    float h_step = 0.f;
};

// Ordered bands of one PathBlend pass. Empty means the configuration is degenerate (ramp end at
// or below the floor): the caller then prints the pass as a flat solid band.
std::vector<PBBand> compute_pb_bands(const PathBlendPassConfig &pb, double bottom_z, double H, double min_band_h, bool want_cap,
                                     int target_k = 0, double min_printable_h = 0.0);

// ---------------------------------------------------------------------------------------------
// The Sandwich pass stack. Storage: neotko_surface_passes_top (JSON, same schema as NeotkoCM) or,
// when empty, the discrete multipass_* / interlayer_colormix_* / pathblend_* keys.
struct SurfacePass
{
    SurfacePassKind kind  = SurfacePassKind::Solid;
    double          ratio = 0.0;       // share of the layer height; the stack is normalized to 1

    int             solid_tool  = 0;   // 0-based filament (Solid passes); < 0 = the natural filament
    int             angle       = -1;  // -1 = auto, 0..359 fixed
    int             fan         = -1;  // kept for JSON round trip (not applied by this port)
    int             speed_pct   = 100; // kept for JSON round trip (not applied by this port)
    std::string     gcode_start;       // kept for JSON round trip (not applied by this port)
    std::string     gcode_end;

    // colorstitch.kv : interlayer_colormix_* overrides of this pass (empty = object values)
    // pathblend.kv   : "blob" = PathBlendPassConfig::to_blob_json()
    SurfaceEffectPayload colorstitch;
    SurfaceEffectPayload pathblend;
};

struct SurfacePassStack
{
    static constexpr int kMaxPasses = 3;

    bool                     enabled                  = false;
    bool                     perimeter_override       = false; // round trip only
    bool                     bottom_supported_control = false; // round trip only
    std::vector<SurfacePass> passes;                           // bottom -> top

    bool empty() const { return passes.empty(); }
    bool all_solid() const;
    // True when any pass does something (kind != None).
    bool any_effect() const;

    std::string             to_json() const;
    static SurfacePassStack from_json(const std::string &text);

    // Builds the stack from the object's discrete keys (PrintObjectConfig: the keys are per object,
    // they do not fit in PrintRegionConfig, which is at the Boost.PP sequence limit):
    //   multipass_enabled         -> multipass_num_passes passes (kind / tool / ratio / angle)
    //   interlayer_colormix_enabled -> one full height ColorStitch pass
    //   multipass_path_gradient   -> one full height PathBlend pass
    static SurfacePassStack synthesize_from_config(const PrintObjectConfig &cfg);
    // neotko_surface_passes_top when set, else synthesize_from_config(). The single entry point
    // used by the fill engine.
    static SurfacePassStack resolve(const PrintObjectConfig &cfg);
    // Same for a zone. Top = resolve(). Penultimate / Bottom: the zone JSON stack when set, else the
    // quick mode (penu_zone_mode / bottom_zone_mode: one Solid pass, one ColorStitch pass with the
    // object's ColorStitch settings, or a copy of the Top stack), else empty.
    static SurfacePassStack resolve_zone(const PrintObjectConfig &cfg, SurfaceZone zone);
    // Cheap gate of a zone: false for every object with default settings.
    static bool config_has_zone(const PrintObjectConfig &cfg, SurfaceZone zone);
    // Cheap test used to gate everything else: false for every object with default settings.
    static bool config_has_sandwich(const PrintObjectConfig &cfg);
};

} // namespace Slic3r

#endif // slic3r_ColorStitch_hpp_
