// Orca Zoukei: Photo Studio window, see PhotoStudio.hpp.
//
// Why a separate window with its own GL canvas: the studio needs a large live view with its own camera, mouse
// handling and a native side panel, and must not disturb the preview canvas (its camera, its ImGui, its render
// loop). Orca creates one wxGLContext for the whole application, so a second wxGLCanvas made with the same pixel
// format can make that context current and draw the preview's toolpaths (same buffers, same VAOs) from its own
// camera. Everything the studio changes in the G-code viewer is restored after each of its renders.
#include "PhotoStudio.hpp"
#include "StudioMedia.hpp"
#include "StudioRender.hpp"

#include <glad/gl.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#include <boost/algorithm/string/case_conv.hpp>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/clrpicker.h>
#include <wx/dataobj.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/glcanvas.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/progdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <boost/functional/hash.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

namespace {

constexpr const char* kSection = "zoukei_studio";

struct Palette
{
    wxColour bg, panel, fg, muted, accent;
};

Palette palette()
{
    if (wxGetApp().dark_mode())
        return { wxColour("#242428"), wxColour("#2D2D31"), wxColour("#EFEFF0"), wxColour("#A0A0A6"), wxColour("#00A693") };
    return { wxColour("#F4F4F5"), wxColour("#FFFFFF"), wxColour("#363636"), wxColour("#7C7C7C"), wxColour("#009688") };
}

const double kAspects[] = { 0.0, 1.0, 4.0 / 5.0, 4.0 / 3.0, 3.0 / 2.0, 16.0 / 9.0, 9.0 / 16.0 };
const int    kStillEdges[] = { 1920, 2560, 3840, 7680 };
const int    kStillSS[] = { 1, 2, 4 };
const int    kVideoEdges[] = { 854, 1280, 1920, 2560, 3840 };
const int    kVideoFps[] = { 12, 15, 24, 25, 30, 50, 60 };

enum class VideoFormat { Mp4, Gif, PngSequence };

// Everything about the exports that is not the scene.
struct ExportOptions
{
    int         aspect_idx{ 0 };
    bool        thirds{ true };
    int         res_idx{ 2 };
    int         aa_idx{ 2 };
    bool        watermark{ false };
    std::string wm_text{ "Orca Zoukei" };
    std::string wm_logo;
    int         wm_pos{ 0 };
    float       wm_opacity{ 0.6f };
    float       wm_size{ 0.14f };
    bool        caption{ false };
    int         footer_style{ 0 };
    int         vid_format{ 0 };
    int         vid_kind{ 0 };
    int         vid_seconds{ 8 };
    int         vid_fps_idx{ 4 };
    int         vid_res_idx{ 2 };
    int         vid_aa{ 1 };
    bool        tl_orbit{ true };
};

// Name, print time, filament weight and cost, filament colours of the plate's slice.
struct CaptionInfo
{
    wxString              title;
    wxString              details;
    std::vector<wxColour> colours;
};

CaptionInfo gather_caption()
{
    CaptionInfo info;
    Plater*     plater = wxGetApp().plater();
    if (plater == nullptr)
        return info;
    const Print& print = plater->get_partplate_list().get_current_fff_print();
    std::vector<std::string> names;
    for (const PrintObject* po : print.objects()) {
        if (po == nullptr || po->model_object() == nullptr)
            continue;
        const std::string& n = po->model_object()->name;
        if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    }
    std::string title;
    if (names.size() == 1)
        title = names.front();
    else if (names.size() <= 3) {
        for (size_t i = 0; i < names.size(); ++i)
            title += (i > 0 ? ", " : "") + names[i];
    }
    else
        title = std::to_string(names.size()) + " " + _u8L("objects");
    // without the file extension
    for (const char* ext : { ".stl", ".STL", ".3mf", ".3MF", ".obj", ".OBJ", ".step", ".STEP", ".stp" })
        if (title.size() > std::strlen(ext) && title.compare(title.size() - std::strlen(ext), std::string::npos, ext) == 0)
            title.erase(title.size() - std::strlen(ext));
    info.title = wxString::FromUTF8(title);

    const PrintStatistics&      ps     = print.print_statistics();
    PartPlate*                  plate  = plater->get_partplate_list().get_curr_plate();
    const GCodeProcessorResult* result = plate != nullptr ? plate->get_slice_result() : nullptr;
    std::vector<wxString>       parts;
    if (result != nullptr) {
        const float t = result->print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        if (t > 0.0f)
            parts.push_back(wxString::FromUTF8(short_time(get_time_dhms(t))));
    }
    if (ps.total_weight > 0.0)
        parts.push_back(wxString::Format("%.1f g", ps.total_weight));
    if (ps.total_cost > 0.0)
        parts.push_back(_L("Cost") + wxString::Format(" %.2f", ps.total_cost));
    for (size_t i = 0; i < parts.size(); ++i)
        info.details += (i > 0 ? "   |   " : "") + parts[i];

    const std::vector<std::string> colours = plater->get_extruder_colors_from_plater_config(result);
    std::set<size_t>               used;
    for (const auto& kv : ps.filament_stats)
        if (kv.second > 0.0)
            used.insert(kv.first);
    if (used.empty() && result != nullptr)
        for (const auto& kv : result->print_statistics.total_volumes_per_extruder)
            if (kv.second > 0.0)
                used.insert(kv.first);
    for (size_t id : used)
        if (id < colours.size()) {
            wxColour c(wxString::FromUTF8(colours[id]));
            if (c.IsOk())
                info.colours.push_back(c);
        }
    return info;
}

std::string sanitize(const wxString& name)
{
    std::string out;
    for (const char ch : std::string(name.ToUTF8().data())) {
        const unsigned char u = (unsigned char) ch;
        if (u < 32 || std::strchr("<>:\"/\\|?*", ch) != nullptr)
            out += '_';
        else
            out += ch;
    }
    if (out.empty() || out.size() > 80)
        out = "photo";
    return out;
}

double luminance(const wxColour& c) { return (0.2126 * c.Red() + 0.7152 * c.Green() + 0.0722 * c.Blue()) / 255.0; }

wxImage to_wx(const std::vector<uint8_t>& rgba, int w, int h)
{
    wxImage img(w, h, false);
    img.InitAlpha();
    unsigned char* rgb   = img.GetData();
    unsigned char* alpha = img.GetAlpha();
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
        alpha[i]       = rgba[i * 4 + 3];
    }
    return img;
}

std::vector<uint8_t> from_wx(const wxImage& img)
{
    const int            w = img.GetWidth(), h = img.GetHeight();
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
    const unsigned char* rgb   = img.GetData();
    const unsigned char* alpha = img.HasAlpha() ? img.GetAlpha() : nullptr;
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        out[i * 4 + 0] = rgb[i * 3 + 0];
        out[i * 4 + 1] = rgb[i * 3 + 1];
        out[i * 4 + 2] = rgb[i * 3 + 2];
        out[i * 4 + 3] = alpha != nullptr ? alpha[i] : 255;
    }
    return out;
}

// ---- the dialog -----------------------------------------------------------------------------------

class StudioDialog;
StudioDialog* s_dialog = nullptr;

class StudioDialog : public DPIDialog
{
public:
    explicit StudioDialog(wxWindow* parent);
    ~StudioDialog() override;

    void on_dpi_changed(const wxRect&) override
    {
        Layout();
        Refresh();
    }

private:
    // view
    bool   make_current();
    void   on_paint();
    void   on_mouse(wxMouseEvent& evt);
    wxSize canvas_px() const;
    double frame_aspect() const;
    std::array<float, 4> frame_rect(int cw, int ch) const;
    void   init_camera();
    void   refit();
    void   set_view(double yaw, double pitch);

    // panel
    void build_panel(wxWindow* panel);
    wxFlexGridSizer* add_section(wxWindow* panel, wxSizer* top, const wxString& title);
    void add_slider(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, float* value, float lo, float hi, float step,
                    const char* fmt);
    void add_check(wxWindow* parent, wxSizer* sizer, const wxString& label, bool* value, std::function<void()> after = nullptr);
    void add_choice(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxArrayString& items, int* value,
                    std::function<void()> after = nullptr);
    void add_colour(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, std::array<float, 3>* value);
    void sync_controls();
    void changed();
    void set_status(const wxString& text);

    // persistence
    void load_settings();
    void save_settings();

    // export
    void   output_size(int long_edge, int& w, int& h, bool even) const;
    wxImage compose(const std::vector<uint8_t>& rgba, int w, int h, const CaptionInfo& caption) const;
    bool   render_picture(const StudioCamera& cam, int w, int h, int ss, int layer_top, std::vector<uint8_t>& rgba,
                          wxProgressDialog* progress, int step, int steps);
    void   export_still(bool clipboard);
    void   export_batch();
    void   export_video();
    std::vector<VideoFormat> video_formats() const;

