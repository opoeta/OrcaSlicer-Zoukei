// Layer Slayer dialog. See LayerSlayerDialog.hpp.
#include "LayerSlayerDialog.hpp"
#include "LayerSlayer.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/LabeledStaticBox.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"

#include <wx/dcbuffer.h>
#include <wx/filefn.h>
#include <wx/dcgraph.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <map>
#include <memory>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

using namespace LayerSlayer;

// Values the user typed, kept while the application runs.
std::map<std::string, Values> &remembered()
{
    static std::map<std::string, Values> m;
    return m;
}

std::string &last_test()
{
    static std::string id;
    return id;
}

struct Palette
{
    wxColour bg, fg, muted, row_hover, row_selected, accent, bed, bed_line, piece, piece_line;
};

Palette palette()
{
    if (wxGetApp().dark_mode())
        return {wxColour("#2D2D31"), wxColour("#EFEFF0"), wxColour("#A0A0A6"), wxColour("#36363C"), wxColour("#1F4D48"),
                wxColour("#00A693"), wxColour("#3A3A40"), wxColour("#55555C"), wxColour("#00A693"), wxColour("#7FE0D4")};
    return {wxColour("#FFFFFF"), wxColour("#363636"), wxColour("#7C7C7C"), wxColour("#F2F2F2"), wxColour("#DDF1EE"),
            wxColour("#009688"), wxColour("#EEEEEE"), wxColour("#C8C8C8"), wxColour("#7ECFC5"), wxColour("#00796B")};
}

// The test's art (resources/images/calib_<id>.png) fitted into w x h.
wxBitmap art_bitmap(const std::string &id, int w, int h)
{
    wxImage img;
    const std::string path = Slic3r::resources_dir() + "/images/" + icon_name(id) + ".png";
    if (!wxFileExists(from_u8(path)) || !img.LoadFile(from_u8(path), wxBITMAP_TYPE_PNG) || !img.IsOk()) {
        wxImage blank(w, h);
        blank.SetRGB(wxRect(0, 0, w, h), 0x00, 0x96, 0x88);
        return wxBitmap(blank);
    }
    const double k = std::min(double(w) / img.GetWidth(), double(h) / img.GetHeight());
    const int    nw = std::max(1, int(img.GetWidth() * k)), nh = std::max(1, int(img.GetHeight() * k));
    img.Rescale(nw, nh, wxIMAGE_QUALITY_HIGH);
    return wxBitmap(img);
}

wxString unit_text(const std::string &unit)
{
    if (unit == "C")
        return wxString::FromUTF8("\xC2\xB0" "C");
    return from_u8(unit);
}

// ---- plate drawing ---------------------------------------------------------------------------
class PlatePreview : public wxPanel
{
public:
    explicit PlatePreview(wxWindow *parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(300, 210)))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PlatePreview::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
            Refresh();
            e.Skip();
        });
    }

    void set(const Env &env, const std::vector<Obj> *objects)
    {
        m_env = env;
        m_boxes.clear();
        if (objects != nullptr)
            for (const Obj &o : *objects)
                m_boxes.push_back({o.x - o.w / 2., o.y - o.d / 2., o.w, o.d});
        Refresh();
    }

private:
    struct Box
    {
        double x, y, w, d;
    };

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const Palette         pal = palette();
        dc.SetBackground(wxBrush(pal.bg));
        dc.Clear();
        wxGCDC       gdc(dc);
        const wxSize sz = GetClientSize();
        const double margin = FromDIP(8);
        const double k = std::min((sz.x - 2 * margin) / std::max(1.0, m_env.bed_w), (sz.y - 2 * margin) / std::max(1.0, m_env.bed_d));
        const double ox = (sz.x - m_env.bed_w * k) / 2., oy = (sz.y - m_env.bed_d * k) / 2.;
        auto to_px = [&](double x, double y) { return wxPoint(int(ox + (x - m_env.bed_x0) * k), int(oy + (m_env.bed_y0 + m_env.bed_d - y) * k)); };
        gdc.SetPen(wxPen(pal.bed_line, 1));
        gdc.SetBrush(wxBrush(pal.bed));
        gdc.DrawRoundedRectangle(int(ox), int(oy), int(m_env.bed_w * k), int(m_env.bed_d * k), 4);
        gdc.SetPen(wxPen(pal.piece_line, 1));
        gdc.SetBrush(wxBrush(pal.piece));
        for (const Box &b : m_boxes) {
            const wxPoint tl = to_px(b.x, b.y + b.d);
            gdc.DrawRectangle(tl.x, tl.y, std::max(1, int(b.w * k)), std::max(1, int(b.d * k)));
        }
    }

    Env              m_env;
    std::vector<Box> m_boxes;
};

