// Ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0: Surface ColorStitch, PathBlend and the Sandwich
// pass stack. See ColorStitch.hpp for what is ported and what is left out.
#include "ColorStitch.hpp"

#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Polyline.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <set>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

// =============================================================================================
// ColorStitchParams
// =============================================================================================

static int colorstitch_engine_mode(ColorStitchPatternMode m)
{
    switch (m) {
    case ColorStitchPatternMode::Pattern:   return csmPattern;
    case ColorStitchPatternMode::Blend2:    return csmBlend2;
    case ColorStitchPatternMode::Blend3:    return csmBlend3;
    case ColorStitchPatternMode::StripesMM: return csmBandsMM;
    case ColorStitchPatternMode::Weave:     return csmWeave;
    default:                                return csmBlend2;
    }
}

ColorStitchParams ColorStitchParams::from_config(const PrintObjectConfig &cfg)
{
    ColorStitchParams p;
    p.mode          = colorstitch_engine_mode(cfg.interlayer_colormix_mode.value);
    p.pattern       = cfg.interlayer_colormix_pattern_top.value;
    p.weave         = int(cfg.interlayer_colormix_weave.value);
    // Config filaments are 1-based with 0 = off; the engine works with 0-based tools, -1 = off.
    p.tool[0]       = cfg.interlayer_colormix_filament_a.value - 1;
    p.tool[1]       = cfg.interlayer_colormix_filament_b.value - 1;
    p.tool[2]       = cfg.interlayer_colormix_filament_c.value - 1;
    p.tool[3]       = cfg.interlayer_colormix_filament_d.value - 1;
    p.pct_a         = cfg.interlayer_colormix_pct_a.value;
    p.pct_b         = cfg.interlayer_colormix_pct_b.value;
    p.easing        = int(cfg.interlayer_colormix_easing.value);
    p.gamma         = cfg.interlayer_colormix_gamma.value;
    p.overlap       = cfg.interlayer_colormix_overlap.value;
    p.invert        = cfg.interlayer_colormix_invert.value;
    p.band_mm[0]    = cfg.interlayer_colormix_band_mm_a.value;
    p.band_mm[1]    = cfg.interlayer_colormix_band_mm_b.value;
    p.band_mm[2]    = cfg.interlayer_colormix_band_mm_c.value;
    p.band_mm[3]    = cfg.interlayer_colormix_band_mm_d.value;
    p.grad_span_mm  = std::max(0.0, double(cfg.interlayer_colormix_gradient_span_mm.value));
    p.repetitions   = std::max(1, cfg.interlayer_colormix_repetitions.value);
    p.angle         = cfg.interlayer_colormix_angle.value;
    p.min_length_mm = cfg.interlayer_colormix_min_length.value;
    p.min_lines     = cfg.interlayer_colormix_min_surface_lines.value;
    return p;
}

void ColorStitchParams::apply_kv(const std::map<std::string, std::string> &kv)
{
    static const std::string prefix = "interlayer_colormix_";
    auto find = [&](const char *base) -> const std::string * {
        auto it = kv.find(prefix + base);
        return it == kv.end() ? nullptr : &it->second;
    };
    auto get_int = [&](const char *base, int &dst) {
        if (const std::string *s = find(base)) {
            try { dst = std::stoi(*s); } catch (...) {}
        }
    };
    auto get_dbl = [&](const char *base, double &dst) {
        if (const std::string *s = find(base)) {
            try { dst = std::stod(*s); } catch (...) {}
        }
    };
    auto get_bool = [&](const char *base, bool &dst) {
        if (const std::string *s = find(base))
            dst = (*s == "1" || *s == "true");
    };

    // Pattern mode: NeotkoCM stores the number (0..4), this port's config stores a name.
    if (const std::string *s = find("mode")) {
        static const std::map<std::string, int> names = {
            { "pattern", csmPattern }, { "blend2", csmBlend2 }, { "blend3", csmBlend3 },
            { "stripes", csmBandsMM }, { "weave", csmWeave } };
        auto it = names.find(*s);
        if (it != names.end())
            mode = it->second;
        else {
            try { mode = std::clamp(std::stoi(*s), 0, 5); } catch (...) {}
        }
    }
    if (const std::string *s = find("weave")) {
        static const std::map<std::string, int> names = {
            { "plain", cswPlain }, { "twill22", cswTwill22 }, { "twill31", cswTwill31 }, { "satin5", cswSatin5 } };
        auto it = names.find(*s);
        if (it != names.end())
            weave = it->second;
        else {
            try { weave = std::clamp(std::stoi(*s), 0, 3); } catch (...) {}
        }
    }
    if (const std::string *s = find("easing")) {
        static const std::map<std::string, int> names = {
            { "linear", 0 }, { "ease_in", 1 }, { "ease_out", 2 }, { "s_curve", 3 }, { "gamma", 4 }, { "hard_step", 5 } };
        auto it = names.find(*s);
        if (it != names.end())
            easing = it->second;
        else {
            try { easing = std::clamp(std::stoi(*s), 0, 5); } catch (...) {}
        }
    }
    if (const std::string *s = find("pattern_top"))
        pattern = *s;
    get_int("tool_a", tool[0]);
    get_int("tool_b", tool[1]);
    get_int("tool_c", tool[2]);
    get_int("tool_d", tool[3]);
    // This port's own keys (1-based filaments, 0 = off) when the payload was snapshot from our config.
    {
        static const char *names[4] = { "filament_a", "filament_b", "filament_c", "filament_d" };
        for (int k = 0; k < 4; ++k) {
            int v = -1000;
            get_int(names[k], v);
            if (v != -1000)
                tool[k] = v - 1;
        }
    }
    get_int("pct_a", pct_a);
    get_int("pct_b", pct_b);
    get_dbl("gamma", gamma);
    get_dbl("overlap", overlap);
    get_bool("invert", invert);
    get_int("band_count_a", band_count[0]);
    get_int("band_count_b", band_count[1]);
    get_int("band_count_c", band_count[2]);
    get_int("band_count_d", band_count[3]);
    get_dbl("band_mm_a", band_mm[0]);
    get_dbl("band_mm_b", band_mm[1]);
    get_dbl("band_mm_c", band_mm[2]);
    get_dbl("band_mm_d", band_mm[3]);
    get_dbl("gradient_span_mm", grad_span_mm);
    grad_span_mm = std::max(0.0, grad_span_mm); // NeotkoCM 2.4.6: the legacy (-1) route is retired
    get_int("repetitions", repetitions);
    repetitions = std::max(1, repetitions);
    get_int("angle", angle);
    get_dbl("min_length", min_length_mm);
    get_int("min_surface_lines", min_lines);
}

