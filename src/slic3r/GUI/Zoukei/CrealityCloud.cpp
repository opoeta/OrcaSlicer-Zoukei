// src/slic3r/GUI/Zoukei/CrealityCloud.cpp
//
// Orca Zoukei: Creality Cloud account and filament upload. The cloud protocol and the way a filament preset is
// linked to a printer (compatible_printers = one Creality printer preset, upload type "materia") follow Creality
// Print (AGPL-3.0, github.com/CrealityOfficial/CrealityPrint: SyncUserPresets.cpp, CommunicateWithCXCloud.cpp,
// CreatePresetsDialog.cpp). See CrealityCloud.hpp.
#include "CrealityCloud.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/CrealityCloudCallback.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>

#include <wx/secretstore.h>
#include <wx/thread.h>
#include <wx/utils.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdlib>
#include <random>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI { namespace CrCloud {

using json = nlohmann::json;

namespace {

std::mutex s_mutex;        // guards the session cache
bool       s_loaded = false;
Session    s_session;      // token only in memory and in the secure store
std::mutex s_store_mutex;  // guards the uploads file

wxString secret_service()
{
    return is_mock() ? wxString("OrcaZoukei/CrealityCloud/mock") : wxString("OrcaZoukei/CrealityCloud");
}

AppConfig* config() { return wxGetApp().app_config; }

// Reads the secure store once (UI or any thread; wxSecretStore is thread safe for these calls).
void ensure_loaded_locked()
{
    if (s_loaded)
        return;
    s_loaded  = true;
    s_session = Session();
    AppConfig* ac = config();
    if (ac == nullptr)
        return;
    s_session.region = CrealityCloud::region_from_key(ac->get("creality_cloud_region"));
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    wxString      err;
    if (!store.IsOk(&err))
        return;
    wxString      user;
    wxSecretValue value;
    if (store.Load(secret_service(), user, value) && value.IsOk()) {
        const std::string stored = value.GetAsString(wxConvUTF8).ToStdString(wxConvUTF8);
        const json        j      = json::parse(stored, nullptr, false);
        if (j.is_object() && j.contains("token") && j["token"].is_string()) {
            s_session.token   = j["token"].get<std::string>();
            s_session.user_id = std::string(user.ToUTF8().data());
            if (j.contains("region") && j["region"].is_string())
                s_session.region = CrealityCloud::region_from_key(j["region"].get<std::string>());
            s_session.nickname = ac->get("creality_cloud_nickname");
        }
    }
#endif
}

fs::path uploads_file() { return fs::path(data_dir()) / "zoukei" / "creality_cloud_uploads.json"; }

json read_uploads_locked()
{
    json            j;
    const fs::path  p = uploads_file();
    boost::system::error_code ec;
    if (!fs::exists(p, ec))
        return json::object();
    try {
        boost::nowide::ifstream f(p.string());
        std::stringstream       ss;
        ss << f.rdbuf();
        j = json::parse(ss.str(), nullptr, false);
    } catch (...) {
    }
    return j.is_object() ? j : json::object();
}

void write_uploads_locked(const json& j)
{
    const fs::path p = uploads_file();
    boost::system::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    boost::nowide::ofstream f(p.string(), std::ios::trunc);
    f << j.dump(2);
}

std::string account_key(const Session& s) { return CrealityCloud::region_key(s.region) + ":" + s.user_id; }

// What was uploaded for one preset of the account ({} when nothing).
json uploaded_entry(const Session& s, const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_store_mutex);
    const json all = read_uploads_locked();
    const std::string key = account_key(s);
    if (all.contains(key) && all[key].is_object() && all[key].contains(name) && all[key][name].is_object())
        return all[key][name];
    return json::object();
}

