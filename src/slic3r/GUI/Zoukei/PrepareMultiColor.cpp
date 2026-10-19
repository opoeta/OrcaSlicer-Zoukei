#include "PrepareMultiColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

namespace {

constexpr int MODE_OFF      = 0;
constexpr int MODE_COAXIAL  = 1;
constexpr int MODE_GRADIENT = 2;

struct Entry
{
    int                      mode  = MODE_OFF;
    int                      count = 0;
    std::array<ColorRGBA, 4> colors;
    float                    cycle_mm = 100.f;
};

struct State
{
    bool               active = false;
    std::vector<Entry> entries;
    // uniform locations (-1 = absent)
    int loc_mode = -1, loc_count = -1, loc_cycle = -1;
    int loc_color[4] = { -1, -1, -1, -1 };
    // what was last sent, to skip redundant uniform calls between volumes
    int  cur_filament = -1;
    bool cur_selected = false;
};

State s_state;

void send_off(const GLShaderProgram& shader)
{
    shader.set_uniform(s_state.loc_mode, MODE_OFF);
    s_state.cur_filament = -1;
}

} // namespace

bool prepare_multicolor_enabled()
{
    const AppConfig* cfg = wxGetApp().app_config;
    return cfg == nullptr || cfg->get("zoukei_prepare_multicolor") != "0";
}

float prepare_multicolor_cycle_mm(double gradient_length_m)
{
    const double metres = gradient_length_m > 0. ? gradient_length_m : 50.; // 0 = automatic = 50 m
    return float(std::clamp(metres * 2., 20., 200.));
}

PrepareMultiColorScope::PrepareMultiColorScope(const GLShaderProgram* shader, bool enable)
{
    s_state.active = false;
    s_state.entries.clear();
    s_state.cur_filament = -1;
    if (!enable || shader == nullptr || shader->get_uniform_location("mc_mode") < 0 || !prepare_multicolor_enabled())
        return;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || wxGetApp().preset_bundle == nullptr)
        return;

    // One entry per filament; the mixed-gradient slots come back as a two-colour gradient.
    const std::vector<std::string> packs = plater->get_filament_colors_render_info();
    const std::vector<std::string> types = plater->get_filament_color_render_type();
    const auto* lengths = wxGetApp().preset_bundle->project_config.option<ConfigOptionFloats>("filament_gradient_length");

    bool any = false;
    s_state.entries.resize(packs.size());
    for (size_t i = 0; i < packs.size(); ++i) {
        Entry& e = s_state.entries[i];
        std::istringstream is(packs[i]);
        for (std::string tok; is >> tok && e.count < 4;) {
            ColorRGBA c;
            if (decode_color(tok, c)) {
                c.a(1.f);
                e.colors[e.count++] = adjust_color_for_rendering(c);
            }
        }
        if (e.count < 2) {
            e.count = 0;
            continue;
        }
        const bool gradient = i < types.size() && types[i] == "0"; // type "1" or empty = solid split
        e.mode     = gradient ? MODE_GRADIENT : MODE_COAXIAL;
        e.cycle_mm = prepare_multicolor_cycle_mm(lengths != nullptr && i < lengths->values.size() ? lengths->values[i] : 0.);
        any        = true;
    }
    if (!any) {
        s_state.entries.clear();
        return;
    }

    s_state.loc_mode     = shader->get_uniform_location("mc_mode");
    s_state.loc_count    = shader->get_uniform_location("mc_count");
    s_state.loc_cycle    = shader->get_uniform_location("mc_cycle");
    s_state.loc_color[0] = shader->get_uniform_location("mc_c0");
    s_state.loc_color[1] = shader->get_uniform_location("mc_c1");
    s_state.loc_color[2] = shader->get_uniform_location("mc_c2");
    s_state.loc_color[3] = shader->get_uniform_location("mc_c3");
    s_state.active = true;
    send_off(*shader);
}

PrepareMultiColorScope::~PrepareMultiColorScope()
{
    if (s_state.active) {
        // The program is shared with every other caller of the object shader: leave the mode off.
        GLShaderProgram* shader = wxGetApp().get_current_shader();
        if (shader != nullptr)
            send_off(*shader);
    }
    s_state.active = false;
    s_state.entries.clear();
}

bool prepare_multicolor_active() { return s_state.active; }

void prepare_multicolor_apply(const GLShaderProgram& shader, int filament_id, bool allowed, bool selected)
{
    if (!s_state.active)
        return;

    // Unset or past the list = the first filament, as the single colour does.
    size_t idx = filament_id > 0 ? size_t(filament_id - 1) : 0;
    if (idx >= s_state.entries.size())
        idx = 0;
    const Entry& e = s_state.entries[idx];

    if (!allowed || e.mode == MODE_OFF) {
        if (s_state.cur_filament != -1)
            send_off(shader);
        return;
    }
    if (s_state.cur_filament == int(idx) && s_state.cur_selected == selected)
        return;

    for (int k = 0; k < 4; ++k) {
        ColorRGBA c = e.colors[size_t(std::min(k, e.count - 1))];
        if (selected)
            c = GLVolume::brighten_color(c, 1.25f);
        shader.set_uniform(s_state.loc_color[k], c);
    }
    shader.set_uniform(s_state.loc_count, e.count);
    shader.set_uniform(s_state.loc_cycle, e.cycle_mm);
    shader.set_uniform(s_state.loc_mode, e.mode);
    s_state.cur_filament = int(idx);
    s_state.cur_selected = selected;
}

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
