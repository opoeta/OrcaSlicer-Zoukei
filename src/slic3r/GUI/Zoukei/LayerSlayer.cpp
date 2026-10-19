// Layer Slayer calibrations, native. See LayerSlayer.hpp.
//
// The geometry is a straight port of the plugin's mesh.py / text.py / calibrations/*.py, so a plate built here
// matches the 3MF the plugin wrote, piece for piece. What the plugin did with name tags and its G-code
// post-processor is done here with the slicer's own options (see the header).
#include "LayerSlayer.hpp"

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <locale>
#include <set>
#include <sstream>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace LayerSlayer {

namespace {

constexpr double PI = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------
// numbers
// ---------------------------------------------------------------------------------------------

// A number as OrcaSlicer writes it: no trailing zeros ("1.05", "20", "0.2").
std::string fnum(double value, int digits = 4)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss.setf(std::ios::fixed);
    ss.precision(digits);
    ss << value;
    std::string s = ss.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    if (s.empty() || s == "-0")
        s = "0";
    return s;
}

std::string signed_num(double value) { return (value > 0 ? "+" : "") + fnum(value, 3); }

std::vector<double> frange(double start, double step, int count)
{
    std::vector<double> out;
    for (int i = 0; i < count; ++i)
        out.push_back(std::round((start + i * step) * 1e6) / 1e6);
    return out;
}

bool parse_double(const std::string &text, double &out)
{
    std::string t = text;
    t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return std::isspace((unsigned char) c) != 0; }), t.end());
    if (t.empty())
        return false;
    std::istringstream ss(t);
    ss.imbue(std::locale::classic());
    double v = 0.;
    ss >> v;
    if (ss.fail() || !ss.eof() || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

// ---------------------------------------------------------------------------------------------
// meshes (mesh.py)
// ---------------------------------------------------------------------------------------------
using Pt   = std::pair<double, double>;
using Poly = std::vector<Pt>;
using Mesh = indexed_triangle_set;

void add(Mesh &a, const Mesh &b) { its_merge(a, b); }

Mesh translated(Mesh m, double dx, double dy, double dz)
{
    for (stl_vertex &v : m.vertices)
        v += stl_vertex(float(dx), float(dy), float(dz));
    return m;
}

Mesh box(double x0, double y0, double z0, double x1, double y1, double z1)
{
    Mesh m;
    const double v[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                            {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    for (const auto &p : v)
        m.vertices.emplace_back(float(p[0]), float(p[1]), float(p[2]));
    const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                          {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    for (const auto &t : f)
        m.indices.emplace_back(t[0], t[1], t[2]);
    return m;
}

double area2(const Poly &poly)
{
    double a = 0.;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Pt &p = poly[i], &q = poly[(i + 1) % poly.size()];
        a += p.first * q.second - q.first * p.second;
    }
    return a;
}

// Ear clipping of a simple polygon (no holes) -> index triples.
std::vector<std::array<int, 3>> triangulate(const Poly &pts)
{
    const int n = int(pts.size());
    std::vector<std::array<int, 3>> out;
    if (n < 3)
        return out;
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i)
        order[i] = i;
    if (area2(pts) < 0)
        std::reverse(order.begin(), order.end());
    auto cross = [&](int o, int a, int b) {
        return (pts[a].first - pts[o].first) * (pts[b].second - pts[o].second) -
               (pts[a].second - pts[o].second) * (pts[b].first - pts[o].first);
    };
    auto inside = [&](int p, int a, int b, int c) {
        return cross(a, b, p) >= -1e-12 && cross(b, c, p) >= -1e-12 && cross(c, a, p) >= -1e-12;
    };
    int guard = 0;
    while (order.size() > 3 && guard < 10 * n * n) {
        ++guard;
        bool       clipped = false;
        const int  m       = int(order.size());
        for (int k = 0; k < m; ++k) {
            const int a = order[(k - 1 + m) % m], b = order[k], c = order[(k + 1) % m];
            if (cross(a, b, c) <= 1e-12)
                continue;
            bool blocked = false;
            for (int p : order) {
                if (p == a || p == b || p == c || pts[p] == pts[a] || pts[p] == pts[b] || pts[p] == pts[c])
                    continue;
                if (inside(p, a, b, c)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked)
                continue;
            out.push_back({a, b, c});
            order.erase(order.begin() + k);
            clipped = true;
            break;
        }
        if (!clipped) // degenerate input: drop a vertex
            order.erase(order.begin());
    }
    if (order.size() == 3)
        out.push_back({order[0], order[1], order[2]});
    return out;
}

// A simple polygon (x, y) lifted between z0 and z1.
Mesh extrude(Poly pts, double z0, double z1)
{
    if (area2(pts) < 0)
        std::reverse(pts.begin(), pts.end());
    const int n    = int(pts.size());
    const auto tris = triangulate(pts);
    Mesh m;
    for (const Pt &p : pts)
        m.vertices.emplace_back(float(p.first), float(p.second), float(z0));
    for (const Pt &p : pts)
        m.vertices.emplace_back(float(p.first), float(p.second), float(z1));
    for (const auto &t : tris)
        m.indices.emplace_back(t[0], t[2], t[1]);
    for (const auto &t : tris)
        m.indices.emplace_back(t[0] + n, t[1] + n, t[2] + n);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        m.indices.emplace_back(i, j, j + n);
        m.indices.emplace_back(i, j + n, i + n);
    }
    return m;
}

// A polygon in the XZ plane (x, z) extruded along Y between y0 and y1.
Mesh extrude_profile_y(Poly pts, double y0, double y1)
{
    if (area2(pts) < 0)
        std::reverse(pts.begin(), pts.end());
    const int  n    = int(pts.size());
    const auto tris = triangulate(pts);
    Mesh m;
    for (const Pt &p : pts)
        m.vertices.emplace_back(float(p.first), float(y0), float(p.second));
    for (const Pt &p : pts)
        m.vertices.emplace_back(float(p.first), float(y1), float(p.second));
    for (const auto &t : tris)
        m.indices.emplace_back(t[0], t[1], t[2]);
    for (const auto &t : tris)
        m.indices.emplace_back(t[0] + n, t[2] + n, t[1] + n);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        m.indices.emplace_back(i, j + n, j);
        m.indices.emplace_back(i, i + n, j + n);
    }
    return m;
}

Poly circle_outline(double cx, double cy, double r, int n)
{
    Poly p;
    for (int i = 0; i < n; ++i)
        p.emplace_back(cx + r * std::cos(2 * PI * i / n), cy + r * std::sin(2 * PI * i / n));
    return p;
}

Mesh cylinder(double cx, double cy, double r, double z0, double z1, int segments = 48)
{
    return extrude(circle_outline(cx, cy, r, std::max(8, segments)), z0, z1);
}

// n points of a square of side 2*half, one per ray of a circle's n points (counter-clockwise).
Poly square_outline(double cx, double cy, double half, int n)
{
    Poly p;
    for (int i = 0; i < n; ++i) {
        const double a = 2 * PI * i / n, c = std::cos(a), s = std::sin(a);
        const double k = half / std::max(std::abs(c), std::abs(s));
        p.emplace_back(cx + k * c, cy + k * s);
    }
    return p;
}

// A flat ring between two outlines with the same number of points on the same rays.
Mesh ring_prism(const Poly &outer, const Poly &inner, double z0, double z1)
{
    const int n = int(outer.size());
    Mesh      m;
    for (const Pt &p : outer) m.vertices.emplace_back(float(p.first), float(p.second), float(z0));
    for (const Pt &p : inner) m.vertices.emplace_back(float(p.first), float(p.second), float(z0));
    for (const Pt &p : outer) m.vertices.emplace_back(float(p.first), float(p.second), float(z1));
    for (const Pt &p : inner) m.vertices.emplace_back(float(p.first), float(p.second), float(z1));
    const int o0 = 0, i0 = n, o1 = 2 * n, i1 = 3 * n;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        m.indices.emplace_back(o0 + i, i0 + i, i0 + j);
        m.indices.emplace_back(o0 + i, i0 + j, o0 + j);
        m.indices.emplace_back(o1 + i, o1 + j, i1 + j);
        m.indices.emplace_back(o1 + i, i1 + j, i1 + i);
        m.indices.emplace_back(o0 + i, o0 + j, o1 + j);
        m.indices.emplace_back(o0 + i, o1 + j, o1 + i);
        m.indices.emplace_back(i0 + i, i1 + i, i1 + j);
        m.indices.emplace_back(i0 + i, i1 + j, i0 + j);
    }
    return m;
}

struct Bounds
{
    double lo[3] = {1e30, 1e30, 1e30};
    double hi[3] = {-1e30, -1e30, -1e30};
    bool   empty() const { return lo[0] > hi[0]; }
    void   add(const Mesh &m)
    {
        for (const stl_vertex &v : m.vertices)
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], double(v[k]));
                hi[k] = std::max(hi[k], double(v[k]));
            }
    }
};

// ---------------------------------------------------------------------------------------------
// stroke font (text.py)
// ---------------------------------------------------------------------------------------------
struct Glyph
{
    char               ch;
    std::vector<float> d; // x, y pairs; 99 ends a stroke
};

