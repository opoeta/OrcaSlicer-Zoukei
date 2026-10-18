// src/slic3r/GUI/RemoteAPI/RemoteAPIUtil.cpp
//
// Orca Zoukei: helpers shared by the Remote API groups G1 (supports), G2 (project / model ops),
// G3 (painting) and G4 (layers and plates). See RemoteAPIUtil.hpp.
#include "RemoteAPIUtil.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI { namespace zk {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
} // namespace

// ---- errors ---------------------------------------------------------------------------------

void fail(const std::string &code, const std::string &detail)
{
    throw Err{code, detail};
}

Response reply(const json &r)
{
    if (r.is_object() && r.contains("error") && r["error"].is_string()) {
        const std::string e = r["error"].get<std::string>();
        if (e.rfind("unknown_", 0) == 0 || e == "not_found")
            return {404, r};
        if (e == "blocked_by_remote_api_policy")
            return {403, r};
        if (e == "slicing" || e == "unsaved_changes" || e == "exists" || e == "busy")
            return {409, r};
        return {422, r};
    }
    return {200, r};
}

// ---- request parsing ------------------------------------------------------------------------

std::string path_of(const Request &req)
{
    return req.target.substr(0, req.target.find('?'));
}

std::string query_value(const Request &req, const std::string &key)
{
    const size_t q = req.target.find('?');
    if (q == std::string::npos)
        return std::string();
    std::string qs = req.target.substr(q + 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        if (amp == std::string::npos)
            amp = qs.size();
        const std::string item = qs.substr(pos, amp - pos);
        const size_t eq = item.find('=');
        if (item.substr(0, eq) == key)
            return eq == std::string::npos ? std::string() : item.substr(eq + 1);
        pos = amp + 1;
    }
    return std::string();
}

bool parse_body(const Request &req, json &body)
{
    if (req.body.empty()) {
        body = json::object();
        return true;
    }
    body = json::parse(req.body, nullptr, false);
    return !body.is_discarded() && body.is_object();
}

bool parse_uint(const std::string &s, uint64_t &out)
{
    if (s.empty() || s.size() > 19)
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    out = std::stoull(s);
    return true;
}

std::vector<std::string> split_path(const std::string &rest)
{
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= rest.size()) {
        size_t slash = rest.find('/', pos);
        if (slash == std::string::npos)
            slash = rest.size();
        parts.push_back(rest.substr(pos, slash - pos));
        pos = slash + 1;
    }
    return parts;
}

double need_num(const json &o, const char *key)
{
    if (!o.is_object() || !o.contains(key))
        fail("missing_field", std::string("missing '") + key + "'");
    if (!o[key].is_number())
        fail("bad_value", std::string("'") + key + "' must be a number");
    const double v = o[key].get<double>();
    if (!std::isfinite(v))
        fail("bad_value", std::string("'") + key + "' must be finite");
    return v;
}

double opt_num(const json &o, const char *key, double def)
{
    if (!o.is_object() || !o.contains(key) || o[key].is_null())
        return def;
    return need_num(o, key);
}

bool opt_bool(const json &o, const char *key, bool def)
{
    if (!o.is_object() || !o.contains(key) || o[key].is_null())
        return def;
    if (!o[key].is_boolean())
        fail("bad_value", std::string("'") + key + "' must be true or false");
    return o[key].get<bool>();
}

std::string opt_str(const json &o, const char *key, const std::string &def)
{
    if (!o.is_object() || !o.contains(key) || o[key].is_null())
        return def;
    if (!o[key].is_string())
        fail("bad_value", std::string("'") + key + "' must be a string");
    return o[key].get<std::string>();
}

std::string need_str(const json &o, const char *key)
{
    if (!o.is_object() || !o.contains(key))
        fail("missing_field", std::string("missing '") + key + "'");
    return opt_str(o, key);
}

bool opt_vec3(const json &o, const char *key, Vec3d &out)
{
    if (!o.is_object() || !o.contains(key) || o[key].is_null())
        return false;
    const json &a = o[key];
    if (!a.is_array() || a.size() != 3)
        fail("bad_value", std::string("'") + key + "' must be [x, y, z]");
    for (int i = 0; i < 3; ++i) {
        if (!a[size_t(i)].is_number())
            fail("bad_value", std::string("'") + key + "' must hold numbers");
        out[i] = a[size_t(i)].get<double>();
        if (!std::isfinite(out[i]))
            fail("bad_value", std::string("'") + key + "' must be finite");
    }
    return true;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower((unsigned char) c));
    return s;
}

