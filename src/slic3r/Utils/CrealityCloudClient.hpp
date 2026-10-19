// src/slic3r/Utils/CrealityCloudClient.hpp
//
// Orca Zoukei: Creality Cloud client - login code exchange and upload of the user's filament profiles, so that a
// Creality printer bound to the same account (K2 / K2 Plus with CFS) lists them among its materials.
//
// The protocol is ported from Creality Print (AGPL-3.0, github.com/CrealityOfficial/CrealityPrint):
//   - src/slic3r/GUI/HttpServer.cpp            "/login" callback: POST /api/cxy/account/v2/oauthLogin {code, clientId, redirecturi}
//   - resources/web/homepage (index-*.js)      account centre address, public OAuth client ids, app id "creality_model",
//                                              platform 2 (PC web) of the login page, back_url http://localhost:13666/login
//   - src/slic3r/GUI/CommunicateWithCXCloud.cpp POST /api/cxy/v2/slice/profile/user/preUploadProfile {md5, type},
//                                              /editProfile {id}, /list {version}; then the preset file goes to the
//                                              returned OSS upload policy (form POST, field "File")
//   - src/slic3r/GUI/GUI_App.cpp get_extra_header  the __CXY_* request headers
//   - src/slic3r/GUI/GUI.cpp get_cloud_api_url    api.crealitycloud.com / api.crealitycloud.cn
// No private key or signature is involved: the client ids are public identifiers of the OAuth client and the token
// comes from the user's own login on the official Creality account page.
//
// The token is never logged, never put in a URL and never sent anywhere but the Creality API host.
// Network calls are blocking (perform_sync): call them from a worker thread.
#ifndef slic3r_Utils_CrealityCloudClient_hpp_
#define slic3r_Utils_CrealityCloudClient_hpp_

#include <string>

namespace Slic3r { namespace CrealityCloud {

enum class Region { Global, China };

std::string region_key(Region region);                // "global" | "china"
Region      region_from_key(const std::string& key); // anything but "china" -> Global

struct Endpoints
{
    std::string api;            // https://api.crealitycloud.com
    std::string account_center; // https://id.creality.com (the official login page)
    std::string website;        // https://www.crealitycloud.com (its /oauth page hands the code to back_url)
    std::string client_id;      // public OAuth client id of Creality Print for this region
    bool        mock = false;
};

// Environment variable ORCA_ZOUKEI_CREALITY_CLOUD_MOCK=http://127.0.0.1:PORT points every endpoint (API, login page,
// OSS) at a local mock (tools/zoukei/mock_creality_cloud.py). Only loopback http addresses are accepted; anything
// else is ignored. Returns "" when the mock is not configured.
std::string mock_base();
Endpoints   endpoints(Region region);

// The login page redirects the browser to this address with ?code=...; the login window catches the navigation
// (nothing listens on the port). Same address as Creality Print, which the account centre expects.
extern const char* const CALLBACK_URL;
constexpr unsigned short CALLBACK_PORT = 13666; // fixed, like Creality Print (the account centre only knows this one)
std::string redirect_uri(const Endpoints& ep);
std::string login_page_url(const Endpoints& ep, const std::string& orca_lang);
bool        is_callback_url(const std::string& url);
std::string callback_code(const std::string& url); // "" when the url carries no code

// Language codes the account page and the API use, from Orca's language ("pt_BR", "en", "zh_CN" ...).
std::string account_center_lang(const std::string& orca_lang);
std::string os_lang_index(const std::string& orca_lang);

struct ClientInfo
{
    std::string lang;        // Orca language code
    std::string duid;        // random id of this installation (never the MAC address)
    std::string app_version; // Orca Zoukei version
};

struct Session
{
    std::string token;
    std::string user_id;
    std::string nickname;
    Region      region = Region::Global;
    bool        valid() const { return !token.empty() && !user_id.empty(); }
};

struct CallResult
{
    bool        ok          = false;
    int         cloud_code  = -1; // "code" of the answer: 0 ok, 4 token invalid / expired, 517 profile not found; -1 no answer
    unsigned    http_status = 0;
    std::string message;          // readable error (never contains the token)
    int         retry_after_s = 0; // Retry-After header of a rate-limited answer (0 = none)
    bool        token_invalid() const { return cloud_code == 4; }
    // "Please do not repeat the same operation many times" (code 1088) or HTTP 429: wait and try again.
    bool        rate_limited() const { return cloud_code == 1088 || http_status == 429; }
};

// POST /api/cxy/account/v2/oauthLogin. Fills `out` (token, user id, nickname, region) on success.
CallResult exchange_login_code(Region region, const ClientInfo& info, const std::string& code, Session& out);

// POST /api/cxy/v2/slice/profile/user/list: cheap check that the token is still accepted.
CallResult check_session(const Session& session, const ClientInfo& info);

struct ProfileUpload
{
    std::string name;       // preset name; also the file name of the upload
    std::string content;    // the preset file (JSON text)
    std::string setting_id; // cloud id of an earlier upload of this preset ("" = create)
};

struct ProfileUploadResult : CallResult
{
    std::string setting_id; // cloud id of the profile
    bool        created = false;
};

// Creates (preUploadProfile, type "materia") or updates (editProfile; falls back to create when the cloud answers
// 517 "not found") the profile, then posts the file to the returned OSS upload policy.
ProfileUploadResult upload_filament_profile(const Session& session, const ClientInfo& info, const ProfileUpload& upload);

std::string md5_hex(const std::string& data);

}} // namespace Slic3r::CrealityCloud

#endif