    wxGLCanvas*      m_canvas{ nullptr };
    StudioRenderer   m_renderer;
    StudioSettings   m_s;
    StudioCamera     m_cam;
    ExportOptions    m_x;
    bool             m_camera_ready{ false };
    bool             m_busy{ false };
    wxPoint          m_drag_start;
    wxPoint          m_last_mouse;
    wxStaticText*    m_status{ nullptr };
    wxStaticText*    m_info{ nullptr };
    std::vector<std::function<void()>> m_syncers;
    wxTimer          m_watch;
    size_t           m_watch_key{ 0 };
    int              m_light_preset{ 0 };
    // the choices and the lens, as the controls edit them
    int              m_proj_idx{ 0 };
    int              m_bg_idx{ 0 };
    int              m_mat_idx{ 1 };
    float            m_fov{ 24.0f };
};

StudioDialog::StudioDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Photo Studio"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX)
{
    const Palette pal = palette();
    SetBackgroundColour(pal.bg);
    SetForegroundColour(pal.fg);
    SetFont(Label::Body_13);
    load_settings();

    auto* top = new wxBoxSizer(wxHORIZONTAL);

    // the live view, on the application's GL context
    m_canvas = OpenGLManager::create_wxglcanvas(*this);
    m_canvas->SetName("ZoukeiPhotoStudioCanvas"); // not a scene canvas: no ImGui, no plater shortcuts
    m_canvas->SetMinSize(FromDIP(wxSize(640, 480)));
    top->Add(m_canvas, 1, wxEXPAND);

    auto* panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(380, -1)), wxVSCROLL);
    panel->SetBackgroundColour(pal.panel);
    panel->SetScrollRate(0, FromDIP(14));
    build_panel(panel);
    top->Add(panel, 0, wxEXPAND);

    SetSizer(top);

    m_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { on_paint(); });
    m_canvas->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
    m_canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        m_canvas->Refresh(false);
        e.Skip();
    });
    m_canvas->Bind(wxEVT_LEFT_DOWN, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_LEFT_UP, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_RIGHT_DOWN, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_RIGHT_UP, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_MIDDLE_DOWN, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_MIDDLE_UP, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_MOTION, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_MOUSEWHEEL, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_LEFT_DCLICK, &StudioDialog::on_mouse, this);
    m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});

    // Follow the preview: a new slice, another view type or layer range repaints the studio.
    m_watch.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (m_busy || !IsShown())
            return;
        size_t key = 0;
        if (GCodeViewer* gv = StudioRenderer::gcode_viewer()) {
            boost::hash_combine(key, gv->has_data());
            boost::hash_combine(key, (int) gv->get_view_type());
            boost::hash_combine(key, gv->get_layers_count());
            boost::hash_combine(key, gv->get_layers_z_range()[0]);
            boost::hash_combine(key, gv->get_layers_z_range()[1]);
            boost::hash_combine(key, gv->get_gcode_view_visible_range()[1]);
            const BoundingBoxf3 b = StudioRenderer::print_box();
            boost::hash_combine(key, b.defined ? b.max.z() : 0.0);
        }
        if (key != m_watch_key) {
            const bool had_print = m_camera_ready;
            m_watch_key = key;
            if (!had_print && StudioRenderer::has_print())
                init_camera();
            m_canvas->Refresh(false);
        }
    }, m_watch.GetId());
    m_watch.Start(400);

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        if (m_busy) {
            e.Veto();
            return;
        }
        save_settings();
        m_watch.Stop();
        if (make_current())
            m_renderer.release();
        s_dialog = nullptr;
        Destroy();
    });

    wxGetApp().UpdateDlgDarkUI(this);
    SetSize(FromDIP(wxSize(1280, 820)));
    SetMinSize(FromDIP(wxSize(900, 560)));
    CenterOnParent();
    init_camera();
    sync_controls();
}

StudioDialog::~StudioDialog()
{
    if (s_dialog == this)
        s_dialog = nullptr;
}

bool StudioDialog::make_current()
{
    if (m_canvas == nullptr)
        return false;
    wxGLContext* ctx = wxGetApp().init_glcontext(*m_canvas);
    return ctx != nullptr && m_canvas->SetCurrent(*ctx);
}

wxSize StudioDialog::canvas_px() const
{
    wxSize s = m_canvas->GetClientSize();
#ifdef __APPLE__
    const double f = m_canvas->GetContentScaleFactor();
    s = wxSize(int(s.x * f), int(s.y * f));
#endif
    return wxSize(std::max(1, s.x), std::max(1, s.y));
}

double StudioDialog::frame_aspect() const
{
    const double a = kAspects[std::clamp(m_x.aspect_idx, 0, int(std::size(kAspects)) - 1)];
    if (a > 0.0)
        return a;
    const wxSize s = canvas_px();
    return double(s.x) / double(s.y);
}

// The frame inside the live view, pixels, bottom-left origin: x0, y0, x1, y1.
std::array<float, 4> StudioDialog::frame_rect(int cw, int ch) const
{
    const double a      = frame_aspect();
    const double margin = kAspects[m_x.aspect_idx] > 0.0 ? 0.92 : 1.0;
    double fw = cw * margin, fh = fw / a;
    if (fh > ch * margin) {
        fh = ch * margin;
        fw = fh * a;
    }
    const double x0 = 0.5 * (cw - fw), y0 = 0.5 * (ch - fh);
    return { float(x0), float(y0), float(x0 + fw), float(y0 + fh) };
}

void StudioDialog::init_camera()
{
    if (!StudioRenderer::has_print())
        return;
    // start from the direction the preview is looked at
    const Camera& pc  = wxGetApp().plater()->get_camera();
    const Vec3d   dir = -pc.get_dir_forward();
    m_cam.pitch_deg   = std::clamp(std::asin(std::clamp(dir.z(), -1.0, 1.0)) * 180.0 / PI, 5.0, 85.0);
    m_cam.yaw_deg     = std::atan2(dir.x(), -dir.y()) * 180.0 / PI;
    fit_camera(m_cam, StudioRenderer::print_box(), frame_aspect());
    m_camera_ready = true;
}

void StudioDialog::refit()
{
    m_cam.fov_deg = m_fov;
    fit_camera(m_cam, StudioRenderer::print_box(), frame_aspect());
    m_canvas->Refresh(false);
}

void StudioDialog::set_view(double yaw, double pitch)
{
    m_cam.yaw_deg   = yaw;
    m_cam.pitch_deg = pitch;
    refit();
}

void StudioDialog::on_paint()
{
    wxPaintDC dc(m_canvas);
    if (m_busy || !make_current())
        return;
    const wxSize sz = canvas_px();
    m_cam.fov_deg = m_fov;
    if (!m_camera_ready && StudioRenderer::has_print())
        init_camera();
    const std::array<float, 4> fr = frame_rect(sz.x, sz.y);
    const double fw = std::max(1.0f, fr[2] - fr[0]), fh = std::max(1.0f, fr[3] - fr[1]);
    FrameWindow  win;
    win.x0 = -1.0 - 2.0 * fr[0] / fw;
    win.x1 = -1.0 + 2.0 * (sz.x - fr[0]) / fw;
    win.y0 = -1.0 - 2.0 * fr[1] / fh;
    win.y1 = -1.0 + 2.0 * (sz.y - fr[1]) / fh;
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
    m_renderer.set_shadow_resolution(2048);
    const bool ok = m_renderer.render(m_s, m_cam, frame_aspect(), win, sz.x, sz.y, true);
    if (ok || StudioRenderer::has_print())
        m_renderer.render_guides(sz.x, sz.y, fr, m_x.thirds, wxGetApp().dark_mode());
    m_canvas->SwapBuffers();
    if (!ok && m_info != nullptr) {
        const std::string& err = m_renderer.last_error();
        m_info->SetLabel(err == "no print" ? _L("Slice the plate: the studio shows the sliced print of the preview.") :
                                             wxString::FromUTF8(err));
    }
    else if (m_info != nullptr && !m_info->GetLabel().empty())
        m_info->SetLabel("");
}