const std::vector<Glyph> &glyphs()
{
    static const std::vector<Glyph> g = {
        {' ', {}},
        {'%', {0.f, 0.f, 4.f, 6.f, 99.f, 0.2f, 4.6f, 0.2f, 6.f, 1.4f, 6.f, 1.4f, 4.6f, 0.2f, 4.6f, 99.f, 2.6f, 1.4f, 2.6f, 0.f, 3.8f, 0.f, 3.8f, 1.4f, 2.6f, 1.4f, 99.f}},
        {'(', {3.f, 6.f, 1.5f, 4.5f, 1.5f, 1.5f, 3.f, 0.f, 99.f}},
        {')', {1.f, 6.f, 2.5f, 4.5f, 2.5f, 1.5f, 1.f, 0.f, 99.f}},
        {'*', {2.f, 1.f, 2.f, 5.f, 99.f, 0.5f, 2.f, 3.5f, 4.f, 99.f, 0.5f, 4.f, 3.5f, 2.f, 99.f}},
        {'+', {0.5f, 3.f, 3.5f, 3.f, 99.f, 2.f, 1.2f, 2.f, 4.8f, 99.f}},
        {',', {2.f, 0.6f, 1.5f, -0.8f, 99.f}},
        {'-', {0.5f, 3.f, 3.5f, 3.f, 99.f}},
        {'.', {2.f, 0.f, 2.f, 0.f, 99.f}},
        {'/', {0.f, 0.f, 4.f, 6.f, 99.f}},
        {'0', {0.f, 1.f, 0.f, 5.f, 1.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f}},
        {'1', {1.f, 5.f, 2.f, 6.f, 2.f, 0.f, 99.f, 1.f, 0.f, 3.f, 0.f, 99.f}},
        {'2', {0.f, 5.f, 1.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 4.f, 0.f, 0.f, 4.f, 0.f, 99.f}},
        {'3', {0.f, 5.f, 1.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 4.f, 3.f, 3.f, 1.5f, 3.f, 99.f, 3.f, 3.f, 4.f, 2.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f}},
        {'4', {3.f, 0.f, 3.f, 6.f, 0.f, 2.f, 4.f, 2.f, 99.f}},
        {'5', {4.f, 6.f, 0.f, 6.f, 0.f, 3.f, 3.f, 3.f, 4.f, 2.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f}},
        {'6', {4.f, 5.f, 3.f, 6.f, 1.f, 6.f, 0.f, 5.f, 0.f, 1.f, 1.f, 0.f, 3.f, 0.f, 4.f, 1.f, 4.f, 2.f, 3.f, 3.f, 0.f, 3.f, 99.f}},
        {'7', {0.f, 6.f, 4.f, 6.f, 1.f, 0.f, 99.f}},
        {'8', {1.f, 3.f, 0.f, 4.f, 0.f, 5.f, 1.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 4.f, 3.f, 3.f, 1.f, 3.f, 0.f, 2.f, 0.f, 1.f, 1.f, 0.f, 3.f, 0.f, 4.f, 1.f, 4.f, 2.f, 3.f, 3.f, 99.f}},
        {'9', {4.f, 3.f, 1.f, 3.f, 0.f, 4.f, 0.f, 5.f, 1.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f}},
        {':', {2.f, 1.f, 2.f, 1.f, 99.f, 2.f, 4.4f, 2.f, 4.4f, 99.f}},
        {'<', {3.5f, 5.f, 0.5f, 3.f, 3.5f, 1.f, 99.f}},
        {'=', {0.5f, 2.f, 3.5f, 2.f, 99.f, 0.5f, 4.f, 3.5f, 4.f, 99.f}},
        {'>', {0.5f, 5.f, 3.5f, 3.f, 0.5f, 1.f, 99.f}},
        {'A', {0.f, 0.f, 2.f, 6.f, 4.f, 0.f, 99.f, 0.8f, 2.f, 3.2f, 2.f, 99.f}},
        {'B', {0.f, 0.f, 0.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 4.f, 3.f, 3.f, 0.f, 3.f, 99.f, 3.f, 3.f, 4.f, 2.f, 4.f, 1.f, 3.f, 0.f, 0.f, 0.f, 99.f}},
        {'C', {4.f, 5.f, 3.f, 6.f, 1.f, 6.f, 0.f, 5.f, 0.f, 1.f, 1.f, 0.f, 3.f, 0.f, 4.f, 1.f, 99.f}},
        {'D', {0.f, 0.f, 0.f, 6.f, 2.5f, 6.f, 4.f, 4.5f, 4.f, 1.5f, 2.5f, 0.f, 0.f, 0.f, 99.f}},
        {'E', {4.f, 6.f, 0.f, 6.f, 0.f, 0.f, 4.f, 0.f, 99.f, 0.f, 3.f, 3.f, 3.f, 99.f}},
        {'F', {4.f, 6.f, 0.f, 6.f, 0.f, 0.f, 99.f, 0.f, 3.f, 3.f, 3.f, 99.f}},
        {'G', {4.f, 5.f, 3.f, 6.f, 1.f, 6.f, 0.f, 5.f, 0.f, 1.f, 1.f, 0.f, 3.f, 0.f, 4.f, 1.f, 4.f, 3.f, 2.5f, 3.f, 99.f}},
        {'H', {0.f, 0.f, 0.f, 6.f, 99.f, 4.f, 0.f, 4.f, 6.f, 99.f, 0.f, 3.f, 4.f, 3.f, 99.f}},
        {'I', {1.f, 6.f, 3.f, 6.f, 99.f, 2.f, 6.f, 2.f, 0.f, 99.f, 1.f, 0.f, 3.f, 0.f, 99.f}},
        {'J', {4.f, 6.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f}},
        {'K', {0.f, 0.f, 0.f, 6.f, 99.f, 4.f, 6.f, 0.f, 2.8f, 99.f, 1.2f, 3.8f, 4.f, 0.f, 99.f}},
        {'L', {0.f, 6.f, 0.f, 0.f, 4.f, 0.f, 99.f}},
        {'M', {0.f, 0.f, 0.f, 6.f, 2.f, 3.f, 4.f, 6.f, 4.f, 0.f, 99.f}},
        {'N', {0.f, 0.f, 0.f, 6.f, 4.f, 0.f, 4.f, 6.f, 99.f}},
        {'O', {0.f, 1.f, 0.f, 5.f, 1.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f}},
        {'P', {0.f, 0.f, 0.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 4.f, 3.f, 3.f, 0.f, 3.f, 99.f}},
        {'Q', {0.f, 1.f, 0.f, 5.f, 1.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f, 2.5f, 1.5f, 4.f, 0.f, 99.f}},
        {'R', {0.f, 0.f, 0.f, 6.f, 3.f, 6.f, 4.f, 5.f, 4.f, 4.f, 3.f, 3.f, 0.f, 3.f, 99.f, 2.f, 3.f, 4.f, 0.f, 99.f}},
        {'S', {4.f, 5.f, 3.f, 6.f, 1.f, 6.f, 0.f, 5.f, 0.f, 4.f, 1.f, 3.f, 3.f, 3.f, 4.f, 2.f, 4.f, 1.f, 3.f, 0.f, 1.f, 0.f, 0.f, 1.f, 99.f}},
        {'T', {0.f, 6.f, 4.f, 6.f, 99.f, 2.f, 6.f, 2.f, 0.f, 99.f}},
        {'U', {0.f, 6.f, 0.f, 1.f, 1.f, 0.f, 3.f, 0.f, 4.f, 1.f, 4.f, 6.f, 99.f}},
        {'V', {0.f, 6.f, 2.f, 0.f, 4.f, 6.f, 99.f}},
        {'W', {0.f, 6.f, 1.f, 0.f, 2.f, 3.f, 3.f, 0.f, 4.f, 6.f, 99.f}},
        {'X', {0.f, 6.f, 4.f, 0.f, 99.f, 4.f, 6.f, 0.f, 0.f, 99.f}},
        {'Y', {0.f, 6.f, 2.f, 3.f, 4.f, 6.f, 99.f, 2.f, 3.f, 2.f, 0.f, 99.f}},
        {'Z', {0.f, 6.f, 4.f, 6.f, 0.f, 0.f, 4.f, 0.f, 99.f}},
    };
    return g;
}

constexpr double ADVANCE = 5.4, GLYPH_W = 4.0, GLYPH_H = 6.0;
constexpr double LABEL_PIXEL = 0.8, LABEL_RAISE = 0.4;

void font_geometry(double pixel, double &unit, double &stroke)
{
    const double height = 7.0 * pixel;
    stroke = std::max(0.5, std::min(0.7, 0.11 * height));
    unit   = (height - stroke) / GLYPH_H;
}

double text_width(const std::string &text, double pixel)
{
    double unit, stroke;
    font_geometry(pixel, unit, stroke);
    if (text.empty())
        return 0.;
    return ((double(text.size()) - 1) * ADVANCE + GLYPH_W) * unit + stroke;
}

Mesh segment(Pt p, Pt q, double width, double z0, double z1)
{
    const double dx = q.first - p.first, dy = q.second - p.second, len = std::hypot(dx, dy), h = width / 2.;
    if (len < 1e-9)
        return extrude({{p.first - h, p.second - h}, {p.first + h, p.second - h}, {p.first + h, p.second + h}, {p.first - h, p.second + h}}, z0, z1);
    const double ux = dx / len, uy = dy / len, nx = -uy, ny = ux;
    return extrude({{p.first - ux * h + nx * h, p.second - uy * h + ny * h},
                    {q.first + ux * h + nx * h, q.second + uy * h + ny * h},
                    {q.first + ux * h - nx * h, q.second + uy * h - ny * h},
                    {p.first - ux * h - nx * h, p.second - uy * h - ny * h}},
                   z0, z1);
}

// Raised text between z0 and z1, lower-left corner at (x, y); align "center" centers it on x, "right" ends it at x.
Mesh text_mesh(const std::string &text, double pixel, double z0, double z1, double x, double y, const char *align = "left")
{
    double unit, stroke;
    font_geometry(pixel, unit, stroke);
    const double width = text_width(text, pixel);
    if (std::string(align) == "center")
        x -= width / 2.;
    else if (std::string(align) == "right")
        x -= width;
    Mesh         m;
    const double left = x + stroke / 2., base = y + stroke / 2.;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = char(std::toupper((unsigned char) text[i]));
        const Glyph *g = nullptr;
        for (const Glyph &gl : glyphs())
            if (gl.ch == ch) {
                g = &gl;
                break;
            }
        if (g == nullptr)
            continue;
        const double ox = left + double(i) * ADVANCE * unit;
        std::vector<Pt> line;
        auto flush = [&]() {
            if (line.size() == 1)
                add(m, segment(line[0], line[0], stroke, z0, z1));
            for (size_t k = 0; k + 1 < line.size(); ++k)
                add(m, segment(line[k], line[k + 1], stroke, z0, z1));
            line.clear();
        };
        for (size_t k = 0; k < g->d.size();) {
            if (g->d[k] > 90.f) {
                flush();
                ++k;
                continue;
            }
            line.emplace_back(ox + g->d[k] * unit, base + g->d[k + 1] * unit);
            k += 2;
        }
        flush();
    }
    return m;
}

// Text standing on a wall that faces -y: raised `depth` out of the face at y = y_face, centered on x, baseline z.
Mesh text_mesh_front(const std::string &text, double pixel, double y_face, double depth, double x, double z)
{
    Mesh flat = text_mesh(text, pixel, 0., depth, 0., 0.);
    if (flat.vertices.empty())
        return flat;
    const double x0 = x - text_width(text, pixel) / 2.;
    for (stl_vertex &v : flat.vertices) {
        const float px = v.x(), py = v.y(), pz = v.z();
        v = stl_vertex(float(x0 + px), float(y_face - pz), float(z + py));
    }
    return flat;
}

// ---------------------------------------------------------------------------------------------
// pieces (base.py / project.py)
// ---------------------------------------------------------------------------------------------
Part make_part(const std::string &name, Mesh mesh, Settings settings = {})
{
    Part p;
    p.name     = name;
    p.mesh     = std::move(mesh);
    p.settings = std::move(settings);
    return p;
}

Part label_part(const std::string &text, double width, double z0, double pixel = LABEL_PIXEL, double raise_by = LABEL_RAISE,
                double cx = 0., double y = 0., const std::string &name = std::string())
{
    const double tw = text_width(text, pixel);
    if (tw > width)
        pixel = std::max(0.5, pixel * width / tw);
    return make_part(name.empty() ? "label " + text : name, text_mesh(text, pixel, z0, z0 + raise_by, cx, y, "center"));
}

std::vector<Part> label_lines(const std::vector<std::string> &lines, double width, double z0, double cx, double y, double pixel = LABEL_PIXEL,
                              double gap = 1.2, double raise_by = LABEL_RAISE)
{
    std::vector<Part> parts;
    const double line_h = 7 * pixel + gap;
    for (size_t k = 0; k < lines.size(); ++k)
        parts.push_back(label_part(lines[lines.size() - 1 - k], width, z0, pixel, raise_by, cx, y + double(k) * line_h));
    return parts;
}

double snap_z(double z, const Env &env)
{
    const int n = std::max(0, int(std::round((z - env.first_layer) / env.layer_height)));
    return std::round((env.first_layer + n * env.layer_height) * 1e4) / 1e4;
}

// Centers the parts on (0, 0) with the lowest point at z = 0 (Obj.normalize) and records the size.
Obj piece(const std::string &name, std::vector<Part> parts, Settings settings = {}, Settings note = {})
{
    Obj o;
    o.name     = name;
    o.parts    = std::move(parts);
    o.settings = std::move(settings);
    o.note     = std::move(note);
    Bounds b;
    for (const Part &p : o.parts)
        b.add(p.mesh);
    if (!b.empty()) {
        const double dx = -(b.lo[0] + b.hi[0]) / 2., dy = -(b.lo[1] + b.hi[1]) / 2., dz = -b.lo[2];
        for (Part &p : o.parts)
            p.mesh = translated(std::move(p.mesh), dx, dy, dz);
        o.w = b.hi[0] - b.lo[0];
        o.d = b.hi[1] - b.lo[1];
        o.h = b.hi[2] - b.lo[2];
    }
    return o;
}

std::vector<Pt> grid_positions(size_t count, size_t cols, double cell_w, double cell_h, double gap)
{
    cols = std::max<size_t>(1, cols);
    const size_t rows    = (count + cols - 1) / cols;
    const double total_w = cols * cell_w + (cols - 1) * gap;
    const double total_h = rows * cell_h + (rows - 1) * gap;
    std::vector<Pt> out;
    for (size_t i = 0; i < count; ++i) {
        const size_t r = i / cols, c = i % cols;
        out.emplace_back(-total_w / 2. + cell_w / 2. + c * (cell_w + gap), total_h / 2. - cell_h / 2. - r * (cell_h + gap));
    }
    return out;
}

void place_grid(std::vector<Obj> &objects, size_t cols, double gap, double w = -1., double d = -1.)
{
    if (w < 0)
        for (const Obj &o : objects) w = std::max(w, o.w);
    if (d < 0)
        for (const Obj &o : objects) d = std::max(d, o.d);
    const auto pos = grid_positions(objects.size(), cols, w, d, gap);
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i].x = pos[i].first;
        objects[i].y = pos[i].second;
    }
}

Plate make_plate(const std::string &title, std::vector<Obj> objects, const Env &env)
{
    Plate p;
    p.title   = title;
    p.objects = std::move(objects);
    // recenter on the bed
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    for (const Obj &o : p.objects) {
        lo[0] = std::min(lo[0], o.x - o.w / 2.);
        hi[0] = std::max(hi[0], o.x + o.w / 2.);
        lo[1] = std::min(lo[1], o.y - o.d / 2.);
        hi[1] = std::max(hi[1], o.y + o.d / 2.);
    }
    const double dx = env.cx() - (lo[0] + hi[0]) / 2., dy = env.cy() - (lo[1] + hi[1]) / 2.;
    for (Obj &o : p.objects) {
        o.x += dx;
        o.y += dy;
    }
    if (hi[0] - lo[0] > env.bed_w + 1e-3 || hi[1] - lo[1] > env.bed_d + 1e-3)
        p.warnings.push_back(Slic3r::format(_u8L("The plate needs %1% x %2% mm and the bed is %3% x %4% mm: use fewer or smaller pieces."),
                                            fnum(hi[0] - lo[0], 0), fnum(hi[1] - lo[1], 0), fnum(env.bed_w, 0), fnum(env.bed_d, 0)));
    return p;
}

