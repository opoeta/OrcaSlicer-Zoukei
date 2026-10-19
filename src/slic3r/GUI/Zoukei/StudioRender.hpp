// Orca Zoukei: Photo Studio renderer. Draws the sliced print of the G-code preview the way a product
// photographer would light it: a key, fill and rim light with colour temperatures, a material preset over the
// realistic toolpath shading, an infinite cyclorama / gradient / solid / transparent background, and a floor
// that takes a soft cast shadow, a contact shadow and an optional faint reflection. It renders into whatever
// framebuffer is bound (the live view of the studio window) or off-screen in tiles with supersampling (stills
// up to 8K, video frames). It only borrows the preview's toolpaths: every viewer setting it touches is put
// back before it returns, so the regular preview does not change.
#ifndef slic3r_GUI_Zoukei_StudioRender_hpp_
#define slic3r_GUI_Zoukei_StudioRender_hpp_

#include "libslic3r/Point.hpp"
#include "libslic3r/BoundingBox.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

class GCodeViewer;

namespace Zoukei {

enum class StudioBackground : int { CycloramaWhite = 0, CycloramaDark, Gradient, Solid, Transparent, Count };
enum class StudioMaterial : int { Preview = 0, MattePLA, SilkSheen, PETGGloss, Count };
enum class StudioLightPreset : int { Soft = 0, Dramatic, HighKey, PreviewLike, Count };

struct StudioLight
{
    bool  on{ true };
    // 1.0 = the key light of the realistic preview.
    float intensity{ 1.0f };
    // Degrees. With "lights follow the camera" 0 is the camera side and +90 the right of the picture,
    // otherwise 0 is the front of the plate and +90 its right.
    float azimuth{ 0.0f };
    // Degrees above the floor.
    float elevation{ 30.0f };
    // Colour temperature, kelvin.
    float kelvin{ 5600.0f };
};

struct StudioSettings
{
    StudioLight key{ true, 1.0f, -45.0f, 40.0f, 5600.0f };
    StudioLight fill{ true, 0.35f, 60.0f, 15.0f, 6500.0f };
    StudioLight rim{ true, 0.7f, 165.0f, 35.0f, 8000.0f };
    float ambient{ 0.45f };
    bool  lights_follow_camera{ true };

    StudioBackground background{ StudioBackground::CycloramaWhite };
    std::array<float, 3> cyclorama_color{ { 0.96f, 0.96f, 0.965f } };
    std::array<float, 3> gradient_top{ { 0.80f, 0.84f, 0.86f } };
    std::array<float, 3> gradient_bottom{ { 0.25f, 0.27f, 0.30f } };
    std::array<float, 3> solid_color{ { 1.0f, 1.0f, 1.0f } };

    bool  floor_shadow{ true };
    float shadow_strength{ 0.55f };
    float shadow_softness{ 0.5f };
    bool  contact_shadow{ true };
    float contact_strength{ 0.6f };
    bool  reflection{ false };
    float reflection_strength{ 0.25f };
    bool  self_shadows{ true };

    StudioMaterial material{ StudioMaterial::MattePLA };
    float exposure{ 1.0f };
    float saturation{ 1.1f };

    // Show every layer instead of the preview's layer range.
    bool whole_print{ false };
};

// The default rig of a light preset.
void apply_light_preset(StudioSettings& s, StudioLightPreset preset);
// Linear RGB of a black body at that temperature, scaled to unit luminance.
std::array<float, 3> kelvin_to_rgb(float kelvin);

// Orbit camera of the studio: Z up, around a target.
struct StudioCamera
{
    Vec3d  target{ Vec3d::Zero() };
    double distance{ 300.0 };
    // 0 = looking from the front of the plate (-Y), 90 = from its right (+X).
    double yaw_deg{ 30.0 };
    // Above the floor; 90 = straight down.
    double pitch_deg{ 25.0 };
    // Vertical field of view of the frame.
    double fov_deg{ 24.0 };
    bool   ortho{ false };