std::string str_opt(const json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

std::string first_string(const DynamicPrintConfig& cfg, const char* key)
{
    if (const auto* opt = cfg.option<ConfigOptionStrings>(key); opt != nullptr && !opt->values.empty())
        return opt->values.front();
    if (const auto* opt = cfg.option<ConfigOptionString>(key); opt != nullptr)
        return opt->value;
    return std::string();
}

bool is_creality_printer_preset(const Preset& p)
{
    return p.is_system && p.vendor != nullptr && boost::iequals(p.vendor->id, "Creality");
}

// The preset file sent to the cloud: the whole resolved preset, linked to one printer preset.
std::string build_payload(const Preset& preset, const std::string& target_printer)
{
    DynamicPrintConfig cfg = preset.config;
    if (!target_printer.empty()) {
        cfg.set_key_value("compatible_printers", new ConfigOptionStrings({target_printer}));
        cfg.set_key_value("compatible_printers_condition", new ConfigOptionString(""));
    }
    cfg.set_key_value("filament_settings_id", new ConfigOptionStrings({preset.name}));
    std::ostringstream ss;
    cfg.save_to_json(ss, preset.name, "User", preset.version.to_string(), true);
    return ss.str();
}

void remember_upload(const Session& s, const PreparedUpload& up, const CrealityCloud::ProfileUploadResult& r)
{
    if (r.setting_id.empty())
        return;
    std::lock_guard<std::mutex> lock(s_store_mutex);
    json all = read_uploads_locked();
    json& acc = all[account_key(s)];
    if (!acc.is_object())
        acc = json::object();
    json entry = {{"id", r.setting_id},
                  {"target", up.target_printer},
                  {"md5", r.ok ? CrealityCloud::md5_hex(up.upload.content) : std::string()},
                  {"time", (long long) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
    acc[up.name] = entry;
    write_uploads_locked(all);
}

} // namespace

// ---- account -------------------------------------------------------------------------------------

bool is_mock() { return !CrealityCloud::mock_base().empty(); }

bool secure_storage_available()
{
#if wxUSE_SECRETSTORE
    wxString err;
    return wxSecretStore::GetDefault().IsOk(&err);
#else
    return false;
#endif
}

bool logged_in()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    ensure_loaded_locked();
    return s_session.valid();
}

Session session()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    ensure_loaded_locked();
    return s_session;
}

Region region()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    ensure_loaded_locked();
    return s_session.region;
}

std::string nickname()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    ensure_loaded_locked();
    if (!s_session.valid())
        return std::string();
    return s_session.nickname.empty() ? s_session.user_id : s_session.nickname;
}

void set_region(Region r)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        ensure_loaded_locked();
        changed = s_session.region != r;
    }
    if (changed && logged_in())
        logout();
    std::lock_guard<std::mutex> lock(s_mutex);
    s_session.region = r;
    if (AppConfig* ac = config()) {
        ac->set("creality_cloud_region", CrealityCloud::region_key(r));
        ac->save();
    }
}

CrealityCloud::ClientInfo client_info()
{
    // The app config is only touched on the UI thread; worker threads get the copy made there.
    static CrealityCloud::ClientInfo cached;
    if (!wxThread::IsMain()) {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!cached.duid.empty())
            return cached;
        CrealityCloud::ClientInfo info;
        info.lang        = "en";
        info.duid        = boost::uuids::to_string(boost::uuids::random_generator()());
        info.app_version = SoftFever_VERSION;
        return info;
    }
    CrealityCloud::ClientInfo info;
    if (AppConfig* ac = config()) {
        info.lang = ac->get("language");
        info.duid = ac->get("creality_cloud_duid");
        if (info.duid.empty()) {
            info.duid = boost::uuids::to_string(boost::uuids::random_generator()());
            ac->set("creality_cloud_duid", info.duid);
        }
    }
    if (info.lang.empty())
        info.lang = "en";
    info.app_version = SoftFever_VERSION;
    std::lock_guard<std::mutex> lock(s_mutex);
    cached = info;
    return info;
}

bool store_session(const Session& s)
{
    bool stored = false;
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    wxString      err;
    if (store.IsOk(&err)) {
        const json secret = {{"token", s.token}, {"region", CrealityCloud::region_key(s.region)}};
        stored = store.Save(secret_service(), wxString::FromUTF8(s.user_id), wxSecretValue(wxString::FromUTF8(secret.dump())));
    }
#endif
    if (!stored)
        BOOST_LOG_TRIVIAL(warning) << "Creality Cloud: the secure store is not available; the login is kept for this session only";
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_loaded  = true;
        s_session = s;
    }
    if (AppConfig* ac = config()) {
        ac->set("creality_cloud_region", CrealityCloud::region_key(s.region));
        ac->set("creality_cloud_nickname", s.nickname);
        ac->save();
    }
    BOOST_LOG_TRIVIAL(info) << "Creality Cloud: logged in (" << CrealityCloud::region_key(s.region) << ")";
    return stored;
}

