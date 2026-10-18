#ifndef slic3r_GUI_ParamTipCard_hpp_
#define slic3r_GUI_ParamTipCard_hpp_

#include <string>
#include <wx/gdicmn.h>
#include <wx/string.h>

class wxWindow;

namespace Slic3r { namespace GUI {

// Rich "parameter tip card" shown after a short hover over a setting label in the settings tabs
// (see OG_CustomCtrl::OnMotion). Content: title, description, optional illustration
// (resources/images/param_tips/<key>.svg), notes + recommendation (resources/param_tips/notes.json),
// config key chip with copy button and a wiki link. Controlled by app_config "param_tip_cards".
class ParamTipCard
{
public:
    // False when the user disabled the cards in Preferences (plain wx tooltips are used then).
    static bool enabled();

    // Call on every mouse motion over a label. `label_rect` is in `anchor` client coordinates,
    // `tooltip` is the Line::label_tooltip (description + "parameter name: key" + default/range).
    // Returns true when the card handles this label; the caller must then not show a plain tooltip.
    static bool hover(wxWindow* anchor, const wxRect& label_rect, const wxString& title, const wxString& tooltip,
                      const std::string& wiki_path);

    // Mouse left the label: cancels a pending card (a visible one hides itself when the mouse is
    // neither over the label nor over the card).
    static void leave();

    // Hide immediately.
    static void hide_now();
};

}} // namespace Slic3r::GUI

#endif
