// src/slic3r/Utils/CrealityCloudClient.cpp
//
// Orca Zoukei: Creality Cloud client. Protocol ported from Creality Print (AGPL-3.0,
// github.com/CrealityOfficial/CrealityPrint): HttpServer.cpp (oauthLogin), CommunicateWithCXCloud.cpp (profile
// upload), GUI_App.cpp get_extra_header (headers), GUI.cpp get_cloud_api_url, resources/web/homepage (login page).
// See CrealityCloudClient.hpp for the details.
#include "CrealityCloudClient.hpp"

#include "Http.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <wx/datetime.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <vector>

namespace fs = boost::filesystem;

namespace Slic3r { namespace CrealityCloud {

using json = nlohmann::json;

const char* const CALLBACK_URL = "http://localhost:13666/login";

namespace {

// Public OAuth client ids and app id of Creality Print (resources/web/homepage, HttpServer.cpp).
constexpr const char* CLIENT_ID_GLOBAL = "f9c302ecc29c59a0a6e921ff39a073ca";
constexpr const char* CLIENT_ID_CHINA  = "8ea5010984fa52a298f12110af8b05d0";
constexpr const char* APP_ID           = "creality_model";
constexpr const char* PLATFORM_API     = "11"; // __CXY_PLATFORM_ of the slicer
constexpr const char* PLATFORM_LOGIN   = "2";  // platform of the login page (PC web)

std::string lower(std::string s)
{
    boost::algorithm::to_lower(s);
    return s;
}

bool is_loopback_http(const std::string& url)
{
    const std::string u = lower(url);
    for (const char* prefix : {"http://127.0.0.1:", "http://localhost:", "http://[::1]:"})
        if (boost::starts_with(u, prefix))
            return true;
    return false;
}

std::string new_request_id()
{
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

// The __CXY_* headers of Creality Print's get_extra_header(). Token and user id only when a session is given.
void add_headers(Http& http, const ClientInfo& info, const Session* session)
{
    http.header("Content-Type", "application/json")
        .header("__CXY_APP_ID_", APP_ID)
        .header("__CXY_PLATFORM_", PLATFORM_API)
        .header("__CXY_BRAND_", "creality")
        .header("__CXY_APP_CH_", "creality")
        .header("__CXY_OS_LANG_", os_lang_index(info.lang))
        .header("__CXY_DUID_", info.duid)
        .header("__CXY_APP_VER_", info.app_version)
        .header("__CXY_TIMEZONE_", std::to_string(wxDateTime::TimeZone(wxDateTime::Local).GetOffset()))
        .header("__CXY_REQUESTID_", new_request_id());
#if defined(_WIN32)
    http.header("__CXY_OS_VER_", "win");
#elif defined(__APPLE__)
    http.header("__CXY_OS_VER_", "macos");
#else
    http.header("__CXY_OS_VER_", "linux");
#endif
    if (session != nullptr) {
        http.header("__CXY_UID_", session->user_id);
        http.header("__CXY_TOKEN_", session->token);
    }
}

// Seconds of the last Retry-After header in a raw header block (0 when absent or not a number of seconds).
int parse_retry_after(const std::string& headers)
{
    int        out = 0;
    const auto lowered = lower(headers);
    size_t     pos = 0;
    while ((pos = lowered.find("retry-after:", pos)) != std::string::npos) {
        pos += 12;
        while (pos < lowered.size() && lowered[pos] == ' ')
            ++pos;
        int v = 0;
        bool any = false;
        while (pos < lowered.size() && lowered[pos] >= '0' && lowered[pos] <= '9' && v < 100000) {
            v = v * 10 + (lowered[pos++] - '0');
            any = true;
        }
        if (any)
            out = v;
    }
    return out;
}

// POST <api><path> with a JSON body; parses {"code", "msg", "result"}.
CallResult post_json(const std::string& api, const std::string& path, const json& body, const ClientInfo& info, const Session* session,
                     json& result_out)
{
    CallResult  r;
    std::string raw_headers;
    Http        http = Http::post(api + path);
    add_headers(http, info, session);
    http.on_header_callback([&raw_headers](std::string h) { raw_headers = std::move(h); })
        .timeout_connect(10)
        .timeout_max(30)
        .set_post_body(body.dump())
        .on_complete([&](std::string resp, unsigned status) {
            r.http_status = status;
            json j        = json::parse(resp, nullptr, false);
            if (!j.is_object() || !j.contains("code") || !j["code"].is_number_integer()) {
                r.message = "unexpected answer from the Creality Cloud (HTTP " + std::to_string(status) + ")";
                return;
            }
            r.cloud_code = j["code"].get<int>();
            if (r.cloud_code != 0) {
                const std::string msg = j.contains("msg") && j["msg"].is_string() ? j["msg"].get<std::string>() : std::string();
                r.message = r.cloud_code == 4 ? std::string("the Creality Cloud login has expired; log in again") :
                                                "Creality Cloud error " + std::to_string(r.cloud_code) + (msg.empty() ? "" : ": " + msg);
                return;
            }
            result_out = j.contains("result") ? j["result"] : json::object();
            r.ok       = true;
        })
        .on_error([&](std::string resp, std::string error, unsigned status) {
            r.http_status = status;
            r.message     = error.empty() ? "HTTP " + std::to_string(status) : error;
            // Some API errors come with HTTP >= 400 and the usual JSON body.
            json j = json::parse(resp, nullptr, false);
            if (j.is_object() && j.contains("code") && j["code"].is_number_integer()) {
                r.cloud_code = j["code"].get<int>();
                if (r.cloud_code == 4)
                    r.message = "the Creality Cloud login has expired; log in again";
            }
        })
        .perform_sync();
    if (!r.ok) {
        if (r.rate_limited()) {
            r.retry_after_s = parse_retry_after(raw_headers);
            if (r.http_status == 429 && r.cloud_code != 1088)
                r.message = "the Creality Cloud asks to slow down (HTTP 429)";
        }
        BOOST_LOG_TRIVIAL(warning) << "Creality Cloud: " << path << " failed: " << r.message;
    }
    return r;
}

std::string json_str(const json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key))
        return std::string();
    const json& v = j[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    return std::string();
}

// The OSS form upload of the profile file (CommunicateWithCXCloud::preUpdateProfile_create/_update).
CallResult post_to_oss(const json& policy, const ProfileUpload& upload, bool mock)
{
    CallResult r;
    const std::string host = json_str(policy, "Host");
    const bool host_ok = boost::istarts_with(host, "https://") || (mock && is_loopback_http(host));
    if (host.empty() || !host_ok) {
        r.message = "the Creality Cloud returned an unusable upload address";
        return r;
    }
    // The form needs a file on disk; a private temporary copy, removed right after.
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path("orca-crcloud-%%%%-%%%%-%%%%.json");
    {
        boost::nowide::ofstream f(tmp.string(), std::ios::binary | std::ios::trunc);
        f << upload.content;
        if (!f) {
            r.message = "could not write a temporary file for the upload";
            return r;
        }
    }
    std::string raw_headers;
    Http        http = Http::post(host);
    http.headers_reset(); // only the form goes to the storage host, never the __CXY_* headers
    http.on_header_callback([&raw_headers](std::string h) { raw_headers = std::move(h); });
    http.form_add("OSSAccessKeyId", json_str(policy, "OSSAccessKeyId"))
        .form_add("Signature", json_str(policy, "Signature"))
        .form_add("Policy", json_str(policy, "Policy"))
        .form_add("Key", json_str(policy, "Key"))
        .form_add("Callback", json_str(policy, "Callback"))
        .form_add_file("File", tmp, upload.name)
        .timeout_connect(10)
        .timeout_max(60)
        .on_complete([&](std::string, unsigned status) {
            r.http_status = status;
            r.cloud_code  = 0;
            r.ok          = true;
        })
        .on_error([&](std::string, std::string error, unsigned status) {
            r.http_status = status;
            r.message     = "upload of the file failed: " + (error.empty() ? "HTTP " + std::to_string(status) : error);
        })
        .perform_sync();
    if (!r.ok && r.http_status == 429)
        r.retry_after_s = parse_retry_after(raw_headers);
    boost::system::error_code ec;
    fs::remove(tmp, ec);
    return r;
}

} // namespace

std::string region_key(Region region) { return region == Region::China ? "china" : "global"; }

Region region_from_key(const std::string& key) { return lower(key) == "china" ? Region::China : Region::Global; }

std::string mock_base()
{
    const char* env = std::getenv("ORCA_ZOUKEI_CREALITY_CLOUD_MOCK");
    if (env == nullptr)
        return std::string();
    std::string base = boost::trim_copy(std::string(env));
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (base.empty())
        return std::string();
    if (!is_loopback_http(base + "/")) {
        BOOST_LOG_TRIVIAL(warning) << "Creality Cloud: ORCA_ZOUKEI_CREALITY_CLOUD_MOCK ignored (only http://127.0.0.1:PORT or http://localhost:PORT)";
        return std::string();
    }
    return base;
}

Endpoints endpoints(Region region)
{
    Endpoints ep;
    ep.client_id = region == Region::China ? CLIENT_ID_CHINA : CLIENT_ID_GLOBAL;
    const std::string mock = mock_base();
    if (!mock.empty()) {
        ep.api            = mock;
        ep.account_center = mock + "/id";
        ep.website        = mock;
        ep.mock           = true;
        return ep;
    }
    if (region == Region::China) {
        ep.api            = "https://api.crealitycloud.cn";
        ep.account_center = "https://id.creality.cn";
        ep.website        = "https://www.crealitycloud.cn";
    } else {
        ep.api            = "https://api.crealitycloud.com";
        ep.account_center = "https://id.creality.com";
        ep.website        = "https://www.crealitycloud.com";
    }
    return ep;
}

std::string redirect_uri(const Endpoints& ep) { return ep.website + "/oauth?back_url=" + CALLBACK_URL; }

std::string login_page_url(const Endpoints& ep, const std::string& orca_lang)
{
    return ep.account_center + "?lang=" + account_center_lang(orca_lang) + "&client_id=" + ep.client_id + "&app_id=" + APP_ID +
           "&redirect_uri=" + Http::url_encode(redirect_uri(ep)) + "&platform=" + PLATFORM_LOGIN;
}

bool is_callback_url(const std::string& url)
{
    const std::string u = lower(url);
    for (const char* host : {"http://localhost:13666/login", "http://127.0.0.1:13666/login"})
        if (boost::starts_with(u, host)) {
            const char next = u.size() > std::string(host).size() ? u[std::string(host).size()] : '\0';
            return next == '\0' || next == '?' || next == '/' || next == '#';
        }
    return false;
}

std::string callback_code(const std::string& url)
{
    if (!is_callback_url(url))
        return std::string();
    const size_t q = url.find('?');
    if (q == std::string::npos)
        return std::string();
    std::string query = url.substr(q + 1);
    const size_t hash = query.find('#');
    if (hash != std::string::npos)
        query.erase(hash);
    std::vector<std::string> pairs;
    boost::split(pairs, query, boost::is_any_of("&"));
    for (const std::string& p : pairs) {
        const size_t eq = p.find('=');
        if (eq != std::string::npos && p.substr(0, eq) == "code")
            return Http::url_decode(p.substr(eq + 1));
    }
    return std::string();
}

std::string account_center_lang(const std::string& orca_lang)
{
    // cp_account_center of Creality Print's homepage (pt_BR is shown in English there).
    static const std::map<std::string, std::string> map = {
        {"ca", "ca"},       {"cs", "cs"},       {"de", "de-DE"},    {"en", "en-US"},    {"es", "es-ES"},    {"fr", "fr-FR"},
        {"hu", "hu"},       {"it", "it-IT"},    {"ja", "ja"},       {"ko", "ko-KR"},    {"nl", "nl-NL"},    {"pl", "pl-PL"},
        {"ru", "ru-RU"},    {"sv", "sv"},       {"tr", "tr"},       {"uk", "uk"},       {"zh_cn", "zh-CN"}, {"zh_tw", "zh-TW"},
    };
    std::string l = lower(orca_lang);
    std::replace(l.begin(), l.end(), '-', '_');
    if (auto it = map.find(l); it != map.end())
        return it->second;
    if (auto it = map.find(l.substr(0, 2)); it != map.end() && l.substr(0, 2) != "zh")
        return it->second;
    return "en-US";
}

std::string os_lang_index(const std::string& orca_lang)
{
    // I18nToLangaugeIndex of Creality Print's GUI_App.cpp.
    static const std::map<std::string, std::string> map = {
        {"en", "0"},     {"zh_cn", "1"}, {"zh_tw", "2"}, {"zh_hk", "3"}, {"ru", "4"},  {"ko", "5"},  {"es", "7"},  {"ca", "7"},
        {"de", "8"},     {"fr", "9"},    {"ja", "10"},   {"pt", "11"},   {"th", "12"}, {"nl", "13"}, {"it", "14"}, {"pt_br", "15"},
        {"tr", "16"},    {"ro", "17"},   {"he", "18"},   {"pl", "19"},   {"id", "20"}, {"hu", "21"}, {"uk", "22"}, {"vi", "23"},
    };
    std::string l = lower(orca_lang);
    std::replace(l.begin(), l.end(), '-', '_');
    if (auto it = map.find(l); it != map.end())
        return it->second;
    if (l.size() >= 2 && l.substr(0, 2) != "zh")
        if (auto it = map.find(l.substr(0, 2)); it != map.end())
            return it->second;
    if (l == "zh")
        return "1";
    return "0";
}

std::string md5_hex(const std::string& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &len, EVP_md5(), nullptr) != 1)
        return std::string();
    std::string out;
    char        buf[3];
    for (unsigned int i = 0; i < len; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", digest[i]);
        out += buf;
    }
    return out;
}

