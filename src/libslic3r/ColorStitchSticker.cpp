// SVG stickers of the ColorStitch Painter, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
// See ColorStitchSticker.hpp.
#include "ColorStitchSticker.hpp"

#include "ClipperUtils.hpp"
#include "Model.hpp"
#include "NSVGUtils.hpp"
#include "SurfaceEffectProfile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

std::string b64_encode(const std::string &text)
{
    if (text.empty())
        return std::string();
    std::string out;
    out.resize(boost::beast::detail::base64::encoded_size(text.size()));
    out.resize(boost::beast::detail::base64::encode(&out[0], text.data(), text.size()));
    return out;
}

std::string b64_decode(const std::string &b64)
{
    if (b64.empty())
        return std::string();
    std::string out;
    out.resize(boost::beast::detail::base64::decoded_size(b64.size()));
    const auto res = boost::beast::detail::base64::decode(&out[0], b64.data(), b64.size());
    out.resize(res.first);
    return out;
}

// 0xRRGGBB of a nanosvg paint (a gradient becomes the mean of its stops), 0xFFFFFFFF when the paint
// has no usable colour. `alpha` receives 0..255.
uint32_t paint_rgb(const NSVGpaint &p, int &alpha)
{
    alpha = 255;
    auto unpack = [](unsigned int c, double &r, double &g, double &b, int &a) {
        r += double(c & 0xFFu);
        g += double((c >> 8) & 0xFFu);
        b += double((c >> 16) & 0xFFu);
        a = int((c >> 24) & 0xFFu);
    };
    double r = 0., g = 0., b = 0.;
    if (p.type == NSVG_PAINT_COLOR) {
        unpack(p.color, r, g, b, alpha);
    } else if ((p.type == NSVG_PAINT_LINEAR_GRADIENT || p.type == NSVG_PAINT_RADIAL_GRADIENT) && p.gradient != nullptr && p.gradient->nstops > 0) {
        const int n = p.gradient->nstops;
        int       a = 255;
        for (int i = 0; i < n; ++i)
            unpack(p.gradient->stops[i].color, r, g, b, a);
        r /= n;
        g /= n;
        b /= n;
        alpha = a;
        return (uint32_t(std::lround(r)) << 16) | (uint32_t(std::lround(g)) << 8) | uint32_t(std::lround(b));
    } else
        return 0xFFFFFFFFu;
    return (uint32_t(std::lround(r)) << 16) | (uint32_t(std::lround(g)) << 8) | uint32_t(std::lround(b));
}

struct LocalShape
{
    uint32_t   rgb        = 0;
    bool       background = false;
    ExPolygons area; // sticker-local frame: scaled mm, drawing centred on the origin, +Y up
};

struct LocalSvg
{
    std::string             svg;
    std::vector<LocalShape> shapes;      // drawing order, background included (flagged)
    std::vector<uint32_t>   colors;      // distinct, first appearance first, background excluded
    Vec2d                   size_mm = Vec2d::Zero();
};

ExPolygons transform_clip(const ExPolygons &src, const float xf[6])
{
    // xf maps clip-local SVG units to image SVG units; the polygons are scaled mm (units * 1/SCALING_FACTOR).
    const double a = xf[0], b = xf[1], c = xf[2], d = xf[3];
    const double e = double(xf[4]) / SCALING_FACTOR, f = double(xf[5]) / SCALING_FACTOR;
    ExPolygons   out;
    out.reserve(src.size());
    auto map_polygon = [&](const Polygon &p) {
        Polygon q;
        q.points.reserve(p.points.size());
        for (const Point &pt : p.points) {
            const double x = double(pt.x()), y = double(pt.y());
            q.points.emplace_back(coord_t(std::llround(a * x + c * y + e)), coord_t(std::llround(b * x + d * y + f)));
        }
        return q;
    };
    for (const ExPolygon &ep : src) {
        ExPolygon q;
        q.contour = map_polygon(ep.contour);
        q.contour.make_counter_clockwise();
        for (const Polygon &h : ep.holes) {
            q.holes.push_back(map_polygon(h));
            q.holes.back().make_clockwise();
        }
        out.push_back(std::move(q));
    }
    return out;
}

