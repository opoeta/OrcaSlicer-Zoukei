#include "WsProxy.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <nlohmann/json.hpp>

#include <wx/app.h>

#include <atomic>
#include <cctype>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace Slic3r { namespace GUI { namespace CrealityDevice {

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace ssl       = asio::ssl;
namespace websocket = beast::websocket;
using tcp           = asio::ip::tcp;
using json          = nlohmann::json;

namespace {

constexpr std::size_t MAX_SESSIONS          = 64;
constexpr std::size_t MAX_TOKEN_SIZE        = 128;
constexpr std::size_t MAX_URL_SIZE          = 2048;
constexpr std::size_t MAX_OUTGOING_SIZE     = 1024 * 1024;
constexpr std::size_t MAX_QUEUED_MESSAGES   = 128;
constexpr std::size_t MAX_INCOMING_SIZE     = 4 * 1024 * 1024;
constexpr std::size_t MAX_PENDING_EVENTS    = 2048;

struct WsEndpoint
{
    bool        secure{false};
    std::string host;
    std::string port;
};

bool is_ipv4_literal(const std::string& value)
{
    boost::system::error_code ec;
    const auto address = asio::ip::make_address(value, ec);
    return !ec && address.is_v4();
}

// ws[s]://<ipv4>[:port][/]; the page script only proxies such URLs.
std::optional<WsEndpoint> parse_ws_url(const std::string& url)
{
    if (url.empty() || url.size() > MAX_URL_SIZE)
        return std::nullopt;
    WsEndpoint endpoint;
    std::size_t pos = 0;
    if (url.rfind("wss://", 0) == 0) {
        endpoint.secure = true;
        pos             = 6;
    } else if (url.rfind("ws://", 0) == 0) {
        pos = 5;
    } else {
        return std::nullopt;
    }
    std::size_t end = url.find('/', pos);
    std::string authority = url.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (end != std::string::npos && url.substr(end) != "/")
        return std::nullopt;
    const std::size_t colon = authority.find(':');
    endpoint.host = authority.substr(0, colon);
    endpoint.port = colon == std::string::npos ? std::string(endpoint.secure ? "443" : "80") : authority.substr(colon + 1);
    if (!is_ipv4_literal(endpoint.host) || endpoint.port.empty() || endpoint.port.size() > 5 ||
        !std::all_of(endpoint.port.begin(), endpoint.port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return std::nullopt;
    const int port = std::stoi(endpoint.port);
    if (port <= 0 || port > 65535)
        return std::nullopt;
    // Same policy as Creality Print: any wss port, plain ws only on the printer's 9999.
    if (!endpoint.secure && port != 9999)
        return std::nullopt;
    return endpoint;
}

std::string json_string(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

class Session;

struct WsProxy::Impl : std::enable_shared_from_this<WsProxy::Impl>
{
    Impl(ScriptSink sink)
        : ssl_context(ssl::context::tls_client)
        , work(asio::make_work_guard(io))
        , sink(std::move(sink))
    {
        beast::error_code ec;
        ssl_context.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3, ec);
        if (!ec)
            ssl_context.set_verify_mode(ssl::verify_peer, ec);
        if (!ec)
            ssl_context.load_verify_file(Slic3r::resources_dir() + "/cert/ca.crt", ec);
        if (ec) {
            ssl_error = ec.message();
            BOOST_LOG_TRIVIAL(error) << "Creality panel WebSocket proxy: TLS setup failed: " << ssl_error;
        } else if (X509_VERIFY_PARAM* param = SSL_CTX_get0_param(ssl_context.native_handle())) {
            // The printers' clocks and certificates are not reliable; keep the chain check only.
            X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_NO_CHECK_TIME);
        }
        worker = std::thread([this] {
            try {
                io.run();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Creality panel WebSocket proxy I/O thread stopped: " << e.what();
            }
        });
    }

    void emit(json event)
    {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            if (pending_events.size() >= MAX_PENDING_EVENTS) {
                // The page fell behind; keep the newest state, which the printer resends anyway.
                pending_events.pop_front();
            }
            pending_events.push_back(std::move(event));
            if (!flush_scheduled) {
                flush_scheduled = true;
                schedule        = true;
            }
        }
        if (schedule && wxTheApp != nullptr) {
            std::weak_ptr<Impl> weak = shared_from_this();
            wxTheApp->CallAfter([weak] {
                if (auto self = weak.lock())
                    self->flush();
            });
        }
    }

    // UI thread.
    void flush()
    {
        std::deque<json> events;
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            events.swap(pending_events);
            flush_scheduled = false;
        }
        if (stopped || !sink || events.empty())
            return;
        json batch = json::array();
        for (auto& event : events) {
            // A closed socket is done; the page opens a new one to reconnect.
            if (event.value("event", std::string()) == "close")
                sessions.erase(std::make_pair(event.value("pageToken", std::string()), event.value("id", 0LL)));
            batch.push_back(std::move(event));
        }
        sink("window.__nativeWebSocketCallbackBatch&&window.__nativeWebSocketCallbackBatch(" +
             batch.dump(-1, ' ', true, json::error_handler_t::replace) + ");");
    }