std::string range_text(double z0, double z1) { return fnum(z0) + " - " + fnum(z1) + " mm"; }

// A thin plate with lines of raised text (precision.py label_plate).
std::vector<Part> label_plate(const std::vector<std::string> &lines, double width, double x, double y0, double pixel = 0.8)
{
    constexpr double LABEL_PLATE = 0.6;
    const double     n      = double(lines.size());
    const double     height = n * 7 * pixel + (n + 1) * 1.2;
    std::vector<Part> parts = {make_part("label plate", box(x - width / 2., y0, 0, x + width / 2., y0 + height, LABEL_PLATE))};
    for (Part &p : label_lines(lines, width - 1.0, LABEL_PLATE, x, y0 + 1.2, pixel))
        parts.push_back(std::move(p));
    return parts;
}

double label_plate_height(size_t lines, double pixel = 0.8) { return double(lines) * 7 * pixel + (double(lines) + 1) * 1.2; }

// ---------------------------------------------------------------------------------------------
// towers (towers.py)
// ---------------------------------------------------------------------------------------------
constexpr double BASE_H = 1.2, TOWER_DEPTH = 14.0, TOWER_WIDTH = 24.0, FRONT_PIXEL = 0.6, FRONT_RAISE = 0.4;
const Settings   OPEN_WALLS = {{"sparse_infill_density", "0%"}, {"wall_loops", "2"}};

Poly segment_profile(double h)
{
    const double pocket = std::min(5.0, h / 2.), dz = std::max(2.0, std::min(6.0, h - 2.0));
    return {{0, 0}, {8, 0}, {8, pocket}, {16, pocket}, {16, 0}, {TOWER_WIDTH, 0}, {TOWER_WIDTH, h - dz}, {TOWER_WIDTH + dz, h}, {0, h}};
}

// A tower of bands (bridge + overhang + a pair of thin posts that string), one label per band on its front.
Obj feature_tower(const std::vector<std::string> &labels, const std::vector<double> &bounds, const std::string &name)
{
    std::vector<Part> parts = {make_part("plate", box(-3, -3, 0, 51, TOWER_DEPTH + 3, BASE_H))};
    Mesh              text;
    for (size_t i = 0; i < labels.size(); ++i) {
        const double z0 = bounds[i], z1 = bounds[i + 1];
        parts.push_back(make_part("band " + labels[i], translated(extrude_profile_y(segment_profile(z1 - z0), 0, TOWER_DEPTH), 0, 0, z0)));
        const double pocket = std::min(5.0, (z1 - z0) / 2.);
        add(text, text_mesh_front(labels[i], FRONT_PIXEL, 0.0, FRONT_RAISE, TOWER_WIDTH / 2., z0 + pocket + 0.4));
    }
    parts.push_back(make_part("labels", std::move(text)));
    Mesh posts;
    for (double x : {36.0, 44.0})
        add(posts, cylinder(x, TOWER_DEPTH / 2., 1.6, BASE_H, bounds.back(), 24));
    parts.push_back(make_part("posts", std::move(posts)));
    return piece(name, std::move(parts));
}

// A square column with a label every so often up its front face.
Obj square_column(double size, double height, Settings settings, const std::vector<std::pair<double, std::string>> &labels,
                  const std::string &name, double pixel = FRONT_PIXEL)
{
    std::vector<Part> parts = {make_part("column", box(0, 0, 0, size, size, height))};
    Mesh              text;
    for (const auto &l : labels)
        add(text, text_mesh_front(l.second, pixel, 0.0, FRONT_RAISE, size / 2., l.first));
    if (!text.vertices.empty())
        parts.push_back(make_part("labels", std::move(text)));
    return piece(name, std::move(parts), std::move(settings));
}

std::vector<double> boundaries_for(int count, double band, const Env &env)
{
    std::vector<double> b;
    for (int i = 0; i <= count; ++i)
        b.push_back(snap_z(BASE_H + i * band, env));
    return b;
}

// Height of the custom G-code that must run at the start of the first layer above `z` (a layer top): OrcaSlicer
// runs an item on the layer whose top is nearest to the item's height (ToolOrdering::assign_custom_gcodes).
double code_z_above(double z, const Env &env) { return std::round((z + env.layer_height) * 1e4) / 1e4; }

std::string pa_command(double k, const std::string &flavor)
{
    std::string f = flavor;
    std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (f.find("klipper") != std::string::npos)
        return "SET_PRESSURE_ADVANCE ADVANCE=" + fnum(k, 4);
    if (f.find("reprap") != std::string::npos || f.find("repetier") != std::string::npos)
        return "M572 D0 S" + fnum(k, 4);
    return "M900 K" + fnum(k, 4);
}

GCodeFlavor flavor_of(const std::string &name)
{
    const t_config_enum_values &map = ConfigOptionEnum<GCodeFlavor>::get_enum_values();
    auto it = map.find(name);
    return it == map.end() ? gcfMarlinLegacy : GCodeFlavor(it->second);
}

double mm3_per_mm(const Env &env)
{
    const double lw = env.line_width > 0 ? env.line_width : env.nozzle * 1.125;
    try {
        return Flow(float(lw), float(env.layer_height), float(env.nozzle)).mm3_per_mm() * (env.flow_ratio > 0 ? env.flow_ratio : 1.);
    } catch (...) {
        return lw * env.layer_height;
    }
}

// ---------------------------------------------------------------------------------------------
// the calibrations
// ---------------------------------------------------------------------------------------------
Plate build_z_offset(const Values &v, const Env &env)
{
    constexpr double PAD = 28.0, STRIP = 8.0, GAP = 6.0, MIN_NOZZLE = 0.05;
    const double h = v.n("pad_height") > 0 ? v.n("pad_height") : env.first_layer;
    const auto   offsets = frange(v.n("start"), v.n("step"), int(v.n("count")));
    const double low     = *std::min_element(offsets.begin(), offsets.end());
    if (h + low < MIN_NOZZLE)
        throw Error(Slic3r::format(_u8L("The lowest pad (%1% mm) would put the nozzle closer than %2% mm to the bed on a %3% mm layer."),
                                   signed_num(low), fnum(MIN_NOZZLE), fnum(h)));
    std::vector<Obj> objects;
    for (double off : offsets) {
        std::vector<Part> parts = {make_part("pad", box(0, 0, 0, PAD, PAD + STRIP, h)),
                                   label_part(signed_num(off), PAD - 2, h, LABEL_PIXEL, LABEL_RAISE, PAD / 2., PAD + (STRIP - 5.6) / 2.)};
        objects.push_back(piece("Z offset " + signed_num(off), std::move(parts), {{"object_z_offset", fnum(off, 4)}},
                                {{"Z offset", signed_num(off) + " mm"}}));
    }
    place_grid(objects, objects.size() <= 9 ? 3 : 4, GAP, PAD, PAD + STRIP);
    return make_plate("Z offset", std::move(objects), env);
}

Plate build_first_layer_tf(const Values &v, const Env &env)
{
    constexpr double STRIP = 9.0;
    const double h = v.n("pad_height") > 0 ? v.n("pad_height") : env.first_layer;
    std::vector<Obj> objects;
    for (double t : v.l("temps"))
        for (double s : v.l("speeds")) {
            std::vector<Part> parts = {make_part("pad", box(0, 0, 0, 26, 26 + STRIP, h)),
                                       label_part(fnum(t) + "/" + fnum(s), 24.0, h, LABEL_PIXEL, LABEL_RAISE, 13.0, 26 + (STRIP - 5.6) / 2.)};
            objects.push_back(piece("First layer " + fnum(t) + " C " + fnum(s) + " mm/s", std::move(parts),
                                    {{"object_initial_layer_temperature", fnum(std::round(t), 0)}, {"object_initial_layer_speed", fnum(s)}},
                                    {{"temperature", fnum(t) + " C"}, {"speed", fnum(s) + " mm/s"}}));
        }
    place_grid(objects, v.l("speeds").size(), 6.0);
    return make_plate("First layer temperature and speed", std::move(objects), env);
}

Plate build_first_layer_flow(const Values &v, const Env &env)
{
    constexpr double PAD = 30.0, STRIP = 8.0, GAP = 6.0;
    const double h = v.n("pad_height") > 0 ? v.n("pad_height") : env.first_layer;
    std::vector<Obj> objects;
    for (double r : frange(v.n("start"), v.n("step"), int(v.n("count")))) {
        std::vector<Part> parts = {make_part("pad", box(0, 0, 0, PAD, PAD + STRIP, h)),
                                   label_part(fnum(r, 3), PAD - 4, h, LABEL_PIXEL, LABEL_RAISE, PAD / 2., (STRIP - 5.6) / 2. + PAD)};
        objects.push_back(piece("flow " + fnum(r, 3), std::move(parts), {{"first_layer_flow_ratio", fnum(r, 4)}}));
    }
    place_grid(objects, 4, GAP, PAD, PAD + STRIP);
    return make_plate("First layer flow", std::move(objects), env);
}

Plate build_temp_tower(const Values &v, const Env &env)
{
    const int  count  = int(v.n("count"));
    const auto temps  = frange(v.n("start"), v.n("step"), count);
    const auto bounds = boundaries_for(count, v.n("band"), env);
    std::vector<std::string> labels;
    for (double t : temps)
        labels.push_back(fnum(t, 1));
    Plate p = make_plate("Temperature tower", {feature_tower(labels, bounds, "temperature tower")}, env);
    const GCodeFlavor flavor = flavor_of(env.flavor);
    // The first band prints at the filament temperature after the first layer: set it to the first value.
    p.filament_edits.push_back({"nozzle_temperature", fnum(std::round(temps[0]), 0)});
    for (int i = 1; i < count; ++i) {
        const unsigned t = unsigned(std::lround(temps[i]));
        LayerCode c;
        c.z    = code_z_above(bounds[i], env);
        c.text = GCodeWriter::set_temperature(t, flavor, false) + GCodeWriter::set_temperature(t, flavor, true);
        p.gcodes.push_back(c);
    }
    for (int i = 0; i < count; ++i)
        p.extra_legend.push_back({fnum(temps[i], 1) + " C", range_text(bounds[i], bounds[i + 1])});
    return p;
}

Plate build_pressure_advance(const Values &v, const Env &env)
{
    const double lo = v.n("start"), hi = v.n("end"), size = v.n("size");
    const int    n  = std::max(1, int(std::round((v.n("height") - env.first_layer) / env.layer_height)));
    const double height = snap_z(env.first_layer + n * env.layer_height, env);
    auto k_at = [&](double z) {
        const double t = height > env.first_layer ? (z - env.first_layer) / (height - env.first_layer) : 0.;
        return lo + (hi - lo) * std::min(1.0, std::max(0.0, t));
    };
    std::vector<std::pair<double, std::string>> labels;
    for (int mark = 1; mark <= int(height / 5.0); ++mark)
        labels.emplace_back(mark * 5.0 - 3.0, fnum(k_at(mark * 5.0), 3));
    Plate p = make_plate("Pressure advance", {square_column(size, height, OPEN_WALLS, labels, "pressure advance tower")}, env);
    for (int i = 0; i < n; ++i) {
        LayerCode c;
        c.z    = std::round((env.first_layer + i * env.layer_height) * 1e4) / 1e4;
        c.text = pa_command(k_at(c.z), env.flavor);
        p.gcodes.push_back(c);
    }
    // the filament's own (adaptive) pressure advance would be sent again on role changes
    p.filament_edits.push_back({"adaptive_pressure_advance", "0"});
    for (double f : {0.0, 0.25, 0.5, 0.75, 1.0})
        p.extra_legend.push_back({fnum(height * f, 1) + " mm", "K " + fnum(k_at(height * f), 4)});
    return p;
}

Plate build_cooling(const Values &v, const Env &env)
{
    std::vector<int> fans;
    for (double f : v.l("fans"))
        fans.push_back(int(std::lround(f)));
    const double band = v.n("band");
    std::vector<double> bounds = {BASE_H};
    for (size_t i = 0; i < fans.size(); ++i)
        bounds.push_back(snap_z(double(i + 1) * band, env));
    for (size_t i = 1; i < bounds.size(); ++i)
        if (bounds[i] <= bounds[i - 1] + 1.0)
            throw Error(_u8L("The bands are too low: raise the band height."));
    std::vector<std::string> labels;
    for (int f : fans)
        labels.push_back(std::to_string(f) + "%");
    Plate p = make_plate("Cooling", {feature_tower(labels, bounds, "cooling tower")}, env);
    const GCodeFlavor flavor = flavor_of(env.flavor);
    for (size_t i = 0; i < fans.size(); ++i) {
        LayerCode c;
        c.z    = code_z_above(snap_z(bounds[i], env), env);
        c.text = GCodeWriter::set_fan(flavor, unsigned(std::max(0, std::min(100, fans[i]))));
        p.gcodes.push_back(c);
        p.extra_legend.push_back({std::to_string(fans[i]) + "%", range_text(bounds[i], bounds[i + 1])});
    }
    // A constant fan from the slicer, so that it never sends its own fan command over the bands' ones.
    p.filament_edits = {{"fan_min_speed", "100"},           {"fan_max_speed", "100"},          {"overhang_fan_speed", "100"},
                        {"internal_bridge_fan_speed", "-1"}, {"ironing_fan_speed", "-1"},       {"support_material_interface_fan_speed", "-1"},
                        {"close_fan_the_first_x_layers", "1"}, {"full_fan_speed_layer", "0"}};
    return p;
}

