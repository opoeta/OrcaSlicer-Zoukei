#include "ZoukeiUpdate.hpp"

#include "Http.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Utils.hpp"
#include "zoukei_version.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r {
namespace ZoukeiUpdate {

namespace {

const char* const GITHUB_API_BASE      = "https://api.github.com/repos/opoeta/OrcaZoukei/";
const char* const GITHUB_DOWNLOAD_BASE = "https://github.com/opoeta/OrcaZoukei/releases/download/";
const char* const INSTALLED_MANIFEST   = "zoukei-installed.json";
const uint64_t    CHUNK                = 8u * 1024 * 1024;

// ORCA_ZOUKEI_UPDATE_URL: only honoured when it is plain http to the loopback interface.
std::string test_url()
{
    const char* e = std::getenv("ORCA_ZOUKEI_UPDATE_URL");
    if (!e || !*e)
        return {};
    std::string u(e);
    if (boost::starts_with(u, "http://127.0.0.1:") || boost::starts_with(u, "http://localhost:"))
        return u;
    BOOST_LOG_TRIVIAL(warning) << "ZoukeiUpdate: ignoring ORCA_ZOUKEI_UPDATE_URL, only http://127.0.0.1:<port> or http://localhost:<port> is accepted";
    return {};
}

// UTF-8 form of a path (the plan file and miniz want UTF-8, boost::filesystem::path::string() is the ANSI code page on Windows).
std::string path_utf8(const fs::path& p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

std::string origin_of(const std::string& u)
{
    size_t scheme = u.find("://");
    if (scheme == std::string::npos)
        return {};
    size_t slash = u.find('/', scheme + 3);
    return (slash == std::string::npos ? u : u.substr(0, slash)) + "/";
}

class Sha256
{
public:
    Sha256() : m_ctx(EVP_MD_CTX_new()) { EVP_DigestInit_ex(m_ctx, EVP_sha256(), nullptr); }
    ~Sha256() { EVP_MD_CTX_free(m_ctx); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t n) { EVP_DigestUpdate(m_ctx, data, n); }
    std::string hex()
    {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int  len = 0;
        EVP_DigestFinal_ex(m_ctx, md, &len);
        static const char* digits = "0123456789abcdef";
        std::string        out;
        for (unsigned i = 0; i < len; ++i) {
            out.push_back(digits[md[i] >> 4]);
            out.push_back(digits[md[i] & 15]);
        }
        return out;
    }

private:
    EVP_MD_CTX* m_ctx;
};

// Hashes a file; `step` is called with the number of bytes read (returns false to stop).
bool hash_file(const fs::path& p, std::string& hex, const std::function<bool(uint64_t)>& step = nullptr)
{
    fs::ifstream in(p, std::ios::binary);
    if (!in)
        return false;
    Sha256            h;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), buf.size());
        std::streamsize n = in.gcount();
        if (n > 0) {
            h.update(buf.data(), size_t(n));
            if (step && !step(uint64_t(n)))
                return false;
        }
    }
    if (in.bad())
        return false;
    hex = h.hex();
    return true;
}

struct HttpResult
{
    bool        completed{false};
    unsigned    status{0};
    std::string body;
    std::string error;
};

HttpResult http_get(const std::string& url, uint64_t limit, long timeout_max, const std::string& range = {})
{
    HttpResult r;
    if (!url_allowed(url)) {
        r.error = "address not allowed: " + url;
        return r;
    }
    Http http = Http::get(url);
    http.header("Accept", range.empty() ? "application/vnd.github+json, application/json, */*" : "application/octet-stream, */*")
        .timeout_connect(10)
        .timeout_max(timeout_max)
        .size_limit(size_t(limit))
        .tls_verify(true);
    if (!range.empty())
        http.set_range(range);
    http.on_complete([&r](std::string body, unsigned status) {
            r.completed = true;
            r.status    = status;
            r.body      = std::move(body);
        })
        .on_error([&r](std::string body, std::string error, unsigned status) {
            r.completed = false;
            r.status    = status;
            r.error     = error.empty() ? ("HTTP " + std::to_string(status)) : error;
            r.body      = std::move(body);
        })
        .perform_sync();
    return r;
}

