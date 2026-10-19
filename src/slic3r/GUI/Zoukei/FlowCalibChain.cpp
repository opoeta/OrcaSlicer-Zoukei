// Flow-rate calibration chain. See FlowCalibChain.hpp.
#include "FlowCalibChain.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <locale>
#include <sstream>

namespace Slic3r { namespace GUI { namespace FlowCalib {

namespace {

Test &last_test()
{
    static Test t;
    return t;
}

std::string num(double v, int decimals)
{
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os.setf(std::ios::fixed);
    os.precision(decimals);
    os << v;
    std::string s = os.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    if (s == "-0" || s.empty())
        s = "0";
    return s;
}

} // namespace

double final_ratio(const Test &test, double mod)
{
    if (test.linear)
        return test.current_flow + mod;
    return test.current_flow * test.base * (1. + mod / 100.);
}

std::string format_mod(double mod, bool linear)
{
    const std::string s = num(mod, linear ? 4 : 2);
    return (mod > 0 ? "+" : "") + s;
}

std::vector<double> orca_pass1_mods()
{
    return {-20, -15, -10, -5, 0, 5, 10, 15, 20};
}

double modifier_from_object_name(const std::string &name, bool &ok)
{
    ok = false;
    if (name.size() <= 9)
        return 0.;
    std::string s = name.substr(9); // "flowrate_"
    if (s[0] == 'm')
        s[0] = '-';
    std::istringstream is(s);
    is.imbue(std::locale::classic());
    double v = 0.;
    if (!(is >> v))
        return 0.;
    ok = true;
    return v;
}

// ---------------------------------------------------------------------------------------------
// the filament
// ---------------------------------------------------------------------------------------------
bool current_filament_flow(double &value, std::string &name, std::string &err)
{
    PresetBundle *b = wxGetApp().preset_bundle;
    if (b == nullptr || b->filament_presets.empty()) {
        err = "no filament preset is selected";
        return false;
    }
    const Preset &p = b->filaments.get_edited_preset();
    name            = p.name;
    if (name.empty()) {
        err = "no filament preset is selected";
        return false;
    }
    const auto *opt = p.config.option<ConfigOptionFloats>("filament_flow_ratio");
    if (opt == nullptr || opt->values.empty()) {
        err = "the selected filament has no flow ratio";
        return false;
    }
    value = opt->values.front();
    return true;
}

bool apply_filament_flow(double ratio, double &old_value, std::string &name, std::string &err)
{
    if (!current_filament_flow(old_value, name, err))
        return false;
    if (!(ratio > 0.1 && ratio < 3.)) {
        err = "the flow ratio is out of range";
        return false;
    }
    Tab *tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
    if (tab == nullptr) {
        err = "the filament tab is not available";
        return false;
    }
    DynamicPrintConfig cfg = wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto              *opt = cfg.option<ConfigOptionFloats>("filament_flow_ratio");
    for (double &v : opt->values)
        v = std::round(ratio * 1e4) / 1e4;
    tab->load_config(cfg);
    wxGetApp().plater()->on_config_change(cfg);
    return true;
}

void        remember(const Test &test) { last_test() = test; }
const Test &last() { return last_test(); }

// ---------------------------------------------------------------------------------------------
// tiles
// ---------------------------------------------------------------------------------------------
namespace {

// A picture of a calibration tile: the pad with the top surface pattern the test uses and its number.
wxBitmap make_tile_bitmap(const wxString &label, const std::string &pattern, int w, int h)
{
    wxBitmap   bmp(w, h, 24);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxColour(250, 250, 250)));
    dc.Clear();

    const int   pad = std::max(2, w / 24);
    const wxRect r(pad, pad, w - 2 * pad, h - 2 * pad);
    dc.SetPen(wxPen(wxColour(176, 170, 158), 1));
    dc.SetBrush(wxBrush(wxColour(232, 226, 214)));
    dc.DrawRoundedRectangle(r, std::max(3, w / 20));

    // top surface lines, clipped to the pad
    dc.SetClippingRegion(r.Deflate(2, 2));
    dc.SetPen(wxPen(wxColour(190, 172, 140), std::max(1, w / 60)));
    const double cx = r.x + r.width / 2., cy = r.y + r.height / 2.;
    if (pattern == "monotonic") {
        for (int y = r.y + 3; y < r.y + r.height; y += std::max(3, h / 18))
            dc.DrawLine(r.x, y, r.x + r.width, y);
    } else {
        // Archimedean chords: a spiral from the centre
        const double step = std::max(2.5, h / 16.);
        wxPoint      prev = wxPoint(int(cx), int(cy));
        for (double a = 0.; a < 40.; a += 0.25) {
            const double  rad = step * a / 6.283185;
            const wxPoint p(int(cx + rad * std::cos(a)), int(cy + rad * std::sin(a)));
            dc.DrawLine(prev, p);
            prev = p;
        }
    }
    dc.DestroyClippingRegion();

