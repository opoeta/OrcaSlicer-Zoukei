// Project -> CFS auto-sync: keeps a Creality printer's CFS slots in step with the sidebar's
// filament list. Each edited filament is written to the slot it is mapped to (explicit choice, else the
// loaded slot of the same type + colour, else the slot it held before the edit), never by list position;
// unmapped filaments and slots no filament uses are left alone (CrealityCfs::sync_targets). Opening or
// creating a project only takes its list as the new baseline; later preset or colour changes are written
// after a short quiet period, and held while the printer is busy. Toggle: Preferences,
// app_config "creality_cfs_auto_sync" (on by default).
//
// Ported from _AutoSync of the "Creality CFS Sync" OrcaSlicer plugin by Heyde Moura.
#ifndef slic3r_GUI_CrealityCfsAutoSync_hpp_
#define slic3r_GUI_CrealityCfsAutoSync_hpp_

#include "slic3r/Utils/CrealityCfs.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/timer.h>

namespace Slic3r {

namespace CrealityCfs { class Client; }

namespace GUI {

class CrealityCfsAutoSync : public wxEvtHandler
{
public:
    static constexpr const char* CONFIG_KEY = "creality_cfs_auto_sync";

    // The one watcher of the application: started with the main frame, stopped before it goes.
    static void start();
    static void shutdown();

    CrealityCfsAutoSync();
    ~CrealityCfsAutoSync() override;

private:
    void tick();
    void release_client();

    wxTimer                              m_timer;
    std::shared_ptr<CrealityCfs::Client> m_client;
    std::string                          m_project_key;
    bool                                 m_have_baseline{false};
    std::string                          m_last_sig;
    std::string                          m_pending_sig;
    bool                                 m_has_pending{false};
    long long                            m_pending_since_ms{0};
    long long                            m_presets_refreshed_ms{0};
    bool                                 m_blocked_notified{false};
    std::vector<CrealityCfs::ProjectFilament> m_pending_project;
    std::vector<std::string>             m_last_sigs;  // per filament, at the last sync
    std::vector<int>                     m_last_slots; // slot key each of them occupied
    long long                            m_hold_until_ms{0};
    std::shared_ptr<std::atomic<bool>>   m_writing;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_CrealityCfsAutoSync_hpp_