bool is_digits(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

struct ParsedVersion
{
    std::vector<uint64_t> nums;
    std::string           suffix;
    bool                  valid{false};
};

ParsedVersion parse_version(const std::string& v)
{
    ParsedVersion p;
    std::string   core = v;
    size_t        dash = v.find('-');
    if (dash != std::string::npos) {
        core     = v.substr(0, dash);
        p.suffix = v.substr(dash + 1);
        if (p.suffix.empty() || !std::all_of(p.suffix.begin(), p.suffix.end(), [](unsigned char c) { return std::isalnum(c) || c == '.'; }))
            return p;
    }
    std::vector<std::string> parts;
    boost::split(parts, core, boost::is_any_of("."));
    if (parts.size() < 2 || parts.size() > 4)
        return p;
    for (const auto& s : parts) {
        if (!is_digits(s) || s.size() > 9)
            return p;
        p.nums.push_back(std::stoull(s));
    }
    p.valid = true;
    return p;
}

std::string json_str(const json& j, const char* key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

uint64_t json_u64(const json& j, const char* key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_number_unsigned()) ? it->get<uint64_t>() : 0;
}

bool read_text_file(const fs::path& p, std::string& out)
{
    fs::ifstream in(p, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string one_line(std::string s)
{
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
    return s;
}

} // namespace

const Asset* ReleaseInfo::find_asset(const std::string& name) const
{
    for (const Asset& a : assets)
        if (a.name == name)
            return &a;
    return nullptr;
}

std::string current_version() { return ORCA_ZOUKEI_VERSION; }

bool is_valid_version(const std::string& v) { return parse_version(v).valid; }

int compare_versions(const std::string& a, const std::string& b)
{
    ParsedVersion pa = parse_version(a), pb = parse_version(b);
    size_t        n = std::max(pa.nums.size(), pb.nums.size());
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = i < pa.nums.size() ? pa.nums[i] : 0, y = i < pb.nums.size() ? pb.nums[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (pa.suffix.empty() != pb.suffix.empty())
        return pa.suffix.empty() ? 1 : -1;
    return pa.suffix.compare(pb.suffix) < 0 ? -1 : (pa.suffix == pb.suffix ? 0 : 1);
}

std::string platform_id()
{
#if defined(_WIN32) && defined(_M_ARM64)
    return "windows-arm64";
#elif defined(_WIN32)
    return "windows-x64";
#else
    return {};
#endif
}

bool self_update_supported() { return !platform_id().empty(); }

bool url_allowed(const std::string& url)
{
    const std::string t = test_url();
    if (!t.empty())
        return boost::starts_with(url, origin_of(t));
    return boost::starts_with(url, GITHUB_API_BASE) || boost::starts_with(url, GITHUB_DOWNLOAD_BASE);
}

bool test_mode() { return !test_url().empty(); }

std::string releases_url()
{
    const std::string t = test_url();
    return t.empty() ? std::string(GITHUB_API_BASE) + "releases?per_page=30" : t;
}

bool is_safe_relpath(const std::string& rel)
{
    if (rel.empty() || rel.size() > 400 || rel.front() == '/' || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos)
        return false;
    for (unsigned char c : rel)
        if (c < 32)
            return false;
    std::vector<std::string> parts;
    boost::split(parts, rel, boost::is_any_of("/"));
    for (const auto& p : parts) {
        if (p.empty() || p == "." || p == "..")
            return false;
        // Windows quirks: trailing dot/space and device names are not valid file names.
        if (p.back() == '.' || p.back() == ' ')
            return false;
    }
    return true;
}

bool is_sha256_hex(const std::string& s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) || (c >= 'a' && c <= 'f'); });
}

