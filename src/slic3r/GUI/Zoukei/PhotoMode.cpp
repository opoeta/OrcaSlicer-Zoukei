// Orca Zoukei: Photo Mode for the G-code preview, ported from OrcaFS-NeotkoCM (Neotko), AGPL-3.0.
#include "PhotoMode.hpp"
#include "PhotoStudio.hpp"

#include <glad/gl.h>

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>
#include <wx/bitmap.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/image.h>

namespace Slic3r {
namespace GUI {

// -------------------------------------------------------------------------------------------------
// The capture itself, a member of the preview canvas: it needs the shadow pass and the camera.
// The picture is the toolpaths as they are on screen (view type, layer range, RealColor...), drawn
// again into an off-screen buffer from the camera of the preview with the plate, the shells and the
// UI left out. The cast shadow is the preview's own shadow map, so it is the same light as the live view.
// -------------------------------------------------------------------------------------------------
bool GLCanvas3D::photo_capture(const Zoukei::PhotoOptions& options, ThumbnailData& out)
{
    if (m_canvas_type != ECanvasType::CanvasPreview || !m_gcode_viewer.has_data() || m_in_render)
        return false;
    if (!_set_current())
        return false;

    const BoundingBoxf3 paths_bb = m_gcode_viewer.get_paths_bounding_box();
    if (!paths_bb.defined)
        return false;

    // Size of the picture: the height asked for, the aspect of the preview. Rendered at twice the size
    // and averaged down when the card allows it, which is the anti-aliasing.
    const Size cnv_size = get_canvas_size();
    const double aspect = cnv_size.get_height() > 0 ? double(cnv_size.get_width()) / double(cnv_size.get_height()) : 1.0;
    GLint max_tex = 0, max_rb = 0;
    glsafe(::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex));
    glsafe(::glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb));
    const int limit = std::max(512, (int) std::min(max_tex > 0 ? max_tex : 4096, max_rb > 0 ? max_rb : 4096));
    int out_h = std::min(4320, std::max(256, options.height_px));
    int out_w = std::max(64, (int) std::lround(double(out_h) * aspect));
    if (out_w > limit || out_h > limit) {
        const double k = double(limit) / double(std::max(out_w, out_h));
        out_w = std::max(64, (int) (out_w * k));
        out_h = std::max(64, (int) (out_h * k));
    }
    int ss = 2;
    while (ss > 1 && (out_w * ss > limit || out_h * ss > limit || (size_t) out_w * ss * out_h * ss > 16000000))
        --ss;
    const int W = out_w * ss;
    const int H = out_h * ss;

