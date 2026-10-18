// src/slic3r/GUI/RemoteAPI/RemoteAPIUtil.hpp
//
// Orca Zoukei: helpers shared by the Remote API groups G1 (supports), G2 (project / model ops),
// G3 (painting) and G4 (layers and plates). Everything here runs on the GUI thread (callers are
// inside Controller::run_on_ui) unless a function says otherwise.
#ifndef slic3r_RemoteAPIUtil_hpp_
#define slic3r_RemoteAPIUtil_hpp_

#include "RemoteAPIServer.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace RemoteAPI { namespace zk {

using json = nlohmann::json;

// ---- errors ---------------------------------------------------------------------------------
// Thrown by the validators below; guard() turns it into {"error": code, "detail": ...}.
struct Err
{
    std::string code;
    std::string detail;
};

[[noreturn]] void fail(const std::string &code, const std::string &detail = std::string());

// Runs f, mapping Err and nlohmann::json type errors to an {"error"} object. Other exceptions
// propagate (the controller turns them into 500).
template<class F> json guard(F &&f)
{
    try {
        return f();
    } catch (const Err &e) {
        json j = {{"error", e.code}};
        if (!e.detail.empty())
            j["detail"] = e.detail;
        return j;
    } catch (const nlohmann::json::exception &e) {
        return {{"error", "bad_value"}, {"detail", e.what()}};
    }
}

// {"error": code} -> HTTP status (404 unknown_*, 403 policy, 409 state conflicts, else 422).
Response reply(const json &r);

// ---- request parsing ------------------------------------------------------------------------
std::string path_of(const Request &req);                              // target without the query
std::string query_value(const Request &req, const std::string &key); // "" when absent
bool        parse_body(const Request &req, json &body);               // empty body -> {}
bool        parse_uint(const std::string &s, uint64_t &out);
std::vector<std::string> split_path(const std::string &rest);

// Typed field access with clear errors. need_* throw Err when missing or of the wrong type.
double      need_num(const json &o, const char *key);
double      opt_num(const json &o, const char *key, double def);
bool        opt_bool(const json &o, const char *key, bool def);
std::string opt_str(const json &o, const char *key, const std::string &def = std::string());
std::string need_str(const json &o, const char *key);
bool        opt_vec3(const json &o, const char *key, Vec3d &out); // false when absent; throws when malformed
std::string lower(std::string s);
bool        has_ext(const std::string &path, const char *ext_lower);
std::string ext_of(const std::string &path);                      // lowercase, with the dot

// ---- model lookups --------------------------------------------------------------------------
int  find_object(const Model &model, uint64_t id); // index into model.objects or -1
ModelObject *need_object(uint64_t id, int &idx);   // throws unknown_object
ModelVolume *need_volume(ModelObject &mo, long index); // throws unknown_volume

const char *volume_type_name(ModelVolumeType t);
bool        volume_type_from_name(const std::string &s, ModelVolumeType &out);

json volumes_json(const ModelObject &mo);

// Rebuilds the object's rows in the object list and the 3D scene after a structural change.
void refresh_object(int obj_idx, bool notify_plate);

// ---- policy / side effects ------------------------------------------------------------------
bool allow_sensitive();                       // Preferences > Remote API > "Allow script, G-code ..."
bool is_sensitive_key(const std::string &key);
std::string value_to_cfg_string(const json &v);
// Toast in the plater (honours the remote_api_notify preference). Safe on the GUI thread.
void notify(const std::string &text, bool warning = false);

// ---- config on objects / volumes ------------------------------------------------------------
// Applies {"key": value, "key": null} (null removes the override) to a ModelConfig, atomically.
// Returns {"applied": [...], "errors": {...}}. Used by the volume config endpoints.
json apply_model_config(ModelConfig &target, const json &settings);

// ---- volumes --------------------------------------------------------------------------------
// type_name overrides body["type"] when non-empty ("support_enforcer", "support_blocker", ...).
json add_volume(uint64_t object_id, const json &body, const std::string &type_name);

// ---- painting -------------------------------------------------------------------------------
enum class PaintKind { Support, Seam, Color, Fuzzy };

bool paint_kind_from_name(const std::string &s, PaintKind &out); // support|seam|color|fuzzy_skin

// body: {"state":..., "volume":i | "volumes":[...], "rule":{...}, "clear":bool}
json paint_apply(PaintKind kind, uint64_t object_id, const json &body, bool force_clear);
// kinds empty = all four. Per model part: painted flag and the facet count of each state.
json paint_summary(uint64_t object_id, const std::vector<PaintKind> &kinds);

}}}} // namespace

#endif