Plate build_vfa(const Values &v, const Env &env)
{
    const double start = v.n("start"), step = v.n("step"), band = v.n("band");
    const int    count  = int(v.n("count"));
    const auto   speeds = frange(start, step, count);
    const double height = count * band;
    std::vector<std::pair<double, std::string>> labels;
    for (int i = 0; i < count; ++i)
        labels.emplace_back(i * band + 1.0, fnum(speeds[i]));
    Obj tower = square_column(20.0, height, OPEN_WALLS, labels, "VFA tower", 0.8);
    for (int i = 0; i < count; ++i) {
        Range r;
        r.lo       = i * band;
        r.hi       = (i + 1) * band;
        r.settings = {{"outer_wall_speed", fnum(speeds[i])}, {"inner_wall_speed", fnum(speeds[i])}};
        tower.ranges.push_back(r);
    }
    Plate p = make_plate("VFA", {std::move(tower)}, env);
    for (int i = 0; i < count; ++i)
        p.extra_legend.push_back({fnum(speeds[i]) + " mm/s", range_text(i * band, (i + 1) * band)});
    const double mm3 = mm3_per_mm(env);
    if (env.max_volumetric > 0 && mm3 > 0 && speeds.back() * mm3 > env.max_volumetric)
        p.warnings.push_back(Slic3r::format(_u8L("The filament's max volumetric speed (%1% mm3/s) limits the walls to about %2% mm/s: the faster bands print at that speed."),
                                            fnum(env.max_volumetric, 1), fnum(env.max_volumetric / mm3, 0)));
    return p;
}

Plate build_max_flow(const Values &v, const Env &env)
{
    const double lo = v.n("start"), hi = v.n("end"), height = v.n("height"), size = v.n("size");
    if (hi <= lo)
        throw Error(_u8L("The last value must be greater than the first one."));
    auto flow = [&](double z) { return lo + (hi - lo) * std::min(1.0, z / height); };
    std::vector<std::pair<double, std::string>> labels;
    std::vector<LegendRow>                      legend;
    for (int i = 1; i <= int(height / 10.0); ++i) {
        labels.emplace_back(i * 10.0 - 3.0, fnum(flow(i * 10.0), 1));
        legend.push_back({fnum(i * 10.0) + " mm", fnum(flow(i * 10.0), 1) + " mm3/s"});
    }
    const double lw = env.line_width > 0 ? env.line_width : env.nozzle * 1.125;
    Obj tower = square_column(size, height,
                              {{"sparse_infill_density", "0%"}, {"wall_loops", "1"}, {"outer_wall_line_width", fnum(lw, 3)},
                               {"enable_overhang_speed", "0"}},
                              labels, "max flow tower", 0.8);
    // The flow ramp as height ranges of wall speed: speed = flow / (mm3 per mm of wall).
    const double mm3   = mm3_per_mm(env);
    const int    steps = std::max(1, int(std::ceil(height / std::max(1.0, height / 60.0))));
    const double band  = height / steps;
    for (int i = 0; i < steps; ++i) {
        Range r;
        r.lo = i * band;
        r.hi = (i + 1) * band;
        const double speed = flow((r.lo + r.hi) / 2.) / mm3;
        r.settings = {{"outer_wall_speed", fnum(speed, 1)}, {"inner_wall_speed", fnum(speed, 1)}};
        tower.ranges.push_back(r);
    }
    Plate p = make_plate("Max volumetric flow", {std::move(tower)}, env);
    p.extra_legend   = legend;
    p.filament_edits = {{"filament_max_volumetric_speed", fnum(std::max(env.max_volumetric, hi * 1.5), 1)}, {"slow_down_layer_time", "0"}};
    p.print_edits    = {{"max_volumetric_extrusion_rate_slope", "0"}};
    return p;
}

Plate build_retraction(const Values &v, const Env &env)
{
    constexpr double POST_GAP = 36.0, POST_R = 3.0, POST_H = 30.0, PLATE_H = 0.8;
    std::vector<Obj> objects;
    for (double len : frange(v.n("start"), v.n("step"), int(v.n("count")))) {
        const double width = POST_GAP + 2 * POST_R + 8;
        std::vector<Part> parts = {make_part("plate", box(0, 0, 0, width, 2 * POST_R + 6 + 9, PLATE_H))};
        Mesh posts = cylinder(4 + POST_R, 3 + POST_R + 9, POST_R, PLATE_H, POST_H);
        add(posts, cylinder(4 + POST_R + POST_GAP, 3 + POST_R + 9, POST_R, PLATE_H, POST_H));
        parts.push_back(make_part("posts", std::move(posts)));
        parts.push_back(label_part(fnum(len, 2), width - 4, PLATE_H, LABEL_PIXEL, LABEL_RAISE, width / 2., 1.6));
        Settings s = {{"object_retraction_length", fnum(len, 3)}};
        if (v.n("speed") > 0)
            s.push_back({"object_retraction_speed", fnum(v.n("speed"))});
        objects.push_back(piece("retraction " + fnum(len, 2), std::move(parts), s, {{"retraction", fnum(len, 2) + " mm"}}));
    }
    place_grid(objects, 2, 6.0);
    Plate p = make_plate("Retraction", std::move(objects), env);
    p.printer_edits = {{"use_firmware_retraction", "0"}};
    return p;
}

Poly arch_profile(double span, double leg_h, double deck, double leg = 6.0)
{
    return {{0, 0}, {leg, 0}, {leg, leg_h}, {leg + span, leg_h}, {leg + span, 0}, {2 * leg + span, 0}, {2 * leg + span, leg_h + deck}, {0, leg_h + deck}};
}

Plate build_bridge_flow(const Values &v, const Env &env)
{
    constexpr double LEG = 6.0, LEG_H = 8.0, DECK = 1.2, DEPTH = 18.0, GAP = 6.0;
    const double span = v.n("span");
    std::vector<Obj> objects;
    for (double r : frange(v.n("start"), v.n("step"), int(v.n("count")))) {
        const double width = 2 * LEG + span;
        std::vector<Part> parts = {make_part("bridge", extrude_profile_y(arch_profile(span, LEG_H, DECK), 0, DEPTH)),
                                   label_part(fnum(r, 3), width - 4, LEG_H + DECK, LABEL_PIXEL, LABEL_RAISE, width / 2., (DEPTH - 5.6) / 2.)};
        objects.push_back(piece("bridge " + fnum(r, 3), std::move(parts), {{"bridge_flow", fnum(r, 4)}}));
    }
    place_grid(objects, objects.size() > 4 ? size_t(3) : objects.size(), GAP);
    return make_plate("Bridge flow", std::move(objects), env);
}

Plate build_overhang(const Values &v, const Env &env)
{
    constexpr double THICK = 2.4, DEPTH = 12.0, STRIP = 9.0, GAP = 5.0;
    const double length = v.n("length");
    std::vector<Obj> objects;
    for (double a : v.l("angles")) {
        const double s = std::sin(a * PI / 180.), c = std::cos(a * PI / 180.);
        const double reach = length * s, width = THICK + reach + 4.0;
        const Poly arm = {{0, BASE_H}, {THICK, BASE_H}, {THICK + length * s, BASE_H + length * c}, {length * s, BASE_H + length * c}};
        std::vector<Part> parts = {make_part("plate", box(0, 0, 0, width, DEPTH + STRIP, BASE_H)),
                                   make_part("wall", extrude_profile_y(arm, STRIP, STRIP + DEPTH)),
                                   label_part(fnum(a, 3), width - 2, BASE_H, LABEL_PIXEL, LABEL_RAISE, width / 2., (STRIP - 5.6) / 2.)};
        objects.push_back(piece("overhang " + fnum(a, 3), std::move(parts), {{"enable_support", "0"}}));
    }
    place_grid(objects, objects.size() > 6 ? size_t(4) : objects.size(), GAP);
    return make_plate("Overhang", std::move(objects), env);
}

Plate build_support_z(const Values &v, const Env &env)
{
    constexpr double LEG_H = 8.0, DECK = 2.4, DEPTH = 16.0, GAP = 12.0;
    const double span = v.n("span");
    std::vector<Obj> objects;
    for (double z : frange(v.n("start"), v.n("step"), int(v.n("count")))) {
        const double width = 12 + span;
        std::vector<Part> parts = {make_part("table", extrude_profile_y(arch_profile(span, LEG_H, DECK), 0, DEPTH)),
                                   label_part(fnum(z, 3), width - 4, LEG_H + DECK, LABEL_PIXEL, LABEL_RAISE, width / 2., (DEPTH - 5.6) / 2.)};
        objects.push_back(piece("support z " + fnum(z, 3), std::move(parts), {{"enable_support", "1"}, {"support_top_z_distance", fnum(z, 4)}}));
    }
    place_grid(objects, objects.size() > 4 ? size_t(3) : objects.size(), GAP);
    return make_plate("Support Z distance", std::move(objects), env);
}

Plate build_ringing(const Values &v, const Env &env)
{
    const Poly STEP_WALL = {{0, 0}, {50, 0}, {50, 12}, {30, 12}, {30, 26}, {0, 26}};
    std::vector<Obj> objects;
    for (double a : v.l("accels"))
        for (double s : v.l("speeds")) {
            std::vector<Part> parts = {make_part("wall", extrude(STEP_WALL, 0, v.n("height")))};
            for (Part &p : label_plate({fnum(s) + " MM/S", fnum(a)}, 40.0, 25.0, -16.0))
                parts.push_back(std::move(p));
            objects.push_back(piece("ringing " + fnum(s) + "/" + fnum(a), std::move(parts),
                                    {{"outer_wall_speed", fnum(s)}, {"inner_wall_speed", fnum(s)}, {"outer_wall_acceleration", fnum(a)}},
                                    {{"speed", fnum(s) + " mm/s"}, {"acceleration", fnum(a) + " mm/s2"}}));
        }
    place_grid(objects, v.l("speeds").size(), 6.0);
    return make_plate("Ringing", std::move(objects), env);
}

Plate build_seam_width(const Values &v, const Env &env)
{
    const std::vector<std::pair<std::string, std::string>> seams = {{"nearest", "NEAREST"}, {"aligned", "ALIGNED"}, {"back", "BACK"}, {"random", "RANDOM"}};
    const double h = v.n("height");
    std::vector<Obj> objects;
    for (const auto &s : seams) {
        std::vector<Part> parts = {make_part("cylinder", cylinder(0, 0, 10.0, 0, h, 64))};
        for (Part &p : label_plate({s.second}, 24.0, 0.0, -22.0))
            parts.push_back(std::move(p));
        objects.push_back(piece("seam " + s.first, std::move(parts), {{"seam_position", s.first}, {"wall_loops", "2"}}, {{"seam", s.first}}));
    }
    for (double w : v.l("widths")) {
        const std::string pct = fnum(w) + "%";
        std::vector<Part> parts = {make_part("tube", box(0, 0, 0, 20, 20, h))};
        for (Part &p : label_plate({pct}, 20.0, 10.0, -12.0))
            parts.push_back(std::move(p));
        objects.push_back(piece("width " + pct, std::move(parts),
                                {{"wall_loops", "1"}, {"sparse_infill_density", "0%"}, {"top_shell_layers", "0"}, {"top_shell_thickness", "0"},
                                 {"outer_wall_line_width", pct}, {"inner_wall_line_width", pct}},
                                {{"line width", fnum(w) + " % of the nozzle"}}));
    }
    place_grid(objects, std::max(seams.size(), v.l("widths").size()), 6.0);
    return make_plate("Seam and line width", std::move(objects), env);
}

