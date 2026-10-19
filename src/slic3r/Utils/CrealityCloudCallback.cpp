// src/slic3r/Utils/CrealityCloudCallback.cpp
//
// Orca Zoukei: loopback listener of the Creality Cloud browser login (see CrealityCloudCallback.hpp). The exclusive
// 127.0.0.1 bind mirrors Creality Print's HttpServer::IOServer (AGPL-3.0, github.com/CrealityOfficial/CrealityPrint).
#include "CrealityCloudCallback.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace Slic3r { namespace CrealityCloud {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
using tcp       = asio::ip::tcp;

namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            out.push_back(char(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2])));
            i += 2;
        } else if (in[i] == '+')
            out.push_back(' ');
        else
            out.push_back(in[i]);
    }
    return out;
}

std::string query_param(const std::string& target, const std::string& key)
{
    const size_t q = target.find('?');
    if (q == std::string::npos)
        return {};
    size_t start = q + 1;
    while (start < target.size()) {
        size_t end = target.find('&', start);
        if (end == std::string::npos)
            end = target.size();
        const size_t eq = target.find('=', start);
        if (eq != std::string::npos && eq < end && target.compare(start, eq - start, key) == 0)
            return url_decode(target.substr(eq + 1, end - eq - 1));
        start = end + 1;
    }
    return {};
}

// Host header of a browser that really came to the loopback address (guards against DNS rebinding).
bool loopback_host(const std::string& host, unsigned short port)
{
    const std::string p = ":" + std::to_string(port);
    const std::string h = boost::algorithm::to_lower_copy(host);
    return h == "localhost" + p || h == "127.0.0.1" + p || h == "[::1]" + p;
}

std::string simple_page(const std::string& text)
{
    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>OrcaSlicer</title></head><body style=\"font-family:sans-serif\"><p>" + text +
           "</p></body></html>";
}

} // namespace

struct LoopbackCallbackServer::Impl
{
    asio::io_context                            io; // first: destroyed after the acceptors that use it
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors;
    std::vector<tcp::endpoint>                  endpoints;
    std::vector<std::thread>                    threads;
    std::thread                                 watchdog;
    std::atomic<bool>                           stopping{false};
    std::atomic<bool>                           accepted{false};
    std::atomic<bool>                           running{false};
    std::mutex                                  handle_mutex; // one request at a time
    std::mutex                                  cv_mutex;
    std::condition_variable                     cv;
    unsigned short                              port{0};
    CodeHandler                                 on_code;
    std::string                                 done_page;
    Event                                       on_done;
    Event                                       on_timeout;

    void handle(tcp::socket socket)
    {
        std::lock_guard<std::mutex> lock(handle_mutex);
        beast::tcp_stream stream(std::move(socket));
        stream.expires_after(std::chrono::seconds(15));
        beast::flat_buffer               buffer;
        http::request<http::string_body> request;
        beast::error_code                ec;
        http::read(stream, buffer, request, ec);
        if (ec || stopping)
            return;

        bool fire_done = false;
        auto respond   = [&](http::status status, const std::string& html) {
            http::response<http::string_body> response{status, request.version()};
            response.set(http::field::content_type, "text/html; charset=utf-8");
            response.set(http::field::cache_control, "no-store");
            response.set("Referrer-Policy", "no-referrer");
            response.keep_alive(false);
            response.body() = html;
            response.prepare_payload();
            beast::error_code write_ec;
            stream.expires_after(std::chrono::seconds(15));
            http::write(stream, response, write_ec);
            stream.socket().shutdown(tcp::socket::shutdown_send, write_ec);
        };

        const std::string target(request.target());
        const std::string path = target.substr(0, target.find('?'));
        const std::string host(request[http::field::host]);
        if (request.method() != http::verb::get || path != "/login" || !loopback_host(host, port)) {
            respond(http::status::not_found, simple_page("Not found"));
            return;
        }
        if (accepted) {
            respond(http::status::ok, done_page);
            return;
        }
        const std::string code = query_param(target, "code");
        if (code.empty() || code.size() > 2048) {
            respond(http::status::bad_request, simple_page("No login code in this address."));
            return;
        }
        bool        ok   = false;
        std::string page;
        try {
            page = on_code ? on_code(code, ok) : simple_page("Not ready");
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Creality Cloud browser login: handler failed: " << e.what();
            page = simple_page("Error");
        }
        if (ok) {
            accepted  = true;
            fire_done = true;
        }
        respond(http::status::ok, page);
        if (fire_done) {
            cv.notify_all();
            if (on_done)
                on_done();
        }
    }

