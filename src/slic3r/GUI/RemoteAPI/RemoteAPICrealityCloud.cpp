// src/slic3r/GUI/RemoteAPI/RemoteAPICrealityCloud.cpp
//
// Orca Zoukei Remote API: Creality Cloud (see GUI/Zoukei/CrealityCloud.hpp). The login itself is never possible
// through the API (it needs the owner on Creality's page) and the token is never returned.
//
// Read-only (always allowed):
//   GET  /api/v1/creality_cloud                {logged_in, region, auto_sync, secure_storage, test_server}
//   GET  /api/v1/creality_cloud/filaments      user filament presets {name, type, vendor, uploaded, changed},
//                                              printer_targets, default_printer
//   POST /api/v1/creality_cloud/upload         {presets: [names], printer?, dry_run: true}
//                                              what would be sent (create / update, linked printer, bytes); no network
//
// Control (Preferences > Remote API > "Allow printer control from the Remote API" AND "confirm": true):
//   POST /api/v1/creality_cloud/upload         {presets: [names], printer?, confirm: true}
//                                              uploads to the logged in account; per-preset result
//
// Test only, when OrcaSlicer runs with ORCA_ZOUKEI_CREALITY_CLOUD_MOCK (tools/zoukei/mock_creality_cloud.py):
//   POST /api/v1/creality_cloud/mock_login     {callback_url}: the redirect the login window would catch
#include "RemoteAPIController.hpp"
#include "RemoteAPIUtil.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Zoukei/CrealityCloud.hpp"
#include "slic3r/Utils/CrealityCloudClient.hpp"

#include <boost/log/trivial.hpp>

#include <memory>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using json = nlohmann::json;

namespace {

bool control_enabled() { return wxGetApp().app_config->get_bool("remote_api_printer_control"); }

void require_control(const json& body, const char* action)
{
    if (!control_enabled())
        zk::fail("blocked_by_remote_api_policy",
                 std::string("printer control is off (") + action + " refused). The owner must enable Preferences > Remote API > "
                 "\"Allow printer control from the Remote API\" in OrcaSlicer; it cannot be changed through the API.");
    const bool confirmed = body.is_object() && body.contains("confirm") && body["confirm"].is_boolean() && body["confirm"].get<bool>();
    if (!confirmed)
        zk::fail("blocked_by_remote_api_policy",
                 std::string(action) + " needs \"confirm\": true in the request. Ask the owner first and only then send it.");
}

json status_json()
{
    return {{"logged_in", CrCloud::logged_in()},
            {"region", CrealityCloud::region_key(CrCloud::region())},
            {"auto_sync", wxGetApp().app_config->get_bool("creality_cloud_auto_sync")},
            {"secure_storage", CrCloud::secure_storage_available()},
            {"test_server", CrCloud::is_mock()},
            {"login", "only in OrcaSlicer: Preferences > Creality Cloud account, or the Creality device tab"}};
}

json filaments_json()
{
    json list = json::array();
    for (const CrCloud::FilamentEntry& e : CrCloud::user_filaments())
        list.push_back({{"name", e.name}, {"type", e.type}, {"vendor", e.vendor}, {"inherits", e.inherits}, {"uploaded", e.uploaded}, {"changed", e.changed}});
    return {{"filaments", list}, {"printer_targets", CrCloud::printer_targets()}, {"default_printer", CrCloud::default_printer_target()},
            {"logged_in", CrCloud::logged_in()}};
}

std::vector<std::string> need_presets(const json& body)
{
    if (!body.contains("presets") || !body["presets"].is_array() || body["presets"].empty())
        zk::fail("bad_value", "\"presets\" must be a non-empty array of filament preset names (see GET /creality_cloud/filaments)");
    std::vector<std::string> out;
    for (const json& v : body["presets"]) {
        if (!v.is_string() || v.get<std::string>().empty())
            zk::fail("bad_value", "every entry of \"presets\" must be a preset name");
        out.push_back(v.get<std::string>());
    }
    if (out.size() > 100)
        zk::fail("bad_value", "at most 100 presets per request");
    return out;
}

} // namespace

