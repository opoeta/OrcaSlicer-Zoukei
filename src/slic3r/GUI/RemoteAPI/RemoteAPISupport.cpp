// src/slic3r/GUI/RemoteAPI/RemoteAPISupport.cpp
//
// Orca Zoukei Remote API, group G1: supports. Every handler runs on the UI thread (run_on_ui),
// validates its input and takes an undo snapshot where the equivalent UI action does.
//
//   GET    /api/v1/objects/{id}/support_volumes        list support enforcer / blocker volumes
//   POST   /api/v1/objects/{id}/support_volumes        add one: {kind: enforcer|blocker,
//                                                      shape: box|cylinder|sphere, size:[x,y,z], position:[x,y,z]}
//                                                      or {kind, file: "x.stl"} (optional scale, position)
//   DELETE /api/v1/objects/{id}/support_volumes        {volume: i} | {all: true, kind?: enforcer|blocker}
//   POST   /api/v1/objects/{id}/support_paint          paint support enforcers / blockers on the facets a rule
//                                                      selects: {state: enforcer|blocker, volume?, rule:{...}}
//                                                      or clear: {clear: true, volume?, rule?}
//   GET    /api/v1/objects/{id}/support_paint          painted state summary (facet counts per volume)
//
// Rules (RemoteAPIUtil.cpp, parse_rule): {fill:true} | {overhang_angle: deg from vertical} |
// {z_min, z_max} | {normal:[x,y,z], normal_angle} | {box_min, box_max}; criteria combine with AND,
// "coverage" = centroid | any | all. Per-object support settings are PUT /objects/{id}/config.
#include "RemoteAPIController.hpp"
#include "RemoteAPIUtil.hpp"

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

bool Controller::dispatch_support(const Request &req, Response &out)
{
    static const std::string base = "/api/v1/objects/";
    const std::string        path = zk::path_of(req);
    if (path.compare(0, base.size(), base) != 0)
        return false;
    const std::vector<std::string> parts = zk::split_path(path.substr(base.size()));
    if (parts.size() < 2 || (parts[1] != "support_volumes" && parts[1] != "support_paint"))
        return false;
    if (parts.size() > 3 || (parts.size() == 3 && !(parts[1] == "support_paint" && parts[2] == "clear")))
        return false;
    uint64_t id = 0;
    if (!zk::parse_uint(parts[0], id)) {
        out = {400, {{"error", "bad_object_id"}}};
        return true;
    }
    json body;
    if (req.method != "GET" && !zk::parse_body(req, body)) {
        out = {400, {{"error", "invalid_json"}}};
        return true;
    }

    // ---- support volumes ------------------------------------------------------------------
    if (parts[1] == "support_volumes") {
        if (req.method == "GET") {
            out = zk::reply(run_on_ui([id]() -> json {
                return zk::guard([&]() -> json {
                    int          idx = -1;
                    ModelObject *mo  = zk::need_object(id, idx);
                    json         all = zk::volumes_json(*mo);
                    json         list = json::array();
                    for (const json &v : all)
                        if (v["type"] == "support_enforcer" || v["type"] == "support_blocker")
                            list.push_back(v);
                    return {{"id", id}, {"support_volumes", list}, {"count", list.size()}};
                });
            }));
            return true;
        }
        if (req.method == "POST") {
            out = zk::reply(run_on_ui([id, body]() -> json {
                return zk::guard([&]() -> json {
                    const std::string kind = zk::lower(zk::need_str(body, "kind"));
                    if (kind != "enforcer" && kind != "blocker")
                        zk::fail("bad_kind", "enforcer | blocker");
                    return zk::add_volume(id, body, kind == "enforcer" ? "support_enforcer" : "support_blocker");
                });
            }, 60));
            if (out.status == 200) out.status = 201;
            return true;
        }
        if (req.method == "DELETE") {
            out = zk::reply(run_on_ui([id, body]() -> json {
                return zk::guard([&]() -> json {
                    Plater      *plater = wxGetApp().plater();
                    int          idx    = -1;
                    ModelObject *mo     = zk::need_object(id, idx);
                    std::vector<size_t> victims;
                    if (body.contains("volume") && !body["volume"].is_null()) {
                        if (!body["volume"].is_number_integer())
                            zk::fail("bad_value", "volume must be an integer index");
                        const long i = (long) body["volume"].get<long long>();
                        ModelVolume *mv = zk::need_volume(*mo, i);
                        if (!mv->is_support_modifier())
                            zk::fail("not_a_support_volume", "volume " + std::to_string(i) + " is not a support enforcer/blocker");
                        victims.push_back(size_t(i));
                    } else if (zk::opt_bool(body, "all", false)) {
                        const std::string kind = zk::lower(zk::opt_str(body, "kind"));
                        if (!kind.empty() && kind != "enforcer" && kind != "blocker")
                            zk::fail("bad_kind", "enforcer | blocker");
                        for (size_t i = 0; i < mo->volumes.size(); ++i) {
                            const ModelVolume *v = mo->volumes[i];
                            if ((kind.empty() && v->is_support_modifier()) || (kind == "enforcer" && v->is_support_enforcer()) ||
                                (kind == "blocker" && v->is_support_blocker()))
                                victims.push_back(i);
                        }
                    } else {
                        zk::fail("missing_selector", "give volume (index) or all:true");
                    }
                    if (victims.empty())
                        return {{"id", id}, {"removed", 0}};
                    plater->take_snapshot("Remove support volumes (remote)");
                    std::sort(victims.begin(), victims.end());
                    for (auto it = victims.rbegin(); it != victims.rend(); ++it)
                        mo->delete_volume(*it);
                    zk::refresh_object(idx, false);
                    zk::notify("Removed " + std::to_string(victims.size()) + " support volume(s) from '" + mo->name + "'");
                    return {{"id", id}, {"removed", victims.size()}, {"volumes", zk::volumes_json(*mo)}};
                });
            }, 30));
            return true;
        }
        return false;
    }

    // ---- support painting -----------------------------------------------------------------
    if (req.method == "GET" && parts.size() == 2) {
        out = zk::reply(run_on_ui([id]() -> json {
            return zk::guard([&]() -> json { return zk::paint_summary(id, {zk::PaintKind::Support}); });
        }, 60));
        return true;
    }
    if (req.method == "POST") {
        const bool clear_route = parts.size() == 3;
        out = zk::reply(run_on_ui([id, body, clear_route]() -> json {
            return zk::guard([&]() -> json { return zk::paint_apply(zk::PaintKind::Support, id, body, clear_route); });
        }, 120));
        return true;
    }
    return false;
}

}}} // namespace