// ---- the dialog --------------------------------------------------------------------------------
class LayerSlayerDialog : public DPIDialog
{
public:
    LayerSlayerDialog(wxWindow *parent, Plater *plater, const std::string &id);
    void on_dpi_changed(const wxRect &suggested_rect) override
    {
        Layout();
        Refresh();
    }

private:
    void build_list();
    void select(const std::string &id);
    void build_params();
    bool read_values(Values &out, std::string &error);
    void refresh_plate();
    void on_create();
    void on_defaults();

public:
    Values      result_values;
    std::string result_id;

private:

    std::string                m_id;
    Env                        m_env;
    wxScrolledWindow          *m_list   = nullptr;
    std::map<std::string, wxPanel *> m_rows;
    wxStaticBitmap            *m_art    = nullptr;
    wxStaticText              *m_title  = nullptr;
    wxStaticText              *m_desc   = nullptr;
    wxStaticText              *m_printer = nullptr;
    wxStaticText              *m_how    = nullptr;
    wxStaticText              *m_error  = nullptr;
    wxPanel                   *m_params = nullptr;
    PlatePreview              *m_preview = nullptr;
    wxTextCtrl                *m_legend = nullptr;
    wxScrolledWindow          *m_right  = nullptr;
    std::vector<std::pair<const Param *, wxWindow *>> m_inputs;
};