bool Controller::dispatch_creality_cloud(const Request& req, Response& out)
{
    static const std::string base = "/api/v1/creality_cloud";
    const std::string        path = zk::path_of(req);
    if (path.compare(0, base.size(), base) != 0 || (path.size() > base.size() && path[base.size()] != '/'))
        return false;
    const std::string rest = path.substr(base.size());
    const std::string& m   = req.method;

    if (m == "GET" && (rest.empty() || rest == "/")) {
        out = zk::reply(run_on_ui([]() -> json { return zk::guard([&]() -> json { return status_json(); }); }, 30));
        return true;
    }
    if (m == "GET" && rest == "/filaments") {
        out = zk::reply(run_on_ui([]() -> json { return zk::guard([&]() -> json { return filaments_json(); }); }, 60));
        return true;
    }

    const bool upload     = m == "POST" && rest == "/upload";
    const bool mock_login = m == "POST" && rest == "/mock_login" && CrCloud::is_mock();
    if (!upload && !mock_login)
        return false;

    json body = json::object();
    if (!zk::parse_body(req, body)) {
        out = {400, {{"error", "invalid_json"}}};
        return true;
    }

    if (mock_login) {
        out = zk::reply(zk::guard([&]() -> json {
            if (!body.contains("callback_url") || !body["callback_url"].is_string())
                zk::fail("bad_value", "\"callback_url\" (the address the login page redirects to) is required");
            const std::string code = CrealityCloud::callback_code(body["callback_url"].get<std::string>());
            if (code.empty())
                zk::fail("bad_value", "the callback url carries no code");
            // App config reads stay on the UI thread (returned by value: no reference outlives a ui_timeout).
            const json ci = run_on_ui([]() -> json {
                const CrealityCloud::ClientInfo i = CrCloud::client_info();
                return {{"lang", i.lang}, {"duid", i.duid}, {"ver", i.app_version}, {"region", CrealityCloud::region_key(CrCloud::region())}};
            }, 30);
            CrealityCloud::ClientInfo info;
            info.lang        = ci.value("lang", std::string("en"));
            info.duid        = ci.value("duid", std::string());
            info.app_version = ci.value("ver", std::string());
            const CrCloud::Region region = CrealityCloud::region_from_key(ci.value("region", std::string()));
            CrealityCloud::Session          s;
            const CrealityCloud::CallResult r = CrealityCloud::exchange_login_code(region, info, code, s);
            if (!r.ok)
                zk::fail("login_failed", r.message);
            run_on_ui([s]() -> json {
                CrCloud::store_session(s);
                return json::object();
            }, 30);
            return status_json();
        }));
        return true;
    }

    // upload: prepare on the UI thread (preset bundle), send from this thread.
    const bool        dry_run = body.contains("dry_run") && body["dry_run"].is_boolean() && body["dry_run"].get<bool>();
    std::vector<std::string> names;
    std::string              printer;
    json pre = zk::guard([&]() -> json {
        names = need_presets(body);
        if (body.contains("printer") && !body["printer"].is_null()) {
            if (!body["printer"].is_string())
                zk::fail("bad_value", "\"printer\" must be a printer preset name (see printer_targets)");
            printer = body["printer"].get<std::string>();
        }
        if (!dry_run) {
            require_control(body, "creality cloud upload");
        }
        return json::object();
    });
    if (pre.contains("error")) {
        out = zk::reply(pre);
        return true;
    }

    // Everything the UI-thread lambda touches is owned by it (copies / shared_ptr): a ui_timeout must not leave it
    // writing into this stack frame.
    auto holder = std::make_shared<std::vector<CrCloud::PreparedUpload>>();
    json prep   = run_on_ui([holder, names, printer]() -> json {
        return zk::guard([&]() -> json {
            *holder = CrCloud::prepare_uploads(names, printer);
            return json::object();
        });
    }, 60);
    if (prep.contains("error")) {
        out = zk::reply(prep);
        return true;
    }
    std::vector<CrCloud::PreparedUpload> prepared = std::move(*holder);

    json plan = json::array();
    for (const CrCloud::PreparedUpload& p : prepared) {
        json e = {{"name", p.name}, {"printer", p.target_printer}};
        if (!p.error.empty())
            e["error"] = p.error;
        else {
            e["action"] = p.update ? "update" : "create";
            e["bytes"]  = p.bytes;
        }
        plan.push_back(e);
    }
    const bool logged = CrCloud::logged_in();
    if (dry_run) {
        out = zk::reply(json{{"dry_run", true}, {"logged_in", logged}, {"region", CrealityCloud::region_key(CrCloud::region())}, {"would_send", plan}});
        return true;
    }
    if (!logged) {
        out = zk::reply(json{{"error", "not_logged_in"},
                             {"detail", "no Creality Cloud account is logged in; the owner logs in from OrcaSlicer (Preferences or the Creality device tab)"}});
        return true;
    }
    BOOST_LOG_TRIVIAL(info) << "RemoteAPI printer control: creality cloud upload of " << prepared.size() << " filament preset(s)";
    const std::vector<CrCloud::UploadReport> reports = CrCloud::upload_blocking(std::move(prepared));
    json results = json::array();
    int  ok      = 0;
    for (const CrCloud::UploadReport& r : reports) {
        results.push_back({{"name", r.name}, {"ok", r.ok}, {"created", r.created}, {"message", r.message}});
        ok += r.ok ? 1 : 0;
    }
    zk::notify("Creality Cloud: " + std::to_string(ok) + " of " + std::to_string(reports.size()) + " filament preset(s) sent by the Remote API");
    out = zk::reply(json{{"action", "creality cloud upload"}, {"sent", ok}, {"total", reports.size()}, {"results", results}, {"ok", ok == int(reports.size())}});
    return true;
}

}}} // namespace Slic3r::GUI::RemoteAPI