// =============================================================================================
// Sequence builders (verbatim algorithms from NeotkoCM)
// =============================================================================================

double ColorStitch::colorstitch_easing_apply(double t, int easing, double gamma)
{
    t = std::clamp(t, 0.0, 1.0);
    switch (easing) {
    case kColormixEasing_EaseIn:    return t * t;
    case kColormixEasing_EaseOut:   return 1.0 - (1.0 - t) * (1.0 - t);
    case kColormixEasing_EaseInOut: return t * t * (3.0 - 2.0 * t); // smoothstep
    case kColormixEasing_Gamma:     return std::pow(t, std::clamp(gamma, 0.1, 10.0));
    case kColormixEasing_HardBand:  return (t < 0.5) ? 0.0 : 1.0;
    case kColormixEasing_Linear:
    default:                        return t;
    }
}

std::vector<int> ColorStitch::build_dithered_tools_2color(int n_lines, int tool_a, int tool_b, int pct_a, int easing, double gamma)
{
    if (n_lines <= 0)
        return {};
    std::vector<double> t_lin(static_cast<size_t>(n_lines));
    const double denom = std::max(1.0, static_cast<double>(n_lines - 1));
    for (int i = 0; i < n_lines; ++i)
        t_lin[i] = (n_lines == 1) ? 0.5 : (static_cast<double>(i) / denom);
    return build_dithered_tools_2color_at(t_lin, tool_a, tool_b, pct_a, easing, gamma);
}

std::vector<int> ColorStitch::build_dithered_tools_2color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int pct_a,
                                                             int easing, double gamma)
{
    std::vector<int> out;
    const int n_lines = static_cast<int>(t_asc.size());
    if (n_lines <= 0)
        return out;
    out.reserve(static_cast<size_t>(n_lines));

    const int p_a = std::clamp(pct_a, 0, 100);
    const int p_b = 100 - p_a;
    if (p_b == 0) { out.assign(static_cast<size_t>(n_lines), tool_a); return out; }
    if (p_a == 0) { out.assign(static_cast<size_t>(n_lines), tool_b); return out; }

    // Total B emissions across the whole sequence; walk it comparing emitted_b with the target
    // curve evaluated at the real position of each entry (easing reshapes the curve).
    const int total_b   = static_cast<int>(std::lround(static_cast<double>(n_lines) * static_cast<double>(p_b) / 100.0));
    int       emitted_b = 0;
    for (int i = 0; i < n_lines; ++i) {
        const double t_eff  = colorstitch_easing_apply(std::clamp(t_asc[static_cast<size_t>(i)], 0.0, 1.0), easing, gamma);
        const int    need_b = static_cast<int>(std::lround(t_eff * static_cast<double>(total_b)));
        if (emitted_b < need_b && emitted_b < total_b) {
            out.push_back(tool_b);
            ++emitted_b;
        } else
            out.push_back(tool_a);
    }
    return out;
}

std::vector<int> ColorStitch::build_dithered_tools_3color(int n_lines, int tool_a, int tool_b, int tool_c, int pct_a, int pct_b,
                                                          int easing, double gamma, double overlap)
{
    if (n_lines <= 0)
        return {};
    std::vector<double> t_lin(static_cast<size_t>(n_lines));
    const double d = std::max(1.0, static_cast<double>(n_lines - 1));
    for (int i = 0; i < n_lines; ++i)
        t_lin[i] = (n_lines == 1) ? 0.5 : (static_cast<double>(i) / d);
    return build_dithered_tools_3color_at(t_lin, tool_a, tool_b, tool_c, pct_a, pct_b, easing, gamma, overlap);
}

std::vector<int> ColorStitch::build_dithered_tools_3color_at(const std::vector<double> &t_asc, int tool_a, int tool_b, int tool_c,
                                                             int pct_a, int pct_b, int easing, double gamma, double overlap)
{
    std::vector<int> out;
    const int n_lines = static_cast<int>(t_asc.size());
    if (n_lines <= 0)
        return out;
    out.reserve(static_cast<size_t>(n_lines));

    const int p_a = std::clamp(pct_a, 0, 100);
    const int p_b = std::clamp(pct_b, 0, 100 - p_a);

    const double n              = static_cast<double>(n_lines);
    const int    target_a_total = static_cast<int>(std::lround(n * p_a / 100.0));
    const int    target_b_total = static_cast<int>(std::lround(n * p_b / 100.0));
    const int    target_c_total = n_lines - target_a_total - target_b_total;

    // Hat weights anchored at t=0 (A), 0.5 (B), 1 (C); overlap widens them. The Bresenham deficit
    // below keeps the global ratios exact, only the local distribution changes with overlap.
    const double halfwidth = 0.5 + std::clamp(overlap, 0.0, 1.0) * 0.5;

    std::vector<double> cum_a(n_lines), cum_b(n_lines), cum_c(n_lines);
    double sum_a = 0.0, sum_b = 0.0, sum_c = 0.0;
    for (int i = 0; i < n_lines; ++i) {
        const double t_eff = colorstitch_easing_apply(std::clamp(t_asc[static_cast<size_t>(i)], 0.0, 1.0), easing, gamma);
        auto hat = [&](double anchor) -> double { return std::max(0.0, halfwidth - std::abs(t_eff - anchor)) / halfwidth; };
        sum_a += hat(0.0);
        sum_b += hat(0.5);
        sum_c += hat(1.0);
        cum_a[i] = sum_a;
        cum_b[i] = sum_b;
        cum_c[i] = sum_c;
    }
    const double area_a = std::max(1e-9, sum_a);
    const double area_b = std::max(1e-9, sum_b);
    const double area_c = std::max(1e-9, sum_c);

    int emitted_a = 0, emitted_b = 0, emitted_c = 0;
    for (int i = 0; i < n_lines; ++i) {
        const double exp_a = (cum_a[i] / area_a) * static_cast<double>(target_a_total);
        const double exp_b = (cum_b[i] / area_b) * static_cast<double>(target_b_total);
        const double exp_c = (cum_c[i] / area_c) * static_cast<double>(target_c_total);
        const double def_a = (emitted_a < target_a_total) ? (exp_a - emitted_a) : -1e9;
        const double def_b = (emitted_b < target_b_total) ? (exp_b - emitted_b) : -1e9;
        const double def_c = (emitted_c < target_c_total) ? (exp_c - emitted_c) : -1e9;
        if (def_a >= def_b && def_a >= def_c) { out.push_back(tool_a); ++emitted_a; }
        else if (def_b >= def_c)              { out.push_back(tool_b); ++emitted_b; }
        else                                  { out.push_back(tool_c); ++emitted_c; }
    }
    return out;
}