Plate build_dimensional(const Values &v, const Env &env)
{
    constexpr double BLOCK_H = 8.0;
    constexpr int    RING    = 96;
    const double size = v.n("size"), dia = v.n("diameter");
    if (dia > size - 2.0)
        throw Error(_u8L("The hole must be at least 2 mm narrower than the block."));
    auto block = [&](const std::string &name, Settings settings, const std::vector<std::string> &lines, Settings note) {
        std::vector<Part> parts = {make_part("block", ring_prism(square_outline(0, 0, size / 2., RING), circle_outline(0, 0, dia / 2., RING), 0, BLOCK_H))};
        for (Part &p : label_plate(lines, size, 0.0, -size / 2. - 2.0 - label_plate_height(lines.size())))
            parts.push_back(std::move(p));
        return piece(name, std::move(parts), std::move(settings), std::move(note));
    };
    std::vector<Obj> objects;
    for (double h : v.l("hole"))
        for (double c : v.l("contour"))
            objects.push_back(block("block C" + signed_num(c) + " H" + signed_num(h),
                                    {{"xy_contour_compensation", fnum(c, 3)}, {"xy_hole_compensation", fnum(h, 3)}},
                                    {"C" + signed_num(c), "H" + signed_num(h)},
                                    {{"contour", signed_num(c) + " mm"}, {"hole", signed_num(h) + " mm"}}));
    for (double f : v.l("foot"))
        objects.push_back(block("block EF" + fnum(f, 2), {{"elefant_foot_compensation", fnum(f, 3)}}, {"EF", fnum(f, 2)},
                                {{"elephant foot", fnum(f, 2) + " mm"}}));
    place_grid(objects, v.l("contour").size(), 5.0);
    return make_plate("Dimensional", std::move(objects), env);
}

Plate build_tolerance(const Values &v, const Env &env)
{
    constexpr int RING = 96;
    const double  size = v.n("size"), plate = size + 8.0;
    std::vector<Obj> objects;
    for (double c : frange(v.n("start"), v.n("step"), int(v.n("count")))) {
        const double peg = size - 2 * c;
        if (peg < 1.0)
            throw Error(_u8L("The clearance leaves no peg: lower it or make the pegs bigger."));
        std::vector<Part> parts = {
            make_part("square hole", ring_prism(square_outline(0, 0, plate / 2., RING), square_outline(0, 0, size / 2., RING), 0, 5.0)),
            make_part("round hole", translated(ring_prism(square_outline(0, 0, plate / 2., RING), circle_outline(0, 0, size / 2., RING), 0, 5.0), plate + 4.0, 0, 0)),
            make_part("square peg", box(plate * 2 + 12 - peg / 2., -peg / 2., 0, plate * 2 + 12 + peg / 2., peg / 2., 8.0)),
            make_part("round peg", cylinder(plate * 2 + 12 + size + 4.0, 0, peg / 2., 0, 8.0, 64))};
        for (Part &p : label_plate({fnum(c, 2)}, plate * 2 + 4.0, plate + 2.0, -plate / 2. - 12.0))
            parts.push_back(std::move(p));
        objects.push_back(piece("fit " + fnum(c, 2), std::move(parts), {}, {{"clearance per side", fnum(c, 2) + " mm"}}));
    }
    place_grid(objects, 2, 6.0);
    return make_plate("Tolerance", std::move(objects), env);
}

// ---- sheets (sheet.py / sheets.py) ------------------------------------------------------------
struct Cell
{
    std::string name;
    Settings    settings;
    std::string label;
};

struct SheetOptions
{
    double                   base_h = 1.2, gap = 4.0;
    bool                     round = false, label_under = true;
    std::vector<std::string> top_labels, left_labels;
    Settings                 base_settings;
};

Obj cell_sheet(const std::string &name, const std::vector<Cell> &cells, size_t cols, double size, double height, const SheetOptions &o)
{
    constexpr double STRIP = 8.0, MARGIN = 4.0;
    cols = std::max<size_t>(1, cols);
    const size_t rows  = (cells.size() + cols - 1) / cols;
    const double under = o.label_under ? STRIP : 0.;
    const double top   = o.top_labels.empty() ? 0. : STRIP;
    double       left  = 0.;
    for (const std::string &t : o.left_labels)
        left = std::max(left, text_width(t, LABEL_PIXEL) + 3.0);
    const double cw = size, ch = size + under;
    const double width = left + cols * cw + (cols - 1) * o.gap + 2 * MARGIN;
    const double depth = top + rows * ch + (rows - 1) * o.gap + 2 * MARGIN;
    std::vector<Part> parts = {make_part("base", box(0, 0, 0, width, depth, o.base_h), o.base_settings)};
    Mesh              text;
    const double      x0 = MARGIN + left, ytop = depth - MARGIN - top;
    for (size_t i = 0; i < cells.size(); ++i) {
        const size_t r = i / cols, c = i % cols;
        const double cx = x0 + c * (cw + o.gap) + size / 2., cy_top = ytop - r * (ch + o.gap), cy = cy_top - size / 2.;
        Mesh mesh = o.round ? cylinder(cx, cy, size / 2., o.base_h, o.base_h + height)
                            : box(cx - size / 2., cy - size / 2., o.base_h, cx + size / 2., cy + size / 2., o.base_h + height);
        parts.push_back(make_part(cells[i].name, std::move(mesh), cells[i].settings));
        if (o.label_under && !cells[i].label.empty())
            add(text, label_part(cells[i].label, size, o.base_h, LABEL_PIXEL, LABEL_RAISE, cx, cy_top - size - under + (under - 5.6) / 2.).mesh);
    }
    for (size_t c = 0; c < o.top_labels.size(); ++c)
        add(text, label_part(o.top_labels[c], size, o.base_h, LABEL_PIXEL, LABEL_RAISE, x0 + c * (cw + o.gap) + size / 2.,
                             depth - MARGIN - STRIP + (STRIP - 5.6) / 2.).mesh);
    for (size_t r = 0; r < o.left_labels.size(); ++r)
        add(text, label_part(o.left_labels[r], left, o.base_h, LABEL_PIXEL, LABEL_RAISE, MARGIN + left / 2. - 1.0,
                             ytop - r * (ch + o.gap) - size / 2. - 2.8).mesh);
    if (!text.vertices.empty())
        parts.push_back(make_part("labels", std::move(text)));
    return piece(name, std::move(parts));
}

struct PatternName
{
    const char *value;
    const char *label; // printed on the sheet
};

const std::vector<PatternName> &infill_patterns()
{
    static const std::vector<PatternName> p = {
        {"concentric", "Concentric"}, {"grid", "Grid"},          {"cubic", "Cubic"},
        {"triangles", "Triangles"},   {"tri-hexagon", "Tri-hex"}, {"gyroid", "Gyroid"},
        {"honeycomb", "Honeycomb"},   {"adaptivecubic", "Adaptive"}, {"3dhoneycomb", "3D honey"},
        {"hilbertcurve", "Hilbert"},  {"archimedeanchords", "Archimed"}, {"octagramspiral", "Octagram"},
        {"lightning", "Lightning"},   {"crosshatch", "Crosshatch"}, {"crosszag", "Cross zag"},
        {"lateral-lattice", "Lattice"}};
    return p;
}

bool infill_pattern_known(const std::string &value)
{
    const t_config_enum_values &map = ConfigOptionEnum<InfillPattern>::get_enum_values();
    return map.find(value) != map.end();
}

const Settings OPEN_TOP = {{"top_shell_layers", "0"}, {"top_shell_thickness", "0"}};

Settings with(Settings base, const Settings &more)
{
    for (const auto &kv : more)
        base.push_back(kv);
    return base;
}

std::string pct(double value) { return fnum(value, 2) + "%"; }

Plate build_infill_pattern(const Values &v, const Env &env)
{
    std::vector<Cell> cells;
    for (const PatternName &p : infill_patterns())
        if (infill_pattern_known(p.value))
            cells.push_back({p.label, with(OPEN_TOP, {{"sparse_infill_pattern", p.value}, {"sparse_infill_density", pct(v.n("density"))}}), p.label});
    return make_plate("Infill patterns", {cell_sheet("Infill patterns", cells, 4, v.n("size"), v.n("height"), SheetOptions())}, env);
}

Plate build_infill_density(const Values &v, const Env &env)
{
    std::vector<Cell> cells;
    for (double d : v.l("densities"))
        cells.push_back({pct(d), with(OPEN_TOP, {{"sparse_infill_pattern", v.s("pattern")}, {"sparse_infill_density", pct(d)}}), pct(d)});
    return make_plate("Infill density", {cell_sheet("Infill density", cells, cells.size() > 3 ? size_t(3) : cells.size(), v.n("size"), v.n("height"), SheetOptions())}, env);
}

Plate build_top_surface(const Values &v, const Env &env)
{
    const std::vector<PatternName> tops = {{"monotonic", "Monotonic"}, {"monotonicline", "Mono line"}, {"concentric", "Concentric"},
                                           {"rectilinear", "Rectilinear"}, {"alignedrectilinear", "Aligned"}, {"hilbertcurve", "Hilbert"},
                                           {"archimedeanchords", "Archimed"}, {"octagramspiral", "Octagram"}};
    std::vector<Cell> cells;
    for (const PatternName &p : tops)
        if (infill_pattern_known(p.value))
            cells.push_back({p.label, {{"top_surface_pattern", p.value}}, p.label});
    return make_plate("Top surface patterns", {cell_sheet("Top surface patterns", cells, 4, v.n("size"), v.n("height"), SheetOptions())}, env);
}

Plate build_ironing(const Values &v, const Env &env)
{
    std::vector<Cell> cells;
    for (double flow : v.l("flows"))   // rows = flow, columns = speed
        for (double speed : v.l("speeds"))
            cells.push_back({fnum(speed) + "-" + fnum(flow),
                             {{"ironing_type", "topmost"}, {"ironing_speed", fnum(speed)}, {"ironing_flow", fnum(flow, 2) + "%"}},
                             ""});
    SheetOptions o;
    o.gap         = 3.0;
    o.round       = true;
    o.label_under = false;
    for (double s : v.l("speeds"))
        o.top_labels.push_back(fnum(s));
    for (double f : v.l("flows"))
        o.left_labels.push_back(fnum(f) + "%");
    o.base_settings = {{"ironing_type", "no ironing"}};
    return make_plate("Ironing", {cell_sheet("Ironing", cells, v.l("speeds").size(), v.n("size"), v.n("height"), o)}, env);
}

// ---- colour change (color_change.py) ----------------------------------------------------------
const std::vector<int> &color_sequence(int n)
{
    static const std::vector<int> s2 = {1, 2, 1}, s3 = {1, 2, 3, 1, 3, 2, 1}, s4 = {1, 2, 3, 4, 1, 3, 2, 4, 3, 1, 4, 2, 1};
    return n <= 2 ? s2 : n == 3 ? s3 : s4;
}

Plate build_color_change(const Values &v, const Env &env)
{
    const std::string mode = v.s("mode");
    const int         n    = int(v.n("colors"));
    const auto       &order = color_sequence(n);
    if (mode != "manual" && env.filaments < n)
        throw Error(Slic3r::format(_u8L("This test needs %1% filaments in the project and there are %2%: add filaments first, or use the one-extruder mode."),
                                   n, env.filaments));
    if (mode == "strips") {
        constexpr double TICK = 0.2;
        const double length = v.n("strip_length"), width = v.n("strip_width");
        const double thick  = snap_z(std::max(0.4, env.first_layer + env.layer_height), env);
        std::vector<Obj> objects;
        char             buf[16];
        for (size_t i = 0; i < order.size(); ++i) {
            const int color = order[i];
            snprintf(buf, sizeof(buf), "%02d", int(i + 1));
            std::vector<Part> parts;
            std::string       name;
            Settings          note;
            if (i == 0) { // a thin bar in the first colour: the nozzle starts clean
                name  = std::string(buf) + " start " + std::to_string(color);
                parts = {make_part("bar", box(0, 0, 0, width, 10.0, thick))};
                note  = {{"transition", "start with " + std::to_string(color)}};
            } else {
                const std::string tr = std::to_string(order[i - 1]) + ">" + std::to_string(color);
                name  = std::string(buf) + " " + tr;
                parts = {make_part("strip", box(0, 0, 0, width, length, thick))};
                Mesh scale;
                for (int k = 1; k <= int((length - 1.0) / 10.); ++k) {
                    const double mm = k * 10., sz = (k * 10) % 50 == 0 ? 6.0 : 3.0;
                    add(scale, box(0.0, mm - 0.4, thick, sz, mm + 0.4, thick + TICK));
                    add(scale, box(width - sz, mm - 0.4, thick, width, mm + 0.4, thick + TICK));
                }
                parts.push_back(make_part("scale", std::move(scale)));
                parts.push_back(label_part(tr, width - 2, thick, LABEL_PIXEL, TICK, width / 2., length - 9.0, "label"));
                note = {{"transition", std::to_string(order[i - 1]) + " -> " + std::to_string(color)}};
            }
            objects.push_back(piece(name, std::move(parts), {{"extruder", std::to_string(color)}}, note));
        }
        double w = 0., d = 0.;
        for (const Obj &o : objects) {
            w = std::max(w, o.w);
            d = std::max(d, o.d);
        }
        size_t rows = 1;
        while (((objects.size() + rows - 1) / rows) * (w + 4.0) > env.bed_w - 20.0 && rows < objects.size())
            ++rows;
        place_grid(objects, (objects.size() + rows - 1) / rows, 4.0, w, d);
        Plate p     = make_plate("Colour change strips", std::move(objects), env);
        p.by_object = true;
        return p;
    }
    // tower / manual
    const bool   multi = mode == "tower";
    const double size  = std::max(v.n("strip_width"), 20.0);
    std::vector<double> bounds = {BASE_H};
    for (size_t i = 0; i < order.size(); ++i)
        bounds.push_back(snap_z(BASE_H + double(i + 1) * v.n("band"), env));
    std::vector<Part>      parts;
    std::vector<LegendRow> legend;
    std::vector<LayerCode> codes;
    for (size_t i = 0; i < order.size(); ++i) {
        const int    color = order[i];
        const double z0 = bounds[i], z1 = bounds[i + 1];
        Settings     ext;
        if (multi)
            ext = {{"extruder", std::to_string(color)}};
        if (i == 0)
            parts.push_back(make_part("plate", box(0, 0, 0, size, size, z0), ext));
        parts.push_back(make_part("band " + std::to_string(i + 1) + ": filament " + std::to_string(color), box(0, 0, z0, size, size, z1), ext));
        const std::string tag = i ? std::to_string(order[i - 1]) + ">" + std::to_string(color) : std::to_string(color);
        parts.push_back(make_part("label " + tag, text_mesh_front(tag, 0.6, 0.0, 0.4, size / 2., z0 + 0.6), ext));
        legend.push_back({std::to_string(i + 1) + ". " + (i ? std::to_string(order[i - 1]) + " -> " + std::to_string(color) : "start with " + std::to_string(color)),
                          range_text(z0, z1)});
        if (!multi && i) {
            LayerCode c;
            c.z    = code_z_above(bounds[i], env);
            c.type = CodeType::Pause;
            c.text = "swap to filament " + std::to_string(color);
            codes.push_back(c);
        }
    }
    Plate p        = make_plate("Colour change", {piece("color change tower", std::move(parts))}, env);
    p.gcodes       = codes;
    p.extra_legend = legend;
    return p;
}

