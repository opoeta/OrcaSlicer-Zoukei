// Orca Zoukei: multi-colour filaments (coaxial / gradient) in the Prepare 3D view.
// The object shaders (gouraud, phong) colour a volume from a handful of uniforms: coaxial filaments by the
// side a surface faces (equal sectors of the azimuth of the normal, thin stripes on top and bottom faces),
// gradient filaments by height (one full cycle every N mm, N = clamp(2 * gradient_length_m, 20, 200); 0 =
// automatic = 50 m). It is an approximation of the print, not a simulation of it.
#ifndef slic3r_GUI_Zoukei_PrepareMultiColor_hpp_
#define slic3r_GUI_Zoukei_PrepareMultiColor_hpp_

namespace Slic3r {

class GLShaderProgram;

namespace GUI {
namespace Zoukei {

// Preferences > Graphics > "Show multi-colour filaments in Prepare" (default on).
bool prepare_multicolor_enabled();

// Mm of height per full colour cycle of a gradient filament, from its length in metres (0 = automatic).
float prepare_multicolor_cycle_mm(double gradient_length_m);

// Alive while a collection of volumes is drawn with `shader`. Reads the project's filaments once; it stays
// inactive (and prepare_multicolor_apply() does nothing) when none of them is multi-colour, when `enable`
// is false or when the shader lacks the uniforms. The multi-colour mode is switched off again on destruction.
class PrepareMultiColorScope
{
public:
    PrepareMultiColorScope(const GLShaderProgram* shader, bool enable);
    ~PrepareMultiColorScope();
    PrepareMultiColorScope(const PrepareMultiColorScope&) = delete;
    PrepareMultiColorScope& operator=(const PrepareMultiColorScope&) = delete;
};

// True while a scope is active.
bool prepare_multicolor_active();

// Sets the multi-colour uniforms of the shader for a part printed with filament `filament_id` (1-based).
// `allowed` false (modifiers, disabled or unprintable volumes...) or a single-colour filament turns the mode off.
// `selected` brightens the colours like the selection does for a single colour.
void prepare_multicolor_apply(const GLShaderProgram& shader, int filament_id, bool allowed, bool selected);

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_PrepareMultiColor_hpp_