std::vector<int> ColorStitch::build_custom_bands(int n_lines, int tool_a, int cnt_a, int tool_b, int cnt_b, int tool_c, int cnt_c,
                                                 int tool_d, int cnt_d)
{
    std::vector<int> out;
    if (n_lines <= 0)
        return out;
    out.reserve(static_cast<size_t>(n_lines));

    struct Band { int tool; int count; };
    std::vector<Band> bands;
    if (cnt_a > 0 && tool_a >= 0) bands.push_back({ tool_a, cnt_a });
    if (cnt_b > 0 && tool_b >= 0) bands.push_back({ tool_b, cnt_b });
    if (cnt_c > 0 && tool_c >= 0) bands.push_back({ tool_c, cnt_c });
    if (cnt_d > 0 && tool_d >= 0) bands.push_back({ tool_d, cnt_d });
    if (bands.empty()) {
        out.assign(static_cast<size_t>(n_lines), tool_a >= 0 ? tool_a : 0);
        return out;
    }
    int produced = 0;
    while (produced < n_lines) {
        for (const Band &b : bands) {
            for (int j = 0; j < b.count && produced < n_lines; ++j, ++produced)
                out.push_back(b.tool);
            if (produced >= n_lines)
                break;
        }
    }
    return out;
}

int ColorStitch::estimate_surface_line_count(double area_mm2, double line_width_mm, double overlap_fraction, double pattern_factor)
{
    if (area_mm2 <= 0.0 || line_width_mm <= 0.0)
        return 0;
    const double spacing = line_width_mm * (1.0 - std::clamp(overlap_fraction, 0.0, 0.99));
    if (spacing <= 1e-6)
        return 0;
    const double n = std::sqrt(std::max(0.0, area_mm2)) * std::sqrt(2.0) / spacing * std::clamp(pattern_factor, 0.5, 1.5);
    if (n > 1e6)
        return 1000000;
    return static_cast<int>(std::lround(std::max(0.0, n)));
}

std::string ColorStitch::weave_pattern(int weave)
{
    switch (weave) {
    case cswTwill22: return "1122";
    case cswTwill31: return "1112";
    case cswSatin5:  return "11112";
    case cswPlain:
    default:         return "12";
    }
}

int ColorStitch::weave_offset_per_layer(int weave)
{
    // NeotkoCM lists the twill shift (WeaveLibrary offset_per_layer) but never applied it; here the
    // lane index is shifted by layer_id * offset, which is what turns 1122 into a real diagonal.
    return (weave == cswTwill22 || weave == cswTwill31) ? 1 : 0;
}

// =============================================================================================
// The field engine (NeotkoCM 2.4.6): every line asks which colour its spot on the surface has.
// =============================================================================================

namespace {

struct LaneVec2 { double x = 0.0, y = 0.0; };

// One colourable fill line.
struct RawLine
{
    Polyline3            pl;
    const ExtrusionPath *src   = nullptr; // attributes (role, flow, width, height...)
    bool                 whole = false;   // the line is the whole source path (clone it as is)
    bool                 short_line = false;
};

LaneVec2 line_centroid(const Polyline3 &pl)
{
    LaneVec2 c;
    if (pl.points.empty())
        return c;
    for (const Point3 &pt : pl.points) {
        c.x += static_cast<double>(pt.x());
        c.y += static_cast<double>(pt.y());
    }
    c.x /= static_cast<double>(pl.points.size());
    c.y /= static_cast<double>(pl.points.size());
    return c;
}

double polyline3_length_mm(const Polyline3 &pl)
{
    double len = 0.0;
    for (size_t i = 1; i < pl.points.size(); ++i) {
        const double dx = double(pl.points[i].x() - pl.points[i - 1].x());
        const double dy = double(pl.points[i].y() - pl.points[i - 1].y());
        len += std::sqrt(dx * dx + dy * dy);
    }
    return len * SCALING_FACTOR;
}

// Splits a connected zig-zag into its straight lines (direction change > ~20 degrees).
std::vector<Polyline3> split_zigzag(const Polyline3 &path)
{
    const Points3 &pts = path.points;
    std::vector<Polyline3> lines;
    if (pts.size() < 2)
        return lines;
    Polyline3 current;
    current.points.push_back(pts[0]);
    double prev_dx = 0, prev_dy = 0;
    bool   has_prev = false;
    for (size_t i = 1; i < pts.size(); ++i) {
        const double dx  = static_cast<double>(pts[i].x() - pts[i - 1].x());
        const double dy  = static_cast<double>(pts[i].y() - pts[i - 1].y());
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0) {
            current.points.push_back(pts[i]);
            continue;
        }
        const double nx = dx / len, ny = dy / len;
        if (has_prev && (prev_dx * nx + prev_dy * ny) < 0.94) {
            current.points.push_back(pts[i]);
            if (current.points.size() >= 2)
                lines.push_back(std::move(current));
            current = Polyline3();
            current.points.push_back(pts[i]);
            has_prev = false;
            continue;
        }
        current.points.push_back(pts[i]);
        prev_dx  = nx;
        prev_dy  = ny;
        has_prev = true;
    }
    if (current.points.size() >= 2)
        lines.push_back(std::move(current));
    return lines;
}

void collect_paths(const ExtrusionEntity *e, std::vector<const ExtrusionPath *> &out)
{
    if (e == nullptr)
        return;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        for (const ExtrusionEntity *c : coll->entities)
            collect_paths(c, out);
    } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e))
        out.push_back(p);
    else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(e)) {
        for (const ExtrusionPath &sp : mp->paths)
            out.push_back(&sp);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(e)) {
        for (const ExtrusionPath &sp : loop->paths)
            out.push_back(&sp);
    }
}

// Axis perpendicular to the fill lines, measured on the lines themselves (length weighted mean
// of the doubled segment angles, so a line and its way back add up instead of cancelling) and
// canonized to a fixed half plane so its sign is deterministic.
LaneVec2 measured_perp_axis(const std::vector<RawLine> &lines)
{
    double s2x = 0.0, s2y = 0.0;
    for (const RawLine &rl : lines) {
        const Points3 &pts = rl.pl.points;
        for (size_t k = 1; k < pts.size(); ++k) {
            const double dx = double(pts[k].x() - pts[k - 1].x());
            const double dy = double(pts[k].y() - pts[k - 1].y());
            const double L  = std::sqrt(dx * dx + dy * dy);
            if (L <= 0.0)
                continue;
            const double th = std::atan2(dy, dx);
            s2x += L * std::cos(2.0 * th);
            s2y += L * std::sin(2.0 * th);
        }
    }
    double line_ang = 0.0;
    if (s2x * s2x + s2y * s2y > 0.0)
        line_ang = 0.5 * std::atan2(s2y, s2x);
    LaneVec2 perp{ -std::sin(line_ang), std::cos(line_ang) };
    if (perp.y < -1e-12 || (std::abs(perp.y) <= 1e-12 && perp.x < 0.0)) {
        perp.x = -perp.x;
        perp.y = -perp.y;
    }
    return perp;
}