    GLint prev_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo));
    GLint prev_viewport[4] = { 0, 0, 0, 0 };
    glsafe(::glGetIntegerv(GL_VIEWPORT, prev_viewport));
    GLfloat prev_clear[4] = { 0.f, 0.f, 0.f, 0.f };
    glsafe(::glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear));

    GLuint fbo = 0, color_tex = 0, depth_rb = 0;
    glsafe(::glGenFramebuffers(1, &fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    glsafe(::glGenTextures(1, &color_tex));
    glsafe(::glBindTexture(GL_TEXTURE_2D, color_tex));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0));
    glsafe(::glGenRenderbuffers(1, &depth_rb));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, depth_rb));
    glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb));
    const GLenum draw_bufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, draw_bufs));

    bool ok = false;
    std::vector<unsigned char> white_px, black_px;
    if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        // The plater camera is what the toolpaths and the light read: swap it, always restore it.
        Camera& global_camera = wxGetApp().plater()->get_camera();
        struct CameraRestore {
            Camera& cam;
            const Camera saved;
            ~CameraRestore() { cam = saved; }
        } camera_restore{ global_camera, global_camera };
        struct PhotoState {
            GLCanvas3D& canvas;
            ~PhotoState() {
                canvas.m_photo_active = false;
                canvas.m_shadow_map_valid = false;
                canvas.m_shadow_map_key = 0;
            }
        } photo_state{ *this };

        BoundingBoxf3 scene_box = paths_bb;
        if (PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate()) {
            const BoundingBoxf3 plate_box = plate->get_build_volume();
            if (plate_box.defined)
                scene_box.merge(plate_box);
        }

        Camera camera = global_camera;
        const double zoom_before = camera.get_zoom();
        camera.set_viewport(0, 0, (unsigned int) W, (unsigned int) H);
        camera.set_scene_box(scene_box);
        if (options.frame_print)
            camera.zoom_to_box(paths_bb, 1.2);
        else if (cnv_size.get_height() > 0)
            camera.set_zoom(zoom_before * double(H) / double(cnv_size.get_height()));
        camera.apply_projection(scene_box);
        global_camera = camera;

        // Light: from the azimuth / elevation of the panel, a static light of the room
        const double az = double(options.light_azimuth) * PI / 180.0;
        const double el = double(options.light_elevation) * PI / 180.0;
        m_photo_light_dir = Vec3d(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)).normalized();
        m_photo_shadows   = options.shadows;
        m_photo_active    = true;

        auto render_pass = [&](float r, float g, float b, float a, std::vector<unsigned char>& pixels) {
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
            global_camera.apply_viewport();
            glsafe(::glClearColor(r, g, b, a));
            glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
            glsafe(::glEnable(GL_DEPTH_TEST));
            m_shadow_map_valid = false;
            m_gcode_viewer.set_clipping_plane(_get_section_view_plane());
            // The cast shadow on the floor (drawn as black with alpha over what is behind) and the map the toolpaths sample
            _render_shadows(global_camera.get_view_matrix(), global_camera.get_projection_matrix());

            const bool receive_shadows = m_shadow_map_valid && m_shadow_map_texture_id != 0 && m_shadow_map_size != 0;
            if (receive_shadows) {
                glsafe(::glActiveTexture(GL_TEXTURE4));
                glsafe(::glBindTexture(GL_TEXTURE_2D, m_shadow_map_texture_id));
                glsafe(::glActiveTexture(GL_TEXTURE0));
                m_gcode_viewer.set_shadow_map(4, m_shadow_light_vp, 0.35f, 1.0f / static_cast<float>(m_shadow_map_size));
            }
            else
                m_gcode_viewer.set_shadow_map(4, Transform3d::Identity(), 0.0f, 0.0f);
            m_gcode_viewer.set_tone(receive_shadows ? 1.1f : 1.0f, 1.15f);
            m_gcode_viewer.set_light_top_dir(_static_light_dir_eye().value_or(Vec3d(-0.4574957, 0.4574957, 0.7624929)));
            m_gcode_viewer.render_toolpaths_with_camera(global_camera);
            if (receive_shadows) {
                glsafe(::glActiveTexture(GL_TEXTURE4));
                glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
                glsafe(::glActiveTexture(GL_TEXTURE0));
            }
            glsafe(::glDisable(GL_BLEND));

            pixels.assign((size_t) W * (size_t) H * 4, 0);
            glsafe(::glPixelStorei(GL_PACK_ALIGNMENT, 1));
            glsafe(::glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, (void*) pixels.data()));
        };

        if (options.transparent) {
            // Same picture over white and over black: what differs is how much of each pixel is the
            // print or its shadow, so the soft shadow survives as partial transparency.
            render_pass(1.f, 1.f, 1.f, 1.f, white_px);
            render_pass(0.f, 0.f, 0.f, 1.f, black_px);
        }
        else
            render_pass(options.background[0], options.background[1], options.background[2], 1.f, white_px);
        ok = true;
    }

    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo));
    glsafe(::glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]));
    glsafe(::glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]));
    if (depth_rb != 0) glsafe(::glDeleteRenderbuffers(1, &depth_rb));
    if (color_tex != 0) glsafe(::glDeleteTextures(1, &color_tex));
    if (fbo != 0) glsafe(::glDeleteFramebuffers(1, &fbo));
    if (!ok)
        return false;

    // Average the supersampled pixels down; with a transparent background un-premultiply with the
    // alpha found from the white and the black picture.
    out.set((unsigned int) out_w, (unsigned int) out_h);
    if (!out.is_valid())
        return false;
    const bool transparent = options.transparent && black_px.size() == white_px.size();
    const float inv_samples = 1.0f / float(ss * ss);
    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            float sum_c[3] = { 0.f, 0.f, 0.f };
            float sum_a = 0.f;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const size_t i = ((size_t) (y * ss + sy) * (size_t) W + (size_t) (x * ss + sx)) * 4;
                    if (transparent) {
                        const float d = (float(white_px[i]) - float(black_px[i]) + float(white_px[i + 1]) - float(black_px[i + 1]) +
                                         float(white_px[i + 2]) - float(black_px[i + 2])) / (3.0f * 255.0f);
                        sum_a += std::min(1.0f, std::max(0.0f, 1.0f - d));
                        for (int c = 0; c < 3; ++c)
                            sum_c[c] += float(black_px[i + c]);
                    }
                    else {
                        sum_a += 1.0f;
                        for (int c = 0; c < 3; ++c)
                            sum_c[c] += float(white_px[i + c]);
                    }
                }
            }
            const float a = sum_a * inv_samples;
            const size_t o = ((size_t) y * (size_t) out_w + (size_t) x) * 4;
            for (int c = 0; c < 3; ++c) {
                // black picture = alpha * colour (premultiplied)
                float v = sum_c[c] * inv_samples;
                if (transparent)
                    v = a > 0.003f ? v / a : 0.0f;
                out.pixels[o + c] = (unsigned char) std::min(255.0f, std::max(0.0f, v + 0.5f));
            }
            out.pixels[o + 3] = (unsigned char) std::min(255.0f, std::max(0.0f, a * 255.0f + 0.5f));
        }
    }
    return true;
}