// <clipPath> of a shape, applied to its tessellated area. <mask> is ignored (ColorStitchSticker.hpp).
void apply_clips(const NSVGshape &shape, const NSVGLineParams &params, ExPolygons &area)
{
    for (int i = 0; i < shape.nclips && !area.empty(); ++i) {
        const NSVGclipRef &ref = shape.clips[i];
        if (ref.clip == nullptr || ref.clip->shapes == nullptr || ref.clip->type != NSVG_CLIP_PATH)
            continue;
        NSVGimage tmp{};
        tmp.shapes = ref.clip->shapes;
        ExPolygons clip;
        for (const ExPolygonsWithId &cs : create_shape_with_ids(tmp, params, false))
            append(clip, cs.expoly);
        if (clip.empty()) {
            area.clear(); // a clip that covers nothing hides the shape
            return;
        }
        clip = union_ex(transform_clip(clip, ref.xform));
        area = intersection_ex(area, clip);
    }
}

std::shared_ptr<LocalSvg> build_local_svg(const std::string &svg_data)
{
    auto loc = std::make_shared<LocalSvg>();
    loc->svg = svg_data;
    if (svg_data.empty())
        return loc;
    NSVGimage_ptr image = nsvgParse(svg_data, "mm", 96.0f);
    if (image == nullptr || image->shapes == nullptr)
        return loc;

    // Tessellation tolerance 0.1 mm, as NeotkoCM. Raw coordinates (+Y down): the drawing is centred and
    // flipped below, after the clip paths have been applied.
    const double tol_mm = 0.1;
    NSVGLineParams params{ (tol_mm * tol_mm) / SCALING_FACTOR / SCALING_FACTOR };
    params.is_y_negative = false;
    const ExPolygonsWithIds shapes = create_shape_with_ids(*image, params, false);
    if (shapes.empty())
        return loc;

    std::vector<const NSVGshape *> by_index;
    for (const NSVGshape *sh = image->shapes; sh != nullptr; sh = sh->next)
        by_index.push_back(sh);

    std::vector<LocalShape> raw;
    BoundingBox             all;
    for (const ExPolygonsWithId &sh : shapes) {
        const size_t si        = size_t(sh.id / 2);
        const bool   is_stroke = (sh.id & 1u) != 0;
        if (si >= by_index.size() || sh.expoly.empty())
            continue;
        const NSVGshape &ns = *by_index[si];
        int              alpha = 255;
        const uint32_t   rgb   = paint_rgb(is_stroke ? ns.stroke : ns.fill, alpha);
        if (rgb == 0xFFFFFFFFu || alpha < 13 || ns.opacity < 0.05f)
            continue;
        LocalShape ls;
        ls.rgb  = rgb;
        ls.area = sh.expoly;
        if (ns.nclips > 0)
            apply_clips(ns, params, ls.area);
        if (ls.area.empty())
            continue;
        all.merge(get_extents(ls.area));
        raw.push_back(std::move(ls));
    }
    if (raw.empty() || !all.defined)
        return loc;

    // Centre on the drawing and flip Y (SVG is Y down).
    const Point center = all.center();
    for (LocalShape &ls : raw)
        for (ExPolygon &ep : ls.area) {
            auto fix = [&center](Polygon &p) {
                for (Point &pt : p.points)
                    pt = Point(pt.x() - center.x(), -(pt.y() - center.y()));
            };
            fix(ep.contour);
            ep.contour.make_counter_clockwise();
            for (Polygon &h : ep.holes) {
                fix(h);
                h.make_clockwise();
            }
        }

    // A background is the first shape when it covers the whole drawing and there is more than one shape.
    if (raw.size() > 1) {
        const BoundingBox b0 = get_extents(raw.front().area);
        const Point       s0 = b0.size(), sa = all.size();
        if (double(s0.x()) >= 0.98 * double(sa.x()) && double(s0.y()) >= 0.98 * double(sa.y()))
            raw.front().background = true;
    }

    BoundingBox art;
    for (const LocalShape &ls : raw) {
        if (ls.background)
            continue;
        art.merge(get_extents(ls.area));
        if (std::find(loc->colors.begin(), loc->colors.end(), ls.rgb) == loc->colors.end())
            loc->colors.push_back(ls.rgb);
    }
    if (art.defined)
        loc->size_mm = Vec2d(unscale<double>(art.size().x()), unscale<double>(art.size().y()));
    loc->shapes = std::move(raw);
    return loc;
}

std::mutex                                   g_local_mutex;
std::vector<std::shared_ptr<const LocalSvg>> g_local_cache; // most recent last

