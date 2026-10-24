#ifndef slic3r_GUI_Zoukei_BackgroundWorkers_hpp_
#define slic3r_GUI_Zoukei_BackgroundWorkers_hpp_

// Orca Zoukei: bookkeeping for the background threads of the Zoukei features (Creality Cloud uploads and login,
// Creality CFS links and slot writes). GUI_App::shutdown() stops them all with bounded waits, so a worker sleeping
// in a retry backoff or blocked on the network never keeps the process alive after the main window has closed.

#include <chrono>

namespace Slic3r { namespace GUI { namespace Zoukei {

// True once the application started shutting down: workers stop at the next check and post nothing to the UI.
bool shutting_down();

// Held by a background worker for as long as it runs; shutdown_background_workers() waits (bounded) for every
// token to be released. A token taken after the shutdown started is invalid: the worker must return at once.
class WorkerToken
{
public:
    WorkerToken();
    ~WorkerToken();
    WorkerToken(const WorkerToken&)            = delete;
    WorkerToken& operator=(const WorkerToken&) = delete;
    explicit operator bool() const { return m_valid; }

private:
    bool m_valid{false};
};

// UI thread, from GUI_App::shutdown(): raises the flag, stops the Creality Cloud login listener and the Creality CFS
// links, cancels uploads and waits at most `max_wait` for the workers to return. Workers still busy then (a
// network call that has not timed out yet) are left behind; they hold no window and the process exit ends them.
void shutdown_background_workers(std::chrono::milliseconds max_wait);

}}} // namespace Slic3r::GUI::Zoukei

#endif