std::string ext_of(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    const size_t sep = path.find_last_of("/\\");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
        return std::string();
    return lower(path.substr(dot));
}

bool has_ext(const std::string &path, const char *ext_lower)
{
    return ext_of(path) == ext_lower;
}

// ---- model lookups --------------------------------------------------------------------------

int find_object(const Model &model, uint64_t id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        if ((uint64_t) model.objects[i]->id().id == id)
            return int(i);
    return -1;
}

ModelObject *need_object(uint64_t id, int &idx)
{
    Plater *plater = wxGetApp().plater();
    idx = find_object(plater->model(), id);
    if (idx < 0)
        fail("unknown_object");
    return plater->model().objects[size_t(idx)];
}

ModelVolume *need_volume(ModelObject &mo, long index)
{
    if (index < 0 || size_t(index) >= mo.volumes.size())
        fail("unknown_volume", "volume index 0.." + std::to_string(mo.volumes.size() ? mo.volumes.size() - 1 : 0));
    return mo.volumes[size_t(index)];
}

const char *volume_type_name(ModelVolumeType t)
{
    switch (t) {
    case ModelVolumeType::MODEL_PART:        return "part";
    case ModelVolumeType::NEGATIVE_VOLUME:   return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER:return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER:   return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:  return "support_enforcer";
    default:                                 return "other";
    }
}

bool volume_type_from_name(const std::string &s, ModelVolumeType &out)
{
    const std::string n = lower(s);
    if (n == "part" || n == "model_part")             { out = ModelVolumeType::MODEL_PART; return true; }
    if (n == "negative" || n == "negative_volume")    { out = ModelVolumeType::NEGATIVE_VOLUME; return true; }
    if (n == "modifier")                              { out = ModelVolumeType::PARAMETER_MODIFIER; return true; }
    if (n == "support_blocker" || n == "blocker")     { out = ModelVolumeType::SUPPORT_BLOCKER; return true; }
    if (n == "support_enforcer" || n == "enforcer")   { out = ModelVolumeType::SUPPORT_ENFORCER; return true; }
    return false;
}

json volumes_json(const ModelObject &mo)
{
    json arr = json::array();
    for (size_t i = 0; i < mo.volumes.size(); ++i) {
        const ModelVolume *v = mo.volumes[i];
        const BoundingBoxf3 bb = v->mesh().bounding_box();
        const Vec3d size = bb.size().cwiseProduct(v->get_scaling_factor());
        Vec3d       centre = v->get_offset();
        if (!mo.instances.empty())
            centre = mo.instances.front()->get_matrix() * centre;
        json o = {
            {"index", i},
            {"id", (uint64_t) v->id().id},
            {"name", v->name},
            {"type", volume_type_name(v->type())},
            {"size_mm", {size.x(), size.y(), size.z()}},
            {"center_world", {centre.x(), centre.y(), centre.z()}},
            {"extruder", v->config.has("extruder") ? v->config.extruder() : 0},
            {"facets", v->mesh().its.indices.size()},
            {"painted", {{"support", !v->supported_facets.empty()},
                         {"seam", !v->seam_facets.empty()},
                         {"color", !v->mmu_segmentation_facets.empty()},
                         {"fuzzy_skin", !v->fuzzy_skin_facets.empty()}}},
        };
        json cfg = json::object();
        for (const std::string &k : v->config.keys())
            cfg[k] = v->config.opt_serialize(k);
        o["config"] = cfg;
        arr.push_back(std::move(o));
    }
    return arr;
}

void refresh_object(int obj_idx, bool notify_plate)
{
    Plater *plater = wxGetApp().plater();
    if (obj_idx < 0 || size_t(obj_idx) >= plater->model().objects.size())
        return;
    plater->model().objects[size_t(obj_idx)]->invalidate_bounding_box();
    plater->changed_object(obj_idx);
    if (ObjectList *ol = wxGetApp().obj_list()) {
        ol->add_volumes_to_object_in_list(size_t(obj_idx));
        ol->update_info_items(size_t(obj_idx));
    }
    if (notify_plate)
        plater->get_partplate_list().notify_instance_update(obj_idx, 0, true);
}

