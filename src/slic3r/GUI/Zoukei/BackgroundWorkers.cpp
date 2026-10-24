#include "BackgroundWorkers.hpp"

#include "CrealityCloud.hpp"
#include "slic3r/Utils/CrealityCfsClient.hpp"

#include <boost/log/trivial.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace Slic3r { namespace GUI { namespace Zoukei {

namespace {
std::atomic<bool>       s_shutting_down{false};
std::mutex              s_mutex;
std::condition_variable s_cv;
int                     s_running{0}; // guarded by s_mutex
} // namespace

bool shutting_down() { return s_shutting_down.load(); }

WorkerToken::WorkerToken()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_shutting_down)
        return;
    ++s_running;
    m_valid = true;
}

WorkerToken::~WorkerToken()
{
    if (!m_valid)
        return;
    std::lock_guard<std::mutex> lock(s_mutex);
    --s_running;
    s_cv.notify_all();
}

void shutdown_background_workers(std::chrono::milliseconds max_wait)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_shutting_down.exchange(true))
            return;
    }
    BOOST_LOG_TRIVIAL(info) << "Zoukei background workers: shutdown enter";
    CrCloud::shutdown();
    CrealityCfs::Client::shutdown_all(std::chrono::milliseconds(2000));

    std::unique_lock<std::mutex> lock(s_mutex);
    if (!s_cv.wait_for(lock, max_wait, [] { return s_running == 0; }))
        BOOST_LOG_TRIVIAL(warning) << "Zoukei background workers: " << s_running << " still busy after " << max_wait.count()
                                   << " ms, left to the process exit";
    BOOST_LOG_TRIVIAL(info) << "Zoukei background workers: shutdown exit";
}

}}} // namespace Slic3r::GUI::Zoukei