CheckResult evaluate_releases(const std::string& body, bool beta, const std::string& installed_version)
{
    CheckResult res;
    json        root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !(root.is_array() || root.is_object())) {
        res.error = "invalid answer from the release server";
        return res;
    }
    std::vector<json> list;
    if (root.is_array())
        list.assign(root.begin(), root.end());
    else
        list.push_back(root);

    const std::string manifest_name = platform_id().empty() ? std::string() : "manifest-" + platform_id() + ".json";
    const ReleaseInfo* best         = nullptr;
    std::vector<ReleaseInfo> found;
    for (const json& r : list) {
        if (!r.is_object())
            continue;
        if (r.value("draft", false) || (r.value("prerelease", false) && !beta))
            continue;
        std::string tag = json_str(r, "tag_name");
        std::string ver = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
        if (!is_valid_version(ver))
            continue;
        ReleaseInfo info;
        info.version    = ver;
        info.tag        = tag;
        info.notes      = json_str(r, "body");
        info.page_url   = json_str(r, "html_url");
        info.prerelease = r.value("prerelease", false);
        auto assets     = r.find("assets");
        if (assets != r.end() && assets->is_array()) {
            for (const json& a : *assets) {
                if (!a.is_object())
                    continue;
                Asset as;
                as.name = json_str(a, "name");
                as.url  = json_str(a, "browser_download_url");
                as.size = json_u64(a, "size");
                if (as.name.empty() || as.name.find('/') != std::string::npos || as.name.find('\\') != std::string::npos || !url_allowed(as.url))
                    continue;
                info.assets.push_back(std::move(as));
            }
        }
        if (!manifest_name.empty() && !info.find_asset(manifest_name))
            continue; // nothing installable for this platform in that release
        found.push_back(std::move(info));
    }
    for (const ReleaseInfo& f : found)
        if (!best || compare_versions(f.version, best->version) > 0)
            best = &f;
    if (!best) {
        res.status = CheckResult::UpToDate;
        return res;
    }
    res.release = *best;
    res.status  = compare_versions(best->version, installed_version) > 0 ? CheckResult::Available : CheckResult::UpToDate;
    return res;
}

CheckResult check(bool beta)
{
    HttpResult r = http_get(releases_url(), 8u * 1024 * 1024, 30);
    if (!r.completed || r.status != 200) {
        CheckResult res;
        res.error = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        if (r.status == 403 || r.status == 429)
            res.error += " (rate limit, try again later)";
        BOOST_LOG_TRIVIAL(warning) << "ZoukeiUpdate::check failed: " << res.error;
        return res;
    }
    CheckResult res = evaluate_releases(r.body, beta, current_version());
    BOOST_LOG_TRIVIAL(info) << "ZoukeiUpdate::check: installed " << current_version() << ", status " << int(res.status)
                            << (res.status == CheckResult::Available ? ", available " + res.release.version : std::string());
    return res;
}

bool parse_manifest(const std::string& body, Manifest& m, std::string& err)
{
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", 0) != 1 || json_str(j, "app") != "OrcaZoukei") {
        err = "invalid manifest";
        return false;
    }
    m.version  = json_str(j, "version");
    m.platform = json_str(j, "platform");
    m.exe      = json_str(j, "exe");
    if (!is_valid_version(m.version) || !is_safe_relpath(m.exe)) {
        err = "invalid manifest (version or executable)";
        return false;
    }
    auto files = j.find("files");
    if (files == j.end() || !files->is_object() || files->empty()) {
        err = "invalid manifest (no files)";
        return false;
    }
    for (auto it = files->begin(); it != files->end(); ++it) {
        const json& v = it.value();
        if (!is_safe_relpath(it.key()) || !v.is_array() || v.size() != 2 || !v[0].is_number_unsigned() || !v[1].is_string() || !is_sha256_hex(v[1].get<std::string>())) {
            err = "invalid manifest entry: " + it.key();
            return false;
        }
        m.files[it.key()] = FileEntry{v[0].get<uint64_t>(), v[1].get<std::string>()};
    }
    if (!m.files.count(m.exe)) {
        err = "invalid manifest (executable not listed)";
        return false;
    }
    auto full = j.find("full");
    if (full == j.end() || !full->is_object()) {
        err = "invalid manifest (no full package)";
        return false;
    }
    m.full_asset  = json_str(*full, "asset");
    m.full_size   = json_u64(*full, "size");
    m.full_sha256 = json_str(*full, "sha256");
    if (m.full_asset.empty() || !is_sha256_hex(m.full_sha256) || m.full_size == 0) {
        err = "invalid manifest (full package)";
        return false;
    }
    auto deltas = j.find("deltas");
    if (deltas != j.end() && deltas->is_array()) {
        for (const json& d : *deltas) {
            if (!d.is_object())
                continue;
            DeltaInfo di;
            di.from   = json_str(d, "from");
            di.asset  = json_str(d, "asset");
            di.size   = json_u64(d, "size");
            di.sha256 = json_str(d, "sha256");
            if (!is_valid_version(di.from) || di.asset.empty() || di.size == 0 || !is_sha256_hex(di.sha256))
                continue;
            bool ok = true;
            auto fl = d.find("files");
            if (fl == d.end() || !fl->is_array())
                continue;
            for (const json& f : *fl) {
                if (!f.is_string() || !is_safe_relpath(f.get<std::string>())) {
                    ok = false;
                    break;
                }
                di.files.insert(f.get<std::string>());
            }
            auto rm = d.find("removed");
            if (ok && rm != d.end() && rm->is_array())
                for (const json& f : *rm)
                    if (f.is_string() && is_safe_relpath(f.get<std::string>()))
                        di.removed.push_back(f.get<std::string>());
            if (ok)
                m.deltas.push_back(std::move(di));
        }
    }
    return true;
}

