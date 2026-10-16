// WebSocket proxy for the embedded Creality device panel.
//
// The panel talks to the printer over ws://<ip>:9999 or wss://<ip>:<wssPort>. WebView2 refuses
// the printer's self-signed certificate, so, like Creality Print (print_manage/App/WebSocketProxy*,
// CrealityOfficial/CrealityPrint@59ae8cbb0437dcc7236e27433ecd4aeb52be0244, AGPL-3.0), a page
// script replaces window.WebSocket for printer URLs and the sockets are opened here instead.
// TLS is verified against Creality's printer CA (resources/cert/ca.crt), without host-name or
// validity-date checks. Only LAN IPv4 addresses accepted by the host filter are reached.
#ifndef slic3r_CrealityDevice_WsProxy_hpp_
#define slic3r_CrealityDevice_WsProxy_hpp_

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace Slic3r { namespace GUI { namespace CrealityDevice {

class WsProxy
{
public:
    // Receives a script to run in the page; always called on the UI thread.
    using ScriptSink = std::function<void(const std::string& script)>;
    // Decides whether a host (IPv4 literal) may be reached; called on the UI thread.
    using HostFilter = std::function<bool(const std::string& host)>;

    WsProxy(ScriptSink sink, HostFilter filter);
    ~WsProxy();

    WsProxy(const WsProxy&)            = delete;
    WsProxy& operator=(const WsProxy&) = delete;

    // The page script that routes printer WebSockets through this proxy.
    static std::string page_script();

    // A {"command":"ws_proxy","payload":{...}} payload from the page. UI thread only.
    void handle(const nlohmann::json& payload);
    // Closes every socket and stops the I/O thread. UI thread only.
    void shutdown();

    // Shared with the socket sessions in WsProxy.cpp.
    struct Impl;

private:
    std::shared_ptr<Impl> m_impl;
    HostFilter            m_filter;
};

}}} // namespace Slic3r::GUI::CrealityDevice

#endif // slic3r_CrealityDevice_WsProxy_hpp_