    // the number, like the one embossed on the printed tile
    wxFont font = dc.GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    font.SetPointSize(std::max(8, h / 6));
    dc.SetFont(font);
    const wxSize ts = dc.GetTextExtent(label);
    const wxRect badge(r.x + (r.width - ts.x) / 2 - 6, r.y + r.height - ts.y - 8, ts.x + 12, ts.y + 4);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(0, 130, 116)));
    dc.DrawRoundedRectangle(badge, 4);
    dc.SetTextForeground(*wxWHITE);
    dc.DrawText(label, badge.x + 6, badge.y + 2);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

class SampleTile : public wxPanel
{
public:
    SampleTile(wxWindow *parent, const wxBitmap &bmp, const wxString &caption, const wxString &sub)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
        , m_bmp(bmp), m_caption(caption), m_sub(sub)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(bmp.GetWidth() + FromDIP(16), bmp.GetHeight() + FromDIP(16) + FromDIP(44)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &SampleTile::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
            if (on_click)
                on_click();
        });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent &) {
            if (on_click)
                on_click();
            if (on_activate)
                on_activate();
        });
    }
    void set_selected(bool s)
    {
        m_selected = s;
        Refresh();
    }
    std::function<void()> on_click;
    std::function<void()> on_activate;

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const bool            dark = wxGetApp().dark_mode();
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize sz = GetClientSize();
        dc.SetPen(m_selected ? wxPen(wxColour(0, 150, 136), 3) : wxPen(dark ? wxColour(80, 80, 86) : wxColour(210, 210, 214), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(wxRect(1, 1, sz.x - 2, sz.y - 2), 6);
        dc.DrawBitmap(m_bmp, (sz.x - m_bmp.GetWidth()) / 2, FromDIP(8));
        dc.SetTextForeground(dark ? wxColour(239, 239, 240) : wxColour(54, 54, 54));
        wxFont f = dc.GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(f);
        wxSize ts = dc.GetTextExtent(m_caption);
        dc.DrawText(m_caption, (sz.x - ts.x) / 2, FromDIP(8) + m_bmp.GetHeight() + FromDIP(4));
        f.SetWeight(wxFONTWEIGHT_NORMAL);
        dc.SetFont(f);
        dc.SetTextForeground(dark ? wxColour(160, 160, 166) : wxColour(124, 124, 124));
        ts = dc.GetTextExtent(m_sub);
        dc.DrawText(m_sub, (sz.x - ts.x) / 2, FromDIP(8) + m_bmp.GetHeight() + FromDIP(24));
    }

    wxBitmap m_bmp;
    wxString m_caption, m_sub;
    bool     m_selected = false;
};

enum class Mode { PickFirstPass, Save };

class SampleDialog : public DPIDialog
{
public:
    SampleDialog(wxWindow *parent, const Test &test, Mode mode)
        : DPIDialog(parent, wxID_ANY, mode == Mode::PickFirstPass ? _L("Flow rate: choose the best sample") : _L("Flow rate: save the result"),
                    wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_test(test)
        , m_mode(mode)
    {
        SetFont(Label::Body_14);
        auto *top = new wxBoxSizer(wxVERTICAL);

        wxString intro;
        if (mode == Mode::PickFirstPass)
            intro = _L("These are the samples of pass 1. Choose the one whose top surface is the smoothest and most closed, with no gaps between the lines and no ridges. Pass 2 is then made around it.");
        else
            intro = _L("Choose the sample of the last flow test that looks best. The filament's flow ratio is calculated from it.");
        auto *intro_text = new wxStaticText(this, wxID_ANY, intro);
        intro_text->Wrap(FromDIP(560));
        top->Add(intro_text, 0, wxALL, FromDIP(12));

        const size_t n    = test.mods.size();
        const int    cols = n <= 9 ? 3 : 4;
        auto        *grid = new wxGridSizer(cols, FromDIP(8), FromDIP(8));
        const wxSize bsz(FromDIP(104), FromDIP(78));
        for (size_t i = 0; i < n; ++i) {
            const double   mod  = test.mods[i];
            const wxString cap  = wxString::FromUTF8(format_mod(mod, test.linear) + (test.linear ? "" : " %"));
            const wxString sub  = wxString::FromUTF8(num(final_ratio(test, mod), 4));
            auto          *tile = new SampleTile(this, make_tile_bitmap(wxString::FromUTF8(format_mod(mod, test.linear)), test.pattern, bsz.x, bsz.y), cap, sub);
            tile->on_click      = [this, i]() { select(int(i)); };
            tile->on_activate   = [this]() {
                if (m_selected >= 0 && m_primary != nullptr && m_primary->IsEnabled())
                    on_primary();
            };
            m_tiles.push_back(tile);
            grid->Add(tile, 0, wxALIGN_CENTER);
        }
        top->Add(grid, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(12));

        m_result = new wxStaticText(this, wxID_ANY, wxEmptyString);
        top->Add(m_result, 0, wxALL, FromDIP(12));

        auto *btns = new wxBoxSizer(wxHORIZONTAL);
        if (mode == Mode::PickFirstPass) {
            m_primary = new wxButton(this, wxID_ANY, _L("Make pass 2 around it"));
            m_primary->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_primary(); });
            m_save = new wxButton(this, wxID_ANY, _L("Save it as the result"));
            m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { finish(Action::Save); });
            auto *cur = new wxButton(this, wxID_ANY, _L("Use the filament's current flow"));
            cur->SetToolTip(_L("Skip the choice: pass 2 is made around the flow ratio the filament has now."));
            cur->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { finish(Action::UseCurrent); });
            btns->Add(m_primary, 0, wxRIGHT, FromDIP(8));
            btns->Add(m_save, 0, wxRIGHT, FromDIP(8));
            btns->Add(cur, 0, wxRIGHT, FromDIP(8));
        } else {
            m_primary = new wxButton(this, wxID_ANY, _L("Set it on the filament"));
            m_primary->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_primary(); });
            btns->Add(m_primary, 0, wxRIGHT, FromDIP(8));
        }
        auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        btns->Add(cancel, 0);
        top->Add(btns, 0, wxALIGN_RIGHT | wxALL, FromDIP(12));

        SetSizer(top);
        wxGetApp().UpdateDlgDarkUI(this);
        Fit();
        CentreOnParent();
        update_state();
    }

    void on_dpi_changed(const wxRect &) override
    {
        Layout();
        Refresh();
    }

    Choice choice() const { return m_choice; }