// ---- policy / side effects ------------------------------------------------------------------

bool allow_sensitive()
{
    return wxGetApp().app_config->get_bool("remote_api_allow_sensitive");
}

// Same rule as the controller's PUT /config policy (RemoteAPIController.cpp, is_sensitive_config_key):
// script, G-code templates and printer connection keys are not slicing settings.
bool is_sensitive_key(const std::string &key)
{
    static const std::string gcode_suffix = "_gcode";
    if (key == "post_process" || key == "printer_model" || key == "filename_format" || key.rfind("printhost_", 0) == 0)
        return true;
    if (key.size() > gcode_suffix.size() && key.compare(key.size() - gcode_suffix.size(), gcode_suffix.size(), gcode_suffix) == 0)
        return key != "emit_machine_limits_to_gcode";
    const std::vector<std::string> &pp = PhysicalPrinter::printer_options();
    return std::find(pp.begin(), pp.end(), key) != pp.end();
}

std::string value_to_cfg_string(const json &v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        std::ostringstream ss;
        ss << v.get<double>();
        return ss.str();
    }
    throw std::runtime_error("unsupported value type");
}

void notify(const std::string &text, bool warning)
{
    wxGetApp().CallAfter([text, warning]() {
        AppConfig *ac = wxGetApp().app_config;
        const bool enabled = (ac && ac->has("remote_api_notify")) ? ac->get_bool("remote_api_notify") : true;
        if (!enabled)
            return;
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr)
            return;
        NotificationManager *nm = plater->get_notification_manager();
        if (nm == nullptr)
            return;
        nm->close_notification_of_type(NotificationType::RemoteAPIChange);
        nm->push_notification(NotificationType::RemoteAPIChange,
                              warning ? NotificationManager::NotificationLevel::WarningNotificationLevel :
                                        NotificationManager::NotificationLevel::RegularNotificationLevel,
                              text);
    });
}

// ---- config on objects / volumes ------------------------------------------------------------

json apply_model_config(ModelConfig &target, const json &settings)
{
    if (!settings.is_object())
        fail("bad_config", "config must be an object {key: value}; null removes the override");
    DynamicPrintConfig cfg = target.get();
    json               applied = json::array();
    json               errors  = json::object();
    const bool         sens    = allow_sensitive();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const std::string &key = it.key();
        if (print_config_def.get(key) == nullptr) {
            errors[key] = "unknown_key";
            continue;
        }
        if (!sens && is_sensitive_key(key)) {
            errors[key] = "blocked_by_remote_api_policy";
            continue;
        }
        if (it.value().is_null()) {
            cfg.erase(key);
            applied.push_back(key);
            continue;
        }
        try {
            cfg.set_deserialize_strict(key, value_to_cfg_string(it.value()));
            applied.push_back(key);
        } catch (const std::exception &e) {
            errors[key] = e.what();
        }
    }
    if (!errors.empty())
        return {{"applied", json::array()}, {"errors", errors}}; // atomic: nothing applied on any error
    target.assign_config(std::move(cfg));
    return {{"applied", applied}, {"errors", errors}};
}

// ---- volumes --------------------------------------------------------------------------------