void logout()
{
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    wxString      err;
    if (store.IsOk(&err))
        store.Delete(secret_service());
#endif
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const Region r = s_session.region;
        s_session        = Session();
        s_session.region = r;
        s_loaded         = true;
    }
    if (AppConfig* ac = config()) {
        ac->set("creality_cloud_nickname", "");
        ac->save();
    }
    BOOST_LOG_TRIVIAL(info) << "Creality Cloud: logged out";
}

void on_token_invalid()
{
    BOOST_LOG_TRIVIAL(warning) << "Creality Cloud: the token is no longer accepted, logging out";
    logout();
}

// ---- browser login -------------------------------------------------------------------------------

namespace {

std::string html_escape(const std::string& in)
{
    std::string out;
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

// The page the browser shows after the redirect: Orca's colours, translated text, nothing loaded from outside.
std::string callback_page(const std::string& title, const std::string& text, bool ok)
{
    const char* accent = ok ? "#009688" : "#e14747";
    return std::string("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>OrcaSlicer</title><style>"
                       "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#f4f4f5;"
                       "font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif;color:#262e30}"
                       "@media(prefers-color-scheme:dark){body{background:#1f1f22;color:#e6e6e6}.card{background:#2b2b2f!important}}"
                       ".card{background:#fff;border-radius:10px;padding:32px 40px;max-width:460px;box-shadow:0 4px 18px rgba(0,0,0,.12);text-align:center}"
                       ".brand{font-weight:700;letter-spacing:.5px;color:#009688;margin-bottom:18px}"
                       "h1{font-size:20px;margin:0 0 10px;color:") +
           accent + "}p{margin:0;line-height:1.5}</style></head><body><div class=\"card\"><div class=\"brand\">OrcaSlicer</div><h1>" +
           html_escape(title) + "</h1><p>" + html_escape(text) + "</p></div></body></html>";
}

struct BrowserLoginShared
{
    std::mutex  m;
    Session     session;
};

std::unique_ptr<CrealityCloud::LoopbackCallbackServer> s_server;
std::atomic<int>                                       s_browser_state{int(BrowserLoginState::Idle)};
std::atomic<unsigned>                                  s_browser_gen{0};
BrowserLoginChange                                     s_browser_cb; // UI thread
std::string                                            s_browser_url; // UI thread

void set_browser_state(BrowserLoginState st) { s_browser_state = int(st); }

// UI thread: report a change to the dialog of the running attempt (if it is still the current one).
void browser_notify(unsigned gen, BrowserLoginState st, const std::string& message)
{
    if (gen != s_browser_gen)
        return;
    set_browser_state(st);
    const bool final_state = st != BrowserLoginState::Waiting;
    if (final_state && s_server)
        s_server->stop();
    BrowserLoginChange cb = s_browser_cb;
    if (final_state)
        s_browser_cb = nullptr;
    if (cb)
        cb(st, message);
}

} // namespace

std::string browser_login_state_key(BrowserLoginState s)
{
    switch (s) {
    case BrowserLoginState::Idle: return "idle";
    case BrowserLoginState::Waiting: return "waiting";
    case BrowserLoginState::Done: return "done";
    case BrowserLoginState::Failed: return "failed";
    case BrowserLoginState::TimedOut: return "timed_out";
    case BrowserLoginState::PortBusy: return "port_busy";
    case BrowserLoginState::Cancelled: return "cancelled";
    }
    return "idle";
}

BrowserLoginState browser_login_state() { return BrowserLoginState(s_browser_state.load()); }

std::string browser_login_url() { return s_browser_url; }

void cancel_browser_login()
{
    ++s_browser_gen; // late callbacks of the old attempt are ignored
    s_browser_cb = nullptr;
    if (s_server)
        s_server->stop();
    if (browser_login_state() == BrowserLoginState::Waiting)
        set_browser_state(BrowserLoginState::Cancelled);
}

BrowserLoginState start_browser_login(bool open_browser, int timeout_s, BrowserLoginChange on_change, std::string& message)
{
    cancel_browser_login();
    const unsigned                  gen    = ++s_browser_gen;
    const Region                    reg    = region();
    const CrealityCloud::ClientInfo info   = client_info();
    const CrealityCloud::Endpoints  ep     = CrealityCloud::endpoints(reg);
    s_browser_url                          = CrealityCloud::login_page_url(ep, info.lang);

    // Pages translated here, on the UI thread.
    const std::string ok_page   = callback_page(_u8L("Login complete"), _u8L("You can close this tab and go back to OrcaSlicer."), true);
    const std::string done_page = callback_page(_u8L("Login complete"), _u8L("This login was already completed. You can close this tab."), true);
    const std::string fail_title = _u8L("Login failed");
    const std::string fail_text  = _u8L("OrcaSlicer could not finish the login: %1%. Go back to OrcaSlicer and try again.");

    auto shared = std::make_shared<BrowserLoginShared>();
    auto on_code = [reg, info, shared, ok_page, fail_title, fail_text, gen](const std::string& code, bool& accepted) -> std::string {
        CrealityCloud::Session          s;
        const CrealityCloud::CallResult r = CrealityCloud::exchange_login_code(reg, info, code, s);
        if (r.ok) {
            {
                std::lock_guard<std::mutex> lock(shared->m);
                shared->session = s;
            }
            accepted = true; // one successful code ends the attempt; a failed one keeps waiting
            return ok_page;
        }
        const std::string msg = r.message;
        wxGetApp().CallAfter([gen, msg]() { browser_notify(gen, BrowserLoginState::Waiting, msg); });
        return callback_page(fail_title, (boost::format(fail_text) % msg).str(), false);
    };
    auto on_done = [shared, gen]() {
        wxGetApp().CallAfter([shared, gen]() {
            if (gen != s_browser_gen)
                return;
            Session s;
            {
                std::lock_guard<std::mutex> lock(shared->m);
                s = shared->session;
            }
            store_session(s);
            browser_notify(gen, BrowserLoginState::Done, std::string());
        });
    };
    auto on_timeout = [gen]() { wxGetApp().CallAfter([gen]() { browser_notify(gen, BrowserLoginState::TimedOut, std::string()); }); };

    if (!s_server)
        s_server = std::make_unique<CrealityCloud::LoopbackCallbackServer>();
    std::string error;
    const auto  st = s_server->start(CrealityCloud::CALLBACK_PORT, std::max(5, timeout_s), on_code, done_page, on_done, on_timeout, error);
    if (st == CrealityCloud::LoopbackCallbackServer::Start::PortBusy) {
        set_browser_state(BrowserLoginState::PortBusy);
        message = (boost::format(_u8L("Port %1% on this computer is already in use, most likely by Creality Print, which receives its own "
                                      "logins there. Close Creality Print and try again, or log in in this window with your e-mail.")) %
                   CrealityCloud::CALLBACK_PORT)
                      .str();
        BOOST_LOG_TRIVIAL(warning) << "Creality Cloud browser login: port " << CrealityCloud::CALLBACK_PORT << " is in use";
        return BrowserLoginState::PortBusy;
    }
    if (st != CrealityCloud::LoopbackCallbackServer::Start::Ok) {
        set_browser_state(BrowserLoginState::Failed);
        message = (boost::format(_u8L("The login listener could not start: %1%")) % error).str();
        return BrowserLoginState::Failed;
    }
    set_browser_state(BrowserLoginState::Waiting);
    s_browser_cb = std::move(on_change);
    if (open_browser)
        wxLaunchDefaultBrowser(wxString::FromUTF8(s_browser_url));
    return BrowserLoginState::Waiting;
}

// ---- filament presets ------------------------------------------------------------------------------

std::vector<FilamentEntry> user_filaments()
{
    std::vector<FilamentEntry> out;
    PresetBundle*              bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;
    const Session s       = session();
    json          account = json::object();
    if (s.valid()) {
        std::lock_guard<std::mutex> lock(s_store_mutex);
        const json all = read_uploads_locked();
        if (all.contains(account_key(s)) && all[account_key(s)].is_object())
            account = all[account_key(s)];
    }
    for (const Preset& p : bundle->filaments) {
        if (!p.is_user() || p.name.empty())
            continue;
        FilamentEntry e;
        e.name     = p.name;
        e.type     = first_string(p.config, "filament_type");
        e.vendor   = first_string(p.config, "filament_vendor");
        e.inherits = p.inherits();
        if (account.contains(p.name) && account[p.name].is_object()) {
            const json& u = account[p.name];
            e.uploaded    = !str_opt(u, "id").empty();
            const std::string md5 = str_opt(u, "md5");
            e.changed     = e.uploaded && (md5.empty() || md5 != CrealityCloud::md5_hex(build_payload(p, str_opt(u, "target"))));
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const FilamentEntry& a, const FilamentEntry& b) { return boost::ilexicographical_compare(a.name, b.name); });
    return out;
}

std::vector<std::string> printer_targets()
{
    std::vector<std::string> out;
    PresetBundle*            bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;
    std::set<std::string> seen;
    for (const Preset& p : bundle->printers)
        if (is_creality_printer_preset(p) && p.is_visible && seen.insert(p.name).second)
            out.push_back(p.name);
    // Also the installed but hidden K2 family, so a K2 Plus is always offered.
    for (const Preset& p : bundle->printers)
        if (is_creality_printer_preset(p) && boost::icontains(p.name, "K2") && seen.insert(p.name).second)
            out.push_back(p.name);
    std::sort(out.begin(), out.end());
    return out;
}

std::string default_printer_target()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        const Preset& sel = bundle->printers.get_selected_preset();
        if (is_creality_printer_preset(sel))
            return sel.name;
        if (const Preset* parent = bundle->printers.get_selected_preset_parent(); parent != nullptr && is_creality_printer_preset(*parent))
            return parent->name;
    }
    const std::vector<std::string> targets = printer_targets();
    for (const std::string& t : targets)
        if (boost::icontains(t, "K2 Plus 0.4"))
            return t;
    return targets.empty() ? std::string("Creality K2 Plus 0.4 nozzle") : targets.front();
}