    asio::io_context                                      io;
    ssl::context                                          ssl_context;
    std::string                                           ssl_error;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread                                           worker;
    ScriptSink                                            sink;
    std::atomic<bool>                                     stopped{false};

    std::mutex       events_mutex;
    std::deque<json> pending_events;
    bool             flush_scheduled{false};

    // UI thread only: (pageToken, id) -> session.
    std::map<std::pair<std::string, long long>, std::shared_ptr<Session>> sessions;
};

class Session : public std::enable_shared_from_this<Session>
{
public:
    using PlainStream  = websocket::stream<beast::tcp_stream>;
    using SecureStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    // The owner outlives its sessions' handlers: they only run on its I/O thread.
    Session(WsProxy::Impl* owner, WsEndpoint endpoint, long long id, std::string page_token, std::string socket_token)
        : m_owner(owner)
        , m_strand(asio::make_strand(m_owner->io))
        , m_resolver(m_strand)
        , m_endpoint(std::move(endpoint))
        , m_id(id)
        , m_page_token(std::move(page_token))
        , m_socket_token(std::move(socket_token))
    {
        if (m_endpoint.secure)
            m_wss = std::make_unique<SecureStream>(m_strand, m_owner->ssl_context);
        else
            m_ws = std::make_unique<PlainStream>(m_strand);
    }

    void start()
    {
        auto self = shared_from_this();
        asio::post(m_strand, [self] { self->do_resolve(); });
    }

    void send(std::string data)
    {
        auto self = shared_from_this();
        asio::post(m_strand, [self, data = std::move(data)]() mutable {
            if (self->m_finished)
                return;
            if (self->m_queue.size() >= MAX_QUEUED_MESSAGES) {
                self->fail("send queue overflow");
                return;
            }
            self->m_queue.push_back(std::move(data));
            self->pump_write();
        });
    }

    void close(int code, std::string reason)
    {
        auto self = shared_from_this();
        asio::post(m_strand, [self, code, reason = std::move(reason)] {
            if (self->m_finished)
                return;
            if (!self->m_open) {
                self->finish(code == 0 ? 1000 : code, reason, true);
                return;
            }
            if (self->m_closing)
                return;
            self->m_closing = true;
            websocket::close_reason cr(static_cast<websocket::close_code>(code >= 1000 && code < 5000 ? code : 1000));
            cr.reason = reason.substr(0, 123);
            self->visit([&](auto& ws) {
                ws.async_close(cr, [self](beast::error_code) { self->finish(1000, std::string(), true); });
            });
        });
    }

    // Drops the connection without notifying the page (page reload or shutdown).
    void abort()
    {
        auto self = shared_from_this();
        asio::post(m_strand, [self] {
            self->m_silent = true;
            self->m_finished = true;
            beast::error_code ec;
            self->lowest().socket().close(ec);
        });
    }

private:
    template<class F> void visit(F&& f)
    {
        if (m_wss)
            f(*m_wss);
        else
            f(*m_ws);
    }

