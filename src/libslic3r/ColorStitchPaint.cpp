// ColorStitch Painter, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0. See ColorStitchPaint.hpp.
#include "ColorStitchPaint.hpp"

#include "ClipperUtils.hpp"
#include "Config.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "SurfaceEffectProfile.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace ColorStitchPaint {

int profile_for_slot(const ModelVolume &mv, int slot)
{
    if (slot <= 0 || slot >= int(mv.colorstitch_slot_to_profile_id.size()))
        return 0;
    return std::max(0, mv.colorstitch_slot_to_profile_id[size_t(slot)]);
}

bool volume_has_paint(const ModelVolume &mv)
{
    if (mv.colorstitch_paint_facets.empty())
        return false;
    for (size_t s = 1; s < mv.colorstitch_slot_to_profile_id.size(); ++s)
        if (mv.colorstitch_slot_to_profile_id[s] > 0)
            return true;
    return false;
}

bool object_has_paint(const ModelObject &mo)
{
    for (const ModelVolume *mv : mo.volumes)
        if (mv != nullptr && mv->is_model_part() && volume_has_paint(*mv))
            return true;
    return false;
}

namespace {

// FNV-1a, 64 bit.
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
};

void add_payload(Fnv &f, const SurfaceEffectPayload &p)
{
    f.add(p.present ? 1 : 0);
    for (const auto &kv : p.kv) {
        f.add(kv.first);
        f.add(kv.second);
    }
}

bool same_recipe(const SurfaceEffectProfile &a, const SurfaceEffectProfile &b)
{
    auto same_payload = [](const SurfaceEffectPayload &x, const SurfaceEffectPayload &y) {
        return x.present == y.present && x.kv == y.kv;
    };
    return a.name == b.name && a.stack_top_json == b.stack_top_json && a.stack_penu_json == b.stack_penu_json &&
           a.stack_bottom_json == b.stack_bottom_json && same_payload(a.colorstitch, b.colorstitch) &&
           same_payload(a.pathblend, b.pathblend) && same_payload(a.multipass, b.multipass);
}

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

} // namespace

uint64_t profiles_fingerprint(const ModelVolume &mv)
{
    if (!volume_has_paint(mv))
        return 0;
    const SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    Fnv f;
    for (size_t s = 1; s < mv.colorstitch_slot_to_profile_id.size(); ++s) {
        const int pid = mv.colorstitch_slot_to_profile_id[s];
        if (pid <= 0)
            continue;
        f.add(int(s));
        f.add(pid);
        if (const SurfaceEffectProfile *p = mgr.find(pid)) {
            f.add(p->stack_top_json);
            add_payload(f, p->colorstitch);
            add_payload(f, p->pathblend);
            add_payload(f, p->multipass);
        } else
            f.add(-1); // orphan: the recipe was deleted
    }
    // 0 is reserved for "no paint".
    return f.h == 0 ? 1 : f.h;
}

std::string serialize_slot_table(const std::vector<int> &table)
{
    bool any = false;
    for (int v : table)
        if (v != 0) {
            any = true;
            break;
        }
    if (!any)
        return std::string();
    std::string out;
    for (size_t i = 0; i < table.size(); ++i) {
        if (i > 0)
            out += ',';
        out += std::to_string(table[i]);
    }
    return out;
}

std::vector<int> parse_slot_table(const std::string &csv)
{
    std::vector<int> out;
    size_t start = 0;
    while (start <= csv.size() && int(out.size()) < kSlotCount) {
        size_t end = csv.find(',', start);
        if (end == std::string::npos)
            end = csv.size();
        int v = 0;
        try {
            v = std::stoi(csv.substr(start, end - start));
        } catch (...) {
            v = 0;
        }
        out.push_back(std::max(0, v));
        start = end + 1;
    }
    // NeotkoCM writes up to 255 entries; the slots this fork cannot paint are dropped.
    bool any = false;
    for (int v : out)
        any |= v != 0;
    if (!any)
        out.clear();
    return out;
}

void load_volume_paint(ModelVolume &mv, const std::vector<std::string> &mask, size_t first_triangle, size_t count)
{
    if (mask.size() <= first_triangle)
        return; // no paint in this file (or not on this volume)
    mv.colorstitch_paint_facets.reserve(int(count));
    for (size_t i = 0; i < count; ++i) {
        const size_t index = first_triangle + i;
        if (index < mask.size() && !mask[index].empty())
            mv.colorstitch_paint_facets.set_triangle_from_string(int(i), mask[index]);
    }
    mv.colorstitch_paint_facets.shrink_to_fit();
    mv.colorstitch_paint_facets.touch();
}

std::string library_to_metadata()
{
    const SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    if (mgr.size() == 0)
        return std::string();
    return b64_encode(mgr.to_json());
}