std::vector<PreparedUpload> prepare_uploads(const std::vector<std::string>& names, const std::string& target_printer)
{
    std::vector<PreparedUpload> out;
    PresetBundle*               bundle = wxGetApp().preset_bundle;
    const Session               s      = session();
    client_info(); // refreshes the copy the upload thread uses
    for (const std::string& name : names) {
        PreparedUpload up;
        up.name           = name;
        up.target_printer = target_printer.empty() ? default_printer_target() : target_printer;
        const Preset* p   = bundle != nullptr ? bundle->filaments.find_preset(name, false) : nullptr;
        if (p == nullptr || p->name != name) {
            up.error = "no filament preset with this name";
        } else if (!p->is_user()) {
            up.error = "only your own (user) filament presets can be uploaded";
        } else {
            up.upload.name    = name;
            up.upload.content = build_payload(*p, up.target_printer);
            up.bytes          = up.upload.content.size();
            if (s.valid())
                up.upload.setting_id = str_opt(uploaded_entry(s, name), "id");
            up.update = !up.upload.setting_id.empty();
        }
        out.push_back(std::move(up));
    }
    return out;
}

namespace {

struct Throttle
{
    int interval_ms = UPLOAD_INTERVAL_MS;
    int backoff_ms  = UPLOAD_BACKOFF_MS;
    int max_ms      = UPLOAD_BACKOFF_MAX;
};

Throttle throttle()
{
    Throttle t;
    if (!is_mock())
        return t;
    const char* env = std::getenv("ORCA_ZOUKEI_CREALITY_CLOUD_THROTTLE");
    if (env == nullptr)
        return t;
    std::vector<std::string> parts;
    boost::split(parts, std::string(env), boost::is_any_of(","));
    auto num = [&](size_t i, int def) {
        try {
            return i < parts.size() ? std::max(0, std::stoi(parts[i])) : def;
        } catch (...) {
            return def;
        }
    };
    t.interval_ms = num(0, t.interval_ms);
    t.backoff_ms  = num(1, t.backoff_ms);
    t.max_ms      = num(2, t.max_ms);
    return t;
}

std::mutex                            s_upload_run_mutex; // one upload stream at a time
std::chrono::steady_clock::time_point s_last_upload;      // guarded by s_upload_run_mutex

bool cancelled(const std::shared_ptr<UploadControl>& c) { return c && c->cancel.load(); }

// Sleeps `ms`, reporting the seconds left once per second. False when cancelled.
bool wait_ms(int ms, const std::shared_ptr<UploadControl>& control, const UploadProgressFn& progress, UploadProgress pr)
{
    using clock     = std::chrono::steady_clock;
    const auto end  = clock::now() + std::chrono::milliseconds(ms);
    int        last = -1;
    while (clock::now() < end) {
        if (cancelled(control))
            return false;
        const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - clock::now()).count() + 999) / 1000;
        if (left != last && progress) {
            pr.wait_s = std::max(1, left);
            progress(pr);
            last = left;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !cancelled(control);
}

std::mutex   s_api_job_mutex;
ApiUploadJob s_api_job;
std::shared_ptr<UploadControl> s_api_control;

} // namespace