json add_volume(uint64_t object_id, const json &body, const std::string &type_name)
{
    Plater *plater = wxGetApp().plater();
    int     idx    = -1;
    ModelObject *mo = need_object(object_id, idx);
    if (mo->instances.empty())
        fail("no_instance");
    if (!body.is_object())
        fail("body_must_be_object");

    const std::string tn = type_name.empty() ? opt_str(body, "type", "modifier") : type_name;
    ModelVolumeType   vt = ModelVolumeType::INVALID;
    if (!volume_type_from_name(tn, vt))
        fail("bad_type", "part | modifier | negative | support_enforcer | support_blocker");

    const std::string shape = lower(opt_str(body, "shape"));
    const std::string file  = opt_str(body, "file");
    if (shape.empty() == file.empty())
        fail("need_shape_or_file", "give exactly one of shape (box | cylinder | sphere) or file (stl | obj | 3mf)");

    TriangleMesh mesh;
    if (!file.empty()) {
        const std::string ext = ext_of(file);
        if (ext != ".stl" && ext != ".obj" && ext != ".3mf")
            fail("unsupported_format", "file must be .stl, .obj or .3mf");
        if (!boost::filesystem::exists(boost::filesystem::path(boost::nowide::widen(file))))
            fail("not_found", file);
        Model loaded;
        try {
            loaded = Model::read_from_file(file, nullptr, nullptr, LoadStrategy::LoadModel);
        } catch (const std::exception &e) {
            fail("load_failed", e.what());
        }
        for (ModelObject *o : loaded.objects)
            for (ModelVolume *v : o->volumes)
                if (v->is_model_part()) {
                    TriangleMesh t(v->mesh());
                    t.transform(v->get_matrix(), true);
                    mesh.merge(t);
                }
        if (mesh.empty())
            fail("empty_mesh", "the file has no geometry");
        const double scale = opt_num(body, "scale", 1.0);
        if (!(scale > 0.0) || scale > 1000.0)
            fail("bad_scale", "0 < scale <= 1000");
        if (scale != 1.0)
            mesh.scale(float(scale));
    } else {
        Vec3d sz(0, 0, 0);
        if (!opt_vec3(body, "size", sz))
            fail("missing_size", "size [x, y, z] in mm (cylinder: [diameter_x, diameter_y, height])");
        if (sz.minCoeff() <= 0.0 || sz.maxCoeff() > 100000.0)
            fail("bad_size", "each size component must be > 0 and <= 100000 mm");
        if (shape == "box")
            mesh = TriangleMesh(its_make_cube(1.0, 1.0, 1.0));
        else if (shape == "cylinder")
            mesh = TriangleMesh(its_make_cylinder(0.5, 1.0));
        else if (shape == "sphere")
            mesh = TriangleMesh(its_make_sphere(0.5, 3.14159265358979323846 / 90.0));
        else
            fail("bad_shape", "box | cylinder | sphere");
        Transform3d S = Transform3d::Identity();
        S.scale(sz);
        mesh.transform(S);
    }

    const ModelInstance *inst = mo->instances.front();
    Vec3d                pos(0, 0, 0);
    if (!opt_vec3(body, "position", pos))
        pos = mo->instance_bounding_box(0).center();

    plater->take_snapshot("Add volume (remote)");
    ModelVolume *nv = mo->add_volume(std::move(mesh), vt);
    // Same placement as the "Add primitive" menu: world-aligned, unscaled volume with its centre at pos.
    const Transform3d inv = inst->get_transformation().get_matrix_no_offset().inverse();
    nv->set_transformation(inv);
    nv->set_offset(inv * (pos - inst->get_offset()));
    nv->name = opt_str(body, "name", std::string("Remote ") + volume_type_name(vt));
    if (vt == ModelVolumeType::MODEL_PART && mo->config.has("extruder"))
        nv->config.set_key_value("extruder", new ConfigOptionInt(mo->config.opt_int("extruder")));
    nv->source.is_from_builtin_objects = !shape.empty();
    mo->sort_volumes(true);

    refresh_object(idx, vt == ModelVolumeType::MODEL_PART);
    notify(std::string("Added ") + volume_type_name(vt) + " to '" + mo->name + "'");

    int new_index = -1;
    for (size_t i = 0; i < mo->volumes.size(); ++i)
        if (mo->volumes[i] == nv)
            new_index = int(i);
    return {{"added", true}, {"id", object_id}, {"volume", new_index}, {"type", volume_type_name(vt)},
            {"volumes", volumes_json(*mo)}};
}

// ---- painting -------------------------------------------------------------------------------

bool paint_kind_from_name(const std::string &s, PaintKind &out)
{
    const std::string n = lower(s);
    if (n == "support" || n == "supports") { out = PaintKind::Support; return true; }
    if (n == "seam")                       { out = PaintKind::Seam;    return true; }
    if (n == "color" || n == "colour" || n == "mmu") { out = PaintKind::Color; return true; }
    if (n == "fuzzy_skin" || n == "fuzzy") { out = PaintKind::Fuzzy;   return true; }
    return false;
}

