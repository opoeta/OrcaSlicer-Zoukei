// The "What's new" window: resources/data/whats_new.json, shown once for every new entry and from the Help menu.
#ifndef slic3r_GUI_Zoukei_WhatsNewDialog_hpp_
#define slic3r_GUI_Zoukei_WhatsNewDialog_hpp_

namespace Slic3r { namespace GUI { namespace Zoukei {

// Help menu: always shows the newest entry.
void show_whats_new();
// Startup: shows it when the newest entry differs from the one last seen (app_config "zoukei_whats_new_seen").
void show_whats_new_if_unseen();

}}} // namespace Slic3r::GUI::Zoukei

#endif // slic3r_GUI_Zoukei_WhatsNewDialog_hpp_
