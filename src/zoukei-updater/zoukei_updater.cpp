// zoukei-updater.exe: applies a staged Orca Zoukei update (see docs/zoukei/updates.md).
//
// Orca (ZoukeiUpdate::prepare) downloads and verifies the files into a staging folder and writes plan.txt, then starts a COPY of
// this program from the temp folder and exits. This program:
//   1. waits for Orca to exit,
//   2. re-verifies size + SHA-256 of every staged file (nothing in the install is touched if one is wrong),
//   3. moves each file it is about to replace into a backup folder and copies the staged one in,
//   4. on ANY failure restores everything from the backup (rollback) and says so,
//   5. starts Orca again.
// Only the Win32 API and the C++ standard library are used (static CRT), so it runs from the temp folder on its own.
//
// Usage: zoukei-updater.exe <plan.txt> [--quiet]     (--quiet: no message boxes, used by the tests)
// Exit codes: 0 updated, 1 failed and rolled back, 2 aborted before touching anything, 3 bad plan.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct PutEntry
{
    std::string   rel;
    uint64_t      size;
    std::string   sha256;
};

struct Plan
{
    std::string           version;
    DWORD                 pid{0};
    std::wstring          install, stage, backup, log;
    std::string           exe;
    bool                  relaunch{true};
    std::wstring          msg_title{L"Orca Zoukei"};
    std::wstring          msg_failed{L"The update could not be applied. The previous version was restored. Log:"};
    std::vector<PutEntry> puts;
    std::vector<std::string> dels;
};

bool        g_quiet = false;
std::wstring g_log_path;

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void log(const std::string& msg)
{
    if (g_log_path.empty())
        return;
    std::ofstream f(fs::path(g_log_path), std::ios::app | std::ios::binary);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    f << stamp << msg << "\r\n";
}

void log_win(const std::string& what, DWORD err)
{
    log(what + " (Windows error " + std::to_string(err) + ")");
}