void library_from_metadata(Model &model, const std::string &b64)
{
    const std::string json = b64_decode(b64);
    if (json.empty())
        return;
    SurfaceEffectProfileManager &mgr = SurfaceEffectProfileManager::get();
    // Parse through the manager, then put the current library back and merge into it.
    std::vector<SurfaceEffectProfile> kept    = mgr.list();
    const int                         kept_id = mgr.next_id();
    if (!mgr.from_json(json)) {
        mgr.restore(std::move(kept), kept_id);
        BOOST_LOG_TRIVIAL(warning) << "ColorStitch Painter: cannot parse the recipe library of the 3MF";
        return;
    }
    std::vector<SurfaceEffectProfile> loaded = mgr.list();
    mgr.restore(std::move(kept), kept_id);

    std::map<int, int> remap;
    for (SurfaceEffectProfile &p : loaded) {
        const SurfaceEffectProfile *existing = mgr.find(p.id);
        if (existing == nullptr) {
            const int id = p.id;
            if (!mgr.adopt(std::move(p)))
                BOOST_LOG_TRIVIAL(warning) << "ColorStitch Painter: recipe " << id << " not loaded";
        } else if (!same_recipe(*existing, p)) {
            const int old_id = p.id;
            remap[old_id]    = mgr.add(std::move(p));
        }
    }
    if (remap.empty())
        return;
    for (ModelObject *mo : model.objects)
        for (ModelVolume *mv : mo->volumes)
            for (int &pid : mv->colorstitch_slot_to_profile_id) {
                auto it = remap.find(pid);
                if (it != remap.end())
                    pid = it->second;
            }
}

} // namespace ColorStitchPaint

bool model_colorstitch_paint_data_changed(const ModelObject &mo, const ModelObject &mo_new)
{
    std::vector<const ModelVolume *> old_parts, new_parts;
    for (const ModelVolume *mv : mo.volumes)
        if (mv->is_model_part())
            old_parts.push_back(mv);
    for (const ModelVolume *mv : mo_new.volumes)
        if (mv->is_model_part())
            new_parts.push_back(mv);
    bool changed = old_parts.size() != new_parts.size();
    for (size_t i = 0; i < new_parts.size(); ++i) {
        const ModelVolume &mv_new = *new_parts[i];
        // Cache, not model data: the recipes live outside the model (see profiles_fingerprint).
        const uint64_t fp = ColorStitchPaint::profiles_fingerprint(mv_new);
        const_cast<ModelVolume &>(mv_new).colorstitch_profiles_fingerprint = fp;
        if (changed)
            continue;
        const ModelVolume &mv_old = *old_parts[i];
        changed = !mv_old.colorstitch_paint_facets.timestamp_matches(mv_new.colorstitch_paint_facets) ||
                  mv_old.colorstitch_slot_to_profile_id != mv_new.colorstitch_slot_to_profile_id ||
                  mv_old.colorstitch_profiles_fingerprint != fp;
    }
    return changed;
}

