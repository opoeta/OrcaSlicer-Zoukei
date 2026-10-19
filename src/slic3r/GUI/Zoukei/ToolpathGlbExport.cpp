// Orca Zoukei: GLB export of the G-code preview toolpaths, see ToolpathGlbExport.hpp.
#include "ToolpathGlbExport.hpp"

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/dialog.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

using json = nlohmann::json;

namespace {

struct P3
{
    float x, y, z;
};
inline P3    operator+(P3 a, P3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline P3    operator-(P3 a, P3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline P3    operator*(P3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline P3    cross(P3 a, P3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float len(P3 a) { return std::sqrt(dot(a, a)); }
inline P3    norm(P3 a)
{
    const float l = len(a);
    return l > 1e-9f ? a * (1.0f / l) : P3{ 1.0f, 0.0f, 0.0f };
}

boost::filesystem::path to_fs(const std::string& utf8) { return boost::filesystem::path(boost::nowide::widen(utf8)); }

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// -------- roles ----------------------------------------------------------------------------------------

struct RoleInfo
{
    const char* id;
    const char* label;
};

RoleInfo role_info(libvgcode::EGCodeExtrusionRole r)
{
    using R = libvgcode::EGCodeExtrusionRole;
    switch (r) {
    case R::Perimeter:                return { "inner_wall", "Inner wall" };
    case R::ExternalPerimeter:        return { "outer_wall", "Outer wall" };
    case R::OverhangPerimeter:        return { "overhang_wall", "Overhang wall" };
    case R::InternalInfill:           return { "sparse_infill", "Sparse infill" };
    case R::SolidInfill:              return { "internal_solid_infill", "Internal solid infill" };
    case R::TopSolidInfill:           return { "top_surface", "Top surface" };
    case R::Ironing:                  return { "ironing", "Ironing" };
    case R::BridgeInfill:             return { "bridge", "Bridge" };
    case R::GapFill:                  return { "gap_infill", "Gap infill" };
    case R::Skirt:                    return { "skirt", "Skirt" };
    case R::SupportMaterial:          return { "support", "Support" };
    case R::SupportMaterialInterface: return { "support_interface", "Support interface" };
    case R::WipeTower:                return { "prime_tower", "Prime tower" };
    case R::Custom:                   return { "custom", "Custom" };
    case R::BottomSurface:            return { "bottom_surface", "Bottom surface" };
    case R::InternalBridgeInfill:     return { "internal_bridge", "Internal bridge" };
    case R::Brim:                     return { "brim", "Brim" };
    case R::SupportTransition:        return { "support_transition", "Support transition" };
    case R::Mixed:                    return { "mixed", "Mixed" };
    default:                          return { "other", "Other" };
    }
}

// -------- colours --------------------------------------------------------------------------------------

struct Rgb
{
    uint8_t r = 128, g = 128, b = 128;
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};

std::string to_hex(const Rgb& c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

bool parse_hex(const std::string& s, Rgb& out)
{
    size_t i = (!s.empty() && s[0] == '#') ? 1 : 0;
    if (s.size() < i + 6)
        return false;
    unsigned v = 0;
    for (size_t k = 0; k < 6; ++k) {
        const char c = s[i + k];
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
        else return false;
        v = v * 16 + d;
    }
    out = { uint8_t(v >> 16), uint8_t((v >> 8) & 255), uint8_t(v & 255) };
    return true;
}

float srgb_to_linear(uint8_t c)
{
    const float f = float(c) / 255.0f;
    return f <= 0.04045f ? f / 12.92f : std::pow((f + 0.055f) / 1.055f, 2.4f);
}

// One filament as the exporter sees it.
struct FilamentInfo
{
    std::string      name;
    std::string      type;
    Rgb              colour;
    std::vector<Rgb> multi;        // 2..4 colours when coaxial or gradient
    bool             gradient = false;
    float            cycle_m  = 0.0f;
    float            density  = 1.24f; // g/cm3
    double           mm3      = 0.0;    // extruded volume
    double           length_mm = 0.0;   // filament length
};

// -------- mesh accumulation ----------------------------------------------------------------------------

struct Mesh
{
    std::vector<float>    pos; // glTF space, 3 floats per vertex
    std::vector<uint32_t> idx;
    size_t                vcount() const { return pos.size() / 3; }
};

struct ChainPoint
{
    P3    p;
    float w, h;
};

// glTF is Y-up, the G-code Z-up: (x, y, z) -> (x, z, -y)
inline void push_gltf(Mesh& m, P3 p)
{
    m.pos.push_back(p.x);
    m.pos.push_back(p.z);
    m.pos.push_back(-p.y);
}

// Douglas-Peucker on a chain of points, keeps the first and the last. Width / height of kept points stay.
void simplify_chain(std::vector<ChainPoint>& pts, float eps)
{
    if (eps <= 0.0f || pts.size() < 3)
        return;
    std::vector<char>                     keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack;
    stack.emplace_back(0, pts.size() - 1);
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1)
            continue;
        const P3    ab = pts[b].p - pts[a].p;
        const float l2 = std::max(dot(ab, ab), 1e-12f);
        float       best = -1.0f;
        size_t      bi   = a;
        for (size_t k = a + 1; k < b; ++k) {
            const P3    ak = pts[k].p - pts[a].p;
            const float t  = std::min(1.0f, std::max(0.0f, dot(ak, ab) / l2));
            const float d  = len(ak - ab * t);
            if (d > best) {
                best = d;
                bi   = k;
            }
        }
        if (best > eps) {
            keep[bi] = 1;
            stack.emplace_back(a, bi);
            stack.emplace_back(bi, b);
        }
    }
    size_t o = 0;
    for (size_t k = 0; k < pts.size(); ++k)
        if (keep[k])
            pts[o++] = pts[k];
    pts.resize(o);
}

// A tube along the chain: one ring of `sides` vertices per point, caps at both ends.
// `lateral` shifts the tube sideways (in mm, along the bead's right axis), `wscale` narrows it (coaxial stripes).
void add_tube(Mesh& m, const std::vector<ChainPoint>& pts, int sides, float lateral, float wscale, uint64_t& tris)
{
    const size_t n = pts.size();
    if (n < 2)
        return;
    const uint32_t base = uint32_t(m.vcount());
    std::vector<P3> dirs(n - 1);
    for (size_t k = 0; k + 1 < n; ++k)
        dirs[k] = norm(pts[k + 1].p - pts[k].p);
    const P3 Z{ 0.0f, 0.0f, 1.0f };
    for (size_t k = 0; k < n; ++k) {
        P3 d = k == 0 ? dirs[0] : (k + 1 == n ? dirs[n - 2] : norm(dirs[k - 1] + dirs[k]));
        P3 right = cross(d, Z);
        if (len(right) < 1e-4f)
            right = P3{ 1.0f, 0.0f, 0.0f };
        right = norm(right);
        const P3    up = norm(cross(right, d));
        const float hw = 0.5f * pts[k].w * wscale;
        const float hh = 0.5f * pts[k].h;
        const P3    c  = pts[k].p + right * lateral;
        for (int j = 0; j < sides; ++j) {
            const float a = 6.2831853f * float(j) / float(sides);
            push_gltf(m, c + right * (std::cos(a) * hw) + up * (std::sin(a) * hh));
        }
    }
    auto V = [&](size_t ring, int j) { return base + uint32_t(ring * size_t(sides) + size_t(j % sides)); };
    for (size_t k = 0; k + 1 < n; ++k)
        for (int j = 0; j < sides; ++j) {
            // outward-facing winding (rings run right -> up, the tube runs along +forward)
            m.idx.insert(m.idx.end(), { V(k, j), V(k + 1, j + 1), V(k, j + 1), V(k, j), V(k + 1, j), V(k + 1, j + 1) });
            tris += 2;
        }
    // caps: centre vertices
    for (int cap = 0; cap < 2; ++cap) {
        const size_t ring = cap == 0 ? 0 : n - 1;
        P3           sum{ 0, 0, 0 };
        for (int j = 0; j < sides; ++j) {
            const size_t o = (size_t(V(ring, j))) * 3;
            sum = sum + P3{ m.pos[o], m.pos[o + 1], m.pos[o + 2] };
        }
        sum = sum * (1.0f / float(sides));
        const uint32_t ci = uint32_t(m.vcount());
        m.pos.push_back(sum.x);
        m.pos.push_back(sum.y);
        m.pos.push_back(sum.z);
        for (int j = 0; j < sides; ++j) {
            if (cap == 0)
                m.idx.insert(m.idx.end(), { ci, V(ring, j), V(ring, j + 1) });
            else
                m.idx.insert(m.idx.end(), { ci, V(ring, j + 1), V(ring, j) });
            ++tris;
        }
    }
}

// -------- GLB container --------------------------------------------------------------------------------

struct Bin
{
    std::vector<uint8_t> data;
    void align4()
    {
        while (data.size() % 4)
            data.push_back(0);
    }
    size_t append(const void* p, size_t bytes)
    {
        const size_t off = data.size();
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + bytes);
        return off;
    }
};

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(uint8_t((x >> (8 * i)) & 255));
}

std::string base64(const std::vector<uint8_t>& in)
{
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += T[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = uint32_t(in[i]) << 16;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool write_file(const boost::filesystem::path& p, const void* data, size_t bytes)
{
    boost::nowide::ofstream f(p.string().c_str(), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(static_cast<const char*>(data), std::streamsize(bytes));
    f.flush();
    return bool(f);
}

std::string format_duration(float seconds)
{
    long s = long(seconds + 0.5f);
    char buf[48];
    if (s >= 3600)
        std::snprintf(buf, sizeof(buf), "%ldh %02ldm", s / 3600, (s % 3600) / 60);
    else
        std::snprintf(buf, sizeof(buf), "%ldm %02lds", s / 60, s % 60);
    return buf;
}

std::string read_text(const boost::filesystem::path& p)
{
    boost::nowide::ifstream f(p.string().c_str(), std::ios::binary);
    if (!f)
        return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void replace_all(std::string& s, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Escapes for text placed in HTML and in a JS string literal inside <script>
std::string html_escape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

// -------- filaments ------------------------------------------------------------------------------------

std::vector<FilamentInfo> collect_filaments(const libvgcode::Viewer& viewer)
{
    std::vector<FilamentInfo> out;
    const libvgcode::Palette& tc = viewer.get_tool_colors();
    out.resize(std::max<size_t>(tc.size(), 1));
    PresetBundle* pb = wxGetApp().preset_bundle;
    for (size_t i = 0; i < out.size(); ++i) {
        FilamentInfo& f = out[i];
        if (i < tc.size())
            f.colour = { tc[i][0], tc[i][1], tc[i][2] };
        f.name = "Filament " + std::to_string(i + 1);
        if (pb != nullptr && i < pb->filament_presets.size() && !pb->filament_presets[i].empty())
            f.name = pb->filament_presets[i];
    }
    if (pb != nullptr) {
        const DynamicPrintConfig& pc = pb->project_config;
        const auto* mc = pc.option<ConfigOptionStrings>("filament_multi_colour");
        const auto* ty = pc.option<ConfigOptionStrings>("filament_colour_type");
        const auto* gl = pc.option<ConfigOptionFloats>("filament_gradient_length");
        const auto* de = pc.option<ConfigOptionFloats>("filament_density");
        const auto* ft = pc.option<ConfigOptionStrings>("filament_type");
        for (size_t i = 0; i < out.size(); ++i) {
            FilamentInfo& f = out[i];
            if (mc != nullptr && i < mc->values.size()) {
                std::istringstream is(mc->values[i]);
                for (std::string c; is >> c && f.multi.size() < 4;) {
                    Rgb rgb;
                    if (parse_hex(c, rgb))
                        f.multi.push_back(rgb);
                }
                f.gradient = ty != nullptr && i < ty->values.size() && ty->values[i] == "0" && f.multi.size() >= 2;
                if (f.multi.size() < 2)
                    f.multi.clear();
            }
            if (gl != nullptr && i < gl->values.size())
                f.cycle_m = float(gl->values[i]);
            if (de != nullptr && i < de->values.size() && de->values[i] > 0.0)
                f.density = float(de->values[i]);
            if (ft != nullptr && i < ft->values.size())
                f.type = ft->values[i];
        }
    }
    return out;
}

} // namespace

// ------------------------------------------------------------------------------------------------------

const char* glb_detail_name(GlbDetail d)
{
    switch (d) {
    case GlbDetail::Preview: return "preview";
    case GlbDetail::Full:    return "full";
    default:                 return "standard";
    }
}

bool glb_detail_from_string(const std::string& s, GlbDetail& out)
{
    const std::string l = lower(s);
    if (l == "preview") out = GlbDetail::Preview;
    else if (l == "standard") out = GlbDetail::Standard;
    else if (l == "full") out = GlbDetail::Full;
    else return false;
    return true;
}

bool glb_html_from_string(const std::string& s, GlbHtml& out)
{
    const std::string l = lower(s);
    if (l == "none") out = GlbHtml::None;
    else if (l == "linked") out = GlbHtml::Linked;
    else if (l == "embedded") out = GlbHtml::Embedded;
    else return false;
    return true;
}

std::string glb_html_path_for(const std::string& glb_path_utf8)
{
    boost::filesystem::path p = to_fs(glb_path_utf8);
    p.replace_extension(".html");
    return boost::nowide::narrow(p.wstring());
}

GlbExportResult export_toolpaths_glb(const std::string& glb_path_utf8, const GlbExportOptions& opt)
{
    GlbExportResult res;
    auto fail = [&res](const char* code, const std::string& msg) {
        res.ok         = false;
        res.error_code = code;
        res.error      = msg;
        return res;
    };

    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->printer_technology() != ptFFF)
        return fail("not_sliced", "slice the plate and show the Preview first");
    GLCanvas3D* canvas = plater->get_preview_canvas3D();
    // The preview only reloads while its panel is on screen, so an API-driven slice
    // leaves the GCodeViewer empty: load it on demand, as the render route does.
    if (canvas != nullptr && !canvas->get_gcode_viewer().has_data()) {
        PartPlate*                  plate  = plater->get_partplate_list().get_curr_plate();
        const GCodeProcessorResult* result = (plate != nullptr && plate->is_slice_result_valid()) ? plate->get_slice_result() : nullptr;
        if (result != nullptr && !result->moves.empty())
            canvas->load_gcode_preview(*result, plater->get_extruder_colors_from_plater_config(result), std::vector<std::string>(), false);
    }
    if (canvas == nullptr || !canvas->get_gcode_viewer().has_data())
        return fail("not_sliced", "there are no toolpaths in the Preview");
    const libvgcode::Viewer& viewer = canvas->get_gcode_viewer().get_libvgcode_viewer();

    // ---- target paths ----
    const boost::filesystem::path glb_fs = to_fs(glb_path_utf8);
    if (lower(glb_fs.extension().string()) != ".glb")
        return fail("bad_extension", "path must end with .glb");
    boost::system::error_code ec;
    if (!glb_fs.has_parent_path() || !boost::filesystem::is_directory(glb_fs.parent_path(), ec))
        return fail("no_directory", "the folder of path must exist");
    boost::filesystem::path html_fs = glb_fs;
    html_fs.replace_extension(".html");
    if (boost::filesystem::exists(glb_fs, ec) && !opt.overwrite)
        return fail("exists", "the .glb exists; pass overwrite:true to replace it");
    if (opt.html != GlbHtml::None && boost::filesystem::exists(html_fs, ec) && !opt.overwrite)
        return fail("exists", "the .html exists; pass overwrite:true to replace it");

    std::string tmpl;
    if (opt.html != GlbHtml::None) {
        tmpl = read_text(boost::filesystem::path(boost::nowide::widen(resources_dir())) / "web" / "zoukeiViewer" / "viewer.html");
        if (tmpl.empty())
            return fail("no_template", "resources/web/zoukeiViewer/viewer.html is missing");
    }

    wxBusyCursor busy;

    // ---- geometry ----
    const int   sides = opt.detail == GlbDetail::Full ? 8 : 4;
    const float eps   = opt.detail == GlbDetail::Preview ? 0.15f : (opt.detail == GlbDetail::Standard ? 0.02f : 0.0f);
    const float min_chain_len = opt.detail == GlbDetail::Preview ? 0.6f : 0.0f;

    std::vector<FilamentInfo> filaments = collect_filaments(viewer);

    // materials: (colour) -> index
    std::vector<Rgb> materials;
    auto material_of = [&materials](const Rgb& c) -> int {
        for (size_t i = 0; i < materials.size(); ++i)
            if (materials[i] == c)
                return int(i);
        materials.push_back(c);
        return int(materials.size() - 1);
    };

    using Role = libvgcode::EGCodeExtrusionRole;
    std::map<std::pair<int, int>, Mesh> meshes; // (role, material) -> mesh
    Mesh                                travel_mesh;
    uint64_t                            tris = 0;
    std::vector<double>                 gradient_used_mm(filaments.size(), 0.0);
    constexpr double                    filament_area = 2.405; // mm2 of 1.75 mm filament

    struct Chain
    {
        int                     role = -1;
        std::vector<int>        mats; // 1 entry, or one per coaxial stripe
        std::vector<ChainPoint> pts;
    } chain;

    auto flush = [&]() {
        if (chain.pts.size() >= 2) {
            simplify_chain(chain.pts, eps);
            float total = 0.0f;
            for (size_t k = 0; k + 1 < chain.pts.size(); ++k)
                total += len(chain.pts[k + 1].p - chain.pts[k].p);
            if (total >= min_chain_len) {
                const int ns = int(chain.mats.size());
                for (int s = 0; s < ns; ++s) {
                    Mesh& m = meshes[{ chain.role, chain.mats[size_t(s)] }];
                    if (ns == 1)
                        add_tube(m, chain.pts, sides, 0.0f, 1.0f, tris);
                    else {
                        // coaxial stripes: ns side-by-side tubes, each a share of the width
                        std::vector<ChainPoint> narrow = chain.pts;
                        for (size_t k = 0; k < narrow.size(); ++k) {
                            narrow[k].w = narrow[k].w / float(ns);
                        }
                        // lateral offsets vary with the bead width: use the mean width of the chain
                        float wm = 0.0f;
                        for (const ChainPoint& cp : chain.pts)
                            wm += cp.w;
                        wm /= float(chain.pts.size());
                        add_tube(m, narrow, sides, (float(s) - 0.5f * float(ns - 1)) * wm / float(ns), 1.0f, tris);
                    }
                }
            }
        }
        chain.pts.clear();
        chain.role = -1;
        chain.mats.clear();
    };

    const size_t nverts = viewer.get_vertices_count();
    std::array<double, 3> bbmin{ 1e30, 1e30, 1e30 }, bbmax{ -1e30, -1e30, -1e30 };
    for (size_t i = 1; i < nverts; ++i) {
        const libvgcode::PathVertex& v = viewer.get_vertex_at(i);
        const libvgcode::PathVertex& p = viewer.get_vertex_at(i - 1);
        if (!(p.position[0] < 1.0e6f) || !(v.position[0] < 1.0e6f)) {
            flush();
            continue;
        }
        const P3 a{ p.position[0], p.position[1], p.position[2] };
        const P3 b{ v.position[0], v.position[1], v.position[2] };
        if (v.is_extrusion()) {
            if (len(b - a) < 1e-4f) // zero length: nothing to draw
                continue;
            const size_t tool = std::min<size_t>(v.extruder_id, filaments.size() - 1);
            FilamentInfo& f   = filaments[tool];
            const float seg_len = len(b - a);
            const double seg_mm3 = double(v.mm3_per_mm) * seg_len;
            f.mm3 += seg_mm3;
            f.length_mm += seg_mm3 / filament_area;

            std::vector<int> mats;
            if (f.multi.size() >= 2 && !f.gradient) {
                for (const Rgb& c : f.multi)
                    mats.push_back(material_of(c));
            } else if (f.multi.size() >= 2 && f.gradient) {
                const double cycle = f.cycle_m > 0.0f ? double(f.cycle_m) : 50.0;
                const double t     = (gradient_used_mm[tool] + 0.5 * seg_mm3 / filament_area) * 0.001 / cycle;
                const double x     = (t - std::floor(t)) * double(f.multi.size());
                mats.push_back(material_of(f.multi[std::min<size_t>(size_t(x), f.multi.size() - 1)]));
                gradient_used_mm[tool] += seg_mm3 / filament_area;
            } else
                mats.push_back(material_of(f.colour));

            const int role = int(v.role);
            if (chain.pts.empty() || chain.role != role || chain.mats != mats) {
                flush();
                chain.role = role;
                chain.mats = mats;
                // the viewer draws the beads half a height below the vertex z (the vertex z is the layer top)
                chain.pts.push_back({ { a.x, a.y, a.z - 0.5f * v.height }, v.width, v.height });
            }
            chain.pts.push_back({ { b.x, b.y, b.z - 0.5f * v.height }, v.width, v.height });
            for (int k = 0; k < 3; ++k) {
                bbmin[size_t(k)] = std::min<double>(bbmin[size_t(k)], v.position[size_t(k)]);
                bbmax[size_t(k)] = std::max<double>(bbmax[size_t(k)], v.position[size_t(k)]);
            }
        } else {
            flush();
            if (opt.travel && v.type == libvgcode::EMoveType::Travel) {
                const uint32_t base = uint32_t(travel_mesh.vcount());
                push_gltf(travel_mesh, a);
                push_gltf(travel_mesh, b);
                travel_mesh.idx.push_back(base);
                travel_mesh.idx.push_back(base + 1);
            }
        }
    }
    flush();
    if (meshes.empty())
        return fail("empty", "the Preview has no extrusion moves");

    // ---- bounds of the quantisation grid (glTF space) ----
    std::array<float, 3> qmin{ 1e30f, 1e30f, 1e30f }, qmax{ -1e30f, -1e30f, -1e30f };
    auto grow = [&](const Mesh& m) {
        for (size_t i = 0; i + 2 < m.pos.size(); i += 3)
            for (size_t k = 0; k < 3; ++k) {
                qmin[k] = std::min(qmin[k], m.pos[i + k]);
                qmax[k] = std::max(qmax[k], m.pos[i + k]);
            }
    };
    for (const auto& kv : meshes)
        grow(kv.second);
    if (!travel_mesh.pos.empty())
        grow(travel_mesh);
    std::array<float, 3> qscale{};
    for (size_t k = 0; k < 3; ++k) {
        const float ext = qmax[k] - qmin[k];
        qscale[k]       = ext > 1e-6f ? ext / 65535.0f : 1.0f;
    }

    // ---- binary chunk + glTF json ----
    Bin   bin;
    json  buffer_views = json::array(), accessors = json::array(), glb_materials = json::array(), meshes_json = json::array(),
          nodes = json::array(), role_nodes = json::array();

    auto add_view = [&](size_t offset, size_t bytes, int target, int stride) {
        json bv = { { "buffer", 0 }, { "byteOffset", offset }, { "byteLength", bytes } };
        if (target)
            bv["target"] = target;
        if (stride)
            bv["byteStride"] = stride;
        buffer_views.push_back(bv);
        return int(buffer_views.size() - 1);
    };

    // returns {position accessor, index accessor}
    auto add_mesh_data = [&](const Mesh& m) -> std::pair<int, int> {
        const size_t nv = m.vcount();
        // positions: u16 x3, padded to 8 bytes per vertex (attribute alignment)
        std::vector<uint16_t> q(nv * 4, 0);
        std::array<uint16_t, 3> mn{ 65535, 65535, 65535 }, mx{ 0, 0, 0 };
        for (size_t i = 0; i < nv; ++i)
            for (size_t k = 0; k < 3; ++k) {
                const float f = (m.pos[i * 3 + k] - qmin[k]) / qscale[k];
                const uint16_t u = uint16_t(std::min(65535.0f, std::max(0.0f, std::round(f))));
                q[i * 4 + k] = u;
                mn[k]        = std::min(mn[k], u);
                mx[k]        = std::max(mx[k], u);
            }
        bin.align4();
        const size_t po = bin.append(q.data(), q.size() * sizeof(uint16_t));
        const int    pv = add_view(po, q.size() * sizeof(uint16_t), 34962, 8);
        accessors.push_back({ { "bufferView", pv }, { "componentType", 5123 }, { "count", nv }, { "type", "VEC3" },
                              { "min", { mn[0], mn[1], mn[2] } }, { "max", { mx[0], mx[1], mx[2] } } });
        const int pa = int(accessors.size() - 1);

        bin.align4();
        int ia;
        if (nv <= 65535) {
            std::vector<uint16_t> ix(m.idx.begin(), m.idx.end());
            const size_t io = bin.append(ix.data(), ix.size() * 2);
            const int    iv = add_view(io, ix.size() * 2, 34963, 0);
            accessors.push_back({ { "bufferView", iv }, { "componentType", 5123 }, { "count", ix.size() }, { "type", "SCALAR" } });
        } else {
            const size_t io = bin.append(m.idx.data(), m.idx.size() * 4);
            const int    iv = add_view(io, m.idx.size() * 4, 34963, 0);
            accessors.push_back({ { "bufferView", iv }, { "componentType", 5125 }, { "count", m.idx.size() }, { "type", "SCALAR" } });
        }
        ia = int(accessors.size() - 1);
        return { pa, ia };
    };

    // materials
    std::map<int, int> material_index; // internal -> glTF
    for (size_t i = 0; i < materials.size(); ++i) {
        const Rgb& c = materials[i];
        // filament name whose colour this is, for the viewer legend
        std::string name = to_hex(c);
        for (const FilamentInfo& f : filaments)
            if (f.colour == c) {
                name = f.name;
                break;
            }
        glb_materials.push_back({ { "name", name },
                                  { "doubleSided", true },
                                  { "pbrMetallicRoughness",
                                    { { "baseColorFactor", { srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b), 1.0f } },
                                      { "metallicFactor", 0.0f },
                                      { "roughnessFactor", 0.75f } } },
                                  { "extras", { { "hex", to_hex(c) } } } });
        material_index[int(i)] = int(i);
    }

    // one node per role (mesh with a primitive per material)
    std::map<int, std::vector<std::pair<int, const Mesh*>>> by_role;
    for (const auto& kv : meshes)
        by_role[kv.first.first].emplace_back(kv.first.second, &kv.second);

    json roles_meta = json::array();
    for (const auto& kv : by_role) {
        const Role  role = static_cast<Role>(kv.first);
        const RoleInfo info = role_info(role);
        json        prims = json::array();
        uint64_t    role_tris = 0;
        for (const auto& pm : kv.second) {
            const auto acc = add_mesh_data(*pm.second);
            prims.push_back({ { "attributes", { { "POSITION", acc.first } } }, { "indices", acc.second },
                              { "material", material_index[pm.first] }, { "mode", 4 } });
            role_tris += pm.second->idx.size() / 3;
        }
        meshes_json.push_back({ { "name", info.id }, { "primitives", prims } });
        const Rgb rc{ viewer.get_extrusion_role_color(role)[0], viewer.get_extrusion_role_color(role)[1],
                      viewer.get_extrusion_role_color(role)[2] };
        json extras = { { "role", info.id }, { "label", info.label }, { "roleColour", to_hex(rc) }, { "triangles", role_tris },
                        { "timeSeconds", viewer.get_extrusion_role_estimated_time(role) } };
        nodes.push_back({ { "name", info.id }, { "mesh", int(meshes_json.size() - 1) }, { "extras", extras } });
        role_nodes.push_back(int(nodes.size() - 1));
        roles_meta.push_back(extras);
    }
    if (!travel_mesh.idx.empty()) {
        const auto acc = add_mesh_data(travel_mesh);
        glb_materials.push_back({ { "name", "travel" }, { "doubleSided", true },
                                  { "pbrMetallicRoughness", { { "baseColorFactor", { 0.2f, 0.45f, 0.9f, 1.0f } }, { "metallicFactor", 0.0f }, { "roughnessFactor", 1.0f } } } });
        meshes_json.push_back({ { "name", "travel" },
                                { "primitives", json::array({ { { "attributes", { { "POSITION", acc.first } } }, { "indices", acc.second },
                                                                { "material", int(glb_materials.size() - 1) }, { "mode", 1 } } }) } });
        json extras = { { "role", "travel" }, { "label", "Travel" }, { "roleColour", "#3373E6" },
                        { "timeSeconds", viewer.get_travels_estimated_time() } };
        nodes.push_back({ { "name", "travel" }, { "mesh", int(meshes_json.size() - 1) }, { "extras", extras } });
        role_nodes.push_back(int(nodes.size() - 1));
        roles_meta.push_back(extras);
    }

    // root node undoes the quantisation
    nodes.push_back({ { "name", "Orca Zoukei toolpaths" },
                      { "children", role_nodes },
                      { "translation", { qmin[0], qmin[1], qmin[2] } },
                      { "scale", { qscale[0], qscale[1], qscale[2] } } });
    const int root = int(nodes.size() - 1);

    // ---- metadata ----
    json meta;
    meta["generator"] = std::string("Orca Zoukei ") + SoftFever_VERSION;
    meta["detail"]    = glb_detail_name(opt.detail);
    meta["travel"]    = opt.travel && !travel_mesh.idx.empty();
    meta["units"]     = "mm";
    meta["printTimeSeconds"] = viewer.get_estimated_time();
    meta["printTime"] = format_duration(viewer.get_estimated_time());
    meta["layerCount"] = viewer.get_layers_count();
    json lz = json::array();
    for (float z : viewer.get_layers_zs())
        lz.push_back(std::round(z * 1000.0f) / 1000.0f);
    meta["layerZ"] = lz;
    json fil = json::array();
    double total_g = 0.0;
    for (size_t i = 0; i < filaments.size(); ++i) {
        const FilamentInfo& f = filaments[i];
        const double grams = f.mm3 / 1000.0 * double(f.density);
        total_g += grams;
        json jf = { { "index", i }, { "name", f.name }, { "type", f.type }, { "colour", to_hex(f.colour) },
                    { "weightG", std::round(grams * 100.0) / 100.0 }, { "lengthM", std::round(f.length_mm) / 1000.0 },
                    { "used", f.mm3 > 0.0 } };
        if (f.multi.size() >= 2) {
            json cols = json::array();
            for (const Rgb& c : f.multi)
                cols.push_back(to_hex(c));
            jf["colours"]  = cols;
            jf["coaxial"]  = !f.gradient;
            jf["gradient"] = f.gradient;
        }
        fil.push_back(jf);
    }
    meta["filaments"]    = fil;
    meta["totalWeightG"] = std::round(total_g * 100.0) / 100.0;
    meta["roles"]        = roles_meta;
    json objs = json::array();
    for (const ModelObject* mo : wxGetApp().plater()->model().objects)
        objs.push_back(mo->name);
    meta["objects"] = objs;
    meta["boundsMm"] = { { "min", { bbmin[0], bbmin[1], bbmin[2] } }, { "max", { bbmax[0], bbmax[1], bbmax[2] } } };

    bin.align4();
    json gltf = {
        { "asset", { { "version", "2.0" }, { "generator", meta["generator"] }, { "extras", { { "zoukei", meta } } } } },
        { "extensionsUsed", { "KHR_mesh_quantization" } },
        { "extensionsRequired", { "KHR_mesh_quantization" } },
        { "scene", 0 },
        { "scenes", json::array({ { { "name", "Toolpaths" }, { "nodes", { root } }, { "extras", { { "zoukei", meta } } } } }) },
        { "nodes", nodes },
        { "meshes", meshes_json },
        { "materials", glb_materials },
        { "accessors", accessors },
        { "bufferViews", buffer_views },
        { "buffers", json::array({ { { "byteLength", bin.data.size() } } }) }
    };
    std::string js = gltf.dump(-1, ' ', false, json::error_handler_t::replace);
    while (js.size() % 4)
        js.push_back(' ');

    std::vector<uint8_t> glb;
    glb.reserve(28 + js.size() + bin.data.size());
    glb.push_back('g'); glb.push_back('l'); glb.push_back('T'); glb.push_back('F');
    put_u32(glb, 2);
    put_u32(glb, uint32_t(12 + 8 + js.size() + 8 + bin.data.size()));
    put_u32(glb, uint32_t(js.size()));
    put_u32(glb, 0x4E4F534Au); // JSON
    glb.insert(glb.end(), js.begin(), js.end());
    put_u32(glb, uint32_t(bin.data.size()));
    put_u32(glb, 0x004E4942u); // BIN
    glb.insert(glb.end(), bin.data.begin(), bin.data.end());
    bin.data.clear();
    bin.data.shrink_to_fit();

    if (glb.size() >= 0xFFFFFFF0ull)
        return fail("too_large", "the GLB would exceed 4 GB: use a lower detail level");
    if (!write_file(glb_fs, glb.data(), glb.size()))
        return fail("write_failed", "could not write the .glb (is it open in another program?)");
    res.glb_path  = boost::nowide::narrow(glb_fs.wstring());
    res.glb_bytes = glb.size();

    // ---- viewer html ----
    if (opt.html != GlbHtml::None) {
        std::string html = tmpl;
        const std::string title = html_escape(wxGetApp().plater()->get_project_name().ToUTF8().data());
        replace_all(html, "__ZK_TITLE__", title.empty() ? std::string("Orca Zoukei toolpaths") : title);
        if (opt.html == GlbHtml::Embedded) {
            replace_all(html, "__ZK_GLB_FILE__", "");
            replace_all(html, "__ZK_GLB_BASE64__", base64(glb));
        } else {
            replace_all(html, "__ZK_GLB_FILE__", html_escape(glb_fs.filename().string()));
            replace_all(html, "__ZK_GLB_BASE64__", "");
        }
        if (!write_file(html_fs, html.data(), html.size())) {
            res.ok = false;
            res.error_code = "write_failed";
            res.error = "the .glb was written but the .html could not be";
            return res;
        }
        res.html_path  = boost::nowide::narrow(html_fs.wstring());
        res.html_bytes = html.size();
    }

    res.ok        = true;
    res.triangles = tris;
    res.roles     = uint32_t(by_role.size());
    res.materials = uint32_t(materials.size());
    return res;
}

// ------------------------------------------------------------------------------------------------------

void export_toolpaths_glb_dialog(wxWindow* parent)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->printer_technology() != ptFFF || !plater->is_preview_loaded()) {
        wxMessageBox(_L("Slice the plate and open the Preview first."), _L("Export toolpaths as GLB"), wxOK | wxICON_INFORMATION, parent);
        return;
    }

    wxDialog dlg(parent, wxID_ANY, _L("Export toolpaths as GLB / 3D web viewer"), wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE);
    wxBoxSizer* top  = new wxBoxSizer(wxVERTICAL);
    wxFlexGridSizer* grid = new wxFlexGridSizer(2, 8, 8);
    grid->AddGrowableCol(1);

    wxChoice* detail = new wxChoice(&dlg, wxID_ANY);
    detail->Append(_L("Preview (smallest file)"));
    detail->Append(_L("Standard"));
    detail->Append(_L("Full (rounded beads, largest file)"));
    detail->SetSelection(1);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Detail level")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(detail, 1, wxEXPAND);

    wxChoice* html = new wxChoice(&dlg, wxID_ANY);
    html->Append(_L("GLB only"));
    html->Append(_L("GLB + viewer .html next to it"));
    html->Append(_L("One .html file with the model embedded (to share)"));
    html->SetSelection(2);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("3D web viewer")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(html, 1, wxEXPAND);

    wxCheckBox* travel = new wxCheckBox(&dlg, wxID_ANY, _L("Include travel moves"));
    travel->SetValue(false);

    top->Add(grid, 0, wxEXPAND | wxALL, 12);
    top->Add(travel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    top->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
    dlg.SetSizerAndFit(top);
    dlg.CentreOnParent();
    if (dlg.ShowModal() != wxID_OK)
        return;

    GlbExportOptions opt;
    opt.detail = detail->GetSelection() == 0 ? GlbDetail::Preview : (detail->GetSelection() == 2 ? GlbDetail::Full : GlbDetail::Standard);
    opt.html   = html->GetSelection() == 0 ? GlbHtml::None : (html->GetSelection() == 1 ? GlbHtml::Linked : GlbHtml::Embedded);
    opt.travel = travel->GetValue();

    wxString default_name = plater->get_project_name();
    if (default_name.empty())
        default_name = "toolpaths";
    wxFileDialog fd(parent, _L("Save toolpaths as GLB"), wxEmptyString, default_name + ".glb", "GLB (*.glb)|*.glb",
                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fd.ShowModal() != wxID_OK)
        return;
    std::string path = fd.GetPath().ToUTF8().data();
    if (lower(to_fs(path).extension().string()) != ".glb")
        path += ".glb";

    // the user confirmed the .glb; ask for the .html twin separately
    opt.overwrite = true;
    if (opt.html != GlbHtml::None) {
        boost::system::error_code ec;
        if (boost::filesystem::exists(to_fs(glb_html_path_for(path)), ec) &&
            wxMessageBox(_L("The viewer .html already exists. Replace it?"), _L("Export toolpaths as GLB"), wxYES_NO | wxICON_QUESTION, parent) != wxYES)
            opt.html = GlbHtml::None;
    }

    const GlbExportResult r = export_toolpaths_glb(path, opt);
    if (!r.ok) {
        wxMessageBox(wxString::FromUTF8(r.error.c_str()), _L("Export toolpaths as GLB"), wxOK | wxICON_ERROR, parent);
        return;
    }
    wxString msg = wxString::Format(_L("Saved %s (%.1f MB)."), wxString::FromUTF8(r.glb_path.c_str()), double(r.glb_bytes) / 1048576.0);
    if (!r.html_path.empty())
        msg += "\n" + wxString::Format(_L("Viewer: %s"), wxString::FromUTF8(r.html_path.c_str()));
    wxMessageBox(msg, _L("Export toolpaths as GLB"), wxOK | wxICON_INFORMATION, parent);
}

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