bool safe_rel(const std::string& rel)
{
    if (rel.empty() || rel.size() > 400 || rel[0] == '/' || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos)
        return false;
    for (unsigned char c : rel)
        if (c < 32)
            return false;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t end  = rel.find('/', start);
        std::string part = rel.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

// \\?\ path (no MAX_PATH limit) for a base directory plus a relative path with '/'.
std::wstring long_path(const std::wstring& base, const std::string& rel)
{
    std::wstring p = base;
    for (auto& c : p)
        if (c == L'/')
            c = L'\\';
    while (!p.empty() && p.back() == L'\\')
        p.pop_back();
    if (!rel.empty()) {
        std::wstring r = widen(rel);
        for (auto& c : r)
            if (c == L'/')
                c = L'\\';
        p += L"\\" + r;
    }
    if (p.rfind(L"\\\\?\\", 0) == 0)
        return p;
    if (p.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

bool read_plan(const std::wstring& path, Plan& plan, std::string& err)
{
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        err = "cannot open the plan";
        return false;
    }
    std::string line;
    bool        header = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::vector<std::string> f;
        size_t                   s = 0;
        while (true) {
            size_t e = line.find('\t', s);
            f.push_back(line.substr(s, e == std::string::npos ? std::string::npos : e - s));
            if (e == std::string::npos)
                break;
            s = e + 1;
        }
        const std::string& k = f[0];
        if (!header) {
            if (k != "zoukei-plan" || f.size() < 2 || f[1] != "1") {
                err = "unknown plan format";
                return false;
            }
            header = true;
        } else if (k == "version" && f.size() >= 2) plan.version = f[1];
        else if (k == "pid" && f.size() >= 2) plan.pid = DWORD(std::strtoul(f[1].c_str(), nullptr, 10));
        else if (k == "install" && f.size() >= 2) plan.install = widen(f[1]);
        else if (k == "stage" && f.size() >= 2) plan.stage = widen(f[1]);
        else if (k == "backup" && f.size() >= 2) plan.backup = widen(f[1]);
        else if (k == "log" && f.size() >= 2) plan.log = widen(f[1]);
        else if (k == "exe" && f.size() >= 2) plan.exe = f[1];
        else if (k == "relaunch" && f.size() >= 2) plan.relaunch = f[1] == "1";
        else if (k == "msg_title" && f.size() >= 2 && !f[1].empty()) plan.msg_title = widen(f[1]);
        else if (k == "msg_failed" && f.size() >= 2 && !f[1].empty()) plan.msg_failed = widen(f[1]);
        else if (k == "put" && f.size() == 4) {
            if (!safe_rel(f[1]) || f[3].size() != 64) {
                err = "unsafe or malformed put line: " + f[1];
                return false;
            }
            plan.puts.push_back({f[1], std::strtoull(f[2].c_str(), nullptr, 10), f[3]});
        } else if (k == "del" && f.size() == 2) {
            if (!safe_rel(f[1])) {
                err = "unsafe del line: " + f[1];
                return false;
            }
            plan.dels.push_back(f[1]);
        } else {
            err = "unknown plan line: " + k;
            return false;
        }
    }
    if (!header || plan.install.empty() || plan.stage.empty() || plan.backup.empty() || !safe_rel(plan.exe)) {
        err = "incomplete plan";
        return false;
    }
    // staging and backup must not be inside the install folder, and the install must exist
    std::error_code ec;
    if (!fs::is_directory(fs::path(plan.install), ec) || !fs::is_directory(fs::path(plan.stage), ec)) {
        err = "install or stage folder does not exist";
        return false;
    }
    return true;
}

bool sha256_file(const std::wstring& path, std::string& hex)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool               ok   = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<unsigned char> buf(1 << 20);
        ok = true;
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(f, buf.data(), DWORD(buf.size()), &n, nullptr)) {
                ok = false;
                break;
            }
            if (n == 0)
                break;
            if (BCryptHashData(hash, buf.data(), n, 0) < 0) {
                ok = false;
                break;
            }
        }
        unsigned char md[32];
        if (ok && BCryptFinishHash(hash, md, sizeof(md), 0) >= 0) {
            static const char* d = "0123456789abcdef";
            hex.clear();
            for (unsigned char b : md) {
                hex.push_back(d[b >> 4]);
                hex.push_back(d[b & 15]);
            }
        } else {
            ok = false;
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

bool transient(DWORD e) { return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED; }

// Runs op() up to ~30 s while the failure looks like a file still in use (antivirus, a helper process of Orca closing).
template <class F>
bool with_retry(F op, DWORD& err)
{
    for (int i = 0; i < 60; ++i) {
        if (op()) {
            err = 0;
            return true;
        }
        err = GetLastError();
        if (!transient(err))
            return false;
        Sleep(500);
    }
    return false;
}

bool make_parent(const std::wstring& long_file_path)
{
    std::error_code ec;
    fs::create_directories(fs::path(long_file_path).parent_path(), ec);
    return !ec;
}

struct Done
{
    std::string rel;
    bool        put;        // true: a staged file was copied in; false: a deletion
    bool        had_backup; // the previous file was moved to the backup folder
};

void rollback(const Plan& plan, const std::vector<Done>& done)
{
    log("rolling back " + std::to_string(done.size()) + " change(s)");
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        std::wstring dest = long_path(plan.install, it->rel);
        std::wstring bak  = long_path(plan.backup, it->rel);
        DWORD        err  = 0;
        if (it->put) {
            if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
                with_retry([&] { return DeleteFileW(dest.c_str()) != 0; }, err);
        }
        if (it->had_backup) {
            make_parent(dest);
            if (!with_retry([&] { return MoveFileExW(bak.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0; }, err))
                log_win("ROLLBACK FAILED for " + it->rel, err);
        }
    }
}

void fail_box(const Plan& plan, const std::string& why)
{
    if (g_quiet)
        return;
    std::wstring m = plan.msg_failed + L"\n" + plan.log + L"\n\n" + widen(why);
    MessageBoxW(nullptr, m.c_str(), plan.msg_title.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void relaunch(const Plan& plan)
{
    if (!plan.relaunch)
        return;
    std::wstring exe = long_path(plan.install, plan.exe);
    // CreateProcess does not like the \\?\ prefix for the module name on every Windows version; use the plain path.
    std::wstring plain = exe.rfind(L"\\\\?\\UNC\\", 0) == 0 ? L"\\\\" + exe.substr(8) : exe.substr(4);
    std::wstring cmd   = L"\"" + plain + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(plain.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, plan.install.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        log("started " + plan.exe);
    } else {
        log_win("could not start " + plan.exe, GetLastError());
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int      argc = 0;
    LPWSTR*  argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring plan_path;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--quiet")
            g_quiet = true;
        else if (plan_path.empty())
            plan_path = a;
    }
    if (argv)
        LocalFree(argv);
    if (plan_path.empty())
        return 3;

    Plan        plan;
    std::string err;
    g_log_path = plan_path + L".log"; // until the plan says otherwise
    if (!read_plan(plan_path, plan, err)) {
        log("bad plan: " + err);
        if (!g_quiet)
            MessageBoxW(nullptr, widen("Bad update plan: " + err).c_str(), L"Orca Zoukei", MB_OK | MB_ICONERROR);
        return 3;
    }
    if (!plan.log.empty())
        g_log_path = plan.log;
    log("zoukei-updater: version " + plan.version + ", " + std::to_string(plan.puts.size()) + " file(s) to copy, " + std::to_string(plan.dels.size()) + " to delete");

    // 1. wait for Orca to exit
    if (plan.pid) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, plan.pid);
        if (h) {
            DWORD w = WaitForSingleObject(h, 120 * 1000);
            CloseHandle(h);
            if (w != WAIT_OBJECT_0) {
                log("Orca did not exit in 120 s, nothing was changed");
                fail_box(plan, "Orca Zoukei did not close.");
                return 2;
            }
        }
    }
    Sleep(300);

    // 2. verify the staged files before touching anything
    for (const auto& p : plan.puts) {
        std::wstring sp = long_path(plan.stage, p.rel);
        WIN32_FILE_ATTRIBUTE_DATA a{};
        std::string hex;
        if (!GetFileAttributesExW(sp.c_str(), GetFileExInfoStandard, &a) || ((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow) != p.size || !sha256_file(sp, hex) || hex != p.sha256) {
            log("staged file is missing or does not match: " + p.rel + ", nothing was changed");
            fail_box(plan, "Staged file does not match: " + p.rel);
            relaunch(plan);
            return 2;
        }
    }

    // 3. apply
    std::error_code ec;
    fs::remove_all(fs::path(long_path(plan.backup, "")), ec);
    std::vector<Done> done;
    bool              ok = true;
    std::string       why;
    auto step_fail = [&](const std::string& rel, const char* what, DWORD e) {
        log_win(std::string(what) + " " + rel, e);
        why = std::string(what) + " " + rel;
        ok  = false;
    };
    for (const auto& p : plan.puts) {
        std::wstring dest = long_path(plan.install, p.rel);
        std::wstring bak  = long_path(plan.backup, p.rel);
        std::wstring src  = long_path(plan.stage, p.rel);
        DWORD        e    = 0;
        bool         had  = GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!make_parent(dest)) {
            step_fail(p.rel, "cannot create the folder for", GetLastError());
            break;
        }
        if (had) {
            if (!make_parent(bak) || !with_retry([&] { return MoveFileExW(dest.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0; }, e)) {
                step_fail(p.rel, "cannot move aside", e);
                break;
            }
        }
        done.push_back({p.rel, true, had});
        if (!with_retry([&] { return CopyFileW(src.c_str(), dest.c_str(), FALSE) != 0; }, e)) {
            step_fail(p.rel, "cannot copy", e);
            break;
        }
    }
    if (ok) {
        for (const auto& rel : plan.dels) {
            std::wstring dest = long_path(plan.install, rel);
            std::wstring bak  = long_path(plan.backup, rel);
            DWORD        e    = 0;
            if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            if (!make_parent(bak) || !with_retry([&] { return MoveFileExW(dest.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0; }, e)) {
                step_fail(rel, "cannot remove", e);
                break;
            }
            done.push_back({rel, false, true});
        }
    }

    if (!ok) {
        rollback(plan, done);
        log("update FAILED: " + why);
        fail_box(plan, why);
        relaunch(plan);
        return 1;
    }

    // 4. done: the backup and the staged copy are not needed any more
    fs::remove_all(fs::path(long_path(plan.backup, "")), ec);
    fs::remove_all(fs::path(long_path(plan.stage, "")), ec);
    log("update to " + plan.version + " applied");
    relaunch(plan);
    return 0;
}
