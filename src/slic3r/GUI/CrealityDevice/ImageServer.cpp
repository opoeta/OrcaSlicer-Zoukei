#include "ImageServer.hpp"

#include "slic3r/Utils/Http.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#include <atomic>
#include <cctype>
#include <cstring>
#include <thread>

namespace Slic3r { namespace GUI { namespace CrealityDevice {

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
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            out.push_back(char(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2])));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string query_param(const std::string& target, const std::string& key)
{
    const std::size_t q = target.find('?');
    if (q == std::string::npos)
        return {};
    std::size_t start = q + 1;
    while (start < target.size()) {
        std::size_t end = target.find('&', start);
        if (end == std::string::npos)
            end = target.size();
        const std::size_t eq = target.find('=', start);
        if (eq != std::string::npos && eq < end && target.compare(start, eq - start, key) == 0)
            return url_decode(target.substr(eq + 1, end - eq - 1));
        start = end + 1;
    }
    return {};
}

bool is_lan_ipv4(const std::string& address)
{
    boost::system::error_code ec;
    const auto parsed = asio::ip::make_address(address, ec);
    if (ec || !parsed.is_v4())
        return false;
    const auto bytes = parsed.to_v4().to_bytes();
    return bytes[0] == 10 || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) || (bytes[0] == 192 && bytes[1] == 168) ||
           (bytes[0] == 169 && bytes[1] == 254);
}

bool is_allowed_path(const std::string& path)
{
    if (path.find_first_of("\r\n") != std::string::npos || path.find("..") != std::string::npos)
        return false;
    static const std::string thumbnail_prefix = "/downloads/humbnail/";
    if (path.compare(0, thumbnail_prefix.size(), thumbnail_prefix) == 0)
        return path.size() > thumbnail_prefix.size();
    return path == "/downloads/original/current_print_image.png" || path == "/downloads/cloudSliceHumbnail/cloudSliceHumbnail.png";
}

std::string content_type_for(const std::string& path)
{
    auto ends_with = [&](const char* ext) {
        const std::size_t n = std::strlen(ext);
        if (path.size() < n)
            return false;
        for (std::size_t i = 0; i < n; ++i)
            if (std::tolower((unsigned char) path[path.size() - n + i]) != ext[i])
                return false;
        return true;
    };
    if (ends_with(".jpg") || ends_with(".jpeg"))
        return "image/jpeg";
    if (ends_with(".webp"))
        return "image/webp";
    if (ends_with(".gif"))
        return "image/gif";
    return "image/png";
}

} // namespace

struct ImageServer::Impl
{
    explicit Impl(HostFilter filter) : filter(std::move(filter)), acceptor(io) {}

    void handle(tcp::socket socket)
    {
        beast::tcp_stream stream(std::move(socket));
        stream.expires_after(std::chrono::seconds(15));
        beast::flat_buffer               buffer;
        http::request<http::string_body> request;
        beast::error_code                ec;
        http::read(stream, buffer, request, ec);
        if (ec)
            return;

        auto respond = [&](http::status status, std::string body, const std::string& type, bool cacheable) {
            http::response<http::string_body> response{status, request.version()};
            response.set(http::field::content_type, type);
            response.set(http::field::access_control_allow_origin, "*");
            response.set(http::field::cache_control, cacheable ? "private, max-age=31536000, immutable" : "no-store");
            response.keep_alive(false);
            response.body() = std::move(body);
            response.prepare_payload();
            beast::error_code write_ec;
            http::write(stream, response, write_ec);
            stream.socket().shutdown(tcp::socket::shutdown_send, write_ec);
        };

        const std::string target(request.target());
        if (request.method() != http::verb::get || target.rfind("/device_image?", 0) != 0) {
            respond(http::status::not_found, "Not found", "text/plain", false);
            return;
        }
        const std::string address = query_param(target, "address");
        const std::string path    = query_param(target, "path");
        const std::string secure  = query_param(target, "secure");
        const bool        cacheable = !query_param(target, "v").empty();
        if ((secure != "0" && secure != "1") || !is_lan_ipv4(address) || !is_allowed_path(path) || (filter && !filter(address))) {
            respond(http::status::bad_request, "Invalid device image request", "text/plain", false);
            return;
        }

        const std::string url = (secure == "1" ? "https://" : "http://") + address + path;
        std::string       body;
        unsigned          status = 0;
        try {
            Http::get(url)
                .timeout_connect(3)
                .timeout_max(10)
                .size_limit(20 * 1024 * 1024)
                .on_complete([&](std::string b, unsigned s) {
                    body   = std::move(b);
                    status = s;
                })
                .on_error([&](std::string, std::string error, unsigned s) {
                    status = s;
                    BOOST_LOG_TRIVIAL(info) << "Creality panel image fetch failed: " << error;
                })
                .perform_sync();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(info) << "Creality panel image fetch failed: " << e.what();
        }
        if (status != 200 || body.empty()) {
            respond(status == 404 ? http::status::not_found : http::status::bad_gateway, "Unable to fetch device image", "text/plain", false);
            return;
        }
        respond(http::status::ok, std::move(body), content_type_for(path), cacheable);
    }

    void accept_loop()
    {
        while (!stopping) {
            boost::system::error_code ec;
            tcp::socket               socket(io);
            acceptor.accept(socket, ec);
            if (stopping)
                break;
            if (ec) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            auto shared = std::make_shared<tcp::socket>(std::move(socket));
            asio::post(pool, [this, shared] {
                try {
                    handle(std::move(*shared));
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(warning) << "Creality panel image request failed: " << e.what();
                }
            });
        }
    }

    HostFilter        filter;
    asio::io_context  io;
    tcp::acceptor     acceptor;
    asio::thread_pool pool{4};
    std::thread       thread;
    std::atomic<bool> stopping{false};
    unsigned short    port{0};
};

ImageServer::ImageServer(HostFilter filter) : m_impl(std::make_unique<Impl>(std::move(filter)))
{
    try {
        const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
        m_impl->acceptor.open(endpoint.protocol());
        m_impl->acceptor.bind(endpoint);
        m_impl->acceptor.listen();
        m_impl->port   = m_impl->acceptor.local_endpoint().port();
        m_impl->thread = std::thread([impl = m_impl.get()] { impl->accept_loop(); });
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Creality panel image server did not start: " << e.what();
        m_impl->port = 0;
    }
}

ImageServer::~ImageServer() { stop(); }

unsigned short ImageServer::port() const { return m_impl ? m_impl->port : 0; }

void ImageServer::stop()
{
    if (!m_impl || m_impl->stopping.exchange(true))
        return;
    boost::system::error_code ec;
    if (m_impl->port != 0) {
        // Wake the blocking accept() so the loop sees the stop flag.
        asio::io_context wake_io;
        tcp::socket      wake(wake_io);
        wake.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), m_impl->port), ec);
        wake.close(ec);
    }
    m_impl->acceptor.close(ec);
    if (m_impl->thread.joinable())
        m_impl->thread.join();
    m_impl->pool.join();
}

}}} // namespace Slic3r::GUI::CrealityDevice