private:
    void select(int i)
    {
        m_selected = i;
        for (size_t k = 0; k < m_tiles.size(); ++k)
            m_tiles[k]->set_selected(int(k) == i);
        update_state();
    }
    void update_state()
    {
        if (m_selected < 0) {
            m_result->SetLabel(_L("Click the best sample."));
        } else {
            const double mod = m_test.mods[size_t(m_selected)];
            m_result->SetLabel(wxString::Format(_L("Sample %s: the filament's flow ratio would be %s (now %s)."),
                                                wxString::FromUTF8(format_mod(mod, m_test.linear) + (m_test.linear ? "" : " %")),
                                                wxString::FromUTF8(num(final_ratio(m_test, mod), 4)),
                                                wxString::FromUTF8(num(m_test.current_flow, 4))));
        }
        if (m_primary)
            m_primary->Enable(m_selected >= 0);
        if (m_save)
            m_save->Enable(m_selected >= 0);
        Layout();
    }
    void on_primary() { finish(m_mode == Mode::PickFirstPass ? Action::Generate : Action::Save); }
    void finish(Action a)
    {
        m_choice.action = a;
        m_choice.mod    = (m_selected >= 0 && m_selected < int(m_test.mods.size())) ? m_test.mods[size_t(m_selected)] : 0.;
        EndModal(wxID_OK);
    }

    Test                      m_test;
    Mode                      m_mode;
    std::vector<SampleTile *> m_tiles;
    wxStaticText             *m_result   = nullptr;
    wxButton                 *m_primary  = nullptr;
    wxButton                 *m_save     = nullptr;
    int                       m_selected = -1;
    Choice                    m_choice;
};

} // namespace

Choice pick_first_pass(wxWindow *parent, const Test &pass1)
{
    SampleDialog dlg(parent, pass1, Mode::PickFirstPass);
    if (dlg.ShowModal() != wxID_OK)
        return {};
    return dlg.choice();
}

bool confirm_and_apply(wxWindow *parent, double ratio)
{
    double      old_value = 0.;
    std::string name, err;
    if (!current_filament_flow(old_value, name, err)) {
        MessageDialog(parent, _L("Select a filament preset first."), _L("Flow rate"), wxICON_ERROR | wxOK).ShowModal();
        return false;
    }
    const std::string text = Slic3r::format(_u8L("Filament \"%1%\": flow ratio %2% -> %3%.\n\nSet the new flow ratio on the filament now?"), name,
                                            num(old_value, 4), num(ratio, 4));
    MessageDialog     ask(parent, from_u8(text), _L("Flow rate"), wxICON_QUESTION | wxYES_NO);
    if (ask.ShowModal() != wxID_YES)
        return false;
    if (!apply_filament_flow(ratio, old_value, name, err)) {
        MessageDialog(parent, from_u8(err), _L("Flow rate"), wxICON_ERROR | wxOK).ShowModal();
        return false;
    }
    MessageDialog save(parent,
                       _L("The flow ratio is set and the filament preset is marked as modified. Save the preset now? (If you do not, it stays modified until you save or discard it.)"),
                       _L("Flow rate"), wxICON_QUESTION | wxYES_NO);
    if (save.ShowModal() == wxID_YES)
        if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
            tab->save_preset();
    return true;
}

bool save_result(wxWindow *parent, const Test &test)
{
    if (!test.valid || test.mods.empty())
        return false;
    SampleDialog dlg(parent, test, Mode::Save);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    return confirm_and_apply(parent, final_ratio(test, dlg.choice().mod));
}

void save_last_result(wxWindow *parent)
{
    if (!last().valid) {
        MessageDialog(parent, _L("No flow rate test has been made in this session yet. Make one from the Calibration menu first."), _L("Flow rate"),
                      wxICON_INFORMATION | wxOK)
            .ShowModal();
        return;
    }
    save_result(parent, last());
}

}}} // namespace Slic3r::GUI::FlowCalib