// Position (mm) -> lane -> t. Single owner of that arithmetic (FieldSampler in NeotkoCM).
struct FieldSampler
{
    double    sp     = 1.0;  // real spacing between lanes, mm
    double    period = 0.0;  // > 0 physical period; <= 0 fit to the observed lanes
    double    anchor = 0.0;  // anchor projected on the axis, mm
    long long k0     = 0;
    long long k1     = 0;

    bool periodic() const { return period > 1e-4; }

    long long lane_of(double proj_mm) const
    {
        double pos_mm = proj_mm - anchor;
        if (periodic()) {
            pos_mm = std::fmod(pos_mm, period);
            if (pos_mm < 0.0)
                pos_mm += period;
        }
        return std::llround(pos_mm / std::max(1e-3, sp));
    }

    double t_of_lane(long long k) const
    {
        if (periodic()) {
            const double lanes_per_period = std::max(1.0, period / std::max(1e-3, sp));
            return std::clamp(double(k) / lanes_per_period, 0.0, 1.0);
        }
        if (k1 <= k0)
            return 0.5;
        return double(k - k0) / std::max(1.0, double(k1 - k0));
    }
};

// Groups the lines by lane (all segments of a line split by a hole fall in the same lane and get
// the same colour) and returns the ascending t of every group plus the group of every line.
struct FieldGroups
{
    std::vector<int>    group_of_line;
    std::vector<double> t_of_group;
};

FieldGroups field_groups(const std::vector<double> &proj_mm, double period_mm, double spacing_mm, double anchor_proj_mm, bool invert)
{
    FieldGroups out;
    const int n = static_cast<int>(proj_mm.size());
    out.group_of_line.assign(static_cast<size_t>(std::max(0, n)), 0);
    if (n <= 0)
        return out;

    FieldSampler smp;
    smp.sp     = std::max(1e-3, spacing_mm);
    smp.period = period_mm;
    smp.anchor = anchor_proj_mm;

    std::vector<long long> key(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i)
        key[i] = smp.lane_of(proj_mm[i]);

    std::map<long long, int> group_of_key;
    for (int i = 0; i < n; ++i)
        group_of_key.emplace(key[i], 0);
    int m = 0;
    for (auto &kv : group_of_key)
        kv.second = m++;
    for (int i = 0; i < n; ++i)
        out.group_of_line[i] = group_of_key[key[i]];

    smp.k0 = group_of_key.begin()->first;
    smp.k1 = group_of_key.rbegin()->first;
    out.t_of_group.assign(static_cast<size_t>(m), 0.0);
    if (m == 1)
        out.t_of_group[0] = 0.5;
    else {
        int g = 0;
        for (const auto &kv : group_of_key)
            out.t_of_group[g++] = smp.t_of_lane(kv.first);
    }

    if (invert) {
        // Flip the field, not the list: t_of_group must stay ascending for the builders.
        for (double &t : out.t_of_group)
            t = 1.0 - t;
        std::reverse(out.t_of_group.begin(), out.t_of_group.end());
        const int last = m - 1;
        for (int i = 0; i < n; ++i)
            out.group_of_line[i] = last - out.group_of_line[i];
    }
    return out;
}

} // namespace

