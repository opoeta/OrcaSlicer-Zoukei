// One WebSocket link (ws://<host>:9999) to a Creality K-series printer, shared by the native
// "creality_cfs" printer agent and the project -> CFS auto-sync. Keeps a status snapshot,
// answers heartbeats, polls boxsInfo and performs verified CFS slot writes.
// Ported from CrealityPrinterClient in the "Creality CFS Sync" plugin by Heyde Moura.
#ifndef slic3r_Utils_CrealityCfsClient_hpp_
#define slic3r_Utils_CrealityCfsClient_hpp_

#include "CrealityCfs.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace CrealityCfs {

struct WriteResult
{
    bool        ok{false};
    int         box_id{0};
    int         slot_id{0};
    std::string message;
};

class Client
{
public:
    // Called on the client's I/O thread with each message (or {"__connected__": true} /
    // {"__disconnected__": "<reason>"}).
    using Listener = std::function<void(const nlohmann::json& msg)>;

    // The shared link to `host` ("10.1.2.3", "http://host:port" also accepted), started.
    static std::shared_ptr<Client> get(const std::string& host);
    // "http://10.1.2.3:7125/x" -> "10.1.2.3"
    static std::string strip_host(const std::string& print_host);

    ~Client();

    const std::string& host() const;
    PrinterSnapshot    snapshot() const;

    int  add_listener(Listener fn);
    void remove_listener(int id);

    // Sends {"method": method, "params": params}; false when not connected.
    bool send(const std::string& method, const nlohmann::json& params);
    // Sends a get and waits (blocking; not on the UI thread) for the listed keys.
    nlohmann::json request(const nlohmann::json& params, const std::vector<std::string>& expect, double timeout_s = 6.0);
    // Writes a slot and verifies it by re-reading boxsInfo. Blocking; run on a worker thread.
    WriteResult modify_material(int box_id, int slot_id, const MaterialSpec& spec, bool force = false, double verify_timeout_s = 6.0);
    bool        send_gcode(const std::string& gcode);

    double poll_interval_s{15.0};

    struct Impl;

private:
    explicit Client(std::string host);
    std::shared_ptr<Impl> m_impl;
};

}} // namespace Slic3r::CrealityCfs

#endif // slic3r_Utils_CrealityCfsClient_hpp_
