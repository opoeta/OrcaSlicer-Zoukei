// src/slic3r/Utils/CrealityCloudCallback.hpp
//
// Orca Zoukei: the temporary loopback listener of the Creality Cloud "log in with my browser" mode. Like the "/login"
// route of Creality Print's HttpServer.cpp (AGPL-3.0, github.com/CrealityOfficial/CrealityPrint), it receives the
// browser redirect http://localhost:13666/login?code=... after the user signed in on Creality's page in the system
// browser, hands the one-time code to the caller and answers with a short page.
//
// It only lives during one login: bound to 127.0.0.1 (and [::1] when available) with an exclusive bind, it answers
// GET /login with a code once, refuses every other route, request method or non-loopback Host header, and never logs
// the code. Built into libslic3r_creality_net (Boost.Beast).
#ifndef slic3r_Utils_CrealityCloudCallback_hpp_
#define slic3r_Utils_CrealityCloudCallback_hpp_

#include <functional>
#include <memory>
#include <string>

namespace Slic3r { namespace CrealityCloud {

class LoopbackCallbackServer
{
public:
    enum class Start { Ok, PortBusy, Failed };

    // Called on the server thread for GET /login?code=... while no code was accepted yet. Returns the HTML page sent
    // to the browser; set `accepted` when this code finished the login attempt (success or a definitive failure):
    // later requests then only get `done_page`.
    using CodeHandler = std::function<std::string(const std::string& code, bool& accepted)>;
    using Event       = std::function<void()>; // server / watchdog thread

    LoopbackCallbackServer();
    ~LoopbackCallbackServer();
    LoopbackCallbackServer(const LoopbackCallbackServer&) = delete;
    LoopbackCallbackServer& operator=(const LoopbackCallbackServer&) = delete;

    // on_done: after the page answering the accepted code was written. on_timeout: nothing accepted in timeout_s.
    Start start(unsigned short port, int timeout_s, CodeHandler on_code, const std::string& done_page, Event on_done, Event on_timeout,
                std::string& error);
    // Stops the threads. Never call it from on_code / on_done / on_timeout (post to another thread first).
    void stop();
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p;
};

}} // namespace Slic3r::CrealityCloud

#endif
