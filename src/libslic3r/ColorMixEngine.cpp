#include "ColorMixEngine.hpp"

#include "FilamentMixerModel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <set>

namespace Slic3r {
namespace ColorMix {

namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp01(double v) { return v < 0. ? 0. : (v > 1. ? 1. : v); }

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower((unsigned char) c));
    return s;
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int gcd_int(int a, int b)
{
    while (b != 0) { const int t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}

struct Lin { double r, g, b; };
Lin to_lin(const RGB &c) { return {srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b)}; }
RGB from_lin(const Lin &l) { return {linear_to_srgb(l.r), linear_to_srgb(l.g), linear_to_srgb(l.b)}; }

} // namespace

// ---------------------------------------------------------------------------------------------
// Colour spaces
// ---------------------------------------------------------------------------------------------

RGB rgb_from_u8(int r, int g, int b) { return {r / 255.0, g / 255.0, b / 255.0}; }

void rgb_to_u8(const RGB &c, int &r, int &g, int &b)
{
    r = int(std::lround(clamp01(c.r) * 255.0));
    g = int(std::lround(clamp01(c.g) * 255.0));
    b = int(std::lround(clamp01(c.b) * 255.0));
}

bool rgb_from_hex(const std::string &hex, RGB &out)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return false;
    int v[3];
    for (int i = 0; i < 3; ++i) {
        const int hi = hex_digit(s[size_t(2 * i)]), lo = hex_digit(s[size_t(2 * i + 1)]);
        if (hi < 0 || lo < 0)
            return false;
        v[i] = hi * 16 + lo;
    }
    out = rgb_from_u8(v[0], v[1], v[2]);
    return true;
}

std::string rgb_to_hex(const RGB &c)
{
    int r, g, b;
    rgb_to_u8(c, r, g, b);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return buf;
}