// Parsed once per SVG: make_fills runs on many threads and every layer asks for the same drawing.
std::shared_ptr<const LocalSvg> local_svg(const std::string &svg)
{
    {
        std::lock_guard<std::mutex> lock(g_local_mutex);
        for (size_t i = g_local_cache.size(); i-- > 0;)
            if (g_local_cache[i]->svg == svg)
                return g_local_cache[i];
    }
    std::shared_ptr<const LocalSvg> built = build_local_svg(svg);
    std::lock_guard<std::mutex>     lock(g_local_mutex);
    for (const auto &e : g_local_cache)
        if (e->svg == svg)
            return e;
    if (g_local_cache.size() >= 16)
        g_local_cache.erase(g_local_cache.begin());
    g_local_cache.push_back(built);
    return built;
}

ExPolygons project(const ExPolygons &src, const Transform3d &to_target)
{
    ExPolygons out;
    out.reserve(src.size());
    auto map_polygon = [&to_target](const Polygon &p) {
        Polygon q;
        q.points.reserve(p.points.size());
        for (const Point &pt : p.points) {
            const Vec3d v = to_target * Vec3d(unscale<double>(pt.x()), unscale<double>(pt.y()), 0.0);
            q.points.emplace_back(coord_t(scale_(v.x())), coord_t(scale_(v.y())));
        }
        return q;
    };
    for (const ExPolygon &ep : src) {
        ExPolygon q;
        q.contour = map_polygon(ep.contour);
        q.contour.make_counter_clockwise(); // a mirror or a flip to the bottom reverses the winding
        for (const Polygon &h : ep.holes) {
            q.holes.push_back(map_polygon(h));
            q.holes.back().make_clockwise();
        }
        if (std::abs(q.contour.area()) >= double(SCALED_EPSILON))
            out.push_back(std::move(q));
    }
    return out;
}

ExPolygons unite(const ExPolygons &a, const ExPolygons &b)
{
    Polygons p = to_polygons(a);
    polygons_append(p, to_polygons(b));
    return union_ex(p);
}

struct Fnv
{
    uint64_t h = 1469598103934665603ull;
    void add(const void *data, size_t n)
    {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= uint64_t(p[i]);
            h *= 1099511628211ull;
        }
    }
    void add(const std::string &s)
    {
        add(s.data(), s.size());
        const char sep = '\x1f';
        add(&sep, 1);
    }
    void add(int v) { add(&v, sizeof(v)); }
    void add(double v) { add(&v, sizeof(v)); }
};

void add_payload(Fnv &f, const SurfaceEffectPayload &p)
{
    f.add(p.present ? 1 : 0);
    for (const auto &kv : p.kv) {
        f.add(kv.first);
        f.add(kv.second);
    }
}

} // namespace

namespace ColorStitchStickerTools {

std::string serialize(const std::vector<ColorStitchSticker> &stickers)
{
    if (stickers.empty())
        return std::string();
    nlohmann::json arr = nlohmann::json::array();
    for (const ColorStitchSticker &st : stickers) {
        nlohmann::json j;
        j["n"] = st.name;
        j["s"] = b64_encode(st.svg_data);
        j["p"] = st.profile_id;
        nlohmann::json t = nlohmann::json::array();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                t.push_back(st.transform.matrix()(r, c));
        j["t"] = std::move(t);
        if (!st.colors.empty()) {
            nlohmann::json c = nlohmann::json::array();
            for (const ColorStitchStickerColor &col : st.colors)
                c.push_back({ { "rgb", col.rgb }, { "p", col.profile_id }, { "h", col.hidden ? 1 : 0 } });
            j["c"] = std::move(c);
        }
        if (st.hidden)
            j["h"] = 1;
        arr.push_back(std::move(j));
    }
    return b64_encode(arr.dump());
}

void parse(const std::string &b64, std::vector<ColorStitchSticker> &stickers)
{
    stickers.clear();
    const std::string json_str = b64_decode(b64);
    if (json_str.empty())
        return;
    try {
        const nlohmann::json arr = nlohmann::json::parse(json_str);
        if (!arr.is_array())
            return;
        for (const nlohmann::json &j : arr) {
            ColorStitchSticker st;
            st.name       = j.value("n", std::string());
            st.svg_data   = b64_decode(j.value("s", std::string()));
            st.profile_id = j.value("p", 0);
            if (j.contains("t") && j["t"].is_array() && j["t"].size() == 16) {
                int k = 0;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        st.transform.matrix()(r, c) = j["t"][size_t(k++)].get<double>();
            }
            if (j.contains("c") && j["c"].is_array())
                for (const nlohmann::json &jc : j["c"]) {
                    ColorStitchStickerColor col;
                    col.rgb        = jc.value("rgb", 0u);
                    col.profile_id = jc.value("p", 0);
                    col.hidden     = jc.value("h", 0) != 0;
                    st.colors.push_back(col);
                }
            st.hidden = j.value("h", 0) != 0;
            if (!st.svg_data.empty())
                stickers.push_back(std::move(st));
        }
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "ColorStitch stickers: cannot parse " << MetadataKey << " of the 3MF";
        stickers.clear();
    }
}

std::vector<uint32_t> svg_colors(const std::string &svg)
{
    if (svg.empty())
        return {};
    return local_svg(svg)->colors;
}

Vec2d svg_size_mm(const std::string &svg)
{
    if (svg.empty())
        return Vec2d::Zero();
    return local_svg(svg)->size_mm;
}

int nearest_color(const std::vector<ColorStitchStickerColor> &colors, uint32_t rgb)
{
    int    best  = -1;
    double best_d = std::numeric_limits<double>::max();
    for (size_t i = 0; i < colors.size(); ++i) {
        const double dr = double((colors[i].rgb >> 16) & 0xFFu) - double((rgb >> 16) & 0xFFu);
        const double dg = double((colors[i].rgb >> 8) & 0xFFu) - double((rgb >> 8) & 0xFFu);
        const double db = double(colors[i].rgb & 0xFFu) - double(rgb & 0xFFu);
        const double d  = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best   = int(i);
        }
    }
    return best;
}