namespace {

void apply_payload(PrintObjectConfig &cfg, const SurfaceEffectPayload &payload)
{
    if (!payload.present)
        return;
    for (const auto &kv : payload.kv) {
        if (cfg.optptr(kv.first) == nullptr)
            continue; // a NeotkoCM key this port does not have
        try {
            ConfigSubstitutionContext ctx{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            cfg.set_deserialize_nothrow(kv.first, kv.second, ctx);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "ColorStitch Painter: recipe key " << kv.first << " = " << kv.second << " ignored: " << e.what();
        }
    }
}

// Union of many small triangles, in chunks (one Clipper pass over a big triangle soup is slow).
ExPolygons union_triangle_soup(Polygons &&tris)
{
    constexpr size_t kChunk = 512;
    if (tris.size() <= kChunk)
        return union_ex(tris);
    Polygons level = std::move(tris);
    while (level.size() > kChunk) {
        Polygons next;
        for (size_t i = 0; i < level.size(); i += kChunk) {
            const size_t j = std::min(level.size(), i + kChunk);
            Polygons part(std::make_move_iterator(level.begin() + i), std::make_move_iterator(level.begin() + j));
            append(next, to_polygons(union_ex(part)));
        }
        if (next.size() >= level.size())
            return union_ex(next);
        level = std::move(next);
    }
    return union_ex(level);
}

} // namespace

std::shared_ptr<const ColorStitchPaintIndex> ColorStitchPaintIndex::build(const PrintObject &po)
{
    const ModelObject *mo = po.model_object();
    if (mo == nullptr || !ColorStitchPaint::object_has_paint(*mo))
        return nullptr;

    auto                               index = std::make_shared<ColorStitchPaintIndex>();
    const SurfaceEffectProfileManager &mgr   = SurfaceEffectProfileManager::get();
    std::map<int, int>                 recipe_of_profile; // profile id -> recipe index, -1 = no effect

    auto recipe_index = [&](int pid) -> int {
        auto it = recipe_of_profile.find(pid);
        if (it != recipe_of_profile.end())
            return it->second;
        int idx = -1;
        if (const SurfaceEffectProfile *p = mgr.find(pid)) {
            Recipe r;
            r.profile_id = pid;
            r.config     = po.config();
            apply_payload(r.config, p->multipass);
            apply_payload(r.config, p->colorstitch);
            apply_payload(r.config, p->pathblend);
            r.top = SurfacePassStack::from_json(p->stack_top_json);
            // A recipe saved as payload keys only (no stack): rebuild its stack from those keys.
            if (r.top.passes.empty() && (p->multipass.present || p->colorstitch.present || p->pathblend.present))
                r.top = SurfacePassStack::synthesize_from_config(r.config);
            if (r.top.any_effect()) {
                idx = int(index->recipes.size());
                index->recipes.push_back(std::move(r));
            }
        }
        recipe_of_profile[pid] = idx;
        return idx;
    };

    const Transform3d frame = po.trafo_sliced();
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part() || !ColorStitchPaint::volume_has_paint(*mv))
            continue;
        const Transform3d                 vt = frame * mv->get_matrix();
        std::vector<indexed_triangle_set> per_state;
        mv->colorstitch_paint_facets.get_facets(*mv, per_state);
        for (int slot = 1; slot < int(per_state.size()) && slot < ColorStitchPaint::kSlotCount; ++slot) {
            const indexed_triangle_set &its = per_state[size_t(slot)];
            if (its.indices.empty())
                continue;
            const int pid = ColorStitchPaint::profile_for_slot(*mv, slot);
            if (pid <= 0 || mgr.find(pid) == nullptr) {
                index->orphan_facets += its.indices.size();
                continue;
            }
            const int ri = recipe_index(pid);
            if (ri < 0)
                continue; // a recipe without any effect on the Top zone
            for (const stl_triangle_vertex_indices &tri : its.indices) {
                const Vec3d v0 = vt * its.vertices[size_t(tri[0])].cast<double>();
                const Vec3d v1 = vt * its.vertices[size_t(tri[1])].cast<double>();
                const Vec3d v2 = vt * its.vertices[size_t(tri[2])].cast<double>();
                const Vec3d n  = (v1 - v0).cross(v2 - v0);
                const double len = n.norm();
                // Upward facing only: the Top zone of a recipe prints on top surfaces.
                if (len <= 0. || n.z() / len < 0.05)
                    continue;
                Facet f;
                f.z      = std::max({ v0.z(), v1.z(), v2.z() });
                f.recipe = ri;
                f.poly.points = { Point(scale_(v0.x()), scale_(v0.y())), Point(scale_(v1.x()), scale_(v1.y())),
                                  Point(scale_(v2.x()), scale_(v2.y())) };
                if (std::abs(f.poly.area()) < double(SCALED_EPSILON) * double(SCALED_EPSILON))
                    continue;
                f.poly.make_counter_clockwise();
                index->facets.push_back(std::move(f));
            }
        }
    }
    if (index->facets.empty() && index->orphan_facets == 0)
        return nullptr;
    std::stable_sort(index->facets.begin(), index->facets.end(), [](const Facet &a, const Facet &b) { return a.z < b.z; });
    return index;
}

const ColorStitchPaintIndex::Recipe *ColorStitchPaintIndex::recipe(int profile_id) const
{
    for (const Recipe &r : recipes)
        if (r.profile_id == profile_id)
            return &r;
    return nullptr;
}

std::vector<std::pair<int, ExPolygons>> ColorStitchPaintIndex::footprints(double z_lo, double z_hi) const
{
    std::vector<std::pair<int, ExPolygons>> out;
    if (facets.empty())
        return out;
    constexpr double z_tol = 0.02; // floating point slack around the layer band (NeotkoCM value)
    auto it = std::lower_bound(facets.begin(), facets.end(), z_lo - z_tol, [](const Facet &f, double z) { return f.z < z; });
    std::vector<Polygons> per_recipe(recipes.size());
    for (; it != facets.end() && it->z <= z_hi + z_tol; ++it)
        per_recipe[size_t(it->recipe)].push_back(it->poly);
    for (size_t r = 0; r < per_recipe.size(); ++r)
        if (!per_recipe[r].empty()) {
            ExPolygons fp = union_triangle_soup(std::move(per_recipe[r]));
            if (!fp.empty())
                out.emplace_back(recipes[r].profile_id, std::move(fp));
        }
    return out;
}

} // namespace Slic3r
