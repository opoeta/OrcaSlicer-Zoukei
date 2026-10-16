// Project -> IFS auto-sync for a Flashforge printer running ZMOD: when a project filament's preset / colour changes,
// the IFS slot it is mapped to is rewritten (CHANGE_ZCOLOR, plus HelixScreen's lane_data). Opening or creating a project
// only records a baseline; changes wait while the printer is busy. Same toggle as the Creality CFS sync
// (app_config "creality_cfs_auto_sync", Preferences "Sync project filaments to the CFS / IFS").
#ifndef slic3r_GUI_FlashforgeIfsAutoSync_hpp_
#define slic3r_GUI_FlashforgeIfsAutoSync_hpp_

#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/timer.h>

namespace Slic3r { namespace GUI {

class FlashforgeIfsAutoSync : public wxEvtHandler
{
public:
    static void start();
    static void shutdown();

    FlashforgeIfsAutoSync();
    ~FlashforgeIfsAutoSync() override;

private:
    void tick();

    wxTimer                  m_timer;
    std::string              m_project_key;
    bool                     m_have_baseline{false};
    std::vector<std::string> m_last;     // per filament signature of the last state written / recorded
    std::vector<std::string> m_pending;
    std::vector<int>         m_ports;    // IFS port (1-4) per filament while nothing was being edited, 0 = none / external
    bool                     m_has_pending{false};
    long long                m_pending_since_ms{0};
    bool                     m_blocked_notified{false};
};

}} // namespace Slic3r::GUI

#endif