namespace Zoukei {

namespace {

constexpr const char* kSection = "zoukei_view";

bool          s_loaded = false;
bool          s_open   = false;
PhotoOptions  s_options;
std::string   s_status;

float read_float(const char* key, float fallback, float lo, float hi)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return fallback;
    const std::string v = cfg->get(kSection, key);
    if (v.empty())
        return fallback;
    return std::min(hi, std::max(lo, (float) std::atof(v.c_str())));
}

void save_options()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    const PhotoOptions& o = s_options;
    cfg->set(kSection, "photo_height", std::to_string(o.height_px));
    cfg->set(kSection, "photo_transparent", std::string(o.transparent ? "1" : "0"));
    cfg->set(kSection, "photo_shadows", std::string(o.shadows ? "1" : "0"));
    cfg->set(kSection, "photo_frame", std::string(o.frame_print ? "1" : "0"));
    cfg->set(kSection, "photo_light_az", std::to_string(o.light_azimuth));
    cfg->set(kSection, "photo_light_el", std::to_string(o.light_elevation));
    cfg->set(kSection, "photo_bg_r", std::to_string(o.background[0]));
    cfg->set(kSection, "photo_bg_g", std::to_string(o.background[1]));
    cfg->set(kSection, "photo_bg_b", std::to_string(o.background[2]));
}

void load_options()
{
    if (s_loaded)
        return;
    s_loaded = true;
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    PhotoOptions& o = s_options;
    const int h = (int) read_float("photo_height", 1080.f, 480.f, 4320.f);
    o.height_px = h >= 1800 ? 2160 : (h >= 1260 ? 1440 : 1080);
    if (cfg->has(kSection, "photo_transparent"))
        o.transparent = cfg->get(kSection, "photo_transparent") != "0";
    if (cfg->has(kSection, "photo_shadows"))
        o.shadows = cfg->get(kSection, "photo_shadows") != "0";
    if (cfg->has(kSection, "photo_frame"))
        o.frame_print = cfg->get(kSection, "photo_frame") != "0";
    o.light_azimuth   = read_float("photo_light_az", o.light_azimuth, -180.f, 180.f);
    o.light_elevation = read_float("photo_light_el", o.light_elevation, 5.f, 89.f);
    o.background[0]   = read_float("photo_bg_r", 1.f, 0.f, 1.f);
    o.background[1]   = read_float("photo_bg_g", 1.f, 0.f, 1.f);
    o.background[2]   = read_float("photo_bg_b", 1.f, 0.f, 1.f);
}

// The picture as a wxImage (top row first, with alpha)
wxImage to_image(const ThumbnailData& data)
{
    wxImage image((int) data.width, (int) data.height, false);
    image.InitAlpha();
    unsigned char* rgb   = image.GetData();
    unsigned char* alpha = image.GetAlpha();
    for (unsigned int y = 0; y < data.height; ++y) {
        const size_t src_row = (size_t) (data.height - 1 - y) * data.width * 4;
        for (unsigned int x = 0; x < data.width; ++x) {
            const size_t s = src_row + (size_t) x * 4;
            const size_t d = (size_t) y * data.width + x;
            rgb[d * 3 + 0] = data.pixels[s + 0];
            rgb[d * 3 + 1] = data.pixels[s + 1];
            rgb[d * 3 + 2] = data.pixels[s + 2];
            alpha[d]       = data.pixels[s + 3];
        }
    }
    return image;
}

// Runs from the event loop (never inside a frame): the render needs the GL context to itself.
void do_photo(GLCanvas3D* canvas, PhotoOptions options, bool to_clipboard)
{
    if (canvas == nullptr)
        return;
    std::string path;
    if (!to_clipboard) {
        wxFileDialog dialog(nullptr, _L("Save photo"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()), "photo.png",
                            "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK)
            return;
        path = dialog.GetPath().ToUTF8().data();
    }

    ThumbnailData data;
    if (!canvas->photo_capture(options, data) || !data.is_valid()) {
        s_status = _u8L("Photo mode could not render the preview.");
        canvas->set_as_dirty();
        return;
    }
    const wxImage image = to_image(data);
    if (to_clipboard) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxBitmapDataObject(wxBitmap(image, 32)));
            wxTheClipboard->Close();
            s_status = _u8L("Image copied to the clipboard.");
        }
        else
            s_status = _u8L("The clipboard could not be opened.");
    }
    else {
        if (image.SaveFile(wxString::FromUTF8(path.c_str()), wxBITMAP_TYPE_PNG))
            s_status = _u8L("Saved: ") + path;
        else
            s_status = _u8L("The file could not be written.");
    }
    canvas->set_as_dirty();
    canvas->request_extra_frame();
}

} // namespace