// ---------------------------------------------------------------------------------------------
// catalog
// ---------------------------------------------------------------------------------------------
Param FltP(const char *key, double def, double mn, double mx, double step, const char *unit = "")
{
    Param p;
    p.key  = key;
    p.kind = Kind::Float;
    p.def  = def;
    p.min  = mn;
    p.max  = mx;
    p.step = step;
    p.unit = unit;
    return p;
}

Param IntP(const char *key, double def, double mn, double mx)
{
    Param p = FltP(key, def, mn, mx, 1);
    p.kind  = Kind::Int;
    return p;
}

Param ListP(const char *key, std::vector<double> def, double mn, double mx, const char *unit = "")
{
    Param p    = FltP(key, 0, mn, mx, 0, unit);
    p.kind     = Kind::Floats;
    p.def_list = std::move(def);
    return p;
}

Param SelP(const char *key, const char *def, std::vector<Option> options)
{
    Param p;
    p.key     = key;
    p.kind    = Kind::Select;
    p.def_str = def;
    p.options = std::move(options);
    return p;
}

std::vector<Option> infill_pattern_options()
{
    std::vector<Option> out;
    for (const PatternName &p : infill_patterns())
        out.push_back({p.value, p.label});
    return out;
}

struct Text
{
    const char *id;
    const char *name;
    const char *desc;
    const char *how;
};

const std::vector<Text> &texts()
{
    static const std::vector<Text> t = {
        {"first_layer_flow", L("First layer flow"), L("One-layer pads, each printed with a different first layer flow ratio."), L("Let the plate cool, look at the TOP of the pads without peeling them. Too little flow leaves gaps between lines; too much makes ridges, a rough surface and a raised edge. Pick the pad that is smooth and closed, with no gaps and no ridges. Set its number as the first layer flow ratio (initial_layer_flow_ratio) in the process settings.")},
        {"z_offset", L("Z offset ladder"), L("Pads printed with the nozzle a little closer or farther from the bed, with the same plastic."), L("Look at the top of each pad. Too close (negative) squashes the lines into ridges and a rough, glossy-streaked top; too far (positive) leaves round lines with gaps. The best pad is smooth, closed and slightly shiny with no ridges. Add its value to your Z offset: on Bambu printers use the calibration screen or the G29.1 Z line of the machine start G-code; on others the Z offset of your firmware or printer profile.")},
        {"bridge_flow", L("Bridge flow"), L("Arches bridging a gap, each with a different bridge flow ratio."), L("Look at the underside of each bridge (lift the print and look from below). Too little flow leaves gaps and broken lines; too much makes thick, drooping lines. Pick the smoothest, tightest underside and set its value as the bridge flow (bridge_flow).")},
        {"overhang", L("Overhang angle"), L("Walls leaning out at growing angles, printed without supports."), L("Look at the underside (the face leaning over the plate). Find the steepest wall that is still tight and clean; the next one sags or curls. Subtract that angle from 90 and use the result as the support threshold angle (support_threshold_angle): a limit of 60 gives 30.")},
        {"support_z", L("Support Z distance"), L("Bridges with supports under them, each with a different top Z distance."), L("Break the supports off each piece. Too small a distance welds the support to the bridge; too large leaves a rough, sagging underside. Pick the smallest distance that lets the support come off cleanly and set it as the support top Z distance (support_top_z_distance). Prefer multiples of your layer height.")},
        {"ironing", L("Ironing matrix"), L("Round patches ironed with every combination of speed and flow."), L("Look at the patches under a raking light and run a finger over them. Rough ridges mean too much flow; gaps or lines mean too little flow or too much speed. Pick the smoothest patch: its column is the speed, its row the flow. Use them as ironing_speed and ironing_flow.")},
        {"infill_pattern", L("Infill patterns"), L("One block per infill pattern with the top layers removed, so the pattern shows."), L("A reference sheet: compare density, strength-looking geometry and how each pattern prints on your machine, then choose the pattern for the sparse infill setting (sparse_infill_pattern). Nothing to measure.")},
        {"infill_density", L("Infill density"), L("The same pattern at several densities, tops removed."), L("A reference sheet: see what each percentage really looks like, and pick the one you need for the sparse infill density (sparse_infill_density).")},
        {"top_surface", L("Top surface patterns"), L("One block per top surface pattern."), L("Look at the finished top of each block in angled light and pick the one that looks best on your printer. Set it as the top surface pattern (top_surface_pattern).")},
        {"temp_tower", L("Temperature tower"), L("Bands printed at falling temperatures, each with a bridge, an overhang and two thin posts."), L("Compare the bands: the underside of the bridge, the 45 deg overhang, the strings between the two posts, how well the layers bond (try to snap a band) and the surface. Pick the band with clean bridges and no strings that is still strong, and set its temperature as the filament's nozzle temperature.")},
        {"pressure_advance", L("Pressure advance"), L("A tower whose pressure advance rises with height, to find where the corners come out sharp."), L("Look at the corners all the way up: too little K bulges them, too much rounds and thins them. Find the height where the corner is sharpest and most even. K there = first value + (last value - first value) x height / tower height; the labels on the face already give it. Set it as the pressure advance (Klipper: pressure_advance; Marlin and Bambu: K).")},
        {"cooling", L("Cooling"), L("Bands printed with different part cooling fan power; thin posts show the minimum layer time."), L("Compare bridges, overhangs, the thin posts (small layers) and layer bonding band by band. Choose the lowest fan power that still keeps bridges and overhangs clean, and use it as the fan speed.")},
        {"vfa", L("VFA (vertical ripples)"), L("A column whose walls print at a faster speed band by band, to find the speeds that ripple."), L("Look and feel along each band for vertical ripples. Avoid the speeds where they are strongest and use one on a smooth band as the wall speed.")},
        {"max_flow", L("Maximum volumetric speed"), L("A tower whose flow rises with height until the nozzle cannot keep up."), L("Find the height where the wall turns thin, rough or skips. The flow just below it (the labels) is the limit; take about 10 % off for safety and set it as the filament's max volumetric speed.")},
        {"retraction", L("Retraction"), L("Pairs of posts printed with a longer retraction each, to find the one that leaves no strings."), L("Look for strings between the two posts of each piece. Pick the shortest retraction that leaves none and set it as the retraction length.")},
        {"flow_rate", L("Flow rate"), L("Thin pads with a small flow change each: coarse, fine or YOLO pass, with the top pattern that shows flow best."), L("Look at the top of each pad in raking light, and run a finger over it: gaps between the lines mean too little flow, ridges and roughness too much. Pick the smoothest, closest pad. Its number is the change to apply, and the legend shows the new flow ratio already worked out from your filament's current one (new = current x (1 + number / 100)). Coarse first, then fine around the winner with the new flow; YOLO does a single fine pass when the flow is already close.")},
        {"dimensional", L("Dimensional accuracy"), L("Blocks with a hole and different contour, hole and elephant foot compensations, to measure with calipers."), L("Measure each block: the outside width (target = cell size), the hole diameter (target = the hole diameter) and, on the EF blocks, the flare at the bottom edge. Pick the contour compensation that gives the exact width and the hole compensation that gives the exact hole; for the elephant foot, the smallest value that removes the flare. Set xy_contour_compensation, xy_hole_compensation and elefant_foot_compensation.")},
        {"tolerance", L("Tolerance and fit"), L("Square and round pegs for holes, with a growing clearance."), L("Try each peg in its hole. Pick the smallest clearance that goes in by hand with no force and no play, and use it as the gap in parts that fit together. Run the dimensional test first so the sizes are right.")},
        {"seam_width", L("Seam and line width"), L("Cylinders with each seam position, and thin walls with different line widths."), L("Seams: look at how visible the seam is for each position and pick the one you like (aligned and back hide it best). Line width: measure the wall of each tube at its top edge; pick the width that gives a solid wall without gaps and set it as the outer wall line width.")},
        {"ringing", L("Wall speed and ringing"), L("Walls with sharp corners printed at each speed and acceleration."), L("Look at the corners for ghosting: ripples that echo after a sharp corner. Pick the fastest speed and acceleration whose ghosting you accept and set them as the outer wall speed and acceleration. Do input shaping first.")},
        {"first_layer_tf", L("First layer temperature and speed"), L("Pads whose first layer prints at each temperature and speed."), L("Look at the top and, after peeling, the bottom of each pad. Pick the smooth, fully bonded one that releases easily and set its temperature and speed as the first layer's.")},
        {"color_change", L("Colour change (purge)"), L("One flat strip per colour change, printed one after another, to see how much of the old colour is left in the new one."), L("Strips: each strip is the NEW colour of one change (the label says from>to). Look at its beginning: old colour streaks mean the purge is too short, and the ticks every 10 mm (long ones every 50) tell you for how many millimetres it lasts. Raise the flush volume of that pair (or the flush multiplier) until the first millimetres come out clean; if every strip is clean from the start, you can lower it and waste less. Use strongly contrasting filaments, such as white and a dark colour. Tower: the same changes as bands of one tower, printed by layer. Manual: the printer pauses at each band; swap the filament and resume.")},
    };
    return t;
}

const std::map<std::string, const char *> &labels()
{
    static const std::map<std::string, const char *> m = {
        {"param.start", L("First value")},
        {"param.step", L("Step")},
        {"param.count", L("Number of pads")},
        {"param.height", L("Height")},
        {"param.span", L("Bridge length")},
        {"param.length", L("Wall length")},
        {"param.angles", L("Angles")},
        {"param.speeds", L("Speeds (mm/s)")},
        {"param.flows", L("Flows (%)")},
        {"param.densities", L("Densities (%)")},
        {"param.pattern", L("Infill pattern")},
        {"param.density", L("Infill density")},
        {"param.size", L("Cell size")},
        {"param.pad_height", L("Pad height (0 = first layer)")},
        {"param.end", L("Last value")},
        {"param.speed", L("Speed (0 = the profile's)")},
        {"param.phase", L("Pass")},
        {"param.mode", L("Mode")},
        {"param.colors", L("Number of filaments")},
        {"param.band", L("Band height")},
        {"param.fans", L("Fan power per band (%)")},
        {"param.widths", L("Line widths (% of the nozzle)")},
        {"param.accels", L("Accelerations (mm/s2)")},
        {"param.contour", L("Contour compensation (mm)")},
        {"param.hole", L("Hole compensation (mm)")},
        {"param.foot", L("Elephant foot compensation (mm)")},
        {"param.diameter", L("Hole diameter")},
        {"param.temps", L("Temperatures (C)")},
        {"opt.phase.coarse", L("Coarse (-12 ... +12 %)")},
        {"opt.phase.fine", L("Fine (-3 ... +3 %)")},
        {"opt.phase.custom", L("Custom (the values below)")},
        {"opt.mode.manual", L("One extruder (pause and swap)")},
        {"group.base", L("Foundation")},
        {"group.extrusion", L("Extrusion")},
        {"group.quality", L("Quality")},
        {"group.precision", L("Precision")},
        {"group.finish", L("Finish and references")},
        {"group.color", L("Colour")},
        {"param.top", L("Top surface pattern")},
        {"param.thickness", L("Pad thickness (0 = 8 layers)")},
        {"opt.phase.yolo", L("YOLO (one fine pass, -2.5 ... +2.5 %)")},
        {"opt.top.archimedeanchords", L("Archimedean chords (shows flow best)")},
        {"opt.top.monotonic", L("Monotonic")},
        {"opt.top.zig-zag", L("Rectilinear")},
        {"opt.top.concentric", L("Concentric")},
        {"param.strip_length", L("Strip length")},
        {"param.strip_width", L("Strip width")},
        {"opt.mode.strips", L("Strips, one per colour change (like Creality's test)")},
        {"opt.mode.tower", L("Tower of colour bands")},
    };
    return m;
}