fs::path install_dir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD   n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    if (n > 0 && n < sizeof(buf) / sizeof(buf[0]))
        return fs::path(std::wstring(buf, n)).parent_path();
#endif
    return fs::current_path();
}

bool install_dir_writable(const fs::path& dir)
{
    boost::system::error_code ec;
    fs::path                  probe = dir / (".zoukei-write-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        fs::ofstream out(probe, std::ios::binary);
        if (!out)
            return false;
        out << "x";
    }
    fs::remove(probe, ec);
    return true;
}

namespace {

// Appends GET ranges of `url` to `out`, hashing on the way. Returns false with err set.
bool download_to_file(const Asset& asset, uint64_t expected_size, const std::string& expected_sha, const fs::path& out_path,
                      const ProgressFn& progress, std::string& err)
{
    fs::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + out_path.string();
        return false;
    }
    Sha256   h;
    uint64_t done = 0;
    while (done < expected_size) {
        uint64_t    last = std::min(done + CHUNK, expected_size) - 1;
        HttpResult  r;
        bool        ok = false;
        for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
            if (attempt > 0)
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            r  = http_get(asset.url, CHUNK + 64 * 1024, 180, std::to_string(done) + "-" + std::to_string(last));
            ok = r.completed && ((r.status == 206 && r.body.size() == last - done + 1) ||
                                 // a server that ignores Range answers 200 with everything: acceptable only for the first chunk when it fits
                                 (r.status == 200 && done == 0 && r.body.size() == expected_size));
        }
        if (!ok) {
            err = "download failed (" + asset.name + "): " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
            return false;
        }
        out.write(r.body.data(), std::streamsize(r.body.size()));
        if (!out) {
            err = "cannot write " + out_path.string() + " (disk full?)";
            return false;
        }
        h.update(r.body.data(), r.body.size());
        done += r.body.size();
        if (progress && !progress("download", done, expected_size)) {
            err = "cancelled";
            return false;
        }
    }
    out.close();
    if (h.hex() != expected_sha) {
        err = "checksum mismatch for " + asset.name;
        return false;
    }
    return true;
}

struct ExtractCtx
{
    fs::ofstream* out;
    Sha256*       sha;
    uint64_t      written{0};
    bool          failed{false};
};

size_t extract_cb(void* opaque, mz_uint64 /*ofs*/, const void* buf, size_t n)
{
    auto* c = static_cast<ExtractCtx*>(opaque);
    c->out->write(static_cast<const char*>(buf), std::streamsize(n));
    if (!*c->out) {
        c->failed = true;
        return 0;
    }
    c->sha->update(buf, n);
    c->written += n;
    return n;
}

bool extract_needed(const fs::path& zip_path, const std::vector<std::string>& needed, const std::map<std::string, FileEntry>& entries,
                    const fs::path& dest, const ProgressFn& progress, std::string& err)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path_utf8(zip_path))) {
        err = "cannot open the downloaded package";
        return false;
    }
    bool     ok    = true;
    uint64_t total = 0, done = 0;
    for (const auto& rel : needed)
        total += entries.at(rel).size;
    for (const auto& rel : needed) {
        const FileEntry& fe  = entries.at(rel);
        int              idx = mz_zip_reader_locate_file(&zip, rel.c_str(), nullptr, 0);
        mz_zip_archive_file_stat st;
        if (idx < 0 || !mz_zip_reader_file_stat(&zip, mz_uint(idx), &st) || st.m_is_directory || st.m_uncomp_size != fe.size) {
            err = "file missing or wrong size in the package: " + rel;
            ok  = false;
            break;
        }
        fs::path target = dest / fs::path(rel);
        boost::system::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        fs::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write " + target.string();
            ok  = false;
            break;
        }
        Sha256     sha;
        ExtractCtx ctx{&out, &sha};
        bool       r = mz_zip_reader_extract_to_callback(&zip, mz_uint(idx), extract_cb, &ctx, 0) != 0;
        out.close();
        if (!r || ctx.failed || ctx.written != fe.size || sha.hex() != fe.sha256) {
            err = "checksum mismatch after extracting " + rel;
            ok  = false;
            break;
        }
        done += fe.size;
        if (progress && !progress("extract", done, total)) {
            err = "cancelled";
            ok  = false;
            break;
        }
    }
    close_zip_reader(&zip);
    return ok;
}

} // namespace