std::vector<ColorMask> color_masks(const ColorStitchSticker &sticker, const Transform3d &to_target)
{
    std::vector<ColorMask> out;
    if (sticker.svg_data.empty())
        return out;
    const std::shared_ptr<const LocalSvg> loc = local_svg(sticker.svg_data);
    std::vector<ExPolygons>               projected(loc->shapes.size());
    for (size_t i = 0; i < loc->shapes.size(); ++i)
        if (!loc->shapes[i].background)
            projected[i] = project(loc->shapes[i].area, to_target);

    // Back to front: what a later shape covers is not visible in an earlier one.
    std::map<int, ExPolygons> by_color;
    ExPolygons                covered;
    for (size_t i = loc->shapes.size(); i-- > 0;) {
        if (projected[i].empty())
            continue;
        ExPolygons visible = covered.empty() ? projected[i] : diff_ex(projected[i], covered);
        if (!visible.empty()) {
            const int ci = sticker.colors.empty() ? -1 : nearest_color(sticker.colors, loc->shapes[i].rgb);
            ExPolygons &dst = by_color[ci];
            append(dst, std::move(visible));
        }
        covered = covered.empty() ? projected[i] : unite(covered, projected[i]);
    }
    for (auto &kv : by_color) {
        ColorMask m;
        m.color_index = kv.first;
        m.area        = union_ex(to_polygons(kv.second));
        if (!m.area.empty())
            out.push_back(std::move(m));
    }
    return out;
}

uint64_t fingerprint(const ModelObject &mo)
{
    if (mo.colorstitch_stickers.empty())
        return 0;
    const SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    Fnv                                f;
    auto add_profile = [&](int pid) {
        f.add(pid);
        if (pid <= 0)
            return;
        if (const SurfaceEffectProfile *p = mgr.find(pid)) {
            f.add(p->stack_top_json);
            f.add(p->stack_penu_json);
            f.add(p->stack_bottom_json);
            add_payload(f, p->colorstitch);
            add_payload(f, p->pathblend);
            add_payload(f, p->multipass);
        } else
            f.add(-1);
    };
    for (const ColorStitchSticker &st : mo.colorstitch_stickers) {
        f.add(st.name);
        f.add(st.svg_data);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                f.add(st.transform.matrix()(r, c));
        f.add(st.hidden ? 1 : 0);
        add_profile(st.profile_id);
        for (const ColorStitchStickerColor &col : st.colors) {
            f.add(int(col.rgb));
            f.add(col.hidden ? 1 : 0);
            add_profile(col.profile_id);
        }
    }
    return f.h == 0 ? 1 : f.h;
}

} // namespace ColorStitchStickerTools

bool model_colorstitch_sticker_data_changed(const ModelObject &mo, const ModelObject &mo_new)
{
    // Cache, not model data: the recipes live outside the model (see ColorStitchStickerTools::fingerprint).
    const uint64_t fp = ColorStitchStickerTools::fingerprint(mo_new);
    const_cast<ModelObject &>(mo_new).colorstitch_sticker_fingerprint = fp;
    return mo.colorstitch_stickers != mo_new.colorstitch_stickers || mo.colorstitch_sticker_fingerprint != fp;
}

} // namespace Slic3r