std::vector<std::pair<int, ExtrusionEntityCollection>> ColorStitch::split_by_tool(const ExtrusionEntityCollection &fills,
                                                                                  const ColorStitchParams         &p,
                                                                                  double                           layer_height,
                                                                                  size_t                           layer_id,
                                                                                  double                           anchor_x_mm,
                                                                                  double                           anchor_y_mm,
                                                                                  int                              fallback_tool)
{
    // 1. Collect the lines. The caller fills ColorStitch laminas with unconnected lines
    //    (anchor_length_max = 0), so each path is normally one line; a connected zig-zag is split.
    std::vector<const ExtrusionPath *> paths;
    for (const ExtrusionEntity *e : fills.entities)
        collect_paths(e, paths);

    std::vector<RawLine> lines;
    for (const ExtrusionPath *path : paths) {
        std::vector<Polyline3> split = split_zigzag(path->polyline);
        if (split.size() <= 1) {
            RawLine rl;
            rl.pl    = path->polyline;
            rl.src   = path;
            rl.whole = true;
            lines.push_back(std::move(rl));
        } else {
            for (Polyline3 &pl : split) {
                RawLine rl;
                rl.pl  = std::move(pl);
                rl.src = path;
                lines.push_back(std::move(rl));
            }
        }
    }
    for (RawLine &rl : lines)
        rl.short_line = p.min_length_mm > 0.0 && polyline3_length_mm(rl.pl) < p.min_length_mm;

    // The colourable lines (short ones keep the fallback filament).
    std::vector<size_t> idx;
    for (size_t i = 0; i < lines.size(); ++i)
        if (!lines[i].short_line)
            idx.push_back(i);

    // 2. The recipe: `tools` (sequence) and, per colourable line, the slot it takes in `tools`.
    std::vector<int> tools;
    std::vector<int> slot(idx.size(), 0);

    const double lw_mm = paths.empty() ? 0.0 : double(paths.front()->width);
    const double h_mm  = (layer_height > 1e-4) ? layer_height : 0.2;
    // Real line spacing (Flow::rounded_rectangle_extrusion_spacing), the lane size.
    const double sp_mm = std::max(1e-3, lw_mm - h_mm * (1.0 - 0.25 * M_PI));

    std::vector<RawLine> colourable;
    colourable.reserve(idx.size());
    for (size_t i : idx)
        colourable.push_back(lines[i]);
    const LaneVec2 perp         = colourable.empty() ? LaneVec2{ 0.0, 1.0 } : measured_perp_axis(colourable);
    const double   anchor_proj  = anchor_x_mm * perp.x + anchor_y_mm * perp.y;
    std::vector<double> proj_mm(colourable.size(), 0.0);
    for (size_t i = 0; i < colourable.size(); ++i) {
        const LaneVec2 c = line_centroid(colourable[i].pl);
        proj_mm[i]       = (c.x * perp.x + c.y * perp.y) * SCALING_FACTOR;
    }

    const bool too_few = colourable.size() < 2 || (p.min_lines > 0 && int(colourable.size()) < p.min_lines);
    const int  tool_a  = p.tool[0] >= 0 ? p.tool[0] : std::max(0, fallback_tool);

    if (too_few) {
        tools.push_back(tool_a);
    } else if (p.mode == csmBlend2 || p.mode == csmBlend3) {
        const int tb = p.tool[1] >= 0 ? p.tool[1] : tool_a;
        const int tc = p.tool[2] >= 0 ? p.tool[2] : tb;
        // `repetitions` in fit-to-surface mode is a period of span / reps.
        double period = p.grad_span_mm;
        if (period <= 1e-4 && p.repetitions > 1) {
            const FieldGroups probe = field_groups(proj_mm, 0.0, sp_mm, anchor_proj, false);
            period = probe.t_of_group.size() > 1 ? double(probe.t_of_group.size() - 1) * sp_mm / double(p.repetitions) : 0.0;
        }
        const FieldGroups fg = field_groups(proj_mm, period, sp_mm, anchor_proj, p.invert);
        if (fg.t_of_group.size() >= 2) {
            tools = (p.mode == csmBlend2) ?
                build_dithered_tools_2color_at(fg.t_of_group, tool_a, tb, p.pct_a, p.easing, p.gamma) :
                build_dithered_tools_3color_at(fg.t_of_group, tool_a, tb, tc, p.pct_a, p.pct_b, p.easing, p.gamma, p.overlap);
            for (size_t i = 0; i < slot.size(); ++i)
                slot[i] = std::clamp(fg.group_of_line[i], 0, int(tools.size()) - 1);
        } else
            tools.push_back(tool_a);
    } else if (p.mode == csmBandsMM || p.mode == csmBandsLines) {
        // Stripes: the design is a cycle of bands in mm; each line takes the band its lane falls in.
        std::vector<double> band;
        for (int k = 0; k < 4; ++k) {
            const double w = (p.mode == csmBandsLines) ? double(std::max(0, p.band_count[k])) * sp_mm : p.band_mm[k];
            if (w > 1e-4 && p.tool[k] >= 0) {
                band.push_back(w);
                tools.push_back(p.tool[k]);
            }
        }
        if (band.size() < 2) {
            tools.assign(1, tool_a);
        } else {
            if (p.invert) {
                std::reverse(band.begin(), band.end());
                std::reverse(tools.begin(), tools.end());
            }
            double period = 0.0;
            for (double b : band)
                period += b;
            for (size_t i = 0; i < slot.size(); ++i) {
                double r = std::fmod(proj_mm[i] - anchor_proj, period);
                if (r < 0.0)
                    r += period;
                int    s   = int(band.size()) - 1;
                double acc = 0.0;
                for (int k = 0; k < int(band.size()); ++k) {
                    acc += band[k];
                    if (r < acc) {
                        s = k;
                        break;
                    }
                }
                slot[i] = s;
            }
        }
    } else {
        // Pattern / weave: line colour = pattern[lane mod length], lanes counted from the object
        // origin, so a hole does not shift the pattern and a short pattern repeats.
        std::string pattern = p.pattern;
        int         offset  = 0;
        if (p.mode == csmWeave) {
            pattern = weave_pattern(p.weave);
            offset  = weave_offset_per_layer(p.weave);
        }
        const int tb = p.tool[1] >= 0 ? p.tool[1] : tool_a;
        for (char c : pattern) {
            if (p.mode == csmWeave) {
                if (c == '1') tools.push_back(tool_a);
                else if (c == '2') tools.push_back(tb);
            } else if (c >= '1' && c <= '9')
                tools.push_back(int(c - '1'));
        }
        if (tools.empty())
            tools.push_back(tool_a);
        if (p.invert)
            std::reverse(tools.begin(), tools.end());
        if (tools.size() >= 2) {
            FieldSampler smp;
            smp.sp     = sp_mm;
            smp.anchor = anchor_proj;
            const long long len   = static_cast<long long>(tools.size());
            const long long shift = static_cast<long long>(layer_id) * offset;
            for (size_t i = 0; i < slot.size(); ++i) {
                const long long k = smp.lane_of(proj_mm[i]) + shift;
                slot[i]           = static_cast<int>(((k % len) + len) % len);
            }
        }
    }
    if (tools.empty())
        tools.push_back(tool_a);
    for (int &s : slot)
        s = std::clamp(s, 0, int(tools.size()) - 1);

    // 3. Bucket order = recipe order (the first colour of the recipe prints first), then the
    //    fallback filament if it is not part of the recipe.
    std::vector<int> order;
    for (int t : tools)
        if (std::find(order.begin(), order.end(), t) == order.end())
            order.push_back(t);
    const int fallback = std::max(0, fallback_tool >= 0 ? fallback_tool : tool_a);

    std::vector<std::pair<int, ExtrusionEntityCollection>> buckets;
    auto bucket_of = [&buckets](int tool) -> ExtrusionEntityCollection & {
        for (auto &b : buckets)
            if (b.first == tool)
                return b.second;
        buckets.emplace_back(tool, ExtrusionEntityCollection());
        return buckets.back().second;
    };
    for (int t : order)
        bucket_of(t);

    auto make_path = [](const RawLine &rl) -> ExtrusionPath * {
        if (rl.whole)
            return static_cast<ExtrusionPath *>(rl.src->clone());
        return new ExtrusionPath(rl.pl, *rl.src);
    };
    std::vector<char> is_colourable(lines.size(), 0);
    for (size_t j = 0; j < idx.size(); ++j) {
        is_colourable[idx[j]] = 1;
        bucket_of(tools[size_t(slot[j])]).entities.push_back(make_path(lines[idx[j]]));
    }
    for (size_t i = 0; i < lines.size(); ++i)
        if (!is_colourable[i])
            bucket_of(fallback).entities.push_back(make_path(lines[i]));

    // Drop empty buckets (a recipe colour that no line took on this surface).
    buckets.erase(std::remove_if(buckets.begin(), buckets.end(),
                                 [](const std::pair<int, ExtrusionEntityCollection> &b) { return b.second.entities.empty(); }),
                  buckets.end());
    return buckets;
}

// =============================================================================================
// PathBlend
// =============================================================================================

void PathBlendPassConfig::sync_legacy_view()
{
    if (mode == Mode::Full) {
        num_passes = 2;
        tool[0]    = tool_bottom;
        tool[1]    = tool_top;
    } else {
        num_passes = 1;
        tool[0]    = tool_bottom;
        tool[1]    = -1;
    }
    tool[2] = -1;
    tool[3] = -1;
}