    void accept_loop(tcp::acceptor* acceptor)
    {
        while (!stopping) {
            boost::system::error_code ec;
            tcp::socket               socket(io);
            acceptor->accept(socket, ec);
            if (stopping)
                break;
            if (ec) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // Only loopback peers (the acceptor is bound to loopback anyway).
            boost::system::error_code pec;
            const auto peer = socket.remote_endpoint(pec);
            if (pec || !peer.address().is_loopback())
                continue;
            try {
                handle(std::move(socket));
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "Creality Cloud browser login: request failed: " << e.what();
            }
        }
    }
};

LoopbackCallbackServer::LoopbackCallbackServer() : p(std::make_unique<Impl>()) {}

LoopbackCallbackServer::~LoopbackCallbackServer() { stop(); }

bool LoopbackCallbackServer::running() const { return p && p->running; }

LoopbackCallbackServer::Start LoopbackCallbackServer::start(unsigned short port, int timeout_s, CodeHandler on_code, const std::string& done_page,
                                                            Event on_done, Event on_timeout, std::string& error)
{
    stop();
    p = std::make_unique<Impl>();
    p->port       = port;
    p->on_code    = std::move(on_code);
    p->done_page  = done_page;
    p->on_done    = std::move(on_done);
    p->on_timeout = std::move(on_timeout);

    auto open = [&](const asio::ip::address& addr, bool required) -> Start {
        auto                      acc = std::make_unique<tcp::acceptor>(p->io);
        const tcp::endpoint       ep(addr, port);
        boost::system::error_code ec;
        acc->open(ep.protocol(), ec);
        if (ec) {
            if (required)
                error = ec.message();
            return Start::Failed;
        }
#ifdef _WIN32
        // No other process may share the port (and a socket already bound to it makes this bind fail).
        BOOL exclusive = TRUE;
        ::setsockopt(acc->native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
        acc->bind(ep, ec);
        if (!ec)
            acc->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            if (required)
                error = ec.message();
            const bool busy = ec == asio::error::address_in_use || ec == asio::error::access_denied;
            return busy ? Start::PortBusy : Start::Failed;
        }
        p->endpoints.push_back(ep);
        p->acceptors.push_back(std::move(acc));
        return Start::Ok;
    };

    const Start v4 = open(asio::ip::address_v4::loopback(), true);
    if (v4 != Start::Ok) {
        p->acceptors.clear();
        return v4;
    }
    open(asio::ip::address_v6::loopback(), false); // browsers may resolve localhost to ::1 first; best effort

    p->running = true;
    for (auto& acc : p->acceptors)
        p->threads.emplace_back([impl = p.get(), a = acc.get()] { impl->accept_loop(a); });
    p->watchdog = std::thread([impl = p.get(), timeout_s] {
        std::unique_lock<std::mutex> lock(impl->cv_mutex);
        const bool woke = impl->cv.wait_for(lock, std::chrono::seconds(timeout_s), [impl] { return impl->stopping.load() || impl->accepted.load(); });
        if (!woke && impl->on_timeout)
            impl->on_timeout();
    });
    BOOST_LOG_TRIVIAL(info) << "Creality Cloud browser login: waiting on 127.0.0.1:" << port;
    return Start::Ok;
}

void LoopbackCallbackServer::stop()
{
    if (!p || p->stopping.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lock(p->cv_mutex);
    }
    p->cv.notify_all();
    for (const tcp::endpoint& ep : p->endpoints) {
        // Wake the blocking accept() so the loop sees the stop flag.
        boost::system::error_code ec;
        asio::io_context          wake_io;
        tcp::socket               wake(wake_io);
        wake.connect(ep, ec);
        wake.close(ec);
    }
    for (auto& acc : p->acceptors) {
        boost::system::error_code ec;
        acc->close(ec);
    }
    for (std::thread& t : p->threads)
        if (t.joinable())
            t.join();
    if (p->watchdog.joinable())
        p->watchdog.join();
    p->running = false;
}

}} // namespace Slic3r::CrealityCloud