    beast::tcp_stream& lowest() { return m_wss ? beast::get_lowest_layer(*m_wss) : beast::get_lowest_layer(*m_ws); }

    void do_resolve()
    {
        if (m_finished)
            return;
        auto self = shared_from_this();
        m_resolver.async_resolve(m_endpoint.host, m_endpoint.port, [self](beast::error_code ec, tcp::resolver::results_type results) {
            if (self->m_finished)
                return;
            if (ec) {
                self->fail("resolve failed: " + ec.message());
                return;
            }
            self->lowest().expires_after(std::chrono::seconds(5));
            self->lowest().async_connect(results, [self](beast::error_code ec, const tcp::endpoint&) { self->on_connect(ec); });
        });
    }

    void on_connect(beast::error_code ec)
    {
        if (m_finished)
            return;
        if (ec) {
            fail("connect failed: " + ec.message());
            return;
        }
        if (m_wss) {
            if (!m_owner->ssl_error.empty()) {
                fail("TLS unavailable: " + m_owner->ssl_error);
                return;
            }
            lowest().expires_after(std::chrono::seconds(8));
            auto self = shared_from_this();
            m_wss->next_layer().async_handshake(ssl::stream_base::client, [self](beast::error_code ec) {
                if (self->m_finished)
                    return;
                if (ec) {
                    self->fail("TLS handshake failed: " + ec.message());
                    return;
                }
                self->ws_handshake();
            });
            return;
        }
        ws_handshake();
    }

    void ws_handshake()
    {
        lowest().expires_never();
        auto self = shared_from_this();
        const std::string host = m_endpoint.host + ":" + m_endpoint.port;
        visit([&](auto& ws) {
            ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
            ws.read_message_max(MAX_INCOMING_SIZE);
            ws.async_handshake(host, "/", [self](beast::error_code ec) {
                if (self->m_finished)
                    return;
                if (ec) {
                    self->fail("WebSocket handshake failed: " + ec.message());
                    return;
                }
                self->m_open = true;
                self->visit([](auto& ws) { ws.text(true); });
                self->emit("open", std::string(), 0, std::string(), true);
                self->do_read();
                self->pump_write();
            });
        });
    }

    void do_read()
    {
        if (m_finished)
            return;
        auto self = shared_from_this();
        visit([&](auto& ws) {
            ws.async_read(m_buffer, [self](beast::error_code ec, std::size_t) {
                if (self->m_finished)
                    return;
                if (ec) {
                    if (ec == websocket::error::closed) {
                        int         code   = 1000;
                        std::string reason;
                        self->visit([&](auto& ws) {
                            code   = ws.reason().code;
                            reason = std::string(ws.reason().reason.c_str());
                        });
                        self->finish(code, reason, true);
                    } else {
                        self->fail("read failed: " + ec.message());
                    }
                    return;
                }
                std::string data = beast::buffers_to_string(self->m_buffer.data());
                self->m_buffer.consume(self->m_buffer.size());
                self->emit("message", data, 0, std::string(), true);
                self->do_read();
            });
        });
    }

    void pump_write()
    {
        if (m_finished || !m_open || m_closing || m_writing || m_queue.empty())
            return;
        m_writing = true;
        auto self = shared_from_this();
        visit([&](auto& ws) {
            ws.async_write(asio::buffer(m_queue.front()), [self](beast::error_code ec, std::size_t) {
                self->m_writing = false;
                if (self->m_finished)
                    return;
                if (ec) {
                    self->fail("write failed: " + ec.message());
                    return;
                }
                self->m_queue.pop_front();
                self->pump_write();
            });
        });
    }

    void fail(const std::string& message)
    {
        if (m_finished)
            return;
        BOOST_LOG_TRIVIAL(info) << "Creality panel WebSocket " << m_endpoint.host << ":" << m_endpoint.port << ": " << message;
        emit("error", std::string(), 0, std::string(), false, message);
        finish(1006, std::string(), false);
    }