namespace {

const char *kind_name(PaintKind k)
{
    switch (k) {
    case PaintKind::Support: return "support";
    case PaintKind::Seam:    return "seam";
    case PaintKind::Color:   return "color";
    default:                 return "fuzzy_skin";
    }
}

FacetsAnnotation &facets_of(ModelVolume &mv, PaintKind k)
{
    switch (k) {
    case PaintKind::Support: return mv.supported_facets;
    case PaintKind::Seam:    return mv.seam_facets;
    case PaintKind::Color:   return mv.mmu_segmentation_facets;
    default:                 return mv.fuzzy_skin_facets;
    }
}

std::string state_label(PaintKind k, int st)
{
    switch (k) {
    case PaintKind::Support:
    case PaintKind::Seam:    return st == 1 ? "enforcer" : (st == 2 ? "blocker" : "state_" + std::to_string(st));
    case PaintKind::Fuzzy:   return st == 1 ? "fuzzy_skin" : "state_" + std::to_string(st);
    default:                 return "filament_" + std::to_string(st);
    }
}

// A facet filter: every present criterion must hold (logical AND).
struct Rule
{
    bool        has_overhang { false };
    double      overhang { 0.0 };           // degrees from vertical (the support threshold angle)
    bool        has_z { false };
    double      zmin { -1e30 }, zmax { 1e30 };
    bool        has_normal { false };
    Vec3d       dir = Vec3d(0, 0, 1);
    double      normal_angle { 30.0 };
    bool        has_box { false };
    Vec3d       bmin = Vec3d(0, 0, 0), bmax = Vec3d(0, 0, 0);
    std::string coverage { "centroid" };    // centroid | any | all

