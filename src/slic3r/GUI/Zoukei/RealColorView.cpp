// Orca Zoukei: RealColor view of the G-code preview, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
#include "RealColorView.hpp"
#include "PhotoMode.hpp"
#include "PhotoStudio.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ColorMixEngine.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include <imgui.h>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

namespace {

constexpr const char* kSection = "zoukei_view";
// Where the "Mix colors automatically" dialog keeps the TD of a filament (see ColorMixDialog.cpp)
constexpr const char* kMixSection = "color_mix";

std::string key_safe(std::string s)
{
    for (char& c : s)
        if (!std::isalnum((unsigned char) c))
            c = '_';
    return s;
}

int hex_alpha(const std::string& hex)
{
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 8)
        return 255;
    try {
        return std::stoi(s.substr(6, 2), nullptr, 16);
    } catch (...) {
        return 255;
    }
}

float config_float(const char* key, float fallback, float lo, float hi)
{
    const AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return fallback;
    const std::string v = cfg->get(kSection, key);
    if (v.empty())
        return fallback;
    return std::min(hi, std::max(lo, (float) std::atof(v.c_str())));
}

} // namespace

bool realcolor_enabled()
{
    const AppConfig* cfg = wxGetApp().app_config;
    return cfg != nullptr && cfg->get_bool(kSection, "realcolor");
}

void set_realcolor_enabled(bool enabled)
{
    if (AppConfig* cfg = wxGetApp().app_config)
        cfg->set(kSection, "realcolor", std::string(enabled ? "1" : "0"));
}

libvgcode::RealColorParams build_realcolor_params(const std::vector<std::string>& tool_colors)
{
    libvgcode::RealColorParams params;
    params.enabled = realcolor_enabled();
    params.blur_mm = config_float("realcolor_blur_mm", 0.8f, 0.f, 2.f);
    params.wall_blend = config_float("realcolor_wall_blend", 0.8f, 0.f, 1.f);
    if (!params.enabled)
        return params;

    const AppConfig* cfg = wxGetApp().app_config;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    params.td_mm.reserve(tool_colors.size());
    for (size_t i = 0; i < tool_colors.size(); ++i) {
        const std::string& hex = tool_colors[i];
        std::string name, alias, type;
        if (bundle != nullptr && i < bundle->filament_presets.size()) {
            if (const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[i])) {
                name  = preset->name;
                alias = preset->alias;
                std::string display_type;
                type = const_cast<Preset*>(preset)->config.get_filament_type(display_type);
            }
        }
        // The TD saved by the colour mixing dialog, which names a filament by its colour and its label
        float td = 0.f;
        if (cfg != nullptr) {
            std::string short_name = name;
            const size_t at = short_name.find(" @");
            if (at != std::string::npos)
                short_name.erase(at);
            for (const std::string& candidate : { alias, short_name, name }) {
                if (candidate.empty())
                    continue;
                const std::string v = cfg->get(kMixSection, "td_" + key_safe(hex) + "_" + key_safe(candidate));
                if (!v.empty()) {
                    td = std::max(0.f, (float) std::atof(v.c_str()));
                    break;
                }
            }
        }
        if (td <= 0.f) {
            // No TD known: the opacity the colour mixing engine assumes for this filament (translucent,
            // silk, clear...), as the TD that gives that opacity to a 0.2 mm bead.
            const double opacity = ColorMix::default_opacity(type, name, hex_alpha(hex));
            if (opacity < 0.995)
                td = (float) (0.2 / -std::log10(std::max(1e-3, 1.0 - opacity)));
        }
        params.td_mm.push_back(td);
    }
    return params;
}

bool render_legend_extras(ImGuiWrapper& imgui, float scale, bool realcolor_applicable)
{
    bool changed = false;
    ImGui::Spacing();
    ImGui::Dummy(ImVec2(4.f * scale, 4.f * scale));
    ImGui::SameLine();

    if (realcolor_applicable) {
        bool on = realcolor_enabled();
        if (imgui.bbl_checkbox(_L("RealColor"), on)) {
            set_realcolor_enabled(on);
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(320.f * scale);
            ImGui::TextUnformatted(_u8L("Show the colour that comes out of the printer: layers let light through by the TD of their filament and neighbouring lines blend, so mixed filaments show their blended colour. Approximated simulation, not a guarantee of the final colour. Ported from OrcaFS-NeotkoCM (Neotko).").c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }

    if (imgui.button(photo_panel_open() ? _L("Hide photo mode") : _L("Photo mode...")))
        set_photo_panel_open(!photo_panel_open());
    // Orca Zoukei: Photo Studio, its own window (see PhotoStudio.hpp)
    ImGui::SameLine();
    if (imgui.button(_L("Photo Studio...")))
        wxGetApp().CallAfter([]() { open_photo_studio(); });

    if (realcolor_applicable && realcolor_enabled()) {
        static float s_blur = -1.f;
        static float s_wall = -1.f;
        if (s_blur < 0.f)
            s_blur = config_float("realcolor_blur_mm", 0.8f, 0.f, 2.f);
        if (s_wall < 0.f)
            s_wall = config_float("realcolor_wall_blend", 0.8f, 0.f, 1.f);
        ImGui::Dummy(ImVec2(4.f * scale, 1.f));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.f * scale);
        ImGui::SliderFloat("##zk_rc_blur", &s_blur, 0.f, 2.f, _u8L("Blend %.1f mm").c_str());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            wxGetApp().app_config->set(kSection, "realcolor_blur_mm", std::to_string(s_blur));
            changed = true;
        }
        ImGui::Dummy(ImVec2(4.f * scale, 1.f));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.f * scale);
        ImGui::SliderFloat("##zk_rc_wall", &s_wall, 0.f, 1.f, _u8L("Walls %.2f").c_str());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            wxGetApp().app_config->set(kSection, "realcolor_wall_blend", std::to_string(s_wall));
            changed = true;
        }
    }
    ImGui::Spacing();
    return changed;
}

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