void StudioDialog::on_mouse(wxMouseEvent& evt)
{
    if (m_busy)
        return;
    const wxPoint pos = evt.GetPosition();
    if (evt.LeftDClick()) {
        refit();
        return;
    }
    if (evt.ButtonDown()) {
        m_last_mouse = pos;
        m_canvas->SetFocus();
        if (!m_canvas->HasCapture())
            m_canvas->CaptureMouse();
        return;
    }
    if (evt.ButtonUp()) {
        if (m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
        save_settings();
        return;
    }
    if (evt.GetEventType() == wxEVT_MOUSEWHEEL) {
        const double steps = double(evt.GetWheelRotation()) / double(std::max(1, evt.GetWheelDelta()));
        m_cam.distance = std::clamp(m_cam.distance * std::pow(1.12, -steps), 1.0, 1.0e5);
        m_canvas->Refresh(false);
        return;
    }
    if (evt.Dragging()) {
        const wxPoint d = pos - m_last_mouse;
        m_last_mouse    = pos;
        if (evt.LeftIsDown() && (evt.ControlDown() || evt.CmdDown())) {
            // Ctrl + drag moves the key light around the subject
            m_s.key.azimuth   = float(std::fmod(m_s.key.azimuth + d.x * 0.5 + 540.0, 360.0) - 180.0);
            m_s.key.elevation = std::clamp(m_s.key.elevation - float(d.y) * 0.4f, 0.0f, 90.0f);
            sync_controls();
        }
        else if (evt.LeftIsDown()) {
            m_cam.yaw_deg   = std::fmod(m_cam.yaw_deg - d.x * 0.4, 360.0);
            m_cam.pitch_deg = std::clamp(m_cam.pitch_deg + d.y * 0.3, -30.0, 90.0);
        }
        else if (evt.RightIsDown() || evt.MiddleIsDown()) {
            const wxSize sz = m_canvas->GetClientSize();
            const std::array<float, 4> fr = frame_rect(sz.x, sz.y);
            const double half_h   = m_cam.distance * std::tan(0.5 * m_cam.fov_deg * PI / 180.0);
            const double per_px   = 2.0 * half_h / std::max(1.0f, fr[3] - fr[1]);
            const Vec3d  eye_dir  = (m_cam.eye() - m_cam.target).normalized();
            const Vec3d  up       = m_cam.up();
            const Vec3d  right    = up.cross(eye_dir).normalized();
            m_cam.target += (-double(d.x) * right + double(d.y) * up) * per_px;
        }
        m_canvas->Refresh(false);
    }
}

// ---- panel ----------------------------------------------------------------------------------------

wxFlexGridSizer* StudioDialog::add_section(wxWindow* panel, wxSizer* top, const wxString& title)
{
    const Palette pal = palette();
    auto* head = new wxStaticText(panel, wxID_ANY, title);
    head->SetFont(Label::Head_14);
    head->SetForegroundColour(pal.accent);
    top->Add(head, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* grid = new wxFlexGridSizer(3, FromDIP(4), FromDIP(8));
    grid->AddGrowableCol(1, 1);
    top->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    return grid;
}

void StudioDialog::add_slider(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, float* value, float lo, float hi, float step,
                              const char* fmt)
{
    auto* lbl = new wxStaticText(parent, wxID_ANY, label);
    const int n   = std::max(1, int(std::lround((hi - lo) / step)));
    auto to_int   = [=]() { return std::clamp(int(std::lround((*value - lo) / step)), 0, n); };
    auto* slider  = new wxSlider(parent, wxID_ANY, to_int(), 0, n, wxDefaultPosition, wxSize(FromDIP(150), -1));
    auto* val     = new wxStaticText(parent, wxID_ANY, wxString::Format(fmt, double(*value)), wxDefaultPosition, wxSize(FromDIP(58), -1),
                                     wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
    val->SetForegroundColour(palette().muted);
    grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(slider, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    grid->Add(val, 0, wxALIGN_CENTER_VERTICAL);
    const std::string format(fmt);
    slider->Bind(wxEVT_SLIDER, [=](wxCommandEvent&) {
        *value = lo + step * float(slider->GetValue());
        val->SetLabel(wxString::Format(wxString(format), double(*value)));
        m_canvas->Refresh(false);
    });
    slider->Bind(wxEVT_SCROLL_CHANGED, [this](wxScrollEvent& e) {
        save_settings();
        e.Skip();
    });
    m_syncers.push_back([=]() {
        slider->SetValue(to_int());
        val->SetLabel(wxString::Format(wxString(format), double(*value)));
    });
}

void StudioDialog::add_check(wxWindow* parent, wxSizer* sizer, const wxString& label, bool* value, std::function<void()> after)
{
    auto* cb = new wxCheckBox(parent, wxID_ANY, label);
    cb->SetValue(*value);
    sizer->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    cb->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) {
        *value = cb->GetValue();
        if (after)
            after();
        changed();
    });
    m_syncers.push_back([=]() { cb->SetValue(*value); });
}

void StudioDialog::add_choice(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxArrayString& items, int* value,
                              std::function<void()> after)
{
    auto* lbl    = new wxStaticText(parent, wxID_ANY, label);
    auto* choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);
    choice->SetSelection(std::clamp(*value, 0, int(items.size()) - 1));
    grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(choice, 1, wxEXPAND);
    grid->AddSpacer(1);
    choice->Bind(wxEVT_CHOICE, [=](wxCommandEvent&) {
        *value = choice->GetSelection();
        if (after)
            after();
        changed();
    });
    m_syncers.push_back([=]() { choice->SetSelection(std::clamp(*value, 0, int(choice->GetCount()) - 1)); });
}

void StudioDialog::add_colour(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, std::array<float, 3>* value)
{
    auto to_wx_colour = [value]() {
        return wxColour((unsigned char) std::lround((*value)[0] * 255.0f), (unsigned char) std::lround((*value)[1] * 255.0f),
                        (unsigned char) std::lround((*value)[2] * 255.0f));
    };
    auto* lbl    = new wxStaticText(parent, wxID_ANY, label);
    auto* picker = new wxColourPickerCtrl(parent, wxID_ANY, to_wx_colour());
    grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(picker, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(1);
    picker->Bind(wxEVT_COLOURPICKER_CHANGED, [=](wxColourPickerEvent& e) {
        const wxColour c = e.GetColour();
        *value = { c.Red() / 255.0f, c.Green() / 255.0f, c.Blue() / 255.0f };
        changed();
    });
    m_syncers.push_back([=]() { picker->SetColour(to_wx_colour()); });
}

void StudioDialog::sync_controls()
{
    for (auto& s : m_syncers)
        s();
    m_canvas->Refresh(false);
}

void StudioDialog::changed()
{
    save_settings();
    m_canvas->Refresh(false);
}

void StudioDialog::set_status(const wxString& text)
{
    if (m_status != nullptr) {
        m_status->SetLabel(text);
        m_status->Wrap(FromDIP(340));
        m_status->GetParent()->Layout();
    }
}

void StudioDialog::build_panel(wxWindow* panel)
{
    const Palette pal = palette();
    auto* top = new wxBoxSizer(wxVERTICAL);

    m_info = new wxStaticText(panel, wxID_ANY, "");
    m_info->SetForegroundColour(pal.muted);
    top->Add(m_info, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // ---- view and framing ----
    {
        auto* grid = add_section(panel, top, _L("View and framing"));
        auto* views = new wxBoxSizer(wxHORIZONTAL);
        auto add_view_btn = [&](const wxString& label, std::function<void()> fn) {
            auto* b = new Button(panel, label);
            b->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
            b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
            views->Add(b, 0, wxRIGHT, FromDIP(4));
        };
        add_view_btn(_L("Front"), [this]() { set_view(0.0, 10.0); });
        add_view_btn(_L("Side"), [this]() { set_view(90.0, 10.0); });
        add_view_btn(_L("Top"), [this]() { set_view(0.0, 90.0); });
        add_view_btn(_L("Iso"), [this]() { set_view(45.0, 35.264); });
        add_view_btn(_L("Fit"), [this]() { refit(); });
        grid->Add(new wxStaticText(panel, wxID_ANY, _L("Camera")), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(views, 0);
        grid->AddSpacer(1);

        wxArrayString aspects;
        aspects.Add(_L("Window"));
        for (const char* a : { "1:1", "4:5", "4:3", "3:2", "16:9", "9:16" })
            aspects.Add(a);
        add_choice(panel, grid, _L("Frame"), aspects, &m_x.aspect_idx, [this]() { refit(); });
        wxArrayString proj;
        proj.Add(_L("Perspective"));
        proj.Add(_L("Orthographic"));
        m_proj_idx = m_cam.ortho ? 1 : 0;
        add_choice(panel, grid, _L("Projection"), proj, &m_proj_idx, [this]() {
            m_cam.ortho = m_proj_idx == 1;
            refit();
        });
        m_fov = float(m_cam.fov_deg);
        add_slider(panel, grid, _L("Lens (field of view)"), &m_fov, 8.0f, 60.0f, 1.0f, "%.0f deg");
        auto* checks = new wxBoxSizer(wxHORIZONTAL);
        add_check(panel, checks, _L("Rule of thirds"), &m_x.thirds);
        add_check(panel, checks, _L("Whole print"), &m_s.whole_print);
        top->Add(checks, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        auto* hint = new wxStaticText(panel, wxID_ANY,
                                      _L("Drag: orbit. Right or middle drag: pan. Wheel: zoom. Double click: fit. Ctrl + drag: move the key light."));
        hint->SetForegroundColour(pal.muted);
        hint->SetFont(Label::Body_12);
        hint->Wrap(FromDIP(340));
        top->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    }

    // ---- lights ----
    {
        auto* grid = add_section(panel, top, _L("Lights"));
        wxArrayString presets;
        presets.Add(_L("Soft studio"));
        presets.Add(_L("Dramatic"));
        presets.Add(_L("High key"));
        presets.Add(_L("Like the preview"));
        add_choice(panel, grid, _L("Preset"), presets, &m_light_preset, [this]() {
            apply_light_preset(m_s, StudioLightPreset(m_light_preset));
            sync_controls();
        });
        add_slider(panel, grid, _L("Ambient"), &m_s.ambient, 0.0f, 1.5f, 0.01f, "%.2f");
        auto* follow = new wxBoxSizer(wxHORIZONTAL);
        add_check(panel, follow, _L("Lights follow the camera"), &m_s.lights_follow_camera);
        top->Add(follow, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

        auto add_light = [&](const wxString& name, StudioLight& l) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            add_check(panel, row, name, &l.on);
            top->Add(row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
            auto* g = new wxFlexGridSizer(3, FromDIP(4), FromDIP(8));
            g->AddGrowableCol(1, 1);
            add_slider(panel, g, _L("Intensity"), &l.intensity, 0.0f, 3.0f, 0.01f, "%.2f");
            add_slider(panel, g, _L("Azimuth"), &l.azimuth, -180.0f, 180.0f, 1.0f, "%.0f deg");
            add_slider(panel, g, _L("Elevation"), &l.elevation, 0.0f, 90.0f, 1.0f, "%.0f deg");
            add_slider(panel, g, _L("Temperature"), &l.kelvin, 2500.0f, 10000.0f, 100.0f, "%.0f K");
            top->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
        };
        add_light(_L("Key light"), m_s.key);
        add_light(_L("Fill light"), m_s.fill);
        add_light(_L("Rim light"), m_s.rim);
    }

    // ---- background and floor ----
    {
        auto* grid = add_section(panel, top, _L("Background and floor"));
        wxArrayString bgs;
        bgs.Add(_L("White cyclorama"));
        bgs.Add(_L("Dark cyclorama"));
        bgs.Add(_L("Gradient"));
        bgs.Add(_L("Solid colour"));
        bgs.Add(_L("Transparent"));
        m_bg_idx = int(m_s.background);
        add_choice(panel, grid, _L("Background"), bgs, &m_bg_idx, [this]() { m_s.background = StudioBackground(m_bg_idx); });
        add_colour(panel, grid, _L("Cyclorama colour"), &m_s.cyclorama_color);
        add_colour(panel, grid, _L("Gradient top"), &m_s.gradient_top);
        add_colour(panel, grid, _L("Gradient bottom"), &m_s.gradient_bottom);
        add_colour(panel, grid, _L("Solid colour"), &m_s.solid_color);
        auto* checks = new wxBoxSizer(wxHORIZONTAL);
        add_check(panel, checks, _L("Floor shadow"), &m_s.floor_shadow);
        add_check(panel, checks, _L("Contact shadow"), &m_s.contact_shadow);
        top->Add(checks, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        auto* checks2 = new wxBoxSizer(wxHORIZONTAL);
        add_check(panel, checks2, _L("Self shadows"), &m_s.self_shadows);
        add_check(panel, checks2, _L("Reflection"), &m_s.reflection);
        top->Add(checks2, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        auto* g = new wxFlexGridSizer(3, FromDIP(4), FromDIP(8));
        g->AddGrowableCol(1, 1);
        add_slider(panel, g, _L("Shadow strength"), &m_s.shadow_strength, 0.0f, 1.0f, 0.01f, "%.2f");
        add_slider(panel, g, _L("Shadow softness"), &m_s.shadow_softness, 0.0f, 1.0f, 0.01f, "%.2f");
        add_slider(panel, g, _L("Contact strength"), &m_s.contact_strength, 0.0f, 1.0f, 0.01f, "%.2f");
        add_slider(panel, g, _L("Reflection strength"), &m_s.reflection_strength, 0.0f, 1.0f, 0.01f, "%.2f");
        top->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    }

    // ---- material ----
    {
        auto* grid = add_section(panel, top, _L("Material"));
        wxArrayString mats;
        mats.Add(_L("Preview shading"));
        mats.Add(_L("Matte PLA"));
        mats.Add(_L("Silk sheen"));
        mats.Add(_L("PETG gloss"));
        m_mat_idx = int(m_s.material);
        add_choice(panel, grid, _L("Preset"), mats, &m_mat_idx, [this]() { m_s.material = StudioMaterial(m_mat_idx); });
        add_slider(panel, grid, _L("Exposure"), &m_s.exposure, 0.4f, 2.0f, 0.01f, "%.2f");
        add_slider(panel, grid, _L("Saturation"), &m_s.saturation, 0.0f, 2.0f, 0.01f, "%.2f");
        auto* note = new wxStaticText(panel, wxID_ANY, _L("The colours are the preview's: view type, RealColor, multi-colour stripes."));
        note->SetForegroundColour(pal.muted);
        note->SetFont(Label::Body_12);
        note->Wrap(FromDIP(340));
        top->Add(note, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    }

    // ---- photo ----
    {
        auto* grid = add_section(panel, top, _L("Photo"));
        wxArrayString res;
        res.Add("1080p (1920)");
        res.Add("1440p (2560)");
        res.Add("4K (3840)");
        res.Add("8K (7680)");
        add_choice(panel, grid, _L("Size (long edge)"), res, &m_x.res_idx);
        wxArrayString aa;
        aa.Add(_L("Off"));
        aa.Add("2x");
        aa.Add("4x");
        add_choice(panel, grid, _L("Anti-aliasing"), aa, &m_x.aa_idx);

        auto* checks = new wxBoxSizer(wxHORIZONTAL);
        add_check(panel, checks, _L("Caption footer"), &m_x.caption);
        add_check(panel, checks, _L("Watermark"), &m_x.watermark);
        top->Add(checks, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

        auto* g = new wxFlexGridSizer(3, FromDIP(4), FromDIP(8));
        g->AddGrowableCol(1, 1);
        wxArrayString footer;
        footer.Add(_L("Automatic"));
        footer.Add(_L("White"));
        footer.Add(_L("Black"));
        add_choice(panel, g, _L("Footer"), footer, &m_x.footer_style);
        g->Add(new wxStaticText(panel, wxID_ANY, _L("Watermark text")), 0, wxALIGN_CENTER_VERTICAL);
        auto* wm_text = new wxTextCtrl(panel, wxID_ANY, wxString::FromUTF8(m_x.wm_text));
        g->Add(wm_text, 1, wxEXPAND);
        g->AddSpacer(1);
        wm_text->Bind(wxEVT_TEXT, [this, wm_text](wxCommandEvent&) {
            m_x.wm_text = wm_text->GetValue().ToUTF8().data();
            save_settings();
        });
        g->Add(new wxStaticText(panel, wxID_ANY, _L("Logo")), 0, wxALIGN_CENTER_VERTICAL);
        auto* logo_row  = new wxBoxSizer(wxHORIZONTAL);
        auto* logo_name = new wxStaticText(panel, wxID_ANY, m_x.wm_logo.empty() ? _L("(none, the text is used)") :
                                                                                  wxFileName(wxString::FromUTF8(m_x.wm_logo)).GetFullName(),
                                           wxDefaultPosition, wxSize(FromDIP(120), -1), wxST_ELLIPSIZE_MIDDLE);
        logo_name->SetForegroundColour(pal.muted);
        auto* logo_btn   = new Button(panel, _L("Choose..."));
        auto* logo_clear = new Button(panel, _L("Clear"));
        logo_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        logo_clear->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        logo_row->Add(logo_name, 1, wxALIGN_CENTER_VERTICAL);
        logo_row->Add(logo_btn, 0, wxLEFT, FromDIP(4));
        logo_row->Add(logo_clear, 0, wxLEFT, FromDIP(4));
        g->Add(logo_row, 1, wxEXPAND);
        g->AddSpacer(1);
        logo_btn->Bind(wxEVT_BUTTON, [this, logo_name](wxCommandEvent&) {
            wxFileDialog dlg(this, _L("Choose a logo"), "", "", "PNG (*.png)|*.png|JPEG (*.jpg;*.jpeg)|*.jpg;*.jpeg",
                             wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() != wxID_OK)
                return;
            m_x.wm_logo = dlg.GetPath().ToUTF8().data();
            logo_name->SetLabel(dlg.GetFilename());
            save_settings();
        });
        logo_clear->Bind(wxEVT_BUTTON, [this, logo_name](wxCommandEvent&) {
            m_x.wm_logo.clear();
            logo_name->SetLabel(_L("(none, the text is used)"));
            save_settings();
        });
        wxArrayString pos;
        pos.Add(_L("Bottom right"));
        pos.Add(_L("Bottom left"));
        pos.Add(_L("Top right"));
        pos.Add(_L("Top left"));
        add_choice(panel, g, _L("Position"), pos, &m_x.wm_pos);
        add_slider(panel, g, _L("Opacity"), &m_x.wm_opacity, 0.1f, 1.0f, 0.01f, "%.2f");
        add_slider(panel, g, _L("Size"), &m_x.wm_size, 0.04f, 0.4f, 0.01f, "%.2f");
        top->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* save = new Button(panel, _L("Save PNG..."));
        save->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
        auto* copy = new Button(panel, _L("Copy"));
        copy->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        auto* batch = new Button(panel, _L("Batch views..."));
        batch->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        batch->SetToolTip(_L("Front, side, top, iso and the current view, each as a PNG, into a folder."));
        buttons->Add(save, 0, wxRIGHT, FromDIP(6));
        buttons->Add(copy, 0, wxRIGHT, FromDIP(6));
        buttons->Add(batch, 0);
        top->Add(buttons, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_still(false); });
        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_still(true); });
        batch->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_batch(); });
    }

    // ---- video ----
    {
        auto* grid = add_section(panel, top, _L("Video"));
        wxArrayString formats;
        for (VideoFormat f : video_formats())
            formats.Add(f == VideoFormat::Mp4 ? _L("MP4 (H.264)") : f == VideoFormat::Gif ? _L("Animated GIF") : _L("PNG sequence"));
        add_choice(panel, grid, _L("Format"), formats, &m_x.vid_format);
        wxArrayString kinds;
        kinds.Add(_L("Turntable 360"));
        kinds.Add(_L("Virtual timelapse"));
        add_choice(panel, grid, _L("Animation"), kinds, &m_x.vid_kind);
        grid->Add(new wxStaticText(panel, wxID_ANY, _L("Duration (s)")), 0, wxALIGN_CENTER_VERTICAL);
        auto* secs = new wxSpinCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 120, m_x.vid_seconds);
        grid->Add(secs, 0, wxEXPAND);
        grid->AddSpacer(1);
        secs->Bind(wxEVT_SPINCTRL, [this, secs](wxCommandEvent&) {
            m_x.vid_seconds = secs->GetValue();
            save_settings();
        });
        m_syncers.push_back([this, secs]() { secs->SetValue(m_x.vid_seconds); });
        wxArrayString fps;
        for (int f : kVideoFps)
            fps.Add(wxString::Format("%d fps", f));
        add_choice(panel, grid, _L("Frame rate"), fps, &m_x.vid_fps_idx);
        wxArrayString vres;
        for (const char* r : { "480p (854)", "720p (1280)", "1080p (1920)", "1440p (2560)", "2160p (3840)" })
            vres.Add(r);
        add_choice(panel, grid, _L("Size (long edge)"), vres, &m_x.vid_res_idx);
        wxArrayString vaa;
        vaa.Add(_L("Off"));
        vaa.Add("2x");
        add_choice(panel, grid, _L("Anti-aliasing"), vaa, &m_x.vid_aa);
        auto* checks = new wxBoxSizer(wxHORIZONTAL);
        add_check(panel, checks, _L("Orbit during the timelapse"), &m_x.tl_orbit);
        top->Add(checks, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
#ifndef _WIN32
        auto* mp4_note = new wxStaticText(panel, wxID_ANY, _L("MP4 export needs Windows; on this system the video is saved as an animated GIF or a PNG sequence."));
        mp4_note->SetForegroundColour(pal.muted);
        mp4_note->SetFont(Label::Body_12);
        mp4_note->Wrap(FromDIP(340));
        top->Add(mp4_note, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
#endif
        auto* vbtn = new Button(panel, _L("Export video..."));
        vbtn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
        top->Add(vbtn, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        vbtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_video(); });
    }

    m_status = new wxStaticText(panel, wxID_ANY, "");
    m_status->SetForegroundColour(pal.muted);
    top->Add(m_status, 0, wxALL, FromDIP(12));

    auto* credit = new wxStaticText(panel, wxID_ANY, _L("Presentation only: the G-code and the preview are not changed."));
    credit->SetForegroundColour(pal.muted);
    credit->SetFont(Label::Body_12);
    credit->Wrap(FromDIP(340));
    top->Add(credit, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    panel->SetSizer(top);
    // as wide as the widest row (long translations), with room for the scroll bar
    panel->SetMinSize(wxSize(std::max(FromDIP(380), top->GetMinSize().x + FromDIP(24)), -1));
    panel->FitInside();
}

// ---- persistence ----------------------------------------------------------------------------------

struct FloatKey
{
    const char* key;
    float*      v;
    float       lo, hi;
};

void StudioDialog::load_settings()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    auto get_f = [cfg](const char* key, float& v, float lo, float hi) {
        const std::string s = cfg->get(kSection, key);
        if (!s.empty())
            v = std::clamp(float(std::atof(s.c_str())), lo, hi);
    };
    auto get_b = [cfg](const char* key, bool& v) {
        const std::string s = cfg->get(kSection, key);
        if (!s.empty())
            v = s != "0";
    };
    auto get_i = [cfg](const char* key, int& v, int lo, int hi) {
        const std::string s = cfg->get(kSection, key);
        if (!s.empty())
            v = std::clamp(std::atoi(s.c_str()), lo, hi);
    };
    auto light = [&](const char* p, StudioLight& l) {
        const std::string k(p);
        get_b((k + "_on").c_str(), l.on);
        get_f((k + "_i").c_str(), l.intensity, 0.0f, 3.0f);
        get_f((k + "_az").c_str(), l.azimuth, -180.0f, 180.0f);
        get_f((k + "_el").c_str(), l.elevation, 0.0f, 90.0f);
        get_f((k + "_k").c_str(), l.kelvin, 2500.0f, 10000.0f);
    };
    light("key", m_s.key);
    light("fill", m_s.fill);
    light("rim", m_s.rim);
    get_f("ambient", m_s.ambient, 0.0f, 1.5f);
    get_b("follow", m_s.lights_follow_camera);
    int bg = int(m_s.background);
    get_i("background", bg, 0, int(StudioBackground::Count) - 1);
    m_s.background = StudioBackground(bg);
    for (int i = 0; i < 3; ++i) {
        const std::string n = std::to_string(i);
        get_f(("cyc" + n).c_str(), m_s.cyclorama_color[i], 0.0f, 1.0f);
        get_f(("gtop" + n).c_str(), m_s.gradient_top[i], 0.0f, 1.0f);
        get_f(("gbot" + n).c_str(), m_s.gradient_bottom[i], 0.0f, 1.0f);
        get_f(("solid" + n).c_str(), m_s.solid_color[i], 0.0f, 1.0f);
    }
    get_b("floor_shadow", m_s.floor_shadow);
    get_f("shadow_strength", m_s.shadow_strength, 0.0f, 1.0f);
    get_f("shadow_softness", m_s.shadow_softness, 0.0f, 1.0f);
    get_b("contact", m_s.contact_shadow);
    get_f("contact_strength", m_s.contact_strength, 0.0f, 1.0f);
    get_b("reflection", m_s.reflection);
    get_f("reflection_strength", m_s.reflection_strength, 0.0f, 1.0f);
    get_b("self_shadows", m_s.self_shadows);
    int mat = int(m_s.material);
    get_i("material", mat, 0, int(StudioMaterial::Count) - 1);
    m_s.material = StudioMaterial(mat);
    // Start from the filament on the plate: a silk filament opens with the silk sheen, PETG with the gloss.
    if (const PresetBundle* bundle = wxGetApp().preset_bundle) {
        bool silk = false, petg = false;
        const DynamicPrintConfig   full  = bundle->full_config();
        const ConfigOptionStrings* types = full.option<ConfigOptionStrings>("filament_type");
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            std::string name = boost::algorithm::to_lower_copy(bundle->filament_presets[i]);
            std::string type = (types != nullptr && i < types->values.size()) ? boost::algorithm::to_lower_copy(types->values[i]) : std::string();
            silk = silk || name.find("silk") != std::string::npos || name.find("seda") != std::string::npos || type.find("silk") != std::string::npos;
            petg = petg || type.find("petg") != std::string::npos;
        }
        if (silk)
            m_s.material = StudioMaterial::SilkSheen;
        else if (petg)
            m_s.material = StudioMaterial::PETGGloss;
    }
    get_f("exposure", m_s.exposure, 0.4f, 2.0f);
    get_f("saturation", m_s.saturation, 0.0f, 2.0f);
    get_b("whole_print", m_s.whole_print);
    float fov = float(m_cam.fov_deg);
    get_f("fov", fov, 8.0f, 60.0f);
    m_cam.fov_deg = fov;
    m_fov         = fov;
    get_i("light_preset", m_light_preset, 0, int(StudioLightPreset::Count) - 1);

    get_i("aspect", m_x.aspect_idx, 0, int(std::size(kAspects)) - 1);
    get_b("thirds", m_x.thirds);
    get_i("res", m_x.res_idx, 0, int(std::size(kStillEdges)) - 1);
    get_i("aa", m_x.aa_idx, 0, int(std::size(kStillSS)) - 1);
    get_b("watermark", m_x.watermark);
    if (cfg->has(kSection, "wm_text"))
        m_x.wm_text = cfg->get(kSection, "wm_text");
    m_x.wm_logo = cfg->get(kSection, "wm_logo");
    get_i("wm_pos", m_x.wm_pos, 0, 3);
    get_f("wm_opacity", m_x.wm_opacity, 0.1f, 1.0f);
    get_f("wm_size", m_x.wm_size, 0.04f, 0.4f);
    get_b("caption", m_x.caption);
    get_i("footer", m_x.footer_style, 0, 2);
    get_i("vid_format", m_x.vid_format, 0, int(video_formats().size()) - 1);
    get_i("vid_kind", m_x.vid_kind, 0, 1);
    get_i("vid_seconds", m_x.vid_seconds, 1, 120);
    get_i("vid_fps", m_x.vid_fps_idx, 0, int(std::size(kVideoFps)) - 1);
    get_i("vid_res", m_x.vid_res_idx, 0, int(std::size(kVideoEdges)) - 1);
    get_i("vid_aa", m_x.vid_aa, 0, 1);
    get_b("tl_orbit", m_x.tl_orbit);
}

void StudioDialog::save_settings()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    auto set_f = [cfg](const char* key, float v) { cfg->set(kSection, key, std::to_string(v)); };
    auto set_b = [cfg](const char* key, bool v) { cfg->set(kSection, key, std::string(v ? "1" : "0")); };
    auto set_i = [cfg](const char* key, int v) { cfg->set(kSection, key, std::to_string(v)); };
    auto light = [&](const char* p, const StudioLight& l) {
        const std::string k(p);
        set_b((k + "_on").c_str(), l.on);
        set_f((k + "_i").c_str(), l.intensity);
        set_f((k + "_az").c_str(), l.azimuth);
        set_f((k + "_el").c_str(), l.elevation);
        set_f((k + "_k").c_str(), l.kelvin);
    };
    light("key", m_s.key);
    light("fill", m_s.fill);
    light("rim", m_s.rim);
    set_f("ambient", m_s.ambient);
    set_b("follow", m_s.lights_follow_camera);
    set_i("background", int(m_s.background));
    for (int i = 0; i < 3; ++i) {
        const std::string n = std::to_string(i);
        set_f(("cyc" + n).c_str(), m_s.cyclorama_color[i]);
        set_f(("gtop" + n).c_str(), m_s.gradient_top[i]);
        set_f(("gbot" + n).c_str(), m_s.gradient_bottom[i]);
        set_f(("solid" + n).c_str(), m_s.solid_color[i]);
    }
    set_b("floor_shadow", m_s.floor_shadow);
    set_f("shadow_strength", m_s.shadow_strength);
    set_f("shadow_softness", m_s.shadow_softness);
    set_b("contact", m_s.contact_shadow);
    set_f("contact_strength", m_s.contact_strength);
    set_b("reflection", m_s.reflection);
    set_f("reflection_strength", m_s.reflection_strength);
    set_b("self_shadows", m_s.self_shadows);
    set_i("material", int(m_s.material));
    set_f("exposure", m_s.exposure);
    set_f("saturation", m_s.saturation);
    set_b("whole_print", m_s.whole_print);
    set_f("fov", m_fov);
    set_i("light_preset", m_light_preset);
    set_i("aspect", m_x.aspect_idx);
    set_b("thirds", m_x.thirds);
    set_i("res", m_x.res_idx);
    set_i("aa", m_x.aa_idx);
    set_b("watermark", m_x.watermark);
    cfg->set(kSection, "wm_text", m_x.wm_text);
    cfg->set(kSection, "wm_logo", m_x.wm_logo);
    set_i("wm_pos", m_x.wm_pos);
    set_f("wm_opacity", m_x.wm_opacity);
    set_f("wm_size", m_x.wm_size);
    set_b("caption", m_x.caption);
    set_i("footer", m_x.footer_style);
    set_i("vid_format", m_x.vid_format);
    set_i("vid_kind", m_x.vid_kind);
    set_i("vid_seconds", m_x.vid_seconds);
    set_i("vid_fps", m_x.vid_fps_idx);
    set_i("vid_res", m_x.vid_res_idx);
    set_i("vid_aa", m_x.vid_aa);
    set_b("tl_orbit", m_x.tl_orbit);
}

// ---- export ---------------------------------------------------------------------------------------

std::vector<VideoFormat> StudioDialog::video_formats() const
{
    std::vector<VideoFormat> f;
    if (Mp4Writer::available())
        f.push_back(VideoFormat::Mp4);
    f.push_back(VideoFormat::Gif);
    f.push_back(VideoFormat::PngSequence);
    return f;
}

void StudioDialog::output_size(int long_edge, int& w, int& h, bool even) const
{
    const double a = frame_aspect();
    if (a >= 1.0) {
        w = long_edge;
        h = std::max(16, int(std::lround(long_edge / a)));
    }
    else {
        h = long_edge;
        w = std::max(16, int(std::lround(long_edge * a)));
    }
    if (even) {
        w &= ~1;
        h &= ~1;
    }
}

// The picture with its caption footer and watermark.
wxImage StudioDialog::compose(const std::vector<uint8_t>& rgba, int w, int h, const CaptionInfo& caption) const
{
    wxImage pic = to_wx(rgba, w, h);
    const bool with_footer = m_x.caption && (!caption.title.empty() || !caption.details.empty() || !caption.colours.empty());
    const bool with_mark   = m_x.watermark && (!m_x.wm_text.empty() || !m_x.wm_logo.empty());
    if (!with_footer && !with_mark)
        return pic;

    int fh = 0;
    if (with_footer) {
        fh = std::max(56, int(std::lround(0.085 * std::min(w, h))));
        fh += fh & 1;
        wxImage tall(w, h + fh, false);
        tall.InitAlpha();
        std::memcpy(tall.GetData(), pic.GetData(), size_t(w) * size_t(h) * 3);
        std::memcpy(tall.GetAlpha(), pic.GetAlpha(), size_t(w) * size_t(h));
        std::memset(tall.GetAlpha() + size_t(w) * size_t(h), 255, size_t(w) * size_t(fh));
        pic = tall;
    }

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(pic));
    if (!gc)
        return pic;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetInterpolationQuality(wxINTERPOLATION_BEST);

    if (with_footer) {
        bool dark = m_x.footer_style == 2;
        if (m_x.footer_style == 0)
            dark = m_s.background == StudioBackground::CycloramaDark ||
                   (m_s.background == StudioBackground::Gradient &&
                    0.2126f * m_s.gradient_bottom[0] + 0.7152f * m_s.gradient_bottom[1] + 0.0722f * m_s.gradient_bottom[2] < 0.4f) ||
                   (m_s.background == StudioBackground::Solid &&
                    0.2126f * m_s.solid_color[0] + 0.7152f * m_s.solid_color[1] + 0.0722f * m_s.solid_color[2] < 0.4f);
        const wxColour bg    = dark ? wxColour(30, 30, 33) : wxColour(250, 250, 250);
        const wxColour title = dark ? wxColour(240, 240, 242) : wxColour(40, 40, 42);
        const wxColour info  = dark ? wxColour(165, 165, 170) : wxColour(110, 110, 115);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(bg));
        gc->DrawRectangle(0, h, w, fh);
        // Orca teal hairline
        gc->SetBrush(wxBrush(wxColour(0, 150, 136)));
        gc->DrawRectangle(0, h, w, std::max(2, fh / 28));

        const double pad = fh * 0.24;
        double       sw_total = 0.0;
        const double d = fh * 0.30, gap = d * 0.4;
        if (!caption.colours.empty())
            sw_total = caption.colours.size() * d + (caption.colours.size() - 1) * gap + pad;
        const double text_w = w - 2 * pad - sw_total;
        const int    title_px = std::max(10, int(fh * 0.30));
        const int    info_px  = std::max(8, int(fh * 0.20));
        wxFont       tf(wxFontInfo(wxSize(0, title_px)).Family(wxFONTFAMILY_SWISS).Bold());
        wxFont       inf(wxFontInfo(wxSize(0, info_px)).Family(wxFONTFAMILY_SWISS));
        double       y = h + fh * 0.16;
        if (!caption.title.empty()) {
            gc->SetFont(tf, title);
            wxString t = caption.title;
            double   tw = 0, th = 0;
            gc->GetTextExtent(t, &tw, &th);
            while (tw > text_w && t.size() > 4) {
                t = t.Left(t.size() - 4) + "...";
                gc->GetTextExtent(t, &tw, &th);
            }
            gc->DrawText(t, pad, y);
            y += th + fh * 0.04;
        }
        if (!caption.details.empty()) {
            gc->SetFont(inf, info);
            gc->DrawText(caption.details, pad, y);
        }
        double x = w - pad - (sw_total > 0 ? sw_total - pad : 0.0);
        for (const wxColour& c : caption.colours) {
            gc->SetBrush(wxBrush(c));
            gc->SetPen(wxPen(dark ? wxColour(90, 90, 96) : wxColour(200, 200, 204), std::max(1, int(d / 14))));
            gc->DrawEllipse(x, h + 0.5 * (fh - d), d, d);
            x += d + gap;
        }
    }

    if (with_mark) {
        const double margin = 0.03 * std::min(w, h);
        const bool   right  = m_x.wm_pos == 0 || m_x.wm_pos == 2;
        const bool   bottom = m_x.wm_pos == 0 || m_x.wm_pos == 1;
        double       mw = 0.0, mh = 0.0;
        wxImage      logo;
        if (!m_x.wm_logo.empty())
            logo.LoadFile(wxString::FromUTF8(m_x.wm_logo));
        wxFont font(wxFontInfo(wxSize(0, std::max(10, int(w * m_x.wm_size * 0.2)))).Family(wxFONTFAMILY_SWISS).Bold());
        if (logo.IsOk()) {
            mw = w * m_x.wm_size;
            mh = mw * logo.GetHeight() / std::max(1, logo.GetWidth());
        }
        else {
            gc->SetFont(font, *wxWHITE);
            gc->GetTextExtent(wxString::FromUTF8(m_x.wm_text), &mw, &mh);
        }
        const double x = right ? w - margin - mw : margin;
        const double y = bottom ? h - margin - mh : margin;
        if (logo.IsOk()) {
            logo = logo.Scale(std::max(1, int(mw)), std::max(1, int(mh)), wxIMAGE_QUALITY_HIGH);
            if (!logo.HasAlpha())
                logo.InitAlpha();
            unsigned char* a = logo.GetAlpha();
            for (int i = 0, n = logo.GetWidth() * logo.GetHeight(); i < n; ++i)
                a[i] = (unsigned char) std::lround(a[i] * m_x.wm_opacity);
            gc->DrawBitmap(wxBitmap(logo), x, y, mw, mh);
        }
        else {
            // light text on a dark corner, dark on a light one
            double lum = 0.0, n = 0.0;
            const unsigned char* rgb = pic.GetData();
            const unsigned char* al  = pic.GetAlpha();
            for (int py = std::max(0, int(y)); py < std::min(h, int(y + mh)); py += 2)
                for (int px = std::max(0, int(x)); px < std::min(w, int(x + mw)); px += 2) {
                    const size_t i = size_t(py) * size_t(w) + size_t(px);
                    const double a = al != nullptr ? al[i] / 255.0 : 1.0;
                    lum += a * (0.2126 * rgb[i * 3] + 0.7152 * rgb[i * 3 + 1] + 0.0722 * rgb[i * 3 + 2]) / 255.0 + (1.0 - a) * 0.9;
                    n += 1.0;
                }
            const bool           light_bg = n == 0.0 || lum / n > 0.55;
            const unsigned char  alpha    = (unsigned char) std::lround(255.0 * m_x.wm_opacity);
            gc->SetFont(font, light_bg ? wxColour(25, 25, 28, alpha) : wxColour(255, 255, 255, alpha));
            gc->DrawText(wxString::FromUTF8(m_x.wm_text), x, y);
        }
    }
    gc.reset(); // writes the drawing back into pic
    return pic;
}

bool StudioDialog::render_picture(const StudioCamera& cam, int w, int h, int ss, int layer_top, std::vector<uint8_t>& rgba,
                                  wxProgressDialog* progress, int step, int steps)
{
    if (!make_current())
        return false;
    m_renderer.set_shadow_resolution(4096);
    const bool ok = m_renderer.render_image(m_s, cam, w, h, ss, rgba, layer_top, [&](int done, int total) {
        if (progress == nullptr)
            return true;
        const int value = int(1000.0 * (double(step) + double(done) / double(std::max(1, total))) / double(std::max(1, steps)));
        const bool go = progress->Update(std::min(999, value));
        make_current();
        return go;
    });
    m_renderer.set_shadow_resolution(2048);
    return ok;
}

void StudioDialog::export_still(bool clipboard)
{
    if (m_busy || !StudioRenderer::has_print()) {
        set_status(_L("Slice the plate first."));
        return;
    }
    m_cam.fov_deg = m_fov;
    const CaptionInfo caption = gather_caption();
    wxString path;
    if (!clipboard) {
        wxFileDialog dlg(this, _L("Save photo"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()),
                         wxString::FromUTF8(sanitize(caption.title)) + ".png", "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        path = dlg.GetPath();
    }
    int w = 0, h = 0;
    output_size(kStillEdges[m_x.res_idx], w, h, false);
    const int ss = kStillSS[m_x.aa_idx];
    m_busy = true;
    wxProgressDialog progress(_L("Photo Studio"), _L("Rendering the photo..."), 1000, this,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
    std::vector<uint8_t> rgba;
    const bool ok = render_picture(m_cam, w, h, ss, -1, rgba, &progress, 0, 1);
    progress.Update(1000);
    m_busy = false;
    if (!ok) {
        set_status(m_renderer.last_error() == "cancelled" ? _L("Cancelled.") : _L("Photo Studio could not render the print."));
        m_canvas->Refresh(false);
        return;
    }
    const wxImage img = compose(rgba, w, h, caption);
    if (clipboard) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxBitmapDataObject(wxBitmap(img, 32)));
            wxTheClipboard->Close();
            set_status(wxString::Format(_L("Copied %d x %d to the clipboard."), img.GetWidth(), img.GetHeight()));
        }
        else
            set_status(_L("The clipboard could not be opened."));
    }
    else {
        if (img.SaveFile(path, wxBITMAP_TYPE_PNG)) {
            set_status(wxString::Format(_L("Saved %d x %d: %s"), img.GetWidth(), img.GetHeight(), path));
            wxGetApp().app_config->update_last_output_dir(wxFileName(path).GetPath().ToUTF8().data());
        }
        else
            set_status(_L("The file could not be written."));
    }
    m_canvas->Refresh(false);
}