PhotoOptions& photo_options()
{
    load_options();
    return s_options;
}

bool photo_panel_open()
{
    return s_open;
}

void set_photo_panel_open(bool open)
{
    s_open = open;
}

void render_photo_panel(ImGuiWrapper& imgui, GLCanvas3D& canvas, int canvas_width, int canvas_height, float scale)
{
    if (!s_open)
        return;
    load_options();
    PhotoOptions& o = s_options;

    const float width = 300.f * scale;
    imgui.set_next_window_pos(float(canvas_width) * 0.5f, float(canvas_height) * 0.5f, ImGuiCond_FirstUseEver, 0.5f, 0.5f);
    ImGui::SetNextWindowSize(ImVec2(width, 0.f), ImGuiCond_Always);
    bool open = true;
    const std::string title = _u8L("Photo mode") + "###zk_photo_mode";
    if (imgui.begin(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        bool changed = false;

        ImGui::TextUnformatted(_u8L("Resolution").c_str());
        ImGui::SameLine(110.f * scale);
        const char* res_names[] = { "1080p", "1440p", "2160p" };
        const int res_values[]  = { 1080, 1440, 2160 };
        int res = o.height_px >= 2160 ? 2 : (o.height_px >= 1440 ? 1 : 0);
        ImGui::SetNextItemWidth(width - 130.f * scale);
        if (ImGui::Combo("##zk_photo_res", &res, res_names, 3)) {
            o.height_px = res_values[res];
            changed = true;
        }

        ImGui::TextUnformatted(_u8L("Background").c_str());
        ImGui::SameLine(110.f * scale);
        if (ImGui::RadioButton((_u8L("Transparent") + "##zk_photo_bg_t").c_str(), o.transparent)) {
            o.transparent = true;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton((_u8L("Colour") + "##zk_photo_bg_c").c_str(), !o.transparent)) {
            o.transparent = false;
            changed = true;
        }
        if (!o.transparent) {
            ImGui::SameLine();
            if (ImGui::ColorEdit3("##zk_photo_bg", o.background, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                changed = true;
        }

        if (ImGui::Checkbox((_u8L("Cast shadow") + "##zk_photo_shadow").c_str(), &o.shadows))
            changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox((_u8L("Frame the print") + "##zk_photo_frame").c_str(), &o.frame_print))
            changed = true;

        ImGui::TextUnformatted(_u8L("Light").c_str());
        ImGui::SameLine(110.f * scale);
        ImGui::SetNextItemWidth(width - 130.f * scale);
        if (ImGui::SliderFloat("##zk_photo_az", &o.light_azimuth, -180.f, 180.f, "%.0f deg"))
            changed = true;
        ImGui::TextUnformatted(_u8L("Light height").c_str());
        ImGui::SameLine(110.f * scale);
        ImGui::SetNextItemWidth(width - 130.f * scale);
        if (ImGui::SliderFloat("##zk_photo_el", &o.light_elevation, 5.f, 89.f, "%.0f deg"))
            changed = true;
        if (changed)
            save_options();

        ImGui::Separator();
        GLCanvas3D* cv = &canvas;
        if (ImGui::Button((_u8L("Save PNG...") + "##zk_photo_save").c_str())) {
            const PhotoOptions copy = o;
            wxGetApp().CallAfter([cv, copy]() { do_photo(cv, copy, false); });
        }
        ImGui::SameLine();
        if (ImGui::Button((_u8L("Copy image") + "##zk_photo_copy").c_str())) {
            const PhotoOptions copy = o;
            wxGetApp().CallAfter([cv, copy]() { do_photo(cv, copy, true); });
        }
        // Orca Zoukei: the full studio (lights, backgrounds, 8K, batch views, videos) in its own window
        ImGui::SameLine();
        if (ImGui::Button((_u8L("Photo Studio...") + "##zk_photo_studio").c_str()))
            wxGetApp().CallAfter([]() { open_photo_studio(); });

        if (!s_status.empty()) {
            ImGui::PushTextWrapPos(width - 12.f * scale);
            ImGui::TextUnformatted(s_status.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ImGui::PushTextWrapPos(width - 12.f * scale);
        ImGui::TextDisabled("%s", _u8L("Materials (PLA Silk, matte PLA, PETG), studio lights, 8K and video: Photo Studio.").c_str());
        ImGui::TextDisabled("%s", _u8L("Presentation only: the G-code and the preview are not changed. Ported from OrcaFS-NeotkoCM (Neotko).").c_str());
        ImGui::PopTextWrapPos();
    }
    imgui.end();
    if (!open)
        s_open = false;
}

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