    bool match(const Vec3d w[3], const Vec3d &n) const
    {
        const Vec3d c = (w[0] + w[1] + w[2]) / 3.0;
        auto range_ok = [&](int ax, double lo, double hi) {
            const double mn = std::min({w[0][ax], w[1][ax], w[2][ax]});
            const double mx = std::max({w[0][ax], w[1][ax], w[2][ax]});
            if (coverage == "any")
                return mx >= lo && mn <= hi;
            if (coverage == "all")
                return mn >= lo && mx <= hi;
            return c[ax] >= lo && c[ax] <= hi;
        };
        if (has_z && !range_ok(2, zmin, zmax))
            return false;
        if (has_box)
            for (int ax = 0; ax < 3; ++ax)
                if (!range_ok(ax, bmin[ax], bmax[ax]))
                    return false;
        // Overhang: the facet leans down by more than `overhang` degrees from vertical.
        if (has_overhang && !(n.z() < -std::sin(overhang * kDeg2Rad)))
            return false;
        if (has_normal && !(n.dot(dir) > std::cos(normal_angle * kDeg2Rad)))
            return false;
        return true;
    }
};

Rule parse_rule(const json &body)
{
    if (!body.contains("rule") || !body["rule"].is_object())
        fail("missing_rule", "rule: {fill:true} | overhang_angle | z_min/z_max | normal+normal_angle | box_min+box_max (criteria combine with AND; coverage: centroid|any|all)");
    const json &o = body["rule"];
    Rule        r;
    const bool  fill = opt_bool(o, "fill", false);
    if (o.contains("overhang_angle")) {
        r.has_overhang = true;
        r.overhang     = need_num(o, "overhang_angle");
        if (r.overhang < 0.0 || r.overhang > 90.0)
            fail("bad_overhang_angle", "0..90 degrees from vertical (45 = classic support threshold; the gizmo's highlight angle is 90 minus this)");
    }
    if (o.contains("z_min") || o.contains("z_max")) {
        r.has_z = true;
        r.zmin  = opt_num(o, "z_min", -1e30);
        r.zmax  = opt_num(o, "z_max", 1e30);
        if (r.zmin > r.zmax)
            fail("bad_range", "z_min must be <= z_max (world Z in mm, 0 = bed)");
    }
    if (o.contains("normal")) {
        Vec3d d(0, 0, 0);
        opt_vec3(o, "normal", d);
        if (d.norm() < 1e-9)
            fail("bad_normal", "normal must be a non-zero vector");
        r.has_normal   = true;
        r.dir          = d.normalized();
        r.normal_angle = opt_num(o, "normal_angle", 30.0);
        if (!(r.normal_angle > 0.0) || r.normal_angle > 180.0)
            fail("bad_normal_angle", "0 < normal_angle <= 180");
    }
    if (o.contains("box_min") || o.contains("box_max")) {
        if (!opt_vec3(o, "box_min", r.bmin) || !opt_vec3(o, "box_max", r.bmax))
            fail("bad_box", "box_min and box_max [x,y,z] are both required");
        for (int i = 0; i < 3; ++i)
            if (r.bmin[i] > r.bmax[i])
                fail("bad_box", "box_min must be <= box_max");
        r.has_box = true;
    }
    r.coverage = lower(opt_str(o, "coverage", "centroid"));
    if (r.coverage != "centroid" && r.coverage != "any" && r.coverage != "all")
        fail("bad_coverage", "centroid | any | all");
    if (!fill && !r.has_overhang && !r.has_z && !r.has_normal && !r.has_box)
        fail("empty_rule", "give fill:true to paint every facet, or at least one criterion");
    return r;
}

EnforcerBlockerType parse_state(PaintKind k, const json &body)
{
    if (!body.contains("state"))
        fail("missing_state", k == PaintKind::Color ? "state = 1-based filament slot" :
                              k == PaintKind::Fuzzy ? "state = \"on\"" : "state = \"enforcer\" | \"blocker\"");
    const json &s = body["state"];
    if (k == PaintKind::Color) {
        if (!s.is_number_integer())
            fail("bad_state", "state must be an integer filament slot (1-based)");
        const long long n = s.get<long long>();
        const size_t count = wxGetApp().preset_bundle->filament_presets.size();
        if (n < 1 || n > (long long) EnforcerBlockerType::ExtruderMax || size_t(n) > count)
            fail("unknown_filament", "slot 1.." + std::to_string(count));
        return static_cast<EnforcerBlockerType>(int(n));
    }
    if (k == PaintKind::Fuzzy) {
        if ((s.is_boolean() && s.get<bool>()) || (s.is_string() && (lower(s.get<std::string>()) == "on" || lower(s.get<std::string>()) == "fuzzy_skin")))
            return EnforcerBlockerType::FUZZY_SKIN;
        fail("bad_state", "state = \"on\" (use clear to remove)");
    }
    if (!s.is_string())
        fail("bad_state", "enforcer | blocker");
    const std::string v = lower(s.get<std::string>());
    if (v == "enforcer" || v == "enforce")
        return EnforcerBlockerType::ENFORCER;
    if (v == "blocker" || v == "block")
        return EnforcerBlockerType::BLOCKER;
    fail("bad_state", "enforcer | blocker");
}

std::vector<ModelVolume *> pick_volumes(ModelObject &mo, const json &body)
{
    std::vector<ModelVolume *> out;
    auto add_index = [&](const json &v) {
        if (!v.is_number_integer())
            fail("bad_value", "volume indices must be integers");
        ModelVolume *mv = need_volume(mo, (long) v.get<long long>());
        if (!mv->is_model_part())
            fail("not_a_model_part", "only model parts can be painted (volume " + std::to_string(v.get<long long>()) + ")");
        if (std::find(out.begin(), out.end(), mv) == out.end())
            out.push_back(mv);
    };
    if (body.contains("volume") && !body["volume"].is_null())
        add_index(body["volume"]);
    else if (body.contains("volumes") && body["volumes"].is_array())
        for (const json &v : body["volumes"])
            add_index(v);
    else
        for (ModelVolume *mv : mo.volumes)
            if (mv->is_model_part())
                out.push_back(mv);
    if (out.empty())
        fail("no_volume", "the object has no model part to paint");
    return out;
}

// Paints (or clears, st == NONE) every facet of mv that satisfies the rule. Returns the facet count.
size_t paint_volume(ModelVolume &mv, const ModelInstance &inst, PaintKind kind, EnforcerBlockerType st, const Rule &rule)
{
    const TriangleMesh &mesh = mv.mesh();
    const indexed_triangle_set &its = mesh.its;
    if (its.indices.empty())
        return 0;
    FacetsAnnotation &fa = facets_of(mv, kind);
    TriangleSelector  sel(mesh);
    if (!fa.empty())
        sel.deserialize(fa.get_data());
    const Transform3d world = inst.get_matrix() * mv.get_matrix();
    const Matrix3d    nm    = world.linear().inverse().transpose();
    size_t            hit   = 0;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const stl_triangle_vertex_indices &f = its.indices[i];
        const Vec3d a = its.vertices[size_t(f(0))].cast<double>();
        const Vec3d b = its.vertices[size_t(f(1))].cast<double>();
        const Vec3d c = its.vertices[size_t(f(2))].cast<double>();
        const Vec3d nl = (b - a).cross(c - a);
        if (nl.norm() < 1e-12)
            continue;
        const Vec3d n = (nm * nl).normalized();
        const Vec3d w[3] = {world * a, world * b, world * c};
        if (!rule.match(w, n))
            continue;
        sel.set_facet(int(i), st);
        ++hit;
    }
    if (hit)
        fa.set(sel);
    return hit;
}