using Builder = Plate (*)(const Values &, const Env &);

const std::map<std::string, Builder> &builders()
{
    static const std::map<std::string, Builder> b = {
        {"z_offset", build_z_offset},           {"first_layer_tf", build_first_layer_tf}, {"first_layer_flow", build_first_layer_flow},
        {"temp_tower", build_temp_tower},       {"flow_rate", nullptr},                   {"max_flow", build_max_flow},
        {"pressure_advance", build_pressure_advance}, {"retraction", build_retraction}, {"cooling", build_cooling},
        {"bridge_flow", build_bridge_flow},     {"overhang", build_overhang},             {"support_z", build_support_z},
        {"vfa", build_vfa},                     {"ringing", build_ringing},               {"seam_width", build_seam_width},
        {"dimensional", build_dimensional},     {"tolerance", build_tolerance},           {"ironing", build_ironing},
        {"top_surface", build_top_surface},     {"infill_pattern", build_infill_pattern}, {"infill_density", build_infill_density},
        {"color_change", build_color_change}};
    return b;
}

Plate build_flow_rate(const Values &v, const Env &env)
{
    constexpr double STRIP = 9.0;
    double       start = v.n("start"), step = v.n("step");
    int          count = int(v.n("count"));
    const std::string phase = v.s("phase");
    if (phase == "coarse")    { start = -12.0; step = 3.0; count = 9; }
    else if (phase == "fine") { start = -3.0;  step = 0.5; count = 13; }
    else if (phase == "yolo") { start = -2.5;  step = 0.5; count = 11; }
    const double size   = v.n("size");
    const double height = v.n("thickness") > 0 ? v.n("thickness") : snap_z(env.first_layer + 7 * env.layer_height, env);
    std::vector<Obj> objects;
    for (double pc : frange(start, step, count)) {
        const double      ratio = 1.0 + pc / 100.0;
        const std::string text  = (pc > 0 ? "+" : "") + fnum(pc, 2);
        std::vector<Part> parts = {make_part("pad", box(0, 0, 0, size, size + STRIP, height)),
                                   label_part(text, size - 3, height, LABEL_PIXEL, LABEL_RAISE, size / 2., (STRIP - 5.6) / 2. + size)};
        Settings note = {{"flow", text + " %"}};
        if (env.flow_ratio > 0)
            note.push_back({"new flow ratio", fnum(env.flow_ratio * ratio, 4)});
        objects.push_back(piece("flow " + text + "%", std::move(parts),
                                {{"top_surface_pattern", v.s("top")}, {"ironing_type", "no ironing"}, {"sparse_infill_density", "100%"},
                                 {"top_shell_layers", "4"}, {"print_flow_ratio", fnum(ratio, 4)}},
                                note));
    }
    place_grid(objects, 4, 5.0);
    return make_plate("Flow rate", std::move(objects), env);
}

// ---------------------------------------------------------------------------------------------
// applying a plate to the plater
// ---------------------------------------------------------------------------------------------
void apply_settings(ModelConfig &cfg, const Settings &settings, std::vector<std::string> &problems)
{
    for (const auto &kv : settings) {
        try {
            DynamicPrintConfig one;
            one.set_deserialize_strict(kv.first, kv.second);
            cfg.apply(one);
        } catch (const std::exception &ex) {
            problems.push_back(kv.first + " = " + kv.second + ": " + ex.what());
        }
    }
}

// Sets `key` in an edited preset; vector options get the value in every slot.
bool apply_preset_edits(DynamicPrintConfig &cfg, const Settings &edits, std::vector<std::string> &problems)
{
    bool changed = false;
    for (const auto &kv : edits) {
        const ConfigOption *cur = cfg.option(kv.first);
        if (cur == nullptr) {
            BOOST_LOG_TRIVIAL(info) << "Layer Slayer: preset has no " << kv.first << ", not changed";
            continue;
        }
        std::string value = kv.second;
        if (cur->is_vector()) {
            const size_t n = std::max<size_t>(1, static_cast<const ConfigOptionVectorBase *>(cur)->size());
            for (size_t i = 1; i < n; ++i)
                value += "," + kv.second;
        }
        try {
            cfg.set_deserialize_strict(kv.first, value);
            changed = true;
        } catch (const std::exception &ex) {
            problems.push_back(kv.first + " = " + kv.second + ": " + ex.what());
        }
    }
    return changed;
}

double first_number(const DynamicPrintConfig &cfg, const std::string &key, double def)
{
    if (!cfg.has(key))
        return def;
    std::string s = cfg.opt_serialize(key);
    const size_t comma = s.find(',');
    if (comma != std::string::npos)
        s = s.substr(0, comma);
    if (!s.empty() && s.back() == '%')
        s.pop_back();
    double v = def;
    return parse_double(s, v) ? v : def;
}

} // namespace

// =============================================================================================
// public
// =============================================================================================
double Values::n(const std::string &key) const
{
    auto it = num.find(key);
    return it == num.end() ? 0. : it->second;
}

const std::vector<double> &Values::l(const std::string &key) const
{
    static const std::vector<double> empty;
    auto it = list.find(key);
    return it == list.end() ? empty : it->second;
}

const std::string &Values::s(const std::string &key) const
{
    static const std::string empty;
    auto it = str.find(key);
    return it == str.end() ? empty : it->second;
}

const std::vector<Calib> &all()
{
    static const std::vector<Calib> c = {
        {"z_offset", "base", {FltP("start", -0.08, -0.5, 0.5, 0.005, "mm"), FltP("step", 0.02, 0.005, 0.2, 0.005, "mm"), IntP("count", 9, 3, 20),
                              FltP("pad_height", 0.0, 0.0, 1.0, 0.01, "mm")}},
        {"first_layer_tf", "base", {ListP("temps", {200, 210, 220, 230}, 100, 400, "C"), ListP("speeds", {20, 40, 60}, 5, 300, "mm/s"),
                                    FltP("pad_height", 0.0, 0.0, 1.0, 0.01, "mm")}},
        {"first_layer_flow", "base", {FltP("start", 0.80, 0.3, 2.0, 0.01), FltP("step", 0.04, 0.005, 0.5, 0.005), IntP("count", 12, 2, 24),
                                      FltP("pad_height", 0.0, 0.0, 1.0, 0.01, "mm")}},
        {"temp_tower", "extrusion", {FltP("start", 230, 150, 380, 1, "C"), FltP("step", -5, -50, 50, 1, "C"), IntP("count", 9, 2, 20),
                                     FltP("band", 10, 6, 20, 0.5, "mm")}},
        {"flow_rate", "extrusion", {SelP("phase", "coarse", {{"coarse", L("Coarse (-12 ... +12 %)")}, {"fine", L("Fine (-3 ... +3 %)")},
                                                          {"yolo", L("YOLO (one fine pass, -2.5 ... +2.5 %)")}, {"custom", L("Custom (the values below)")}}),
                                    FltP("start", -10, -50, 50, 0.5, "%"), FltP("step", 2, 0.1, 20, 0.1, "%"), IntP("count", 11, 3, 24),
                                    FltP("size", 24, 15, 40, 1, "mm"),
                                    SelP("top", "archimedeanchords", {{"archimedeanchords", L("Archimedean chords (shows flow best)")}, {"monotonic", L("Monotonic")},
                                                                   {"rectilinear", L("Rectilinear")}, {"concentric", L("Concentric")}}),
                                    FltP("thickness", 0, 0, 6, 0.1, "mm")}},
        {"max_flow", "extrusion", {FltP("start", 6, 1, 60, 0.5, "mm3/s"), FltP("end", 24, 2, 80, 0.5, "mm3/s"), FltP("height", 60, 20, 150, 1, "mm"),
                                   FltP("size", 25, 15, 50, 1, "mm")}},
        {"pressure_advance", "extrusion", {FltP("start", 0.0, 0.0, 2.0, 0.005), FltP("end", 0.08, 0.001, 2.0, 0.005), FltP("height", 40, 10, 120, 1, "mm"),
                                           FltP("size", 30, 15, 60, 1, "mm")}},
        {"retraction", "extrusion", {FltP("start", 0.4, 0.0, 12.0, 0.1, "mm"), FltP("step", 0.2, 0.05, 2.0, 0.05, "mm"), IntP("count", 6, 2, 12),
                                     FltP("speed", 0.0, 0.0, 150.0, 5.0, "mm/s")}},
        {"cooling", "extrusion", {ListP("fans", {100, 80, 60, 40, 20, 0}, 0, 100, "%"), FltP("band", 8, 6, 20, 0.5, "mm")}},
        {"bridge_flow", "quality", {FltP("start", 0.80, 0.3, 2.0, 0.01), FltP("step", 0.05, 0.005, 0.5, 0.005), IntP("count", 6, 2, 12),
                                    FltP("span", 20, 5, 60, 1, "mm")}},
        {"overhang", "quality", {ListP("angles", {10, 20, 30, 40, 50, 55, 60, 65, 70, 75, 80}, 5, 85), FltP("length", 14, 6, 40, 1, "mm")}},
        {"support_z", "quality", {FltP("start", 0.10, 0.0, 1.0, 0.01, "mm"), FltP("step", 0.05, 0.005, 0.5, 0.005, "mm"), IntP("count", 5, 2, 10),
                                  FltP("span", 16, 6, 40, 1, "mm")}},
        {"vfa", "quality", {FltP("start", 40, 10, 400, 5, "mm/s"), FltP("step", 20, 5, 100, 5, "mm/s"), IntP("count", 8, 3, 16), FltP("band", 8, 4, 20, 0.5, "mm")}},
        {"ringing", "quality", {ListP("speeds", {80, 140, 200}, 20, 500, "mm/s"), ListP("accels", {3000, 6000, 10000}, 100, 50000, "mm/s2"),
                                FltP("height", 20, 8, 40, 1, "mm")}},
        {"seam_width", "quality", {ListP("widths", {90, 100, 110, 120, 130}, 50, 250, "%"), FltP("height", 12, 6, 30, 1, "mm")}},
        {"dimensional", "precision", {ListP("contour", {-0.10, -0.05, 0.0, 0.05, 0.10}, -1.0, 1.0, "mm"), ListP("hole", {-0.10, 0.0, 0.10}, -1.0, 1.0, "mm"),
                                      ListP("foot", {0.0, 0.1, 0.2, 0.3}, 0.0, 1.5, "mm"), FltP("size", 20, 10, 40, 1, "mm"), FltP("diameter", 8, 3, 25, 0.5, "mm")}},
        {"tolerance", "precision", {FltP("start", 0.10, 0.0, 1.0, 0.01, "mm"), FltP("step", 0.05, 0.01, 0.5, 0.01, "mm"), IntP("count", 6, 2, 12),
                                    FltP("size", 10, 5, 25, 0.5, "mm")}},
        {"ironing", "finish", {ListP("speeds", {20, 30, 40, 50, 60}, 5, 300, "mm/s"), ListP("flows", {5, 10, 15, 20, 25}, 0, 100, "%"),
                               FltP("size", 14, 8, 30, 1, "mm"), FltP("height", 1.6, 0.8, 6.0, 0.2, "mm")}},
        {"top_surface", "finish", {FltP("size", 28, 15, 45, 1, "mm"), FltP("height", 4, 2, 15, 0.5, "mm")}},
        {"infill_pattern", "finish", {FltP("density", 15, 3, 60, 1, "%"), FltP("size", 28, 15, 45, 1, "mm"), FltP("height", 8, 4, 20, 0.5, "mm")}},
        {"infill_density", "finish", {ListP("densities", {5, 10, 15, 25, 50, 75}, 1, 100, "%"), SelP("pattern", "gyroid", infill_pattern_options()),
                                      FltP("size", 28, 15, 45, 1, "mm"), FltP("height", 8, 4, 20, 0.5, "mm")}},
        {"color_change", "color", {SelP("mode", "strips", {{"strips", L("Strips, one per colour change (like Creality's test)")}, {"tower", L("Tower of colour bands")},
                                                        {"manual", L("One extruder (pause and swap)")}}),
                                   IntP("colors", 2, 2, 4), FltP("strip_length", 100, 40, 200, 5, "mm"), FltP("strip_width", 25, 12, 40, 1, "mm"),
                                   FltP("band", 4, 2, 12, 0.5, "mm")}},
    };
    return c;
}