LayerSlayerDialog::LayerSlayerDialog(wxWindow *parent, Plater *plater, const std::string &id)
    : DPIDialog(parent, wxID_ANY, _L("Layer Slayer calibrations"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const Palette pal = palette();
    SetBackgroundColour(pal.bg);
    SetForegroundColour(pal.fg);
    SetFont(Label::Body_14);
    m_env = read_env(plater);

    auto *top = new wxBoxSizer(wxVERTICAL);
    auto *body = new wxBoxSizer(wxHORIZONTAL);

    // left: the tests, grouped
    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(270, 560)), wxVSCROLL);
    m_list->SetBackgroundColour(pal.bg);
    m_list->SetScrollRate(0, FromDIP(12));
    build_list();
    body->Add(m_list, 0, wxEXPAND | wxALL, FromDIP(10));

    // right: the selected test
    m_right = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(640, 560)), wxVSCROLL);
    m_right->SetBackgroundColour(pal.bg);
    m_right->SetScrollRate(0, FromDIP(12));
    auto *rs = new wxBoxSizer(wxVERTICAL);

    auto *head = new wxBoxSizer(wxHORIZONTAL);
    m_art = new wxStaticBitmap(m_right, wxID_ANY, wxBitmap(FromDIP(150), FromDIP(100)));
    head->Add(m_art, 0, wxRIGHT, FromDIP(12));
    auto *head_text = new wxBoxSizer(wxVERTICAL);
    m_title = new wxStaticText(m_right, wxID_ANY, "");
    m_title->SetFont(Label::Head_18);
    m_desc = new wxStaticText(m_right, wxID_ANY, "");
    m_desc->SetFont(Label::Body_13);
    m_printer = new wxStaticText(m_right, wxID_ANY, "");
    m_printer->SetFont(Label::Body_12);
    m_printer->SetForegroundColour(pal.muted);
    head_text->Add(m_title, 0, wxBOTTOM, FromDIP(4));
    head_text->Add(m_desc, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    head_text->Add(m_printer, 0);
    head->Add(head_text, 1, wxEXPAND);
    rs->Add(head, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    auto *params_box = new LabeledStaticBox(m_right, _L("Parameters"));
    auto *params_sizer = new wxStaticBoxSizer(params_box, wxVERTICAL);
    m_params = new wxPanel(m_right);
    m_params->SetBackgroundColour(pal.bg);
    params_sizer->Add(m_params, 0, wxEXPAND | wxALL, FromDIP(6));
    rs->Add(params_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    auto *plate_box = new LabeledStaticBox(m_right, _L("Plate and what each piece changes"));
    auto *plate_sizer = new wxStaticBoxSizer(plate_box, wxHORIZONTAL);
    m_preview = new PlatePreview(m_right);
    m_legend  = new wxTextCtrl(m_right, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(300, 210)), wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
    m_legend->SetFont(Label::Body_12);
    m_legend->SetBackgroundColour(pal.bg);
    m_legend->SetForegroundColour(pal.fg);
    plate_sizer->Add(m_preview, 1, wxEXPAND | wxALL, FromDIP(4));
    plate_sizer->Add(m_legend, 1, wxEXPAND | wxALL, FromDIP(4));
    rs->Add(plate_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    auto *how_box = new LabeledStaticBox(m_right, _L("How to read it"));
    auto *how_sizer = new wxStaticBoxSizer(how_box, wxVERTICAL);
    m_how = new wxStaticText(m_right, wxID_ANY, "");
    m_how->SetFont(Label::Body_13);
    how_sizer->Add(m_how, 0, wxEXPAND | wxALL, FromDIP(6));
    rs->Add(how_sizer, 0, wxEXPAND);

    m_right->SetSizer(rs);
    body->Add(m_right, 1, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(10));
    top->Add(body, 1, wxEXPAND);

    // bottom: error line and buttons
    auto *bottom = new wxBoxSizer(wxHORIZONTAL);
    m_error = new wxStaticText(this, wxID_ANY, "");
    m_error->SetForegroundColour(wxColour("#E14747"));
    bottom->Add(m_error, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    auto *btn_defaults = new Button(this, _L("Defaults"));
    btn_defaults->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    auto *btn_cancel = new Button(this, _L("Cancel"));
    btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    auto *btn_create = new Button(this, _L("Create plate"));
    btn_create->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    bottom->Add(btn_defaults, 0, wxALL, FromDIP(6));
    bottom->Add(btn_cancel, 0, wxALL, FromDIP(6));
    bottom->Add(btn_create, 0, wxALL, FromDIP(6));
    top->Add(bottom, 0, wxEXPAND | wxBOTTOM | wxRIGHT, FromDIP(8));

    btn_defaults->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_defaults(); });
    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    btn_create->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_create(); });

    SetSizer(top);
    std::string first = id;
    if (first.empty() || find(first) == nullptr)
        first = last_test();
    if (first.empty() || find(first) == nullptr)
        first = all().front().id;
    select(first);

    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    Fit();
    SetMinSize(FromDIP(wxSize(900, 560)));
    CenterOnParent();
}

void LayerSlayerDialog::build_list()
{
    const Palette pal = palette();
    auto *ls = new wxBoxSizer(wxVERTICAL);
    for (const std::string &group : groups()) {
        auto *h = new wxStaticText(m_list, wxID_ANY, _(group_name(group)));
        h->SetFont(Label::Head_13);
        h->SetForegroundColour(pal.muted);
        ls->Add(h, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(6));
        for (const Calib &c : all()) {
            if (c.group != group)
                continue;
            auto *row = new wxPanel(m_list);
            row->SetBackgroundColour(pal.bg);
            auto *rsz = new wxBoxSizer(wxHORIZONTAL);
            auto *icon = new wxStaticBitmap(row, wxID_ANY, art_bitmap(c.id, FromDIP(40), FromDIP(28)), wxDefaultPosition, FromDIP(wxSize(40, 28)));
            auto *text = new wxStaticText(row, wxID_ANY, _(name(c.id)));
            text->SetFont(Label::Body_13);
            rsz->Add(icon, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(4));
            rsz->Add(text, 1, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(4));
            row->SetSizer(rsz);
            const std::string cid = c.id;
            auto on_click = [this, cid](wxMouseEvent &) { select(cid); };
            for (wxWindow *w : std::initializer_list<wxWindow *>{row, icon, text}) {
                w->Bind(wxEVT_LEFT_DOWN, on_click);
                w->SetCursor(wxCursor(wxCURSOR_HAND));
            }
            ls->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(1));
            m_rows[cid] = row;
        }
    }
    m_list->SetSizer(ls);
    m_list->FitInside();
}