void PathBlendPassConfig::apply_constraints(double layer_height_mm)
{
    constexpr float kRampGap = 0.001f; // strict mid > floor
    constexpr float kAutoCap = 0.04f;  // default-only thin cap (not a hard limit)
    const double    H        = std::max(0.04, layer_height_mm);
    const float     floor_max = std::max(0.01f, (float) H - kRampGap);
    floor_mm = std::clamp(floor_mm, 0.01f, floor_max);
    if (mode == Mode::Full) {
        const float mid_min = floor_mm + kRampGap;
        const float mid_max = (float) H;
        if (mid_end_mm < 0.f)
            mid_end_mm = std::max(mid_min, (float) H - kAutoCap);
        mid_end_mm = std::clamp(mid_end_mm, mid_min, std::max(mid_min, mid_max));
    } else
        mid_end_mm = (float) H; // Half: the ramp goes to the layer top, no cap
    constexpr float kMinSpan = 0.02f;
    in_t  = std::clamp(in_t, 0.0f, 1.0f - kMinSpan);
    out_t = std::clamp(out_t, in_t + kMinSpan, 1.0f);
}

double PathBlendPassConfig::profile_u(double t) const
{
    const double a    = std::clamp(double(in_t), 0.0, 1.0);
    const double b    = std::clamp(double(out_t), 0.0, 1.0);
    const double span = b - a;
    if (span <= 1e-6)
        return std::clamp(t, 0.0, 1.0);
    return std::clamp((t - a) / span, 0.0, 1.0);
}

std::string PathBlendPassConfig::to_blob_json() const
{
    nlohmann::json j;
    j["v"]           = 4;
    j["mode"]        = (mode == Mode::Full) ? "full" : "half";
    j["floor_mm"]    = floor_mm;
    j["mid_end_mm"]  = mid_end_mm;
    j["tool_bottom"] = tool_bottom;
    j["tool_top"]    = tool_top;
    j["ease_mode"]   = ease_mode;
    j["fill_angle"]  = fill_angle;
    j["in_t"]        = in_t;
    j["out_t"]       = out_t;
    j["span_mm"]     = span_mm;
    return j.dump();
}

PathBlendPassConfig PathBlendPassConfig::from_blob_json(const std::string &blob)
{
    PathBlendPassConfig c;
    c.sync_legacy_view();
    if (blob.empty())
        return c;
    try {
        const nlohmann::json j = nlohmann::json::parse(blob);
        if (!j.is_object())
            return c;
        auto gi = [&](const char *k, int def) { return (j.contains(k) && j[k].is_number()) ? j[k].get<int>() : def; };
        auto gf = [&](const char *k, double def) { return (j.contains(k) && j[k].is_number()) ? j[k].get<double>() : def; };
        if (j.contains("mode") || j.contains("floor_mm") || j.contains("mid_end_mm") || gi("v", 1) >= 2) {
            const std::string mode_str = (j.contains("mode") && j["mode"].is_string()) ? j["mode"].get<std::string>() : "full";
            c.mode        = (mode_str == "half") ? Mode::Half : Mode::Full;
            c.floor_mm    = static_cast<float>(gf("floor_mm", 0.01));
            c.mid_end_mm  = static_cast<float>(gf("mid_end_mm", -1.0));
            c.tool_bottom = gi("tool_bottom", 0);
            c.tool_top    = gi("tool_top", 1);
            c.ease_mode   = std::clamp(gi("ease_mode", 0), 0, 3);
            c.fill_angle  = gi("fill_angle", -1);
            c.in_t        = static_cast<float>(gf("in_t", 0.0));
            c.out_t       = static_cast<float>(gf("out_t", 1.0));
            c.span_mm     = std::max(0.f, static_cast<float>(gf("span_mm", 0.0)));
        } else {
            // v1 (NeotkoCM s69 miniblob): num_passes / tool[] / min_ratio / max_ratio as fractions
            // of a 0.2 mm layer.
            const int legacy_np = std::clamp(gi("num_passes", 2), 1, 4);
            int       t0 = 0, tlast = 1;
            if (j.contains("tool") && j["tool"].is_array()) {
                if (j["tool"].size() > 0 && j["tool"][0].is_number())
                    t0 = j["tool"][0].get<int>();
                if (int(j["tool"].size()) > legacy_np - 1 && j["tool"][legacy_np - 1].is_number())
                    tlast = j["tool"][legacy_np - 1].get<int>();
            }
            const double H_default = 0.20;
            c.mode        = legacy_np <= 1 ? Mode::Half : Mode::Full;
            c.floor_mm    = static_cast<float>(std::clamp(std::clamp(gf("min_ratio", 0.05), 0.01, 0.49) * H_default, 0.01, H_default - 0.04));
            c.mid_end_mm  = static_cast<float>(std::clamp(std::clamp(gf("max_ratio", 1.0), 0.51, 1.0) * H_default, double(c.floor_mm), H_default - 0.04));
            c.tool_bottom = t0;
            c.tool_top    = (c.mode == Mode::Full) ? tlast : -1;
            c.ease_mode   = std::clamp(gi("ease_mode", 0), 0, 3);
            c.fill_angle  = gi("fill_angle", -1);
        }
        if (c.floor_mm < 0.01f)
            c.floor_mm = 0.01f;
        if (c.mid_end_mm >= 0.f && c.mid_end_mm < c.floor_mm)
            c.mid_end_mm = c.floor_mm;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "PathBlend blob parse error: " << e.what();
    }
    c.sync_legacy_view();
    return c;
}

PathBlendPassConfig PathBlendPassConfig::from_config(const PrintObjectConfig &cfg)
{
    PathBlendPassConfig c;
    if (!cfg.pathblend_top.value.empty())
        c = from_blob_json(cfg.pathblend_top.value);
    else {
        // Config filaments are 1-based (0 = the object's filament, resolved by the fill engine).
        c.mode        = (cfg.pathblend_mode.value == PathBlendMode::Half) ? Mode::Half : Mode::Full;
        c.tool_bottom = cfg.pathblend_filament_ramp.value - 1;
        c.tool_top    = (c.mode == Mode::Full) ? cfg.pathblend_filament_cap.value - 1 : -1;
        c.floor_mm    = std::max(0.01f, float(cfg.pathblend_floor_mm.value));
        c.mid_end_mm  = float(cfg.pathblend_ramp_end_mm.value);
        c.fill_angle  = cfg.pathblend_fill_angle.value;
    }
    c.enabled = cfg.multipass_path_gradient.value;
    c.sync_legacy_view();
    return c;
}

