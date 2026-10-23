// Project <-> Snapmaker U1 heads. Project -> printer: when a project filament's preset / colour changes, the head it is
// mapped to (sidebar badge E1..E4) is rewritten with SET_PRINT_FILAMENT_CONFIG; opening or creating a project only records
// a baseline, changes wait while the printer is busy, and official (NFC) spools are never touched. Printer -> project: a head
// that changed on the printer raises a notification with a "Sync" link (or applies at once with the preference).
// Same toggle as the Creality CFS / Flashforge IFS sync (app_config "creality_cfs_auto_sync").
#ifndef slic3r_GUI_SnapmakerU1AutoSync_hpp_
#define slic3r_GUI_SnapmakerU1AutoSync_hpp_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/timer.h>

namespace Slic3r {
class SnapmakerPrinterAgent;
namespace GUI {

class SnapmakerU1AutoSync : public wxEvtHandler
{
public:
    static void start();
    static void shutdown();
    // "Sincronizar tudo agora": apply every head the printer reports to the mapped project filaments (UI thread,
    // text = what changed).
    static std::string apply_all_to_project();
    static void rebaseline();

    SnapmakerU1AutoSync();
    ~SnapmakerU1AutoSync() override;

private:
    void tick();
    void check_printer_changes(const std::shared_ptr<SnapmakerPrinterAgent>& agent);

    wxTimer                    m_timer;
    std::string                m_project_key;
    bool                       m_have_baseline{false};
    std::vector<std::string>   m_last;    // per filament signature of the last state written / recorded
    std::vector<std::string>   m_pending;
    std::vector<int>           m_heads;   // head (0-3) per filament while nothing was being edited, -1 = none
    long long                  m_hold_until_ms{0}; // keep m_heads (just written) until the printer reports them
    bool                       m_has_pending{false};
    long long                  m_pending_since_ms{0};
    bool                       m_blocked_notified{false};
    std::map<int, std::string> m_printer_last; // head -> type|colour as last seen on the printer
};

}} // namespace Slic3r::GUI

#endif