void LayerSlayerDialog::select(const std::string &id)
{
    const Calib *c = find(id);
    if (c == nullptr)
        return;
    // keep what was typed in the test we leave
    if (!m_id.empty()) {
        Values v;
        std::string err;
        if (read_values(v, err))
            remembered()[m_id] = v;
    }
    m_id        = id;
    last_test() = id;
    const Palette pal = palette();
    for (auto &kv : m_rows) {
        kv.second->SetBackgroundColour(kv.first == id ? pal.row_selected : pal.bg);
        kv.second->Refresh();
    }
    m_art->SetBitmap(art_bitmap(id, FromDIP(150), FromDIP(100)));
    m_title->SetLabel(_(name(id)));
    m_desc->SetLabel(_(description(id)));
    m_desc->Wrap(FromDIP(440));
    m_printer->SetLabel(wxString::Format(_L("Printer: %s - layer %s mm - first layer %s mm"), from_u8(m_env.printer),
                                         wxString::FromCDouble(m_env.layer_height, 2), wxString::FromCDouble(m_env.first_layer, 2)));
    m_how->SetLabel(_(how_to_read(id)));
    m_how->Wrap(FromDIP(600));
    build_params();
    refresh_plate();
    m_right->Layout();
    m_right->FitInside();
    m_right->Scroll(0, 0);
}

void LayerSlayerDialog::build_params()
{
    const Calib *c = find(m_id);
    m_params->DestroyChildren();
    m_inputs.clear();
    if (c == nullptr)
        return;
    if (remembered().count(m_id) == 0)
        remembered()[m_id] = defaults(*c);
    const Values &v = remembered()[m_id];

    auto *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(1, 1);
    for (const Param &p : c->params) {
        wxString label = _(param_label(p.key));
        if (p.kind == Kind::Floats)
            label += " (" + _L("separate with commas") + ")";
        auto *st = new wxStaticText(m_params, wxID_ANY, label);
        grid->Add(st, 0, wxALIGN_CENTER_VERTICAL);
        if (p.kind == Kind::Select) {
            auto *cb = new ComboBox(m_params, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(320, -1)), 0, nullptr, wxCB_READONLY);
            int   sel = 0;
            for (size_t i = 0; i < p.options.size(); ++i) {
                cb->Append(_(p.options[i].label));
                if (p.options[i].value == v.s(p.key))
                    sel = int(i);
            }
            cb->SetSelection(sel);
            cb->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { refresh_plate(); });
            grid->Add(cb, 0, wxEXPAND);
            m_inputs.push_back({&p, cb});
        } else {
            const wxSize size = p.kind == Kind::Floats ? FromDIP(wxSize(320, -1)) : FromDIP(wxSize(140, -1));
            auto *ti = new TextInput(m_params, from_u8(format_value(p, v)), unit_text(p.unit), "", wxDefaultPosition, size);
            ti->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) {
                refresh_plate();
                e.Skip();
            });
            grid->Add(ti, 0, p.kind == Kind::Floats ? wxEXPAND : 0);
            m_inputs.push_back({&p, ti});
        }
    }
    m_params->SetSizer(grid, true);
    wxGetApp().UpdateDarkUIWin(m_params);
    m_params->Layout();
}

