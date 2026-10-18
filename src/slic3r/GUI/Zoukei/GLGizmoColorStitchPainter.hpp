// Orca Zoukei: ColorStitch Painter gizmo, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
//
// Paints saved Surface Effect recipes (SurfaceEffectProfile: a Sandwich pass stack of Solid,
// ColorStitch and PathBlend passes) onto faces of an object. The default tool is the smart fill,
// which floods the coplanar faces under the cursor. The paint goes into
// ModelVolume::colorstitch_paint_facets (one slot per recipe, 32 slots per volume) and the slot ->
// recipe table, both saved in the 3MF together with the recipe library. At slice time every
// painted top surface prints its own recipe (libslic3r/ColorStitchPaint.hpp, Fill/FillSandwich.cpp).
//
// Reduced port of NeotkoCM's GLGizmoColorStitchPainter: one object at a time, a compact recipe
// editor (Top, Penultimate and Bottom zones, up to 3 passes each) and the saved recipe list. The
// Penultimate zone is the solid layer under a top surface, the Bottom zone the supported bottom
// surface (paint it from below). SVG stickers (rough, as in NeotkoCM): load an SVG, click a top or
// bottom face to place it; every colour of the drawing prints with a recipe of its own.
// Not ported: palette generators and TD department, weave preview on the model, multi-object
// selection, MixedFilament Object mode, highlight of the active colour, gradient fills of stickers.
#ifndef slic3r_GUI_Zoukei_GLGizmoColorStitchPainter_hpp_
#define slic3r_GUI_Zoukei_GLGizmoColorStitchPainter_hpp_

#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;
struct SurfaceEffectProfile;
struct SurfacePassStack;

namespace GUI {

class GLGizmoColorStitchPainter : public GLGizmoPainterBase
{
public:
    GLGizmoColorStitchPainter(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;
    // Allocates the paint slot of the active recipe before a stroke starts (and refuses the stroke
    // without an active recipe).
    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering ColorStitch Painter"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving ColorStitch Painter"); }
    std::string get_action_snapshot_name() const override { return _u8L("ColorStitch painting editing"); }

    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType(m_active_slot); }
    // The right button keeps moving the camera; erasing is Shift + left button.
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

private:
    bool on_init() override;

    void update_model_object() override;
    void update_from_model_object(bool first_update) override;

    void             on_opening() override;
    void             on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    // Colours of the paint states of a volume: neutral + one per slot (the recipe swatch).
    std::vector<ColorRGBA> state_colors(const ModelVolume &mv) const;
    void                   refresh_state_colors();
    // Slot of the active recipe on the selected object, allocated on all its parts when missing.
    // 0 when there is no active recipe or no free slot.
    int  ensure_active_slot();
    // Erases one slot from the selected object and frees it.
    void free_slot(int slot);
    void erase_all();

    // Recipe library actions.
    int  new_recipe();
    int  capture_recipe_from_object();
    int  duplicate_recipe(int profile_id);
    void delete_recipe(int profile_id);
    // Stores an edited stack of `zone` (0 Top, 1 Penultimate, 2 Bottom) into the active recipe (swatch
    // included) and schedules a re-slice.
    void commit_stack(const SurfacePassStack &stack, int zone, bool reslice);
    // Swatch of a recipe: its saved one, else the colours of its first zone that prints something.
    uint32_t swatch_argb(const SurfaceEffectProfile &profile) const;
    // Swatch of a recipe: the pass colours weighted by their share. `profile` supplies the payload
    // filaments of passes that carry none.
    uint32_t preview_argb(const SurfacePassStack &stack, const SurfaceEffectProfile *profile) const;
    void     schedule_reslice();

    void render_tools(float max_tooltip_width);
    void render_recipe_list();
    void render_recipe_editor();
    void render_slots_in_use();

    // SVG stickers (ColorStitchSticker.hpp).
    void render_stickers();
    void load_sticker();
    // Recipe of a flat filament colour (Solid pass in Top and Bottom zones), created on first use.
    int  flat_recipe_for_filament(int filament0);
    int  nearest_filament(uint32_t rgb) const;
    // Places sticker `index` where the mouse points (face height, side from the face normal).
    bool place_sticker_at_mouse(size_t index, const Vec2d &mouse_position);
    // Writes `pose` (world frame) into sticker `index`, snapshot and re-slice.
    void commit_stickers(const std::string &snapshot_name);
    void render_sticker_preview();
    void rebuild_sticker_preview();
    const ModelInstance *selected_instance() const;

    // Filament colours of the plater ("#RRGGBB"), 1-based in the UI.
    std::vector<std::string> m_filament_colors;
    int                      object_filament() const; // 1-based filament of the selected object

    int         m_zone           = 0; // zone shown by the recipe editor: 0 Top, 1 Penultimate, 2 Bottom
    int         m_active_profile = 0; // SurfaceEffectProfile id, 0 = none
    int         m_active_slot    = 0; // slot of the active recipe on the selected object
    wchar_t     m_current_tool   = 0;
    std::string m_status;
    bool        m_reslice_pending = false;
    char        m_name_buf[128] = { 0 };
    int         m_name_buf_profile = -1;

    std::map<std::string, wxString> m_desc;

    // Stickers: index into ModelObject::colorstitch_stickers of the one being edited, -1 = none.
    int  m_sticker_sel     = -1;
    bool m_sticker_placing = false; // the next click on a face puts the selected sticker there
    // Preview of the stickers (world frame, one flat colour per region), rebuilt when it is dirty.
    struct StickerPreviewPart
    {
        std::shared_ptr<GLModel> model;
        ColorRGBA                color;
    };
    std::vector<StickerPreviewPart> m_sticker_preview;
    bool                            m_sticker_preview_dirty = true;
    Transform3d                     m_sticker_preview_inst  = Transform3d::Identity();

    // Paint states used on the selected object (any part), refreshed after each change of the paint.
    std::vector<bool> m_used_states;
    bool              m_used_states_dirty = true;
    const std::vector<bool> &used_states();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_GLGizmoColorStitchPainter_hpp_