    void finish(int code, const std::string& reason, bool clean)
    {
        if (m_finished && m_silent)
            return;
        const bool was_finished = m_finished;
        m_finished = true;
        beast::error_code ec;
        lowest().socket().close(ec);
        if (!was_finished || !m_close_sent) {
            m_close_sent = true;
            emit("close", std::string(), code, reason, clean);
        }
    }

    void emit(const char* event, const std::string& data, int code, const std::string& reason, bool clean, const std::string& message = {})
    {
        if (m_silent)
            return;
        json e = {{"id", m_id}, {"socketToken", m_socket_token}, {"pageToken", m_page_token}, {"event", event}};
        if (!data.empty() || std::string(event) == "message")
            e["data"] = data;
        if (std::string(event) == "close") {
            e["code"]     = code;
            e["reason"]   = reason;
            e["wasClean"] = clean;
        }
        if (!message.empty())
            e["message"] = message;
        m_owner->emit(std::move(e));
    }

    WsProxy::Impl*                              m_owner;
    asio::strand<asio::io_context::executor_type> m_strand;
    tcp::resolver                               m_resolver;
    WsEndpoint                                  m_endpoint;
    long long                                   m_id;
    std::string                                 m_page_token;
    std::string                                 m_socket_token;
    std::unique_ptr<PlainStream>                m_ws;
    std::unique_ptr<SecureStream>               m_wss;
    beast::flat_buffer                          m_buffer;
    std::deque<std::string>                     m_queue;
    bool                                        m_open{false};
    bool                                        m_closing{false};
    bool                                        m_writing{false};
    bool                                        m_finished{false};
    bool                                        m_close_sent{false};
    bool                                        m_silent{false};
};

WsProxy::WsProxy(ScriptSink sink, HostFilter filter)
    : m_impl(std::make_shared<Impl>(std::move(sink)))
    , m_filter(std::move(filter))
{}

WsProxy::~WsProxy() { shutdown(); }

void WsProxy::handle(const json& payload)
{
    if (!m_impl || m_impl->stopped || !payload.is_object())
        return;
    const std::string type       = json_string(payload, "type");
    const std::string page_token = json_string(payload, "pageToken");
    if (page_token.empty() || page_token.size() > MAX_TOKEN_SIZE)
        return;

    auto& sessions = m_impl->sessions;
    if (type == "reset") {
        // A new page instance: sockets of any other page are orphans.
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->first.first != page_token) {
                it->second->abort();
                it = sessions.erase(it);
            } else
                ++it;
        }
        return;
    }

    auto id_it = payload.find("id");
    if (id_it == payload.end() || !id_it->is_number_integer())
        return;
    const long long   id           = id_it->get<long long>();
    const std::string socket_token = json_string(payload, "socketToken");
    const auto        key          = std::make_pair(page_token, id);

    auto reject = [&](const std::string& message) {
        m_impl->emit({{"id", id}, {"socketToken", socket_token}, {"pageToken", page_token}, {"event", "error"}, {"message", message}});
        m_impl->emit({{"id", id}, {"socketToken", socket_token}, {"pageToken", page_token}, {"event", "close"}, {"code", 1006}, {"reason", message}, {"wasClean", false}});
    };

    if (type == "open") {
        if (socket_token.empty() || socket_token.size() > MAX_TOKEN_SIZE || sessions.count(key) != 0)
            return;
        // Forget sessions that already finished on the page side.
        if (sessions.size() >= MAX_SESSIONS) {
            reject("too many sockets");
            return;
        }
        const auto endpoint = parse_ws_url(json_string(payload, "url"));
        if (!endpoint) {
            reject("URL not allowed");
            return;
        }
        if (m_filter && !m_filter(endpoint->host)) {
            BOOST_LOG_TRIVIAL(warning) << "Creality panel WebSocket refused for non-LAN host " << endpoint->host;
            reject("host not allowed");
            return;
        }
        auto session = std::make_shared<Session>(m_impl.get(), *endpoint, id, page_token, socket_token);
        sessions.emplace(key, session);
        session->start();
        return;
    }

    auto it = sessions.find(key);
    if (it == sessions.end())
        return;
    if (type == "send") {
        std::string data = json_string(payload, "data");
        if (data.size() > MAX_OUTGOING_SIZE)
            return;
        it->second->send(std::move(data));
    } else if (type == "close") {
        int  code    = 1000;
        auto code_it = payload.find("code");
        if (code_it != payload.end() && code_it->is_number_integer())
            code = code_it->get<int>();
        it->second->close(code, json_string(payload, "reason"));
        sessions.erase(it);
    }
}