void StudioDialog::export_batch()
{
    if (m_busy || !StudioRenderer::has_print()) {
        set_status(_L("Slice the plate first."));
        return;
    }
    wxDirDialog dlg(this, _L("Folder for the views"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()));
    if (dlg.ShowModal() != wxID_OK)
        return;
    const wxString    folder  = dlg.GetPath();
    const CaptionInfo caption = gather_caption();
    const std::string base    = sanitize(caption.title);
    struct View
    {
        const char* suffix;
        double      yaw, pitch;
        bool        current;
    };
    const View views[] = { { "front", 0.0, 10.0, false }, { "side", 90.0, 10.0, false }, { "top", 0.0, 90.0, false },
                           { "iso", 45.0, 35.264, false }, { "current", 0.0, 0.0, true } };
    int w = 0, h = 0;
    output_size(kStillEdges[m_x.res_idx], w, h, false);
    const int ss = kStillSS[m_x.aa_idx];
    m_busy = true;
    wxProgressDialog progress(_L("Photo Studio"), _L("Rendering the views..."), 1000, this,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);
    int saved = 0;
    bool ok = true;
    const BoundingBoxf3 box = StudioRenderer::print_box();
    for (int i = 0; i < 5 && ok; ++i) {
        StudioCamera cam = m_cam;
        if (!views[i].current) {
            cam.yaw_deg   = views[i].yaw;
            cam.pitch_deg = views[i].pitch;
            fit_camera(cam, box, double(w) / double(h));
        }
        std::vector<uint8_t> rgba;
        ok = render_picture(cam, w, h, ss, -1, rgba, &progress, i, 5);
        if (!ok)
            break;
        const wxString path = folder + wxFileName::GetPathSeparator() + wxString::FromUTF8(base + "_" + views[i].suffix + ".png");
        if (compose(rgba, w, h, caption).SaveFile(path, wxBITMAP_TYPE_PNG))
            ++saved;
    }
    progress.Update(1000);
    m_busy = false;
    set_status(ok ? wxString::Format(_L("%d views saved in %s"), saved, folder) :
                    (m_renderer.last_error() == "cancelled" ? _L("Cancelled.") : _L("Photo Studio could not render the print.")));
    m_canvas->Refresh(false);
}

