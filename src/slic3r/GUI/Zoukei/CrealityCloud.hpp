// src/slic3r/GUI/Zoukei/CrealityCloud.hpp
//
// Orca Zoukei: Creality Cloud account and filament upload (logic part; the windows are in CrealityCloudUi).
// The user's filament presets go to the Creality Cloud account the same way Creality Print syncs them (protocol in
// slic3r/Utils/CrealityCloudClient, ported from Creality Print, AGPL-3.0), so a printer bound to that account
// (K2 / K2 Plus with CFS) offers them in its material list.
//
// Storage: the token lives only in the system's secure store (wxSecretStore: Windows Credential Manager, macOS
// Keychain, libsecret), never in OrcaSlicer.conf and never in a log. The app config only keeps the region, the
// nickname shown in the UI, a random installation id and the auto-sync switch. Logout deletes all of it except
// the region and the installation id. Uploaded presets are remembered (cloud id per preset and account) in
// <data_dir>/zoukei/creality_cloud_uploads.json, separate from Orca's own preset sync ids.
#ifndef slic3r_GUI_Zoukei_CrealityCloud_hpp_
#define slic3r_GUI_Zoukei_CrealityCloud_hpp_

#include "slic3r/Utils/CrealityCloudClient.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
class Preset;
namespace GUI { namespace CrCloud {

using CrealityCloud::Region;
using CrealityCloud::Session;

// ---- account (thread safe) ------------------------------------------------------------------
bool        logged_in();
Session     session();           // includes the token: only hand it to CrealityCloudClient
Region      region();
void        set_region(Region r); // a different region logs out
std::string nickname();          // display name of the logged in account ("" when logged out)
bool        is_mock();           // ORCA_ZOUKEI_CREALITY_CLOUD_MOCK is set (tests)
bool        secure_storage_available();
CrealityCloud::ClientInfo client_info();

// Stores a fresh session (secure store + display name). False when the secure store refused it: the session is then
// kept for this run only.
bool store_session(const Session& s);
void logout();
// The cloud said the token is no longer valid: forget it (same as logout, keeps nothing).
void on_token_invalid();

// ---- browser login (system browser + temporary loopback listener) --------------------------------
// The Creality page is opened in the default browser (Google / Apple sign-in refuse embedded web views); the browser
// is redirected to http://localhost:13666/login?code=..., which a listener bound to 127.0.0.1 only during this login
// receives (CrealityCloudCallback). The port is the one Creality Print uses: while Creality Print runs it is taken.
enum class BrowserLoginState { Idle, Waiting, Done, Failed, TimedOut, PortBusy, Cancelled };
std::string browser_login_state_key(BrowserLoginState s); // "idle", "waiting", ...
using BrowserLoginChange = std::function<void(BrowserLoginState state, const std::string& message)>;
constexpr int BROWSER_LOGIN_TIMEOUT_S = 300;
// UI thread. Returns Waiting, PortBusy or Failed (`message` says why). `on_change` runs on the UI thread for every
// later change (a failed code exchange keeps Waiting with a message; Done / TimedOut end it).
BrowserLoginState start_browser_login(bool open_browser, int timeout_s, BrowserLoginChange on_change, std::string& message);
void              cancel_browser_login();   // UI thread; also drops the on_change callback
BrowserLoginState browser_login_state();    // any thread
std::string       browser_login_url();      // the Creality page of the running attempt (UI thread)

// ---- user filament presets ----------------------------------------------------------------------
struct FilamentEntry
{
    std::string name;
    std::string type;       // filament_type
    std::string vendor;     // filament_vendor
    std::string inherits;
    bool        uploaded = false; // this preset was sent to the logged in account before
    bool        changed  = false; // ... and changed since
};
// User filament presets (UI thread).
std::vector<FilamentEntry> user_filaments();

// Creality printer presets a filament can be linked to (system presets, "Creality K2 Plus 0.4 nozzle" ...), and the
// default one: the selected printer when it is a Creality, else a K2 Plus 0.4 nozzle.
std::vector<std::string> printer_targets();
std::string              default_printer_target();

struct PreparedUpload
{
    std::string                  name;
    std::string                  target_printer;
    bool                         update = false; // a cloud id exists: editProfile
    size_t                       bytes  = 0;
    std::string                  error;          // the preset could not be prepared
    CrealityCloud::ProfileUpload upload;
};
// Builds the files to upload (UI thread): the full preset (inherited values resolved, "from": "User"), linked to
// `target_printer` through compatible_printers like a Creality Print custom filament.
std::vector<PreparedUpload> prepare_uploads(const std::vector<std::string>& names, const std::string& target_printer);

struct UploadReport
{
    std::string name;
    bool        ok            = false;
    bool        created       = false;
    bool        token_invalid = false;
    bool        rate_limited  = false; // still refused by the Creality rate limit after every retry
    bool        cancelled     = false;
    int         attempts      = 0;
    std::string message;
};

// Pacing of every upload (window, auto sync, Remote API), shared by all of them: presets go one after the other with
// UPLOAD_INTERVAL between them; a rate-limited answer (Creality code 1088 / HTTP 429) is retried after an exponential
// backoff with jitter (10 s, 20 s, 40 s ... 2 min, or the Retry-After the server sends), up to UPLOAD_RETRIES times.
// A retry never creates the profile twice: once the cloud gave an id, it goes on with editProfile.
// Test builds pointed at the mock may shorten it: ORCA_ZOUKEI_CREALITY_CLOUD_THROTTLE="interval_ms,backoff_ms,max_ms".
constexpr int UPLOAD_INTERVAL_MS = 3000;
constexpr int UPLOAD_BACKOFF_MS  = 10000;
constexpr int UPLOAD_BACKOFF_MAX = 120000;
constexpr int UPLOAD_RETRIES     = 5;

struct UploadProgress
{
    int         index   = 0; // 1-based preset being sent
    int         total   = 0;
    std::string name;
    int         attempt = 0; // 0 = first try
    int         wait_s  = 0; // > 0: waiting (rate_limit: for the Creality limit, else the pause between presets)
    bool        rate_limit = false;
};
using UploadProgressFn = std::function<void(const UploadProgress&)>;
struct UploadControl
{
    std::atomic<bool> cancel{false};
};

// Blocking (worker / Remote API io thread). `progress` runs on the calling thread.
std::vector<UploadReport> upload_blocking(std::vector<PreparedUpload> uploads, UploadProgressFn progress = nullptr,
                                          std::shared_ptr<UploadControl> control = nullptr);
// Runs the uploads on a worker thread; `progress` and `done` run on the UI thread.
void upload_async(std::vector<PreparedUpload> uploads, std::function<void(const std::vector<UploadReport>&)> done,
                  UploadProgressFn progress = nullptr, std::shared_ptr<UploadControl> control = nullptr);

// One background upload started by the Remote API at a time; its state is polled.
struct ApiUploadJob
{
    std::string               state = "idle"; // idle | running | done | cancelled
    UploadProgress            progress;
    std::vector<UploadReport> results;
};
bool         start_api_upload(std::vector<PreparedUpload> uploads); // false: one is already running
ApiUploadJob api_upload_status();                                   // any thread
void         cancel_api_upload();

// Called after a preset was saved (UI thread): with Preferences "creality_cloud_auto_sync" on and a session, a user
// filament preset is uploaded in the background (default target printer) and the result shown as a notification.
void on_preset_saved(const Preset& preset);

}}} // namespace Slic3r::GUI::CrCloud

#endif