CallResult exchange_login_code(Region region, const ClientInfo& info, const std::string& code, Session& out)
{
    const Endpoints ep = endpoints(region);
    json            body;
    body["code"]        = code;
    body["clientId"]    = ep.client_id;
    body["redirecturi"] = redirect_uri(ep);
    json       result;
    CallResult r = post_json(ep.api, "/api/cxy/account/v2/oauthLogin", body, info, nullptr, result);
    if (!r.ok)
        return r;
    Session s;
    s.token   = json_str(result, "token");
    s.user_id = json_str(result, "userId");
    if (result.contains("user_info") && result["user_info"].is_object())
        s.nickname = json_str(result["user_info"], "nickName");
    s.region = region;
    if (!s.valid()) {
        r.ok      = false;
        r.message = "the Creality Cloud answer has no token or user id";
        return r;
    }
    out = s;
    return r;
}

CallResult check_session(const Session& session, const ClientInfo& info)
{
    if (!session.valid()) {
        CallResult r;
        r.cloud_code = 4;
        r.message    = "not logged in to the Creality Cloud";
        return r;
    }
    json result;
    return post_json(endpoints(session.region).api, "/api/cxy/v2/slice/profile/user/list", json{{"version", ""}}, info, &session, result);
}

ProfileUploadResult upload_filament_profile(const Session& session, const ClientInfo& info, const ProfileUpload& upload)
{
    ProfileUploadResult out;
    if (!session.valid()) {
        out.cloud_code = 4;
        out.message    = "not logged in to the Creality Cloud";
        return out;
    }
    const Endpoints ep = endpoints(session.region);
    json            result;
    CallResult      r;
    bool            created = false;
    if (!upload.setting_id.empty()) {
        r = post_json(ep.api, "/api/cxy/v2/slice/profile/user/editProfile", json{{"id", upload.setting_id}}, info, &session, result);
        if (!r.ok && r.cloud_code == 517)
            BOOST_LOG_TRIVIAL(info) << "Creality Cloud: profile was removed from the cloud, creating it again";
    }
    if (upload.setting_id.empty() || (!r.ok && r.cloud_code == 517)) {
        result = json();
        r      = post_json(ep.api, "/api/cxy/v2/slice/profile/user/preUploadProfile", json{{"md5", md5_hex(upload.content)}, {"type", "materia"}},
                           info, &session, result);
        created = true;
    }
    static_cast<CallResult&>(out) = r;
    if (!r.ok)
        return out;
    const std::string id = json_str(result, "id");
    if (id.empty() || !result.contains("uploadPolicy") || !result["uploadPolicy"].is_object()) {
        out.ok      = false;
        out.message = "the Creality Cloud answer has no profile id or upload policy";
        return out;
    }
    const CallResult oss = post_to_oss(result["uploadPolicy"], upload, ep.mock);
    if (!oss.ok) {
        static_cast<CallResult&>(out) = oss;
        out.setting_id                = id; // kept: the next upload re-sends the file through editProfile
        out.created                   = created;
        return out;
    }
    out.ok         = true;
    out.cloud_code = 0;
    out.setting_id = id;
    out.created    = created;
    return out;
}

}} // namespace Slic3r::CrealityCloud