bool prepare(const ReleaseInfo& rel, const fs::path& install, const ProgressFn& progress, const UpdaterTexts& texts, Prepared& out, std::string& err)
{
    if (!self_update_supported()) {
        err = "automatic update is only available on Windows";
        return false;
    }
    const std::string manifest_name = "manifest-" + platform_id() + ".json";
    const Asset*      masset        = rel.find_asset(manifest_name);
    if (!masset) {
        err = "the release has no " + manifest_name;
        return false;
    }
    if (!fs::exists(install / "zoukei-updater.exe")) {
        err = "zoukei-updater.exe is missing from the installation";
        return false;
    }

    // Work area: the whole temp area of the updater is recreated on every attempt.
    boost::system::error_code ec;
    fs::path                  root = fs::temp_directory_path(ec) / "OrcaZoukeiUpdate";
    fs::remove_all(root, ec);
    fs::path work = root / rel.version;
    fs::create_directories(work / "files", ec);
    if (ec) {
        err = "cannot create " + work.string();
        return false;
    }

    // 1. manifest
    HttpResult mr = http_get(masset->url, 64u * 1024 * 1024, 120);
    if (!mr.completed || mr.status != 200) {
        err = "cannot download the manifest: " + (mr.error.empty() ? "HTTP " + std::to_string(mr.status) : mr.error);
        return false;
    }
    Manifest man;
    if (!parse_manifest(mr.body, man, err))
        return false;
    if (man.version != rel.version || man.platform != platform_id()) {
        err = "the manifest does not match the release";
        return false;
    }

    // 2. what differs locally (every file of the manifest is hashed)
    std::vector<std::string> needed;
    {
        uint64_t total = 0, done = 0;
        for (const auto& kv : man.files) {
            boost::system::error_code e2;
            if (fs::is_regular_file(install / fs::path(kv.first), e2) && fs::file_size(install / fs::path(kv.first), e2) == kv.second.size)
                total += kv.second.size;
        }
        for (const auto& kv : man.files) {
            fs::path                  p = install / fs::path(kv.first);
            boost::system::error_code e2;
            bool                      same = false;
            if (fs::is_regular_file(p, e2) && fs::file_size(p, e2) == kv.second.size) {
                std::string hex;
                bool        cancelled = false;
                if (hash_file(p, hex, [&](uint64_t n) {
                        done += n;
                        if (progress && !progress("verify", done, total)) {
                            cancelled = true;
                            return false;
                        }
                        return true;
                    }))
                    same = (hex == kv.second.sha256);
                else if (cancelled) {
                    err = "cancelled";
                    return false;
                }
            }
            if (!same)
                needed.push_back(kv.first);
        }
    }

    // 3. delta or full
    const DeltaInfo* delta = nullptr;
    for (const DeltaInfo& d : man.deltas) {
        if (d.from != current_version() || !rel.find_asset(d.asset) || d.size >= man.full_size)
            continue;
        if (std::all_of(needed.begin(), needed.end(), [&](const std::string& f) { return d.files.count(f) != 0; }))
            if (!delta || d.size < delta->size)
                delta = &d;
    }
    const std::string zip_name = delta ? delta->asset : man.full_asset;
    const uint64_t    zip_size = delta ? delta->size : man.full_size;
    const std::string zip_sha  = delta ? delta->sha256 : man.full_sha256;
    const Asset*      zasset   = rel.find_asset(zip_name);
    uint64_t          need_bytes = 0;
    for (const auto& f : needed)
        need_bytes += man.files.at(f).size;
    if (!needed.empty()) {
        if (!zasset) {
            err = "the release has no " + zip_name;
            return false;
        }
        if (zasset->size != 0 && zasset->size != zip_size) {
            err = "size of " + zip_name + " differs from the manifest";
            return false;
        }
        fs::space_info sp = fs::space(work, ec);
        if (!ec && sp.available < zip_size + need_bytes + 64u * 1024 * 1024) {
            err = "not enough free disk space in the temp folder";
            return false;
        }
    }

    // 4. download + extract into the staging dir (every file verified)
    fs::path zip_path = work / "package.zip";
    if (!needed.empty()) {
        if (!download_to_file(*zasset, zip_size, zip_sha, zip_path, progress, err))
            return false;
        if (!extract_needed(zip_path, needed, man.files, work / "files", progress, err))
            return false;
        fs::remove(zip_path, ec);
    }

    // 5. removals: files of the installed version that the new one no longer has
    std::set<std::string> removals;
    {
        std::string old_text;
        Manifest    old;
        std::string e3;
        if (read_text_file(install / INSTALLED_MANIFEST, old_text) && parse_manifest(old_text, old, e3))
            for (const auto& kv : old.files)
                if (!man.files.count(kv.first))
                    removals.insert(kv.first);
        if (delta)
            for (const auto& f : delta->removed)
                if (!man.files.count(f))
                    removals.insert(f);
    }

    // the new manifest becomes the record of what is installed
    {
        fs::ofstream mo(work / "files" / INSTALLED_MANIFEST, std::ios::binary | std::ios::trunc);
        mo.write(mr.body.data(), std::streamsize(mr.body.size()));
        if (!mo) {
            err = "cannot write the staged manifest";
            return false;
        }
    }
    std::string mhex;
    hash_file(work / "files" / INSTALLED_MANIFEST, mhex);

    // 6. plan for the updater
    fs::path plan = work / "plan.txt";
    {
        fs::ofstream pf(plan, std::ios::binary | std::ios::trunc);
        const bool    relaunch = test_url().empty();
        pf << "zoukei-plan\t1\n";
        pf << "version\t" << man.version << "\n";
#ifdef _WIN32
        pf << "pid\t" << GetCurrentProcessId() << "\n";
#endif
        pf << "install\t" << path_utf8(install) << "\n";
        pf << "stage\t" << path_utf8(work / "files") << "\n";
        pf << "backup\t" << path_utf8(work / "backup") << "\n";
        pf << "log\t" << path_utf8(work / "update.log") << "\n";
        pf << "exe\t" << man.exe << "\n";
        pf << "relaunch\t" << (relaunch ? 1 : 0) << "\n";
        pf << "msg_title\t" << one_line(texts.title) << "\n";
        pf << "msg_failed\t" << one_line(texts.failed) << "\n";
        for (const auto& f : needed)
            pf << "put\t" << f << "\t" << man.files.at(f).size << "\t" << man.files.at(f).sha256 << "\n";
        pf << "put\t" << INSTALLED_MANIFEST << "\t" << fs::file_size(work / "files" / INSTALLED_MANIFEST) << "\t" << mhex << "\n";
        for (const auto& f : removals)
            pf << "del\t" << f << "\n";
        pf.flush();
        if (!pf) {
            err = "cannot write the update plan";
            return false;
        }
    }

    out.version        = man.version;
    out.work_dir       = work;
    out.plan_path      = plan;
    out.used_delta     = delta != nullptr;
    out.download_bytes = needed.empty() ? 0 : zip_size;
    out.files_to_copy  = needed.size();
    BOOST_LOG_TRIVIAL(info) << "ZoukeiUpdate::prepare: " << man.version << " staged, " << needed.size() << " file(s), "
                            << (delta ? "delta from " + delta->from : std::string("full package")) << ", " << out.download_bytes << " bytes downloaded";
    return true;
}

bool launch_updater(const Prepared& prepared, std::string& err)
{
#ifdef _WIN32
    boost::system::error_code ec;
    fs::path                  src = install_dir() / "zoukei-updater.exe";
    fs::path                  dst = prepared.work_dir / "zoukei-updater.exe";
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "cannot copy the updater: " + ec.message();
        return false;
    }
    std::wstring cmd = L"\"" + dst.wstring() + L"\" \"" + prepared.plan_path.wstring() + L"\"";
    std::wstring cwd = prepared.work_dir.wstring();
    for (DWORD flags : {DWORD(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB), DWORD(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP)}) {
        STARTUPINFOW        si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        if (CreateProcessW(dst.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, cwd.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
    }
    err = "cannot start the updater (error " + std::to_string(GetLastError()) + ")";
    return false;
#else
    (void) prepared;
    err = "automatic update is only available on Windows";
    return false;
#endif
}

} // namespace ZoukeiUpdate
} // namespace Slic3r