void StudioDialog::export_video()
{
    if (m_busy || !StudioRenderer::has_print()) {
        set_status(_L("Slice the plate first."));
        return;
    }
    const std::vector<VideoFormat> formats = video_formats();
    const VideoFormat format = formats[std::clamp(m_x.vid_format, 0, int(formats.size()) - 1)];
    const CaptionInfo caption = gather_caption();
    const std::string base    = sanitize(caption.title) + (m_x.vid_kind == 0 ? "_turntable" : "_timelapse");

    wxString path;
    if (format == VideoFormat::PngSequence) {
        wxDirDialog dlg(this, _L("Folder for the frames"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()));
        if (dlg.ShowModal() != wxID_OK)
            return;
        path = dlg.GetPath();
    }
    else {
        const bool mp4 = format == VideoFormat::Mp4;
        wxFileDialog dlg(this, _L("Save video"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()),
                         wxString::FromUTF8(base) + (mp4 ? ".mp4" : ".gif"), mp4 ? "MP4 (*.mp4)|*.mp4" : "GIF (*.gif)|*.gif",
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        path = dlg.GetPath();
    }

    int fps = kVideoFps[std::clamp(m_x.vid_fps_idx, 0, int(std::size(kVideoFps)) - 1)];
    int edge = kVideoEdges[std::clamp(m_x.vid_res_idx, 0, int(std::size(kVideoEdges)) - 1)];
    if (format == VideoFormat::Gif)
        fps = std::min(fps, 50); // GIF delays are whole hundredths, 2 the shortest players honour
    int w = 0, h = 0;
    output_size(edge, w, h, true);
    if (format == VideoFormat::Mp4 && (w > 3840 || h > 2160)) {
        const double k = std::min(3840.0 / w, 2160.0 / h);
        w = int(w * k) & ~1;
        h = int(h * k) & ~1;
    }
    const int ss = m_x.vid_aa == 1 ? 2 : 1;
    const int frames_anim = std::max(2, m_x.vid_seconds * fps);
    const bool timelapse  = m_x.vid_kind == 1;
    const int frames_hold = timelapse ? fps : 0; // a second on the finished print
    const int frames      = frames_anim + frames_hold;

    GCodeViewer* gv = StudioRenderer::gcode_viewer();
    const size_t layers = StudioRenderer::layers_count();
    int top_layer = int(layers) - 1;
    if (gv != nullptr && !m_s.whole_print)
        top_layer = int(gv->get_layers_z_range()[1]);
    const BoundingBoxf3 box = StudioRenderer::print_box();

    auto frame_camera = [&](int i, int& layer_top) {
        StudioCamera cam = m_cam;
        layer_top = -1;
        const int    k = std::min(i, frames_anim - 1);
        const double t = double(k) / double(frames_anim); // [0, 1)
        if (!timelapse) {
            cam.target.x() = box.center().x();
            cam.target.y() = box.center().y();
            cam.yaw_deg    = m_cam.yaw_deg + 360.0 * t;
        }
        else {
            const double done = double(k + 1) / double(frames_anim);
            layer_top = std::clamp(int(std::lround(done * (top_layer + 1))) - 1, 0, std::max(0, top_layer));
            if (m_x.tl_orbit) {
                cam.target.x() = box.center().x();
                cam.target.y() = box.center().y();
                cam.yaw_deg    = m_cam.yaw_deg + 180.0 * t;
            }
        }
        return cam;
    };
    // whole frame: footer and watermark included
    auto render_frame = [&](int i, std::vector<uint8_t>& out, int& ow, int& oh, wxProgressDialog* pd) -> bool {
        int layer_top = -1;
        const StudioCamera cam = frame_camera(i, layer_top);
        std::vector<uint8_t> rgba;
        if (!render_picture(cam, w, h, ss, layer_top, rgba, nullptr, 0, 1))
            return false;
        wxImage img = compose(rgba, w, h, caption);
        if (format == VideoFormat::Mp4 && img.HasAlpha()) {
            // no alpha in H.264: over the solid colour
            const auto& c = m_s.solid_color;
            unsigned char* rgb = img.GetData();
            unsigned char* a   = img.GetAlpha();
            for (int p = 0, n = img.GetWidth() * img.GetHeight(); p < n; ++p) {
                const float al = a[p] / 255.0f;
                for (int ch = 0; ch < 3; ++ch)
                    rgb[p * 3 + ch] = (unsigned char) std::lround(rgb[p * 3 + ch] * al + c[ch] * 255.0f * (1.0f - al));
                a[p] = 255;
            }
        }
        if (format == VideoFormat::Gif && m_s.background == StudioBackground::Transparent && img.HasAlpha()) {
            // GIF transparency is all or nothing: soft edges and shadows go onto the solid colour (the matte),
            // what is mostly see-through becomes transparent
            const auto& c = m_s.solid_color;
            unsigned char* rgb = img.GetData();
            unsigned char* a   = img.GetAlpha();
            for (int p = 0, n = img.GetWidth() * img.GetHeight(); p < n; ++p) {
                if (a[p] == 0 || a[p] == 255)
                    continue;
                const float al = a[p] / 255.0f;
                for (int ch = 0; ch < 3; ++ch)
                    rgb[p * 3 + ch] = (unsigned char) std::lround(rgb[p * 3 + ch] * al + c[ch] * 255.0f * (1.0f - al));
                a[p] = a[p] >= 96 ? 255 : 0;
            }
        }
        ow  = img.GetWidth();
        oh  = img.GetHeight();
        out = from_wx(img);
        if (pd != nullptr)
            return pd->Update(std::min(999, int(1000.0 * double(i + 1) / double(frames))));
        return true;
    };

    m_busy = true;
    wxProgressDialog progress(_L("Photo Studio"), _L("Rendering the video..."), 1000, this,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME);
    bool        ok = true;
    std::string error;
    int         ow = 0, oh = 0;
    std::vector<uint8_t> frame;
    if (format == VideoFormat::Mp4) {
        Mp4Writer writer;
        for (int i = 0; i < frames && ok; ++i) {
            ok = render_frame(i, frame, ow, oh, &progress);
            if (ok && i == 0)
                ok = writer.open(path.ToUTF8().data(), ow, oh, fps, error);
            if (ok)
                ok = writer.add_frame(frame, error);
        }
        std::string close_error;
        if (!writer.close(close_error) && ok) {
            ok    = false;
            error = close_error;
        }
    }
    else if (format == VideoFormat::Gif) {
        // the palette from a few frames across the animation, then every frame against it
        progress.Update(0, _L("Choosing the colours..."));
        std::vector<std::vector<uint8_t>> samples;
        for (double f : { 0.0, 0.3, 0.6, 0.999 }) {
            std::vector<uint8_t> s;
            if (!(ok = render_frame(int(f * (frames_anim - 1)), s, ow, oh, nullptr)))
                break;
            samples.push_back(std::move(s));
        }
        GifWriter writer;
        if (ok) {
            std::vector<const std::vector<uint8_t>*> ptrs;
            for (const auto& s : samples)
                ptrs.push_back(&s);
            writer.build_palette(ptrs, m_s.background == StudioBackground::Transparent);
            ok = writer.open(path.ToUTF8().data(), ow, oh);
            if (!ok)
                error = _u8L("The file could not be written.");
        }
        progress.Update(0, _L("Rendering the video..."));
        for (int i = 0; i < frames && ok; ++i) {
            if (!(ok = render_frame(i, frame, ow, oh, &progress)))
                break;
            const int delay = int(std::lround(100.0 * (i + 1) / fps) - std::lround(100.0 * i / fps));
            ok = writer.add_frame(frame, delay);
        }
        if (!writer.close() && ok)
            ok = false;
    }
    else {
        for (int i = 0; i < frames && ok; ++i) {
            if (!(ok = render_frame(i, frame, ow, oh, &progress)))
                break;
            const wxString name = path + wxFileName::GetPathSeparator() + wxString::FromUTF8(base) + wxString::Format("_%05d.png", i + 1);
            if (!to_wx(frame, ow, oh).SaveFile(name, wxBITMAP_TYPE_PNG)) {
                ok    = false;
                error = _u8L("The file could not be written.");
            }
        }
    }
    progress.Update(1000);
    m_busy = false;
    if (ok)
        set_status(wxString::Format(_L("Saved %d frames, %d x %d at %d fps: %s"), frames, ow, oh, fps, path));
    else if (m_renderer.last_error() == "cancelled" || (error.empty() && progress.WasCancelled()))
        set_status(_L("Cancelled."));
    else
        set_status(error.empty() ? _L("Photo Studio could not render the print.") : wxString::FromUTF8(error));
    m_canvas->Refresh(false);
}

} // namespace

void open_photo_studio()
{
    if (s_dialog == nullptr) {
        wxWindow* parent = wxGetApp().mainframe;
        s_dialog = new StudioDialog(parent);
    }
    s_dialog->Show();
    s_dialog->Raise();
}

bool photo_studio_open()
{
    return s_dialog != nullptr;
}

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