double srgb_to_linear(double v)
{
    v = clamp01(v);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double v)
{
    v = clamp01(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// Björn Ottosson's OKLab (2020).
Lab oklab_from_rgb(const RGB &c)
{
    const Lin   l = to_lin(c);
    const double L = 0.4122214708 * l.r + 0.5363325363 * l.g + 0.0514459929 * l.b;
    const double M = 0.2119034982 * l.r + 0.6806995451 * l.g + 0.1073969566 * l.b;
    const double S = 0.0883024619 * l.r + 0.2817188376 * l.g + 0.6299787005 * l.b;
    const double l_ = std::cbrt(L), m_ = std::cbrt(M), s_ = std::cbrt(S);
    return {0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_};
}

RGB rgb_from_oklab(const Lab &lab)
{
    const double l_ = lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const double m_ = lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const double s_ = lab.L - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
    const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    return from_lin({+4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                     -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                     -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s});
}

Lab cielab_from_rgb(const RGB &c)
{
    const Lin l = to_lin(c);
    const double x = (0.4124564 * l.r + 0.3575761 * l.g + 0.1804375 * l.b) / 0.95047;
    const double y = (0.2126729 * l.r + 0.7151522 * l.g + 0.0721750 * l.b);
    const double z = (0.0193339 * l.r + 0.1191920 * l.g + 0.9503041 * l.b) / 1.08883;
    auto f = [](double t) {
        constexpr double eps = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
        return t > eps ? std::cbrt(t) : (kappa * t + 16.0) / 116.0;
    };
    const double fx = f(x), fy = f(y), fz = f(z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double ciede2000(const Lab &lab1, const Lab &lab2)
{
    const double C1 = std::hypot(lab1.a, lab1.b), C2 = std::hypot(lab2.a, lab2.b);
    const double Cbar = 0.5 * (C1 + C2);
    const double Cbar7 = std::pow(Cbar, 7.0);
    const double G = 0.5 * (1.0 - std::sqrt(Cbar7 / (Cbar7 + std::pow(25.0, 7.0))));
    const double a1p = (1.0 + G) * lab1.a, a2p = (1.0 + G) * lab2.a;
    const double C1p = std::hypot(a1p, lab1.b), C2p = std::hypot(a2p, lab2.b);
    auto hue = [](double b, double ap) {
        if (b == 0.0 && ap == 0.0) return 0.0;
        double h = std::atan2(b, ap) * 180.0 / kPi;
        return h < 0.0 ? h + 360.0 : h;
    };
    const double h1p = hue(lab1.b, a1p), h2p = hue(lab2.b, a2p);

    const double dLp = lab2.L - lab1.L;
    const double dCp = C2p - C1p;
    double dhp = 0.0;
    if (C1p * C2p != 0.0) {
        dhp = h2p - h1p;
        if (dhp > 180.0) dhp -= 360.0;
        else if (dhp < -180.0) dhp += 360.0;
    }
    const double dHp = 2.0 * std::sqrt(C1p * C2p) * std::sin(dhp * kPi / 360.0);

    const double Lbarp = 0.5 * (lab1.L + lab2.L);
    const double Cbarp = 0.5 * (C1p + C2p);
    double hbarp = h1p + h2p;
    if (C1p * C2p != 0.0) {
        if (std::abs(h1p - h2p) <= 180.0) hbarp *= 0.5;
        else if (h1p + h2p < 360.0) hbarp = 0.5 * (hbarp + 360.0);
        else hbarp = 0.5 * (hbarp - 360.0);
    }
    const double T = 1.0 - 0.17 * std::cos((hbarp - 30.0) * kPi / 180.0) + 0.24 * std::cos(2.0 * hbarp * kPi / 180.0) +
                     0.32 * std::cos((3.0 * hbarp + 6.0) * kPi / 180.0) - 0.20 * std::cos((4.0 * hbarp - 63.0) * kPi / 180.0);
    const double dTheta = 30.0 * std::exp(-std::pow((hbarp - 275.0) / 25.0, 2.0));
    const double Cbarp7 = std::pow(Cbarp, 7.0);
    const double Rc = 2.0 * std::sqrt(Cbarp7 / (Cbarp7 + std::pow(25.0, 7.0)));
    const double Lm50 = (Lbarp - 50.0) * (Lbarp - 50.0);
    const double Sl = 1.0 + 0.015 * Lm50 / std::sqrt(20.0 + Lm50);
    const double Sc = 1.0 + 0.045 * Cbarp;
    const double Sh = 1.0 + 0.015 * Cbarp * T;
    const double Rt = -std::sin(2.0 * dTheta * kPi / 180.0) * Rc;
    const double tl = dLp / Sl, tc = dCp / Sc, th = dHp / Sh;
    return std::sqrt(tl * tl + tc * tc + th * th + Rt * tc * th);
}

double delta_e(const RGB &a, const RGB &b) { return ciede2000(cielab_from_rgb(a), cielab_from_rgb(b)); }

double delta_e_ok(const RGB &a, const RGB &b)
{
    const Lab x = oklab_from_rgb(a), y = oklab_from_rgb(b);
    return 100.0 * std::sqrt((x.L - y.L) * (x.L - y.L) + (x.a - y.a) * (x.a - y.a) + (x.b - y.b) * (x.b - y.b));
}

// ---------------------------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------------------------

Method choose_method(const PrinterKind &printer)
{
    if (printer.nozzles > 1 && !printer.single_extruder_mm)
        return Method::SubLayers;
    return Method::AlternatingLayers;
}

const char *method_key(Method m)
{
    switch (m) {
    case Method::Auto: return "auto";
    case Method::AlternatingLayers: return "layers";
    case Method::SubLayers: return "sublayers";
    }
    return "auto";
}

Method method_from_key(const std::string &key)
{
    if (key == "layers") return Method::AlternatingLayers;
    if (key == "sublayers") return Method::SubLayers;
    return Method::Auto;
}

const char *model_key(Model m) { return m == Model::Pigment ? "pigment" : "optical"; }
Model model_from_key(const std::string &key) { return key == "pigment" ? Model::Pigment : Model::Optical; }

double default_opacity(const std::string &type, const std::string &name, int alpha)
{
    const std::string t = lower(type) + " " + lower(name);
    double o = 1.0;
    for (const char *word : {"transparent", "translucent", "clear", "transl", "crystal", "glass"})
        if (t.find(word) != std::string::npos)
            o = std::min(o, 0.35);
    for (const char *word : {"natural", "glow"})
        if (t.find(word) != std::string::npos)
            o = std::min(o, 0.6);
    if (t.find("petg") != std::string::npos)
        o = std::min(o, 0.85);
    if (alpha < 255)
        o = std::min(o, std::max(0.05, alpha / 255.0));
    return o;
}

// ---------------------------------------------------------------------------------------------
// Recipes
// ---------------------------------------------------------------------------------------------

int Recipe::total() const { return std::accumulate(weights.begin(), weights.end(), 0); }

Recipe Recipe::normalized() const
{
    std::vector<std::pair<int, int>> parts;
    for (size_t i = 0; i < components.size() && i < weights.size(); ++i) {
        if (weights[i] <= 0)
            continue;
        auto it = std::find_if(parts.begin(), parts.end(), [&](const auto &p) { return p.first == components[i]; });
        if (it == parts.end())
            parts.emplace_back(components[i], weights[i]);
        else
            it->second += weights[i];
    }
    std::sort(parts.begin(), parts.end());
    int g = 0;
    for (const auto &p : parts)
        g = gcd_int(g, p.second);
    Recipe out;
    for (const auto &p : parts) {
        out.components.push_back(p.first);
        out.weights.push_back(g > 0 ? p.second / g : p.second);
    }
    return out;
}

bool Recipe::operator==(const Recipe &o) const
{
    const Recipe a = normalized(), b = o.normalized();
    return a.components == b.components && a.weights == b.weights;
}

std::string Recipe::pattern() const
{
    const Recipe r = normalized();
    const int    n = r.total();
    std::string  out;
    if (n <= 0)
        return out;
    // Spread each component evenly over the cycle (largest deficit first).
    std::vector<int> emitted(r.components.size(), 0);
    for (int pos = 0; pos < n; ++pos) {
        size_t best = 0;
        double best_deficit = -std::numeric_limits<double>::max();
        for (size_t i = 0; i < r.components.size(); ++i) {
            const double deficit = double(pos + 1) * r.weights[i] / n - emitted[i];
            if (deficit > best_deficit + 1e-9) {
                best_deficit = deficit;
                best = i;
            }
        }
        ++emitted[best];
        out.push_back(char('A' + best));
    }
    return out;
}

std::vector<int> Recipe::percentages() const
{
    const int total_w = total();
    std::vector<int> out(weights.size(), 0);
    if (total_w <= 0)
        return out;
    std::vector<std::pair<double, size_t>> rema;
    int sum = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double exact = 100.0 * weights[i] / total_w;
        out[i] = int(std::floor(exact));
        sum += out[i];
        rema.emplace_back(exact - out[i], i);
    }
    std::sort(rema.begin(), rema.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (size_t k = 0; sum < 100 && k < rema.size(); ++k, ++sum)
        ++out[rema[k].second];
    return out;
}

// ---------------------------------------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------------------------------------

double stripe_opacity(const Filament &f, double thickness_mm)
{
    if (f.td_mm > 0.0) {
        // HueForge's TD is the thickness at which the filament hides its background; model the
        // hiding power as 1 - exp(-3 t / TD) (95 % at t = TD).
        return clamp01(1.0 - std::exp(-3.0 * std::max(0.0, thickness_mm) / f.td_mm));
    }
    return clamp01(f.opacity);
}

// Thickness of one stripe of component i: a whole layer, or its share of the layer as a sub-layer.
static double stripe_thickness(const Recipe &r, size_t i, const ModelParams &p)
{
    if (p.method == Method::SubLayers && r.total() > 0)
        return p.layer_height * r.weights[i] / double(r.total());
    return p.layer_height;
}

static RGB predict_optical(const std::vector<Filament> &palette, const Recipe &r, const ModelParams &p)
{
    const double total_w = double(r.total());
    Lin bulk{0, 0, 0};
    for (size_t i = 0; i < r.components.size(); ++i) {
        const Lin c = to_lin(palette[size_t(r.components[i])].color);
        const double w = r.weights[i] / total_w;
        bulk.r += w * c.r; bulk.g += w * c.g; bulk.b += w * c.b;
    }
    Lin mix{0, 0, 0};
    for (size_t i = 0; i < r.components.size(); ++i) {
        const Filament &f = palette[size_t(r.components[i])];
        const Lin    c = to_lin(f.color);
        const double a = stripe_opacity(f, stripe_thickness(r, i, p));
        const double w = r.weights[i] / total_w;
        mix.r += w * (a * c.r + (1.0 - a) * c.r * bulk.r);
        mix.g += w * (a * c.g + (1.0 - a) * c.g * bulk.g);
        mix.b += w * (a * c.b + (1.0 - a) * c.b * bulk.b);
    }
    return from_lin(mix);
}

// The polynomial without the 8-bit truncation of filament_mixer::lerp(), colours in 0..255.
static void poly_raw(const double a[3], const double b[3], double t, double out[3])
{
    const double x[7] = {a[0], a[1], a[2], b[0], b[1], b[2], t};
    double       features[filament_mixer::detail::N_FEATURES];
    filament_mixer::detail::compute_poly_features(x, features);
    for (int c = 0; c < 3; ++c) {
        double sum = filament_mixer::detail::INTERCEPT[c];
        for (int i = 0; i < filament_mixer::detail::N_FEATURES; ++i)
            sum += features[i] * filament_mixer::detail::COEF[i][c];
        out[c] = sum;
    }
}

// Pigment mix of two colours. The regression does not return exactly A at t = 0 nor B at t = 1;
// like FullSpectrum 0.9.14 ("blends meet the selected filament colours at their endpoints") the
// endpoint residuals are removed linearly, keeping the nonlinear path in between.
static void poly_mix(const double a[3], const double b[3], double t, bool anchor, double out[3])
{
    double p0[3], p1[3];
    poly_raw(a, b, t, out);
    if (!anchor) {
        for (int c = 0; c < 3; ++c)
            out[c] = std::clamp(out[c], 0.0, 255.0);
        return;
    }
    poly_raw(a, b, 0.0, p0);
    poly_raw(a, b, 1.0, p1);
    for (int c = 0; c < 3; ++c)
        out[c] = std::clamp(out[c] - (1.0 - t) * (p0[c] - a[c]) - t * (p1[c] - b[c]), 0.0, 255.0);
}

static RGB predict_pigment(const std::vector<Filament> &palette, const Recipe &r, const ModelParams &p)
{
    // Polynomial pigment mixer (Mixbox approximation shared by Bambu, FullSpectrum and Orca),
    // accumulated pairwise like blend_color_multi(). A translucent filament weighs less, as in
    // FullSpectrum's TD weighting.
    double acc_c[3] = {128, 128, 128};
    double acc = 0.0;
    for (size_t i = 0; i < r.components.size(); ++i) {
        const Filament &f = palette[size_t(r.components[i])];
        const double w = r.weights[i] * std::max(0.05, stripe_opacity(f, stripe_thickness(r, i, p)));
        const double c[3] = {clamp01(f.color.r) * 255.0, clamp01(f.color.g) * 255.0, clamp01(f.color.b) * 255.0};
        if (acc <= 0.0) {
            std::copy(c, c + 3, acc_c);
            acc = w;
        } else {
            double out[3];
            poly_mix(acc_c, c, w / (acc + w), p.anchor_endpoints, out);
            std::copy(out, out + 3, acc_c);
            acc += w;
        }
    }
    return {acc_c[0] / 255.0, acc_c[1] / 255.0, acc_c[2] / 255.0};
}

RGB predict(const std::vector<Filament> &palette, const Recipe &recipe, const ModelParams &p)
{
    const Recipe r = recipe.normalized();
    if (r.components.empty())
        return {0.5, 0.5, 0.5};
    for (int c : r.components)
        if (c < 0 || size_t(c) >= palette.size())
            return {0.5, 0.5, 0.5};
    if (r.components.size() == 1)
        return palette[size_t(r.components.front())].color;
    if (p.measured) {
        std::vector<RGB> colors;
        for (int c : r.components)
            colors.push_back(palette[size_t(c)].color);
        RGB measured;
        if (p.measured(colors, r.percentages(), measured))
            return measured;
    }
    return p.model == Model::Pigment ? predict_pigment(palette, r, p) : predict_optical(palette, r, p);
}

// ---------------------------------------------------------------------------------------------
// Clustering
// ---------------------------------------------------------------------------------------------

std::vector<int> kmeans_oklab(const std::vector<RGB> &colors, const std::vector<double> &weights_in, int k,
                              std::vector<RGB> &centers, std::vector<double> &center_weights, int iterations)
{
    const size_t n = colors.size();
    std::vector<int> label(n, 0);
    centers.clear();
    center_weights.clear();
    if (n == 0 || k <= 0)
        return label;

    std::vector<Lab>    pts(n);
    std::vector<double> w(n, 1.0);
    for (size_t i = 0; i < n; ++i) {
        pts[i] = oklab_from_rgb(colors[i]);
        if (i < weights_in.size())
            w[i] = std::max(0.0, weights_in[i]);
    }
    auto d2 = [](const Lab &a, const Lab &b) {
        return (a.L - b.L) * (a.L - b.L) + (a.a - b.a) * (a.a - b.a) + (a.b - b.b) * (a.b - b.b);
    };

    // Distinct inputs bound k.
    {
        std::set<std::tuple<long, long, long>> distinct;
        for (const Lab &p : pts)
            distinct.emplace(std::lround(p.L * 1e5), std::lround(p.a * 1e5), std::lround(p.b * 1e5));
        k = std::min<int>(k, int(distinct.size()));
    }

    // k-means++ seeding, deterministic: the heaviest point first, then each next centre is the
    // point with the largest weighted squared distance to the chosen ones.
    std::vector<Lab> c;
    c.push_back(pts[size_t(std::max_element(w.begin(), w.end()) - w.begin())]);
    std::vector<double> best_d(n, std::numeric_limits<double>::max());
    while (int(c.size()) < k) {
        size_t pick = 0;
        double pick_score = -1.0;
        for (size_t i = 0; i < n; ++i) {
            best_d[i] = std::min(best_d[i], d2(pts[i], c.back()));
            const double score = best_d[i] * std::max(w[i], 1e-9);
            if (score > pick_score) {
                pick_score = score;
                pick = i;
            }
        }
        c.push_back(pts[pick]);
    }

    for (int it = 0; it < iterations; ++it) {
        bool changed = false;
        for (size_t i = 0; i < n; ++i) {
            int bi = 0;
            double bd = std::numeric_limits<double>::max();
            for (size_t j = 0; j < c.size(); ++j) {
                const double d = d2(pts[i], c[j]);
                if (d < bd) { bd = d; bi = int(j); }
            }
            if (label[i] != bi || it == 0) {
                changed = changed || label[i] != bi;
                label[i] = bi;
            }
        }
        std::vector<Lab>    sum(c.size(), Lab{0, 0, 0});
        std::vector<double> sw(c.size(), 0.0);
        for (size_t i = 0; i < n; ++i) {
            const double wi = std::max(w[i], 1e-9);
            Lab &s = sum[size_t(label[i])];
            s.L += wi * pts[i].L; s.a += wi * pts[i].a; s.b += wi * pts[i].b;
            sw[size_t(label[i])] += wi;
        }
        for (size_t j = 0; j < c.size(); ++j)
            if (sw[j] > 0.0)
                c[j] = {sum[j].L / sw[j], sum[j].a / sw[j], sum[j].b / sw[j]};
        if (!changed && it > 0)
            break;
    }

    // Drop empty clusters and renumber.
    std::vector<int> remap(c.size(), -1);
    center_weights.clear();
    for (size_t i = 0; i < n; ++i) {
        int &r = remap[size_t(label[i])];
        if (r < 0) {
            r = int(centers.size());
            centers.push_back(rgb_from_oklab(c[size_t(label[i])]));
            center_weights.push_back(0.0);
        }
        center_weights[size_t(r)] += w[i];
        label[i] = r;
    }
    return label;
}

// ---------------------------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------------------------

std::vector<Candidate> enumerate_candidates(const std::vector<Filament> &palette, const SolverOptions &opt)
{
    std::vector<Candidate> out;
    const int K = int(palette.size());
    auto add = [&](const Recipe &r, bool existing) {
        const Recipe n = r.normalized();
        if (n.components.empty())
            return;
        for (int comp : n.components)
            if (comp < 0 || comp >= K)
                return;
        for (Candidate &c : out)
            if (c.recipe.components == n.components && c.recipe.weights == n.weights) {
                c.existing = c.existing || existing;
                return;
            }
        Candidate c;
        c.recipe    = n;
        c.predicted = predict(palette, n, opt.model);
        c.lab       = cielab_from_rgb(c.predicted);
        c.existing  = existing;
        out.push_back(std::move(c));
    };

    for (int i = 0; i < K; ++i)
        add(Recipe{{i}, {1}}, true);
    if (opt.only_physical)
        return out;
    for (const Recipe &r : opt.existing_mixes)
        add(r, true);

    const int L = std::max(2, opt.pattern_length);
    for (int a = 0; a < K; ++a)
        for (int b = a + 1; b < K; ++b)
            for (int wa = 1; wa < L; ++wa)
                add(Recipe{{a, b}, {wa, L - wa}}, false);
    if (opt.max_components >= 3 && L >= 3)
        for (int a = 0; a < K; ++a)
            for (int b = a + 1; b < K; ++b)
                for (int c = b + 1; c < K; ++c)
                    for (int wa = 1; wa < L; ++wa)
                        for (int wb = 1; wa + wb < L; ++wb)
                            add(Recipe{{a, b, c}, {wa, wb, L - wa - wb}}, false);
    return out;
}

namespace {

struct Evaluator {
    const std::vector<Candidate> &cand;
    const std::vector<Lab>       &target_lab;
    const std::vector<double>    &target_w;
    double                        min_gain;
    double                        triple_gain;
    std::vector<std::vector<double>> de; // [target][candidate]

    Evaluator(const std::vector<Candidate> &c, const std::vector<Lab> &t, const std::vector<double> &w, double g, double g3)
        : cand(c), target_lab(t), target_w(w), min_gain(g), triple_gain(g3)
    {
        de.assign(t.size(), std::vector<double>(c.size(), 0.0));
        for (size_t i = 0; i < t.size(); ++i)
            for (size_t j = 0; j < c.size(); ++j)
                de[i][j] = ciede2000(t[i], c[j].lab);
    }

    // Best candidate for target i among physical + existing + the `allowed` new mixes. A mix must
    // beat the best physical by min_gain to be taken.
    int best_for(size_t i, const std::vector<char> &allowed) const
    {
        int    best_phys = -1, best_pair = -1, best_multi = -1;
        double dp = std::numeric_limits<double>::max(), d2 = dp, d3 = dp;
        for (size_t j = 0; j < cand.size(); ++j) {
            const size_t n = cand[j].recipe.components.size();
            if (n > 1 && !cand[j].existing && !allowed[j])
                continue;
            const double d = de[i][j];
            if (n == 1) {
                if (d < dp) { dp = d; best_phys = int(j); }
            } else if (n == 2) {
                if (d < d2) { d2 = d; best_pair = int(j); }
            } else if (d < d3) {
                d3 = d; best_multi = int(j);
            }
        }
        int    best_mix = best_pair;
        double dm       = d2;
        if (best_multi >= 0 && (best_pair < 0 || d3 + triple_gain < d2)) {
            best_mix = best_multi;
            dm       = d3;
        }
        if (best_mix >= 0 && (best_phys < 0 || dm + min_gain < dp))
            return best_mix;
        return best_phys;
    }

    double cost(const std::vector<char> &allowed, std::vector<int> *pick = nullptr) const
    {
        double total = 0.0;
        for (size_t i = 0; i < target_lab.size(); ++i) {
            const int j = best_for(i, allowed);
            if (pick) (*pick)[i] = j;
            total += target_w[i] * (j >= 0 ? de[i][size_t(j)] : 100.0);
        }
        return total;
    }
};

} // namespace

void refresh(Solution &sol, const SolverOptions &)
{
    double wsum = 0.0, dsum = 0.0;
    sol.max_delta_e = 0.0;
    std::vector<int> mixes;
    for (Assignment &a : sol.clusters) {
        if (a.candidate < 0 || size_t(a.candidate) >= sol.candidates.size()) {
            a.delta_e = 100.0;
        } else {
            const Candidate &c = sol.candidates[size_t(a.candidate)];
            a.delta_e = ciede2000(cielab_from_rgb(a.target), c.lab);
            if (!c.recipe.is_physical() && !c.existing &&
                std::find(mixes.begin(), mixes.end(), a.candidate) == mixes.end())
                mixes.push_back(a.candidate);
        }
        wsum += a.weight;
        dsum += a.weight * a.delta_e;
        sol.max_delta_e = std::max(sol.max_delta_e, a.delta_e);
    }
    sol.mean_delta_e = wsum > 0.0 ? dsum / wsum : 0.0;
    sol.new_mixes = std::move(mixes);
}

Solution solve(const std::vector<RGB> &targets, const std::vector<double> &weights,
               const std::vector<Filament> &palette, const SolverOptions &opt)
{
    Solution sol;
    if (targets.empty() || palette.empty())
        return sol;

    // 1. Reduce the targets to at most max_colors clusters.
    std::vector<RGB>    centers;
    std::vector<double> center_w;
    if (opt.max_colors > 0) {
        sol.source_cluster = kmeans_oklab(targets, weights, opt.max_colors, centers, center_w);
    } else {
        centers = targets;
        center_w.assign(targets.size(), 1.0);
        for (size_t i = 0; i < targets.size() && i < weights.size(); ++i)
            center_w[i] = weights[i];
        sol.source_cluster.resize(targets.size());
        std::iota(sol.source_cluster.begin(), sol.source_cluster.end(), 0);
    }
    // Normalise weights so the mean Delta E reads per unit of area.
    const double wtotal = std::accumulate(center_w.begin(), center_w.end(), 0.0);
    for (double &w : center_w)
        w = wtotal > 0.0 ? w / wtotal : 1.0 / double(center_w.size());

    // 2. Candidates and the distance table.
    sol.candidates = enumerate_candidates(palette, opt);
    std::vector<Lab> tlab(centers.size());
    for (size_t i = 0; i < centers.size(); ++i)
        tlab[i] = cielab_from_rgb(centers[i]);
    const Evaluator ev(sol.candidates, tlab, center_w, opt.min_gain, opt.triple_gain);

    // 3. Unconstrained choice, then drop the least useful mixes until the limit holds, then
    //    swap-based local search (classic facility-location heuristics).
    const size_t      nc = sol.candidates.size();
    std::vector<char> allowed(nc, 0);
    const int         max_mixes = opt.only_physical ? 0 : std::max(0, opt.max_mixes);
    if (max_mixes > 0) {
        for (size_t j = 0; j < nc; ++j)
            allowed[j] = !sol.candidates[j].recipe.is_physical() && !sol.candidates[j].existing;
        std::vector<int> pick(centers.size(), -1);
        ev.cost(allowed, &pick);
        std::fill(allowed.begin(), allowed.end(), char(0));
        for (int j : pick)
            if (j >= 0 && !sol.candidates[size_t(j)].recipe.is_physical() && !sol.candidates[size_t(j)].existing)
                allowed[size_t(j)] = 1;
        auto used_count = [&]() { return int(std::count(allowed.begin(), allowed.end(), char(1))); };
        while (used_count() > max_mixes) {
            int    drop = -1;
            double drop_cost = std::numeric_limits<double>::max();
            for (size_t j = 0; j < nc; ++j) {
                if (!allowed[j]) continue;
                allowed[j] = 0;
                const double c = ev.cost(allowed);
                allowed[j] = 1;
                if (c < drop_cost) { drop_cost = c; drop = int(j); }
            }
            allowed[size_t(drop)] = 0;
        }
        // Swap search: replace a chosen mix by any other one when that lowers the cost.
        double current = ev.cost(allowed);
        for (int round = 0; round < 8; ++round) {
            bool improved = false;
            for (size_t j = 0; j < nc; ++j) {
                if (!allowed[j]) continue;
                for (size_t k = 0; k < nc; ++k) {
                    if (allowed[k] || sol.candidates[k].recipe.is_physical() || sol.candidates[k].existing) continue;
                    allowed[j] = 0; allowed[k] = 1;
                    const double c = ev.cost(allowed);
                    if (c + 1e-9 < current) {
                        current = c;
                        improved = true;
                        break; // j is gone; continue with the next chosen mix
                    }
                    allowed[k] = 0; allowed[j] = 1;
                }
            }
            // Adding a free slot back when the limit allows it.
            if (used_count() < max_mixes) {
                int    add = -1;
                double best = current;
                for (size_t k = 0; k < nc; ++k) {
                    if (allowed[k] || sol.candidates[k].recipe.is_physical() || sol.candidates[k].existing) continue;
                    allowed[k] = 1;
                    const double c = ev.cost(allowed);
                    allowed[k] = 0;
                    if (c + 1e-9 < best) { best = c; add = int(k); }
                }
                if (add >= 0) { allowed[size_t(add)] = 1; current = best; improved = true; }
            }
            if (!improved)
                break;
        }
    }

    // 4. Final assignment.
    std::vector<int> pick(centers.size(), -1);
    ev.cost(allowed, &pick);
    sol.clusters.resize(centers.size());
    for (size_t i = 0; i < centers.size(); ++i) {
        sol.clusters[i].target    = centers[i];
        sol.clusters[i].weight    = center_w[i];
        sol.clusters[i].candidate = pick[i];
    }
    refresh(sol, opt);

    // Baseline: physical filaments only.
    std::vector<char> none(nc, 0);
    double base = 0.0;
    for (size_t i = 0; i < centers.size(); ++i) {
        double best = std::numeric_limits<double>::max();
        for (size_t j = 0; j < nc; ++j)
            if (sol.candidates[j].recipe.is_physical())
                best = std::min(best, ev.de[i][j]);
        base += center_w[i] * best;
    }
    sol.physical_mean_delta_e = base;
    return sol;
}

} // namespace ColorMix
} // namespace Slic3r