std::vector<PBBand> compute_pb_bands(const PathBlendPassConfig &pb, double bottom_z, double H, double min_band_h, bool want_cap,
                                     int target_k, double min_printable_h)
{
    std::vector<PBBand> out;
    if (H <= 0.0 || min_band_h <= 0.0)
        return out;
    // min_band_h only quantizes the ramp; min_h_phys is the physical floor of every bead.
    const double min_h_phys = (min_printable_h > 0.0) ? min_printable_h : min_band_h;
    const float  floor_mm   = std::max(static_cast<float>(min_h_phys), std::max(0.01f, pb.floor_mm));
    const float  mid_pref   = pb.mid_end_mm < 0.f ? static_cast<float>(pb.mode == PathBlendPassConfig::Mode::Full ? H - min_h_phys : H) :
                                                    pb.mid_end_mm;
    const float  mid_end_mm = (pb.mode == PathBlendPassConfig::Mode::Full) ? std::min(mid_pref, static_cast<float>(H - min_h_phys)) :
                                                                            std::min(mid_pref, static_cast<float>(H));
    if (mid_end_mm <= floor_mm)
        return out;
    const double range_mm  = double(mid_end_mm) - double(floor_mm);
    const int    K_natural = std::max(1, static_cast<int>(std::floor(range_mm / min_band_h)));
    const int    K_ramp    = (target_k > 0) ? std::min(target_k, K_natural) : K_natural;
    if (K_ramp < 1)
        return out;
    const double h_step = range_mm / double(K_ramp);
    const double t_step = 1.0 / double(K_ramp);
    for (int k = 0; k < K_ramp; ++k) {
        PBBand b;
        b.is_cap = false;
        b.t_lo   = static_cast<float>(k * t_step);
        b.t_hi   = static_cast<float>((k + 1) * t_step);
        b.t_mid  = 0.5f * (b.t_lo + b.t_hi);
        b.z_top  = static_cast<float>(bottom_z + double(floor_mm) + double(k + 1) * h_step);
        b.h_step = static_cast<float>(h_step);
        out.push_back(b);
    }
    if (!want_cap)
        return out;
    for (int k = 0; k < K_ramp; ++k) {
        const PBBand r           = out[k];
        const double ramp_at_mid = double(floor_mm) + double(r.t_mid) * range_mm;
        PBBand       c;
        c.is_cap = true;
        c.t_lo   = r.t_lo;
        c.t_hi   = r.t_hi;
        c.t_mid  = r.t_mid;
        c.z_top  = static_cast<float>(bottom_z + H);
        c.h_step = static_cast<float>(std::max(min_h_phys, H - ramp_at_mid));
        out.push_back(c);
    }
    return out;
}

// =============================================================================================
// SurfacePassStack (JSON schema identical to NeotkoCM, so stacks can be pasted across)
// =============================================================================================

namespace {
nlohmann::json sandwich_payload_to_json(const SurfaceEffectPayload &p)
{
    nlohmann::json j;
    j["present"] = p.present;
    j["kv"]      = p.kv;
    return j;
}

SurfaceEffectPayload sandwich_payload_from_json(const nlohmann::json &j)
{
    SurfaceEffectPayload out;
    if (j.is_object()) {
        if (j.contains("present") && j["present"].is_boolean())
            out.present = j["present"].get<bool>();
        if (j.contains("kv") && j["kv"].is_object())
            for (auto it = j["kv"].begin(); it != j["kv"].end(); ++it)
                if (it.value().is_string())
                    out.kv[it.key()] = it.value().get<std::string>();
    }
    return out;
}
} // namespace

bool SurfacePassStack::all_solid() const
{
    for (const SurfacePass &p : passes)
        if (p.kind != SurfacePassKind::Solid)
            return false;
    return true;
}

bool SurfacePassStack::any_effect() const
{
    for (const SurfacePass &p : passes)
        if (p.kind != SurfacePassKind::None)
            return true;
    return false;
}

std::string SurfacePassStack::to_json() const
{
    if (passes.empty())
        return std::string();
    nlohmann::json root;
    root["v"]                        = 1;
    root["perimeter_override"]       = perimeter_override;
    root["bottom_supported_control"] = bottom_supported_control;
    nlohmann::json arr               = nlohmann::json::array();
    if (!enabled || !any_effect()) {
        // "No effect on this zone" is encoded one way only: an explicit None passthrough.
        root["enabled"] = false;
        nlohmann::json e;
        e["kind"]  = static_cast<int>(SurfacePassKind::None);
        e["ratio"] = 1.0;
        arr.push_back(std::move(e));
    } else {
        root["enabled"] = enabled;
        for (const auto &p : passes) {
            nlohmann::json e;
            e["kind"]        = static_cast<int>(p.kind);
            e["ratio"]       = p.ratio;
            e["solid_tool"]  = p.solid_tool;
            e["angle"]       = p.angle;
            e["fan"]         = p.fan;
            e["speed_pct"]   = p.speed_pct;
            e["gcode_start"] = p.gcode_start;
            e["gcode_end"]   = p.gcode_end;
            e["colormix"]    = sandwich_payload_to_json(p.colorstitch);
            e["pathblend"]   = sandwich_payload_to_json(p.pathblend);
            arr.push_back(std::move(e));
        }
    }
    root["passes"] = std::move(arr);
    return root.dump();
}

SurfacePassStack SurfacePassStack::from_json(const std::string &text)
{
    SurfacePassStack st;
    if (text.empty())
        return st;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "Sandwich pass stack: JSON parse error: " << e.what();
        return st;
    }
    if (!root.is_object() || !root.contains("passes") || !root["passes"].is_array())
        return st;
    if (root.contains("enabled") && root["enabled"].is_boolean())
        st.enabled = root["enabled"].get<bool>();
    if (root.contains("perimeter_override") && root["perimeter_override"].is_boolean())
        st.perimeter_override = root["perimeter_override"].get<bool>();
    if (root.contains("bottom_supported_control") && root["bottom_supported_control"].is_boolean())
        st.bottom_supported_control = root["bottom_supported_control"].get<bool>();
    for (const auto &e : root["passes"]) {
        if (!e.is_object())
            continue;
        if (static_cast<int>(st.passes.size()) >= kMaxPasses)
            break;
        SurfacePass p;
        if (e.contains("kind") && e["kind"].is_number_integer()) {
            const int k = e["kind"].get<int>();
            if (k >= 0 && k <= 3)
                p.kind = static_cast<SurfacePassKind>(k);
        }
        if (e.contains("ratio") && e["ratio"].is_number())                 p.ratio       = e["ratio"].get<double>();
        if (e.contains("solid_tool") && e["solid_tool"].is_number_integer()) p.solid_tool = e["solid_tool"].get<int>();
        if (e.contains("angle") && e["angle"].is_number_integer())         p.angle       = e["angle"].get<int>();
        if (e.contains("fan") && e["fan"].is_number_integer())             p.fan         = e["fan"].get<int>();
        if (e.contains("speed_pct") && e["speed_pct"].is_number_integer()) p.speed_pct   = e["speed_pct"].get<int>();
        if (e.contains("gcode_start") && e["gcode_start"].is_string())     p.gcode_start = e["gcode_start"].get<std::string>();
        if (e.contains("gcode_end") && e["gcode_end"].is_string())         p.gcode_end   = e["gcode_end"].get<std::string>();
        if (e.contains("colormix"))
            p.colorstitch = sandwich_payload_from_json(e["colormix"]);
        if (e.contains("pathblend"))
            p.pathblend = sandwich_payload_from_json(e["pathblend"]);
        st.passes.push_back(std::move(p));
    }
    // A legacy "disabled but populated" stack meant "no effect": normalize to the passthrough.
    if (!st.enabled && st.any_effect()) {
        st.passes.clear();
        SurfacePass none_pass;
        none_pass.kind  = SurfacePassKind::None;
        none_pass.ratio = 1.0;
        st.passes.push_back(std::move(none_pass));
    }
    return st;
}

