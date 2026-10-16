// Ported from Creality Print (src/slic3r/GUI/print_manage/Device/LanDeviceProbe.*,
// CrealityOfficial/CrealityPrint@59ae8cbb0437dcc7236e27433ecd4aeb52be0244, AGPL-3.0).
#ifndef slic3r_CrealityDevice_LanDeviceProbe_hpp_
#define slic3r_CrealityDevice_LanDeviceProbe_hpp_

#include "nlohmann/json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace CrealityDevice {

enum class ProbePolicy
{
    PreferSecure,
    SecureOnly,
    HttpOnly
};

enum class ProbeSource
{
    None,
    Https,
    Http
};

enum class ProbeError
{
    None,
    InvalidAddress,
    Timeout,
    NetworkError,
    CertError,
    HttpError,
    ParseError,
    InvalidInfo,
    Cancelled
};

struct ProbeOptions
{
    long        connect_timeout_seconds {3};
    long        https_timeout_seconds {6};
    long        http_timeout_seconds {3};
    bool        preflight_ports {false};
    int         port_probe_timeout_ms {500};
    std::size_t port_probe_concurrency {20};
    std::size_t request_concurrency {5};
    std::size_t response_size_limit {64 * 1024};
    std::function<bool()> cancelled;
};

struct LanConnectionProfile
{
    std::string    address;
    std::string    mac;
    std::string    model;
    bool           secure_connection {false};
    std::uint16_t  wss_port {0};
    std::uint16_t  video_port {0};
    ProbeSource    source {ProbeSource::None};
    nlohmann::json raw_info;

    nlohmann::json to_json() const;
};

struct ProbeAttempt
{
    ProbeSource                   source {ProbeSource::None};
    ProbeError                    error {ProbeError::None};
    unsigned                      http_status {0};
    std::string                   detail;
    std::optional<nlohmann::json> info;

    nlohmann::json to_json() const;
};

struct ProbeResult
{
    std::optional<LanConnectionProfile> profile;
    ProbeError                          error {ProbeError::None};
    unsigned                            http_status {0};
    std::string                         detail;
    std::vector<ProbeAttempt>           attempts;

    bool ok() const { return profile.has_value(); }
    nlohmann::json to_json() const;
};

class LanDeviceProbe
{
public:
    ProbeResult probe(const std::string& address,
                      ProbePolicy       policy,
                      const ProbeOptions& options = {}) const;

    std::vector<ProbeResult> probe_many(const std::vector<std::string>& addresses,
                                        ProbePolicy                    policy,
                                        const ProbeOptions&            options = {}) const;
};

const char* probe_error_name(ProbeError error) noexcept;
const char* probe_source_name(ProbeSource source) noexcept;

void submit_lan_device_probe_task(std::function<void()> task);

}}} // namespace Slic3r::GUI::CrealityDevice

#endif // slic3r_CrealityDevice_LanDeviceProbe_hpp_