const Calib *find(const std::string &id)
{
    for (const Calib &c : all())
        if (c.id == id)
            return &c;
    return nullptr;
}

const std::vector<std::string> &groups()
{
    static const std::vector<std::string> g = {"base", "extrusion", "quality", "precision", "finish", "color"};
    return g;
}

const char *name(const std::string &id)
{
    for (const Text &t : texts())
        if (id == t.id)
            return t.name;
    return "";
}

const char *description(const std::string &id)
{
    for (const Text &t : texts())
        if (id == t.id)
            return t.desc;
    return "";
}

const char *how_to_read(const std::string &id)
{
    for (const Text &t : texts())
        if (id == t.id)
            return t.how;
    return "";
}

const char *group_name(const std::string &group)
{
    auto it = labels().find("group." + group);
    return it == labels().end() ? "" : it->second;
}

const char *param_label(const std::string &key)
{
    auto it = labels().find("param." + key);
    return it == labels().end() ? "" : it->second;
}

std::string icon_name(const std::string &id) { return "calib_" + id; }

Values defaults(const Calib &calib)
{
    Values v;
    for (const Param &p : calib.params) {
        if (p.kind == Kind::Floats)
            v.list[p.key] = p.def_list;
        else if (p.kind == Kind::Select)
            v.str[p.key] = p.def_str;
        else
            v.num[p.key] = p.def;
    }
    return v;
}

std::string parse(const Param &param, const std::string &raw, Values &values)
{
    auto range_error = [&](double value) {
        return Slic3r::format(_u8L("%1% = %2% is out of range (%3% to %4%)."), _u8L(param_label(param.key)), fnum(value), fnum(param.min), fnum(param.max));
    };
    auto bad = [&]() { return Slic3r::format(_u8L("%1%: \"%2%\" is not a valid value."), _u8L(param_label(param.key)), raw.substr(0, 40)); };
    switch (param.kind) {
    case Kind::Select: {
        std::string want = raw;
        std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (want == "zig-zag")
            want = "rectilinear"; // the plugin's name for it
        for (const Option &o : param.options)
            if (o.value == want) {
                values.str[param.key] = o.value;
                return std::string();
            }
        return bad();
    }
    case Kind::Floats: {
        std::vector<double> out;
        std::string         cur;
        auto flush = [&]() -> bool {
            if (cur.empty())
                return true;
            double v = 0.;
            if (!parse_double(cur, v))
                return false;
            out.push_back(v);
            cur.clear();
            return true;
        };
        for (char c : raw) {
            if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n') {
                if (!flush())
                    return bad();
            } else
                cur += c;
        }
        if (!flush() || out.empty())
            return bad();
        for (double v : out)
            if (v < param.min - 1e-9 || v > param.max + 1e-9)
                return range_error(v);
        values.list[param.key] = out;
        return std::string();
    }
    case Kind::Bool: {
        values.num[param.key] = (raw == "1" || raw == "true" || raw == "yes" || raw == "on") ? 1. : 0.;
        return std::string();
    }
    default: {
        std::string t = raw;
        std::replace(t.begin(), t.end(), ',', '.'); // a decimal comma
        double v = 0.;
        if (!parse_double(t, v))
            return bad();
        if (param.kind == Kind::Int)
            v = std::trunc(v);
        if (v < param.min - 1e-9 || v > param.max + 1e-9)
            return range_error(v);
        values.num[param.key] = v;
        return std::string();
    }
    }
}

std::string format_value(const Param &param, const Values &values)
{
    switch (param.kind) {
    case Kind::Select: return values.s(param.key);
    case Kind::Floats: {
        std::string s;
        for (double v : values.l(param.key))
            s += (s.empty() ? "" : ", ") + fnum(v);
        return s;
    }
    case Kind::Int: return fnum(values.n(param.key), 0);
    default: return fnum(values.n(param.key));
    }
}

Env read_env(Plater *plater)
{
    Env           env;
    PresetBundle *b = wxGetApp().preset_bundle;
    if (b == nullptr)
        return env;
    const DynamicPrintConfig &print    = b->prints.get_edited_preset().config;
    const DynamicPrintConfig &printer  = b->printers.get_edited_preset().config;
    const DynamicPrintConfig &filament = b->filaments.get_edited_preset().config;
    if (const auto *area = printer.option<ConfigOptionPoints>("printable_area"); area != nullptr && area->values.size() >= 3) {
        const BoundingBoxf bb = get_extents(area->values);
        env.bed_x0 = bb.min.x();
        env.bed_y0 = bb.min.y();
        env.bed_w  = bb.size().x();
        env.bed_d  = bb.size().y();
    }
    env.layer_height = first_number(print, "layer_height", 0.2);
    env.first_layer  = first_number(print, "initial_layer_print_height", env.layer_height);
    env.nozzle       = first_number(printer, "nozzle_diameter", 0.4);
    if (env.layer_height <= 0)
        env.layer_height = 0.2;
    if (env.first_layer <= 0)
        env.first_layer = env.layer_height;
    env.flavor         = printer.has("gcode_flavor") ? printer.opt_serialize("gcode_flavor") : std::string();
    env.flow_ratio     = first_number(filament, "filament_flow_ratio", 1.0);
    env.max_volumetric = first_number(filament, "filament_max_volumetric_speed", 0.0);
    if (print.has("outer_wall_line_width")) {
        const auto *lw = print.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
        if (lw != nullptr)
            env.line_width = lw->get_abs_value(env.nozzle);
    }
    env.filaments = std::max<int>(1, int(b->filament_presets.size()));
    env.printer   = b->printers.get_selected_preset_name();
    (void) plater;
    return env;
}

std::vector<LegendRow> Plate::legend() const
{
    struct Row
    {
        std::string name;
        Settings    s;
    };
    std::vector<Row> rows;
    for (const Obj &o : objects) {
        if (!o.note.empty())
            rows.push_back({o.name, o.note});
        else if (!o.settings.empty())
            rows.push_back({o.name, o.settings});
        for (const Part &p : o.parts)
            if (!p.settings.empty() && p.name.rfind("label", 0) != 0 && p.name != "base")
                rows.push_back({p.name, p.settings});
    }
    // what every entry shares is not what the test varies
    if (rows.size() > 1) {
        std::set<std::string> shared;
        for (const auto &kv : rows.front().s) {
            bool all_same = true;
            for (const Row &r : rows) {
                auto it = std::find(r.s.begin(), r.s.end(), kv);
                if (it == r.s.end()) {
                    all_same = false;
                    break;
                }
            }
            if (all_same)
                shared.insert(kv.first);
        }
        bool keeps_all = true;
        for (const Row &r : rows) {
            size_t left = 0;
            for (const auto &kv : r.s)
                if (shared.count(kv.first) == 0)
                    ++left;
            keeps_all = keeps_all && left > 0;
        }
        if (keeps_all)
            for (Row &r : rows)
                r.s.erase(std::remove_if(r.s.begin(), r.s.end(), [&](const auto &kv) { return shared.count(kv.first) != 0; }), r.s.end());
    }
    std::vector<LegendRow> out;
    for (const Row &r : rows) {
        std::string text;
        for (const auto &kv : r.s)
            text += (text.empty() ? "" : ", ") + kv.first + " " + kv.second;
        out.push_back({r.name, text});
    }
    for (const LegendRow &r : extra_legend)
        out.push_back(r);
    return out;
}

Plate build(const Calib &calib, const Values &values, const Env &env)
{
    if (calib.id == "flow_rate")
        return build_flow_rate(values, env);
    auto it = builders().find(calib.id);
    if (it == builders().end() || it->second == nullptr)
        throw Error("unknown calibration " + calib.id);
    Plate p = it->second(values, env);
    if (p.objects.empty())
        throw Error(_u8L("Nothing to print with these values."));
    return p;
}

bool run(Plater *plater, const std::string &id, const Values &values, bool skip_confirm, std::string &error, Plate *out)
{
    error.clear();
    const Calib *calib = find(id);
    if (plater == nullptr || calib == nullptr) {
        error = "unknown calibration " + id;
        return false;
    }
    const Env env = read_env(plater);
    Plate     plate;
    try {
        plate = build(*calib, values, env);
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }

    if (plater->new_project(skip_confirm, false, _L(name(id))) == wxID_CANCEL)
        return false; // the user kept the current project

    // ---- objects -------------------------------------------------------------------------------
    std::vector<std::string> problems;
    Model                    tmp;
    for (size_t i = 0; i < plate.objects.size(); ++i) {
        const Obj   &o  = plate.objects[i];
        ModelObject *mo = tmp.add_object();
        mo->name        = o.name;
        Settings settings = o.settings;
        if (std::none_of(settings.begin(), settings.end(), [](const auto &kv) { return kv.first == "extruder"; }))
            settings.insert(settings.begin(), {"extruder", "1"});
        apply_settings(mo->config, settings, problems);
        // the labels of a piece go in one part
        Mesh labels_mesh;
        for (const Part &p : o.parts) {
            if (p.settings.empty() && p.name.rfind("label", 0) == 0 && p.name != "label plate") {
                add(labels_mesh, p.mesh);
                continue;
            }
            if (p.mesh.indices.empty())
                continue;
            ModelVolume *vol = mo->add_volume(TriangleMesh(p.mesh));
            vol->name        = p.name;
            apply_settings(vol->config, p.settings, problems);
        }
        if (!labels_mesh.indices.empty()) {
            ModelVolume *vol = mo->add_volume(TriangleMesh(labels_mesh));
            vol->name        = "labels";
        }
        for (const Range &r : o.ranges) {
            ModelConfig cfg;
            Settings    rs = {{"layer_height", fnum(env.layer_height, 4)}, {"extruder", "0"}};
            rs.insert(rs.end(), r.settings.begin(), r.settings.end());
            apply_settings(cfg, rs, problems);
            mo->layer_config_ranges[{r.lo, r.hi}].assign_config(cfg);
        }
        ModelInstance *inst = mo->add_instance();
        inst->set_offset(Vec3d(o.x, o.y, 0.));
        inst->arrange_order = int(i + 1); // the print order when the plate prints by object
        mo->invalidate_bounding_box();
    }
    plater->load_generated_objects(tmp.objects);

    // ---- presets: edited like Orca's own calibrations do (unsaved changes of the selected presets) -
    PresetBundle *bundle = wxGetApp().preset_bundle;
    auto edit = [&](Preset::Type type, PresetCollection &coll, const Settings &edits) {
        if (edits.empty())
            return;
        if (apply_preset_edits(coll.get_edited_preset().config, edits, problems))
            if (Tab *tab = wxGetApp().get_tab(type)) {
                tab->update_dirty();
                tab->reload_config();
            }
    };
    edit(Preset::TYPE_PRINT, bundle->prints, plate.print_edits);
    edit(Preset::TYPE_FILAMENT, bundle->filaments, plate.filament_edits);
    edit(Preset::TYPE_PRINTER, bundle->printers, plate.printer_edits);

    // ---- per-layer G-code and plate settings ---------------------------------------------------
    PartPlateList &plates    = plater->get_partplate_list();
    const int      plate_idx = plates.get_curr_plate_index();
    if (!plate.gcodes.empty()) {
        CustomGCode::Info &info = plater->model().plates_custom_gcodes[plate_idx];
        info.mode               = env.filaments > 1 ? CustomGCode::MultiAsSingle : CustomGCode::SingleExtruder;
        info.gcodes.clear();
        for (const LayerCode &c : plate.gcodes) {
            CustomGCode::Item item;
            item.print_z  = c.z;
            item.extruder = c.extruder;
            if (c.type == CodeType::Pause) {
                item.type  = CustomGCode::PausePrint;
                item.extra = c.text;
            } else {
                item.type  = CustomGCode::Custom;
                item.extra = c.text;
            }
            info.gcodes.push_back(item);
        }
        std::sort(info.gcodes.begin(), info.gcodes.end());
    }
    if (PartPlate *pp = plates.get_curr_plate()) {
        if (plate.by_object)
            pp->set_print_seq(PrintSequence::ByObject);
        pp->update_slice_result_valid_state(false);
    }
    for (const std::string &p : problems)
        BOOST_LOG_TRIVIAL(warning) << "Layer Slayer " << id << ": " << p;
    plater->schedule_background_process();
    BOOST_LOG_TRIVIAL(info) << "Layer Slayer: created " << id << " with " << plate.objects.size() << " objects, " << plate.gcodes.size()
                            << " layer G-codes";
    if (out != nullptr) {
        *out = std::move(plate);
        for (const std::string &p : problems)
            out->warnings.push_back(p);
    }
    return true;
}

}}} // namespace Slic3r::GUI::LayerSlayer