void WsProxy::shutdown()
{
    if (!m_impl || m_impl->stopped.exchange(true))
        return;
    for (auto& [key, session] : m_impl->sessions)
        session->abort();
    m_impl->sessions.clear();
    m_impl->work.reset();
    // Sessions close their sockets on the strand; give them a moment, then stop.
    asio::post(m_impl->io, [impl = m_impl.get()] { impl->io.stop(); });
    if (m_impl->worker.joinable())
        m_impl->worker.join();
    m_impl->sink = nullptr;
}

std::string WsProxy::page_script()
{
    // Verbatim from Creality Print's WebSocketProxy::GetWebSocketProxyScript().
    const char* script =
        "(function(){"
        "if(window.top!==window){return;}"
        "if(window.__wsProxyInstalled){return;}"
        "window.__wsProxyInstalled=true;"
        "function __install(){"
        "if(!window.wx||!window.wx.postMessage||!window.WebSocket){return false;}"
        "var NativeWebSocket=window.WebSocket;"
        "var PAGE_TOKEN=(window.crypto&&typeof window.crypto.randomUUID==='function')"
        "?window.crypto.randomUUID():Date.now().toString(36)+'-'+Math.random().toString(36).slice(2);"
        "var STATE_CONNECTING=(typeof NativeWebSocket.CONNECTING==='number')?NativeWebSocket.CONNECTING:0;"
        "var STATE_OPEN=(typeof NativeWebSocket.OPEN==='number')?NativeWebSocket.OPEN:1;"
        "var STATE_CLOSING=(typeof NativeWebSocket.CLOSING==='number')?NativeWebSocket.CLOSING:2;"
        "var STATE_CLOSED=(typeof NativeWebSocket.CLOSED==='number')?NativeWebSocket.CLOSED:3;"
        "function __makeSocketToken(id){"
        "return(window.crypto&&typeof window.crypto.randomUUID==='function')"
        "?window.crypto.randomUUID():PAGE_TOKEN+'-'+id+'-'+Date.now().toString(36)+'-'+Math.random().toString(36).slice(2);"
        "}"
        "function __isIPv4(host){"
        "var parts=host.split('.');"
        "if(parts.length!==4){return false;}"
        "for(var i=0;i<parts.length;++i){"
        "if(!/^[0-9]+$/.test(parts[i])||Number(parts[i])>255){return false;}"
        "}"
        "return true;"
        "}"
        "function __shouldProxy(url){"
        "if(url==null){return false;}"
        "try{"
        "var parsed=new URL(String(url));"
        "if(!__isIPv4(parsed.hostname)){return false;}"
        "if(parsed.username||parsed.password){return false;}"
        "if(parsed.pathname!=='/'||parsed.search!==''||parsed.hash!==''){return false;}"
        "return parsed.protocol==='wss:'||(parsed.protocol==='ws:'&&parsed.port==='9999');"
        "}catch(e){return false;}"
        "}"
        "function ProxyWebSocket(url,protocols){"
        "this.url=url;this.protocol='';this.readyState=STATE_CONNECTING;"
        "this.binaryType='blob';this.extensions='';this.bufferedAmount=0;"
        "this.onopen=null;this.onmessage=null;this.onclose=null;this.onerror=null;"
        "var id=ProxyWebSocket.__nextId++,socketToken=__makeSocketToken(id);"
        "this.__id=id;this.__socketToken=socketToken;ProxyWebSocket.__sockets[id]=this;"
        "ProxyWebSocket.__send({type:'open',id:id,socketToken:socketToken,url:url,protocols:protocols});"
        "}"
        "ProxyWebSocket.__nextId=1;"
        "ProxyWebSocket.__sockets={};"
        "ProxyWebSocket.__send=function(msg){"
        "try{msg.pageToken=PAGE_TOKEN;window.wx.postMessage(JSON.stringify({command:'ws_proxy',payload:msg}));}catch(e){}"
        "};"
        "ProxyWebSocket.prototype.send=function(data){"
        "if(this.readyState!==STATE_OPEN&&this.readyState!==STATE_CONNECTING){throw new Error('WebSocket is not open');}"
        "ProxyWebSocket.__send({type:'send',id:this.__id,socketToken:this.__socketToken,data:String(data)});"
        "};"
        "ProxyWebSocket.prototype.close=function(code,reason){"
        "if(this.readyState===STATE_CLOSING||this.readyState===STATE_CLOSED){return;}"
        "this.readyState=STATE_CLOSING;"
        "ProxyWebSocket.__send({type:'close',id:this.__id,socketToken:this.__socketToken,code:code,reason:reason});"
        "};"
        "ProxyWebSocket.CONNECTING=STATE_CONNECTING;"
        "ProxyWebSocket.OPEN=STATE_OPEN;"
        "ProxyWebSocket.CLOSING=STATE_CLOSING;"
        "ProxyWebSocket.CLOSED=STATE_CLOSED;"
        "ProxyWebSocket.__dispatch=function(evt){"
        "var s=ProxyWebSocket.__sockets[evt.id];if(!s||evt.socketToken!==s.__socketToken){return;}"
        "switch(evt.event){"
        "case'open':s.readyState=STATE_OPEN;if(typeof s.onopen==='function'){s.onopen({type:'open',target:s});}break;"
        "case'message':if(typeof s.onmessage==='function'){s.onmessage({type:'message',data:evt.data,target:s});}break;"
        "case'close':s.readyState=STATE_CLOSED;delete ProxyWebSocket.__sockets[evt.id];if(typeof s.onclose==='function'){s.onclose({type:'close',code:evt.code||1000,reason:evt.reason||'',wasClean:!!evt.wasClean,target:s});}break;"
        "case'error':if(typeof s.onerror==='function'){s.onerror({type:'error',message:evt.message||'',target:s});}break;"
        "}"
        "};"
        "function HybridWebSocket(url,protocols){"
        "var proxy=__shouldProxy(url);"
        "if(proxy){return new ProxyWebSocket(url,protocols);}"
        "if(typeof protocols==='undefined'){return new NativeWebSocket(url);}"
        "return new NativeWebSocket(url,protocols);"
        "}"
        "HybridWebSocket.CONNECTING=STATE_CONNECTING;"
        "HybridWebSocket.OPEN=STATE_OPEN;"
        "HybridWebSocket.CLOSING=STATE_CLOSING;"
        "HybridWebSocket.CLOSED=STATE_CLOSED;"
        "if(NativeWebSocket.prototype){HybridWebSocket.prototype=NativeWebSocket.prototype;}"
        "window.__nativeWebSocketCallback=function(evt){"
        "if(!evt||evt.pageToken!==PAGE_TOKEN){return;}"
        "ProxyWebSocket.__dispatch(evt);"
        "};"
        "window.__nativeWebSocketCallbackBatch=function(events){"
        "if(!Array.isArray(events)){return;}"
        "for(var i=0;i<events.length;++i){"
        "try{window.__nativeWebSocketCallback(events[i]);}catch(e){}"
        "}"
        "};"
        "ProxyWebSocket.__send({type:'reset'});"
        "window.WebSocket=HybridWebSocket;"
        "return true;"
        "}"
        "if(!__install()){"
        "var __t=0,__max=200;"
        "var __timer=setInterval(function(){"
        "if(__install()||++__t>=__max){clearInterval(__timer);}"
        "},50);"
        "}"
        "})();";

    return script;
}

}}} // namespace Slic3r::GUI::CrealityDevice