std::vector<UploadReport> upload_blocking(std::vector<PreparedUpload> uploads, UploadProgressFn progress, std::shared_ptr<UploadControl> control)
{
    std::vector<UploadReport>       out;
    std::unique_lock<std::mutex>    run_lock(s_upload_run_mutex); // the window, auto sync and the API share one pace
    const Session                   s    = session();
    const CrealityCloud::ClientInfo info = client_info();
    const Throttle                  th   = throttle();
    bool                            dead = !s.valid();
    std::mt19937                    rng(std::random_device{}());
    const int                       total = int(uploads.size());
    for (int i = 0; i < total; ++i) {
        PreparedUpload& up = uploads[size_t(i)];
        UploadReport    rep;
        rep.name = up.name;
        UploadProgress pr;
        pr.index = i + 1;
        pr.total = total;
        pr.name  = up.name;
        if (cancelled(control)) {
            rep.cancelled = true;
            rep.message   = "cancelled";
        } else if (!up.error.empty()) {
            rep.message = up.error;
        } else if (dead) {
            rep.token_invalid = true;
            rep.message       = "not logged in to the Creality Cloud";
        } else {
            // Pause between presets (also between two upload batches).
            const auto since = std::chrono::steady_clock::now() - s_last_upload;
            const int  gap   = th.interval_ms - int(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
            bool       go    = gap <= 0 || s_last_upload.time_since_epoch().count() == 0 || wait_ms(gap, control, progress, pr);
            CrealityCloud::ProfileUploadResult r;
            for (int attempt = 0; go; ++attempt) {
                pr.attempt    = attempt;
                pr.wait_s     = 0;
                pr.rate_limit = false;
                if (progress)
                    progress(pr);
                r             = CrealityCloud::upload_filament_profile(s, info, up.upload);
                s_last_upload = std::chrono::steady_clock::now();
                remember_upload(s, up, r);
                if (!r.setting_id.empty())
                    up.upload.setting_id = r.setting_id; // resume with editProfile: never a second profile
                rep.attempts = attempt + 1;
                if (r.ok || !r.rate_limited() || attempt >= UPLOAD_RETRIES)
                    break;
                long long delay = std::min<long long>(th.max_ms, (long long) th.backoff_ms << attempt);
                std::uniform_real_distribution<double> jitter(0.8, 1.2);
                delay = (long long) (double(delay) * jitter(rng));
                if (r.retry_after_s > 0)
                    delay = std::min<long long>(300000, std::max<long long>(delay, (long long) r.retry_after_s * 1000));
                BOOST_LOG_TRIVIAL(info) << "Creality Cloud: rate limited, retrying \"" << up.name << "\" in " << delay / 1000 << " s";
                pr.rate_limit = true;
                go            = wait_ms(int(delay), control, progress, pr);
            }
            if (!go && !r.ok) {
                rep.cancelled = true;
                rep.message   = "cancelled";
            } else {
                rep.ok            = r.ok;
                rep.created       = r.created;
                rep.token_invalid = r.token_invalid();
                rep.rate_limited  = !r.ok && r.rate_limited();
                rep.message       = r.ok ? (r.created ? "created" : "updated") : r.message;
                if (rep.token_invalid)
                    dead = true;
            }
        }
        BOOST_LOG_TRIVIAL(info) << "Creality Cloud: upload of filament preset \"" << up.name << "\": " << (rep.ok ? "ok" : rep.message);
        out.push_back(std::move(rep));
    }
    if (dead && s.valid())
        wxGetApp().CallAfter([]() { on_token_invalid(); });
    return out;
}

void upload_async(std::vector<PreparedUpload> uploads, std::function<void(const std::vector<UploadReport>&)> done, UploadProgressFn progress,
                  std::shared_ptr<UploadControl> control)
{
    std::thread([uploads = std::move(uploads), done = std::move(done), progress = std::move(progress), control]() mutable {
        UploadProgressFn on_progress;
        if (progress)
            on_progress = [progress](const UploadProgress& p) { wxGetApp().CallAfter([progress, p]() { progress(p); }); };
        std::vector<UploadReport> reports = upload_blocking(std::move(uploads), on_progress, control);
        wxGetApp().CallAfter([reports = std::move(reports), done = std::move(done)]() {
            if (done)
                done(reports);
        });
    }).detach();
}

bool start_api_upload(std::vector<PreparedUpload> uploads)
{
    std::shared_ptr<UploadControl> control = std::make_shared<UploadControl>();
    {
        std::lock_guard<std::mutex> lock(s_api_job_mutex);
        if (s_api_job.state == "running")
            return false;
        s_api_job                = ApiUploadJob();
        s_api_job.state          = "running";
        s_api_job.progress.total = int(uploads.size());
        s_api_control            = control;
    }
    std::thread([uploads = std::move(uploads), control]() mutable {
        auto on_progress = [](const UploadProgress& p) {
            std::lock_guard<std::mutex> lock(s_api_job_mutex);
            s_api_job.progress = p;
        };
        std::vector<UploadReport> reports = upload_blocking(std::move(uploads), on_progress, control);
        std::lock_guard<std::mutex> lock(s_api_job_mutex);
        s_api_job.results = std::move(reports);
        s_api_job.state   = control->cancel ? "cancelled" : "done";
    }).detach();
    return true;
}

ApiUploadJob api_upload_status()
{
    std::lock_guard<std::mutex> lock(s_api_job_mutex);
    return s_api_job;
}

void cancel_api_upload()
{
    std::lock_guard<std::mutex> lock(s_api_job_mutex);
    if (s_api_control)
        s_api_control->cancel = true;
}

void on_preset_saved(const Preset& preset)
{
    if (preset.type != Preset::TYPE_FILAMENT || !preset.is_user())
        return;
    AppConfig* ac = config();
    if (ac == nullptr || !ac->get_bool("creality_cloud_auto_sync") || !logged_in())
        return;
    const Session     s      = session();
    const std::string target = str_opt(uploaded_entry(s, preset.name), "target");
    std::vector<PreparedUpload> ups = prepare_uploads({preset.name}, target);
    const std::string name = preset.name;
    upload_async(std::move(ups), [name](const std::vector<UploadReport>& reports) {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr || reports.empty())
            return;
        const UploadReport& r = reports.front();
        const std::string text = r.ok ? (boost::format(_u8L("Filament \"%1%\" sent to the Creality Cloud.")) % name).str() :
                                        (boost::format(_u8L("Filament \"%1%\" was not sent to the Creality Cloud: %2%")) % name % r.message).str();
        plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                              r.ok ? NotificationManager::NotificationLevel::RegularNotificationLevel :
                                                                     NotificationManager::NotificationLevel::WarningNotificationLevel,
                                                              text);
    });
}

}}} // namespace Slic3r::GUI::CrCloud