bool LayerSlayerDialog::read_values(Values &out, std::string &error)
{
    const Calib *c = find(m_id);
    if (c == nullptr)
        return false;
    out = defaults(*c);
    for (const auto &in : m_inputs) {
        const Param &p = *in.first;
        std::string  err;
        if (p.kind == Kind::Select) {
            const int sel = static_cast<ComboBox *>(in.second)->GetSelection();
            if (sel >= 0 && size_t(sel) < p.options.size())
                out.str[p.key] = p.options[size_t(sel)].value;
        } else {
            err = parse(p, into_u8(static_cast<TextInput *>(in.second)->GetTextCtrl()->GetValue()), out);
        }
        if (!err.empty()) {
            error = err;
            return false;
        }
    }
    return true;
}

void LayerSlayerDialog::refresh_plate()
{
    Values      v;
    std::string err;
    if (!read_values(v, err)) {
        m_error->SetForegroundColour(wxColour("#E14747"));
        m_error->SetLabel(from_u8(err));
        m_preview->set(m_env, nullptr);
        m_legend->SetValue("");
        Layout();
        return;
    }
    try {
        const Plate plate = build(*find(m_id), v, m_env);
        m_preview->set(m_env, &plate.objects);
        wxString legend;
        for (const LegendRow &r : plate.legend())
            legend += from_u8(r.name) + (r.text.empty() ? wxString() : ":  " + from_u8(r.text)) + "\n";
        m_legend->SetValue(legend);
        wxString warn;
        for (const std::string &w : plate.warnings)
            warn += (warn.empty() ? "" : "  ") + from_u8(w);
        m_error->SetLabel(warn);
        m_error->SetForegroundColour(wxColour("#E0A030"));
    } catch (const std::exception &ex) {
        m_error->SetForegroundColour(wxColour("#E14747"));
        m_error->SetLabel(from_u8(ex.what()));
        m_preview->set(m_env, nullptr);
        m_legend->SetValue("");
    }
    Layout();
}

void LayerSlayerDialog::on_defaults()
{
    if (const Calib *c = find(m_id)) {
        remembered()[m_id] = defaults(*c);
        build_params();
        refresh_plate();
        m_right->Layout();
        m_right->FitInside();
    }
}

void LayerSlayerDialog::on_create()
{
    Values      v;
    std::string err;
    if (!read_values(v, err)) {
        MessageDialog(this, from_u8(err), _L("Layer Slayer"), wxICON_WARNING | wxOK).ShowModal();
        return;
    }
    try {
        build(*find(m_id), v, m_env);
    } catch (const std::exception &ex) {
        MessageDialog(this, from_u8(ex.what()), _L("Layer Slayer"), wxICON_WARNING | wxOK).ShowModal();
        return;
    }
    remembered()[m_id] = v;
    result_values      = v;
    result_id          = m_id;
    EndModal(wxID_OK);
}

} // namespace

void open_layer_slayer_dialog(Plater *plater, const std::string &id)
{
    if (plater == nullptr)
        return;
    LayerSlayer::Values values;
    std::string test;
    {
        LayerSlayerDialog dlg(static_cast<wxWindow *>(wxGetApp().mainframe), plater, id);
        if (dlg.ShowModal() != wxID_OK)
            return;
        values = dlg.result_values;
        test   = dlg.result_id;
    }
    std::string err;
    LayerSlayer::Plate  plate;
    if (!LayerSlayer::run(plater, test, values, false, err, &plate)) {
        if (!err.empty())
            MessageDialog(static_cast<wxWindow *>(wxGetApp().mainframe), from_u8(err), _L("Layer Slayer"), wxICON_ERROR | wxOK).ShowModal();
        return;
    }
    std::string note = Slic3r::format(_u8L("Layer Slayer: %1% is on the plate. Slice and print it with your profiles, then read it as the dialog explains."),
                                      _u8L(LayerSlayer::name(test)));
    for (const std::string &w : plate.warnings)
        note += "\n" + w;
    if (NotificationManager *nm = plater->get_notification_manager())
        nm->push_notification(note);
}

}} // namespace Slic3r::GUI