SurfacePassStack SurfacePassStack::synthesize_from_config(const PrintObjectConfig &cfg)
{
    SurfacePassStack st;
    if (cfg.multipass_enabled.value) {
        const SandwichPassKind kinds[3]  = { cfg.multipass_kind_1.value, cfg.multipass_kind_2.value, cfg.multipass_kind_3.value };
        // 1-based filaments in the config (0 = the object's filament -> -1 here); heights in percent.
        const int              toolv[3]  = { cfg.multipass_filament_1.value - 1, cfg.multipass_filament_2.value - 1, cfg.multipass_filament_3.value - 1 };
        const double           ratios[3] = { 0.01 * cfg.multipass_height_1.value, 0.01 * cfg.multipass_height_2.value, 0.01 * cfg.multipass_height_3.value };
        const int              angles[3] = { cfg.multipass_angle_1.value, cfg.multipass_angle_2.value, cfg.multipass_angle_3.value };
        const int              n         = std::clamp(cfg.multipass_num_passes.value, 1, kMaxPasses);
        for (int i = 0; i < n; ++i) {
            SurfacePass p;
            p.kind       = static_cast<SurfacePassKind>(int(kinds[i]));
            p.ratio      = std::max(0.0, ratios[i]);
            p.solid_tool = toolv[i];
            p.angle      = angles[i];
            if (p.kind == SurfacePassKind::ColorStitch)
                p.colorstitch.present = true; // empty kv: the object's interlayer_colormix_* values
            else if (p.kind == SurfacePassKind::PathBlend) {
                // PathBlend is always a single full height gradient (as in NeotkoCM, picking it
                // collapses the stack to that one pass).
                SurfacePass pb;
                pb.kind              = SurfacePassKind::PathBlend;
                pb.ratio             = 1.0;
                pb.pathblend.present = true;
                pb.pathblend.kv["blob"] = PathBlendPassConfig::from_config(cfg).to_blob_json();
                st.passes.assign(1, pb);
                st.enabled = true;
                return st;
            }
            st.passes.push_back(std::move(p));
        }
        st.enabled = true;
        return st;
    }
    if (cfg.interlayer_colormix_enabled.value) {
        SurfacePass p;
        p.kind                = SurfacePassKind::ColorStitch;
        p.ratio               = 1.0;
        p.colorstitch.present = true;
        st.passes.push_back(std::move(p));
        st.enabled = true;
        return st;
    }
    if (cfg.multipass_path_gradient.value) {
        SurfacePass p;
        p.kind              = SurfacePassKind::PathBlend;
        p.ratio             = 1.0;
        p.pathblend.present = true;
        p.pathblend.kv["blob"] = PathBlendPassConfig::from_config(cfg).to_blob_json();
        st.passes.push_back(std::move(p));
        st.enabled = true;
    }
    return st;
}

SurfacePassStack SurfacePassStack::resolve(const PrintObjectConfig &cfg)
{
    SurfacePassStack st = SurfacePassStack::from_json(cfg.neotko_surface_passes_top.value);
    if (st.passes.empty())
        st = SurfacePassStack::synthesize_from_config(cfg);
    return st;
}

SurfacePassStack SurfacePassStack::resolve_zone(const PrintObjectConfig &cfg, SurfaceZone zone)
{
    if (zone == SurfaceZone::Top)
        return resolve(cfg);
    const bool         penu = zone == SurfaceZone::Penultimate;
    const std::string &json = penu ? cfg.neotko_surface_passes_penu.value : cfg.neotko_surface_passes_bottom.value;
    SurfacePassStack   st   = SurfacePassStack::from_json(json);
    if (!st.passes.empty())
        return st;
    const SandwichZoneMode mode = penu ? cfg.penu_zone_mode.value : cfg.bottom_zone_mode.value;
    const int              fil  = penu ? cfg.penu_zone_filament.value : cfg.bottom_zone_filament.value;
    SurfacePass            p;
    p.ratio = 1.0;
    switch (mode) {
    case SandwichZoneMode::Solid:
        p.kind       = SurfacePassKind::Solid;
        p.solid_tool = fil - 1; // 0 -> -1: the object's own filament
        break;
    case SandwichZoneMode::ColorStitch:
        p.kind                = SurfacePassKind::ColorStitch;
        p.colorstitch.present = true; // empty kv: the object's interlayer_colormix_* values
        break;
    case SandwichZoneMode::SameAsTop:
        return resolve(cfg);
    default:
        return st;
    }
    st.passes.push_back(std::move(p));
    st.enabled = true;
    return st;
}

bool SurfacePassStack::config_has_zone(const PrintObjectConfig &cfg, SurfaceZone zone)
{
    switch (zone) {
    case SurfaceZone::Top:         return config_has_sandwich(cfg);
    case SurfaceZone::Penultimate: return !cfg.neotko_surface_passes_penu.value.empty() || cfg.penu_zone_mode.value != SandwichZoneMode::Off;
    case SurfaceZone::Bottom:      return !cfg.neotko_surface_passes_bottom.value.empty() || cfg.bottom_zone_mode.value != SandwichZoneMode::Off;
    }
    return false;
}

bool SurfacePassStack::config_has_sandwich(const PrintObjectConfig &cfg)
{
    return !cfg.neotko_surface_passes_top.value.empty() || cfg.multipass_enabled.value || cfg.interlayer_colormix_enabled.value ||
           cfg.multipass_path_gradient.value;
}

} // namespace Slic3r