json volume_paint_json(ModelVolume &mv, PaintKind k)
{
    FacetsAnnotation &fa = facets_of(mv, k);
    json              o  = {{"kind", kind_name(k)}, {"painted", !fa.empty()}, {"states", json::array()}};
    if (fa.empty())
        return o;
    const TriangleSelector::TriangleSplittingData &data = fa.get_data();
    TriangleSelector sel(mv.mesh());
    sel.deserialize(data);
    const size_t top = std::min(data.used_states.size(), size_t(EnforcerBlockerType::ExtruderMax) + 1);
    for (size_t st = 1; st < top; ++st) {
        if (!data.used_states[st])
            continue;
        const int n = sel.num_facets(static_cast<EnforcerBlockerType>(int(st)));
        if (n > 0)
            o["states"].push_back({{"state", st}, {"label", state_label(k, int(st))}, {"facets", n}});
    }
    return o;
}

} // namespace

json paint_apply(PaintKind kind, uint64_t object_id, const json &body, bool force_clear)
{
    Plater *plater = wxGetApp().plater();
    int     idx    = -1;
    ModelObject *mo = need_object(object_id, idx);
    if (mo->instances.empty())
        fail("no_instance");
    if (!body.is_object())
        fail("body_must_be_object");

    const bool clear = force_clear || opt_bool(body, "clear", false);
    EnforcerBlockerType st = EnforcerBlockerType::NONE;
    if (!clear)
        st = parse_state(kind, body);
    const bool whole_clear = clear && !body.contains("rule");
    Rule       rule;
    if (!whole_clear)
        rule = parse_rule(body);
    const std::vector<ModelVolume *> vols = pick_volumes(*mo, body);

    plater->take_snapshot(std::string("Remote paint ") + kind_name(kind));
    json   per_volume = json::array();
    size_t total      = 0;
    for (ModelVolume *mv : vols) {
        size_t n = 0;
        if (whole_clear) {
            FacetsAnnotation &fa = facets_of(*mv, kind);
            if (!fa.empty()) {
                n = fa.get_data().triangles_to_split.size();
                fa.reset();
            }
        } else {
            n = paint_volume(*mv, *mo->instances.front(), kind, st, rule);
        }
        total += n;
        size_t vi = 0;
        for (size_t i = 0; i < mo->volumes.size(); ++i)
            if (mo->volumes[i] == mv)
                vi = i;
        per_volume.push_back({{"volume", vi}, {"facets", n}});
    }
    plater->changed_object(idx);
    if (ObjectList *ol = wxGetApp().obj_list())
        ol->update_info_items(size_t(idx));
    notify(std::string(clear ? "Cleared " : "Painted ") + kind_name(kind) + " on '" + mo->name + "'");
    return {{"id", object_id}, {"kind", kind_name(kind)},
            {"action", clear ? "clear" : "paint"},
            {"state", clear ? json(nullptr) : json(state_label(kind, int(st)))},
            {"facets_changed", total}, {"volumes", per_volume}};
}

json paint_summary(uint64_t object_id, const std::vector<PaintKind> &kinds_in)
{
    int          idx = -1;
    ModelObject *mo  = need_object(object_id, idx);
    std::vector<PaintKind> kinds = kinds_in;
    if (kinds.empty())
        kinds = {PaintKind::Support, PaintKind::Seam, PaintKind::Color, PaintKind::Fuzzy};
    json vols = json::array();
    for (size_t i = 0; i < mo->volumes.size(); ++i) {
        ModelVolume *mv = mo->volumes[i];
        if (!mv->is_model_part())
            continue;
        json v = {{"volume", i}, {"name", mv->name}, {"facets", mv->mesh().its.indices.size()}, {"paint", json::array()}};
        for (PaintKind k : kinds)
            v["paint"].push_back(volume_paint_json(*mv, k));
        vols.push_back(std::move(v));
    }
    return {{"id", object_id}, {"name", mo->name}, {"volumes", vols}};
}

}}}} // namespace