    Vec3d eye() const;
    Vec3d up() const;
};

// A rectangle of the frame in its own normalised coordinates: the frame is [-1, 1] x [-1, 1].
// The live view spans more than the frame (the guides darken the rest), a tile of an export less.
struct FrameWindow
{
    double x0{ -1.0 }, y0{ -1.0 }, x1{ 1.0 }, y1{ 1.0 };
};

// Fits the camera to the print for a frame of that aspect (width / height), keeping its direction.
void fit_camera(StudioCamera& camera, const BoundingBoxf3& box, double aspect, double margin = 1.12);

class StudioRenderer
{
public:
    StudioRenderer() = default;
    ~StudioRenderer();
    StudioRenderer(const StudioRenderer&) = delete;
    StudioRenderer& operator=(const StudioRenderer&) = delete;

    // GL objects; the context must be current.
    void release();

    // The print to render, the preview's G-code viewer. False when nothing is sliced.
    static GCodeViewer* gcode_viewer();
    static bool         has_print();
    // Bounding box of the whole print (not just the layers shown), the framing reference.
    static BoundingBoxf3 print_box();
    static size_t        layers_count();

    // Renders the frame window of the camera into the bound framebuffer, viewport (0, 0, width, height).
    // aspect is the one of the frame. checker draws a transparent background as a checkerboard (live view).
    // layer_top >= 0 shows the layers up to that one only (virtual timelapse).
    bool render(const StudioSettings& settings, const StudioCamera& camera, double aspect, const FrameWindow& window, int width,
                int height, bool checker, int layer_top = -1);

    // Off-screen picture of the frame: out_w x out_h, supersampled ss x ss, rendered in tiles when larger than
    // what the card takes. RGBA, top row first, straight (not premultiplied) alpha. progress(done, total) may
    // return false to cancel. False on failure or cancel.
    bool render_image(const StudioSettings& settings, const StudioCamera& camera, int out_w, int out_h, int ss,
                      std::vector<uint8_t>& rgba, int layer_top = -1, const std::function<bool(int, int)>& progress = nullptr);

    // Overlay of the live view: darkens what is outside the frame rectangle (pixels, bottom-left origin) and
    // draws its border and, optionally, the rule of thirds.
    void render_guides(int width, int height, const std::array<float, 4>& frame_px, bool thirds, bool dark_theme);

    // Size of the key light's shadow map: 2048 keeps the live view quick, exports ask for 4096.
    void set_shadow_resolution(int size) { m_shadow_res = size; }

    const std::string& last_error() const { return m_error; }

private:
    bool init_programs();
    bool ensure_depth_target(unsigned int& tex, unsigned int& fbo, int size);
    bool ensure_reflection_target(int w, int h);
    bool build_key_shadow(GCodeViewer& gv, const Vec3d& dir_to_light, const BoundingBoxf3& box, double reach, int size);
    bool build_contact_map(GCodeViewer& gv, const BoundingBoxf3& box, double reach, int size);

    unsigned int m_stage_program{ 0 };
    unsigned int m_guides_program{ 0 };
    unsigned int m_vao{ 0 };
    bool         m_programs_failed{ false };

    int          m_shadow_res{ 2048 };
    unsigned int m_key_tex{ 0 }, m_key_fbo{ 0 };
    int          m_key_size{ 0 };
    size_t       m_key_cache{ 0 };
    Transform3d  m_key_vp{ Transform3d::Identity() };
    double       m_key_texel_mm{ 0.1 };
    double       m_key_depth_mm{ 100.0 };

    unsigned int m_contact_tex{ 0 }, m_contact_fbo{ 0 };
    int          m_contact_size{ 0 };
    size_t       m_contact_cache{ 0 };
    Transform3d  m_contact_vp{ Transform3d::Identity() };
    double       m_contact_z0{ 0.0 };
    double       m_contact_range{ 1.0 };

    unsigned int m_refl_color{ 0 }, m_refl_depth{ 0 }, m_refl_fbo{ 0 };
    int          m_refl_w{ 0 }, m_refl_h{ 0 };

    std::string m_error;
};

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_StudioRender_hpp_
