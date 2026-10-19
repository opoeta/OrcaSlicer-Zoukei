#ifndef slic3r_ZoukeiUpdate_hpp_
#define slic3r_ZoukeiUpdate_hpp_

// Orca Zoukei self-update, without any GUI code (see docs/zoukei/updates.md).
//
//   check()    asks the GitHub Releases API of opoeta/OrcaSlicer-Zoukei for a newer release (blocking, run it on a worker thread).
//   prepare()  downloads manifest + delta (or full) zip, verifies SHA-256 of every file, stages them in a temp dir
//              and writes the plan for zoukei-updater.exe. Nothing downloaded is executed or copied into the install.
//   launch_updater()  starts the updater (a copy of it in the temp dir); the caller then exits Orca.
//
// Test hook: ORCA_ZOUKEI_UPDATE_URL=http://127.0.0.1:<port>/releases.json points everything to a local fake release
// (loopback http only; without it only https://api.github.com/repos/opoeta/OrcaSlicer-Zoukei and
// https://github.com/opoeta/OrcaSlicer-Zoukei/releases/download/ are ever contacted).

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace ZoukeiUpdate {

struct Asset
{
    std::string name;
    std::string url;
    uint64_t    size{0};
};

struct ReleaseInfo
{
    std::string        version;     // "2026.10.17" (tag without the leading v)
    std::string        tag;
    std::string        notes;       // Markdown body of the release
    std::string        page_url;    // html_url of the release
    bool               prerelease{false};
    std::vector<Asset> assets;

    const Asset* find_asset(const std::string& name) const;
};

struct CheckResult
{
    enum Status { UpToDate, Available, Failed };
    Status      status{Failed};
    ReleaseInfo release;
    std::string error;
};

struct FileEntry
{
    uint64_t    size{0};
    std::string sha256;
};

struct DeltaInfo
{
    std::string              from;
    std::string              asset;
    uint64_t                 size{0};
    std::string              sha256;
    std::set<std::string>    files;
    std::vector<std::string> removed;
};

struct Manifest
{
    std::string                      version;
    std::string                      platform;
    std::string                      exe;
    std::map<std::string, FileEntry> files;
    std::string                      full_asset;
    uint64_t                         full_size{0};
    std::string                      full_sha256;
    std::vector<DeltaInfo>           deltas;
};

// What prepare() leaves ready for the updater.
struct Prepared
{
    std::string          version;
    boost::filesystem::path work_dir;
    boost::filesystem::path plan_path;
    bool                 used_delta{false};
    uint64_t             download_bytes{0};
    size_t               files_to_copy{0};
};

// phase: "verify" | "download" | "extract". Return false to cancel.
using ProgressFn = std::function<bool(const std::string& phase, uint64_t done, uint64_t total)>;

// Strings the updater shows when something goes wrong (it has no translations of its own).
struct UpdaterTexts
{
    std::string title;
    std::string failed;
};

std::string current_version();
// "v" prefix is not accepted here; digits and dots, up to 4 numeric parts, optional "-suffix".
bool is_valid_version(const std::string& v);
// <0, 0, >0. A version with a "-suffix" is lower than the same without it.
int compare_versions(const std::string& a, const std::string& b);
// "windows-x64", "windows-arm64" or "" where self-update is not implemented.
std::string platform_id();
bool        self_update_supported();

// Only these origins may be contacted (plus the loopback test URL when ORCA_ZOUKEI_UPDATE_URL is set).
bool        url_allowed(const std::string& url);
std::string releases_url();
// True when ORCA_ZOUKEI_UPDATE_URL points to a local fake release (loopback http).
bool        test_mode();
bool        is_safe_relpath(const std::string& rel);
bool        is_sha256_hex(const std::string& s);

// Parses the JSON body of the Releases API (array). `beta`: also consider pre-releases. Pure, used by check() and the tests.
CheckResult evaluate_releases(const std::string& json_body, bool beta, const std::string& installed_version);
CheckResult check(bool beta);

bool parse_manifest(const std::string& json_body, Manifest& out, std::string& err);

boost::filesystem::path install_dir();
bool                    install_dir_writable(const boost::filesystem::path& dir);

bool prepare(const ReleaseInfo& rel, const boost::filesystem::path& install, const ProgressFn& progress,
             const UpdaterTexts& texts, Prepared& out, std::string& err);

// Starts <work>/zoukei-updater.exe (copied from the install dir) for the plan; it waits for this process to exit.
bool launch_updater(const Prepared& prepared, std::string& err);

} // namespace ZoukeiUpdate
} // namespace Slic3r

#endif
