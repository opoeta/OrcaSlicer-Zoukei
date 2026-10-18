#include "ParamTipCard.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "wxExtensions.hpp"
#include "OptionsGroup.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/bitmap.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/popupwin.h>
#include <wx/region.h>
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI {

namespace {

// ---------------------------------------------------------------------------------------------
// notes.json: { "<config_key>": { "notes": ["..."], "recommend": "..." } }  (English, translated at display)
// ---------------------------------------------------------------------------------------------
struct TipNotes
{
    std::vector<std::string> notes;
    std::string              recommend;
};

const TipNotes* find_notes(const std::string& key)
{
    static std::unordered_map<std::string, TipNotes> s_notes;
    static bool                                      s_loaded = false;

    if (!s_loaded) {
        s_loaded = true;
        try {
            const boost::filesystem::path path = boost::filesystem::path(resources_dir()) / "param_tips" / "notes.json";
            boost::system::error_code     ec;
            if (boost::filesystem::exists(path, ec)) {
                boost::nowide::ifstream ifs(path.string());
                nlohmann::json          j = nlohmann::json::parse(ifs, nullptr, false);
                if (j.is_object()) {
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        if (!it.value().is_object())
                            continue;
                        TipNotes n;
                        auto     nit = it.value().find("notes");
                        if (nit != it.value().end() && nit->is_array())
                            for (const auto& e : *nit)
                                if (e.is_string())
                                    n.notes.push_back(e.get<std::string>());
                        auto rit = it.value().find("recommend");
                        if (rit != it.value().end() && rit->is_string())
                            n.recommend = rit->get<std::string>();
                        if (!n.notes.empty() || !n.recommend.empty())
                            s_notes[it.key()] = std::move(n);
                    }
                }
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "ParamTipCard: cannot load notes.json: " << e.what();
        } catch (...) {
        }
    }

    auto it = s_notes.find(key);
    return it == s_notes.end() ? nullptr : &it->second;
}

bool is_safe_key(const std::string& key)
{
    if (key.empty())
        return false;
    for (char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

// Splits `text` into lines that fit `max_w`, honouring '\n' and breaking over-long words per character.
void wrap_text(wxDC& dc, const wxString& text, int max_w, std::vector<wxString>& out)
{
    wxString para;
    size_t   start = 0;
    const size_t len = text.length();
    while (start <= len) {
        size_t nl = text.find('\n', start);
        if (nl == wxString::npos)
            nl = len;
        para = text.Mid(start, nl - start);
        start = nl + 1;

        if (para.IsEmpty()) {
            out.push_back(wxString());
            continue;
        }
        wxString line;
        wxString word;
        auto flush_word = [&]() {
            if (word.IsEmpty())
                return;
            wxString trial = line.IsEmpty() ? word : line + " " + word;
            if (dc.GetTextExtent(trial).x <= max_w) {
                line = trial;
            } else {
                if (!line.IsEmpty())
                    out.push_back(line);
                line.clear();
                // word alone too wide: break by characters
                wxString piece;
                for (size_t i = 0; i < word.length(); ++i) {
                    const wxUniChar ch = word[i];
                    wxString        t2 = piece;
                    t2 += ch;
                    if (!piece.IsEmpty() && dc.GetTextExtent(t2).x > max_w) {
                        out.push_back(piece);
                        piece = wxString(ch);
                    } else {
                        piece = t2;
                    }
                }
                line = piece;
            }
            word.clear();
        };
        for (size_t i = 0; i < para.length(); ++i) {
            const wxUniChar c = para[i];
            if (c == ' ')
                flush_word();
            else
                word += c; // spaceless (CJK) text ends up as one long word, broken per character above
        }
        flush_word();
        if (!line.IsEmpty())
            out.push_back(line);
    }
    // drop trailing blank lines
    while (!out.empty() && out.back().IsEmpty())
        out.pop_back();
}

struct Palette
{
    wxColour bg, border, text, muted, accent, warn, chip, chip_text;
};

Palette make_palette()
{
    Palette p;
    if (wxGetApp().dark_mode()) {
        p.bg        = wxColour(0x2E, 0x2E, 0x32);
        p.border    = wxColour(0x4A, 0x4A, 0x51);
        p.text      = wxColour(0xEF, 0xEF, 0xF0);
        p.muted     = wxColour(0xB3, 0xB3, 0xB5);
        p.accent    = wxColour(0x2D, 0xC4, 0xB2);
        p.chip      = wxColour(0x3C, 0x3C, 0x42);
        p.chip_text = wxColour(0xEF, 0xEF, 0xF0);
    } else {
        p.bg        = wxColour(0xFF, 0xFF, 0xFF);
        p.border    = wxColour(0xDB, 0xDB, 0xDB);
        p.text      = wxColour(0x26, 0x2E, 0x30);
        p.muted     = wxColour(0x6B, 0x6B, 0x6B);
        p.accent    = wxColour(0x00, 0x96, 0x88);
        p.chip      = wxColour(0xF0, 0xF0, 0xF1);
        p.chip_text = wxColour(0x26, 0x2E, 0x30);
    }
    p.warn = wxColour(0xF5, 0x9E, 0x0B);
    return p;
}

constexpr int CARD_W_DIP = 360;
constexpr int PAD_DIP    = 12;

// ---------------------------------------------------------------------------------------------
// The card window. Non-activating popup: never takes keyboard focus from the settings fields.
// ---------------------------------------------------------------------------------------------
class ParamTipWindow : public wxPopupWindow, public wxEventFilter
{
public:
    explicit ParamTipWindow(wxWindow* parent);
    ~ParamTipWindow() override;

    bool hover(wxWindow* anchor, const wxRect& label_rect, const wxString& title, const wxString& tooltip, const std::string& wiki_path);
    void leave();
    void cancel();
    void dismiss(); // cancel and stay closed until the mouse leaves the label

    int FilterEvent(wxEvent& event) override;

private:
    enum { ID_SHOW_TIMER = wxID_HIGHEST + 4101, ID_POLL_TIMER, ID_COPIED_TIMER };

    bool parse(const wxString& title, const wxString& tooltip, const std::string& wiki_path);
    int  render(wxDC& dc, bool paint);

    void OnShowTimer(wxTimerEvent&);
    void OnPollTimer(wxTimerEvent&);
    void OnCopiedTimer(wxTimerEvent&);
    void OnPaint(wxPaintEvent&);
    void OnLeftDown(wxMouseEvent&);
    void OnMotion(wxMouseEvent&);
    void apply_shape();
    void do_show();

    wxTimer m_show_timer;
    wxTimer m_poll_timer;
    wxTimer m_copied_timer;

    // state
    bool           m_active{false};  // filter installed
    bool           m_pending{false}; // waiting for the show delay
    bool           m_visible{false};
    wxWindow*      m_anchor{nullptr};
    wxString       m_ident;
    wxWindow*      m_suppr_anchor{nullptr}; // dismissed with Esc/click: do not re-open until the mouse leaves
    wxString       m_suppr_ident;
    wxRect         m_label_rect; // screen coords, slightly inflated
    wxLongLong     m_shown_at{0};

    // content
    wxString              m_title;
    wxString              m_desc;
    wxString              m_meta;
    std::string           m_key;
    wxString              m_url;
    std::vector<wxString> m_notes;
    wxString              m_recommend;
    bool                  m_recommend_warn{false};
    wxBitmap              m_bitmap;

    // layout results
    wxRect m_copy_rect;
    wxRect m_link_rect;
    bool   m_copied{false};
};

ParamTipWindow* s_window = nullptr;

ParamTipWindow::ParamTipWindow(wxWindow* parent)
    : wxPopupWindow(parent, wxBORDER_NONE)
    , m_show_timer(this, ID_SHOW_TIMER)
    , m_poll_timer(this, ID_POLL_TIMER)
    , m_copied_timer(this, ID_COPIED_TIMER)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &ParamTipWindow::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &ParamTipWindow::OnLeftDown, this);
    Bind(wxEVT_MOTION, &ParamTipWindow::OnMotion, this);
    Bind(wxEVT_TIMER, &ParamTipWindow::OnShowTimer, this, ID_SHOW_TIMER);
    Bind(wxEVT_TIMER, &ParamTipWindow::OnPollTimer, this, ID_POLL_TIMER);
    Bind(wxEVT_TIMER, &ParamTipWindow::OnCopiedTimer, this, ID_COPIED_TIMER);
}

ParamTipWindow::~ParamTipWindow()
{
    if (m_active)
        wxEvtHandler::RemoveFilter(this);
    m_active = false;
    if (s_window == this)
        s_window = nullptr;
}

void ParamTipWindow::cancel()
{
    m_show_timer.Stop();
    m_poll_timer.Stop();
    m_copied_timer.Stop();
    if (m_active) {
        m_active = false;
        wxEvtHandler::RemoveFilter(this);
    }
    m_pending = false;
    m_copied  = false;
    m_anchor  = nullptr;
    m_ident.clear();
    if (m_visible) {
        m_visible = false;
        Hide();
    }
}

void ParamTipWindow::dismiss()
{
    const wxString ident  = m_ident;
    wxWindow*      anchor = m_anchor;
    cancel();
    m_suppr_anchor = anchor;
    m_suppr_ident  = ident;
}

void ParamTipWindow::leave()
{
    m_suppr_anchor = nullptr;
    m_suppr_ident.clear();
    // A visible card keeps itself alive while the mouse is over it (see OnPollTimer).
    if (m_pending)
        cancel();
}

int ParamTipWindow::FilterEvent(wxEvent& event)
{
    if (!m_active)
        return Event_Skip;

    const wxEventType t = event.GetEventType();
    if (t == wxEVT_MOUSEWHEEL) {
        if (event.GetEventObject() != this)
            cancel();
    } else if (t == wxEVT_KEY_DOWN) {
        if (static_cast<wxKeyEvent&>(event).GetKeyCode() == WXK_ESCAPE)
            dismiss();
    } else if (t == wxEVT_LEFT_DOWN || t == wxEVT_RIGHT_DOWN) {
        if (event.GetEventObject() != this)
            dismiss();
    } else if (t == wxEVT_KILL_FOCUS || t == wxEVT_SET_FOCUS || t == wxEVT_ACTIVATE || t == wxEVT_ACTIVATE_APP) {
        // showing the popup may itself produce activation noise: ignore it briefly
        if (!m_visible || (wxGetLocalTimeMillis() - m_shown_at) > 250)
            if (event.GetEventObject() != this)
                cancel();
    }
    return Event_Skip;
}

bool ParamTipWindow::parse(const wxString& title, const wxString& tooltip, const std::string& wiki_path)
{
    const wxString marker = _L("parameter name") + ": ";
    const int      p      = tooltip.Find(marker);
    if (p == wxNOT_FOUND)
        return false;

    m_title = title;
    m_title.Trim(true).Trim(false);
    m_desc = tooltip.Left(p);
    m_desc.Trim(true).Trim(false);

    wxString rest = tooltip.Mid(size_t(p) + marker.length());
    const int nl  = rest.Find('\n');
    wxString key  = nl == wxNOT_FOUND ? rest : rest.Left(nl);
    m_meta        = nl == wxNOT_FOUND ? wxString() : rest.Mid(size_t(nl) + 1);
    key.Trim(true).Trim(false);
    m_meta.Trim(true).Trim(false);

    // "key[3]" -> "key"
    const int br = key.Find('[');
    if (br != wxNOT_FOUND)
        key = key.Left(br);
    m_key = std::string(key.ToUTF8().data());
    if (!is_safe_key(m_key))
        return false;

    m_url = wiki_path.empty() ? OptionsGroup::get_url(std::string()) : OptionsGroup::get_url(wiki_path);

    m_notes.clear();
    m_recommend.clear();
    m_recommend_warn = false;
    if (const TipNotes* n = find_notes(m_key)) {
        for (const std::string& s : n->notes) {
            wxString t = _L(s);
            t.Trim(true).Trim(false);
            if (t.StartsWith("* ") || t.StartsWith("- "))
                t = t.Mid(2);
            if (!t.IsEmpty())
                m_notes.push_back(t);
        }
        if (!n->recommend.empty()) {
            m_recommend      = _L(n->recommend);
            m_recommend_warn = n->recommend.compare(0, 7, "Warning") == 0 || n->recommend.compare(0, 7, "Caution") == 0 ||
                               n->recommend[0] == '!';
        }
    }

    // illustration (optional)
    m_bitmap = wxNullBitmap;
    const std::string rel = "param_tips/" + m_key;
    boost::system::error_code ec;
    if (boost::filesystem::exists(boost::filesystem::path(Slic3r::var(rel + ".svg")), ec)) {
        try {
            // 480x300 viewBox: height in DIP so the width matches the inner card width
            m_bitmap = create_scaled_bitmap(rel, this, int((CARD_W_DIP - 2 * PAD_DIP) * 300 / 480));
        } catch (...) {
            m_bitmap = wxNullBitmap;
        }
    }
    return true;
}

bool ParamTipWindow::hover(wxWindow* anchor, const wxRect& label_rect, const wxString& title, const wxString& tooltip,
                           const std::string& wiki_path)
{
    if (anchor == m_anchor && (m_pending || m_visible) && m_ident == tooltip)
        return true;
    if (anchor == m_suppr_anchor && m_suppr_ident == tooltip)
        return true;

    cancel();
    if (!parse(title, tooltip, wiki_path))
        return false;

    m_anchor     = anchor;
    m_ident      = tooltip;
    m_label_rect = wxRect(anchor->ClientToScreen(label_rect.GetPosition()), label_rect.GetSize());
    m_label_rect.Inflate(2, 3);

    m_active = true;
    wxEvtHandler::AddFilter(this);
    m_pending = true;
    m_show_timer.StartOnce(500);
    return true;
}

void ParamTipWindow::OnShowTimer(wxTimerEvent&)
{
    if (!m_pending)
        return;
    m_pending = false;
    if (!wxTheApp->IsActive() || !m_label_rect.Contains(wxGetMousePosition())) {
        cancel();
        return;
    }
    do_show();
}

void ParamTipWindow::OnPollTimer(wxTimerEvent&)
{
    if (!m_visible) {
        m_poll_timer.Stop();
        return;
    }
    const wxPoint mp = wxGetMousePosition();
    wxRect        card = GetScreenRect();
    card.Inflate(2, 2);
    if (!card.Contains(mp) && !m_label_rect.Contains(mp))
        cancel();
}

void ParamTipWindow::OnCopiedTimer(wxTimerEvent&)
{
    m_copied = false;
    if (m_visible)
        Refresh();
}

void ParamTipWindow::do_show()
{
    // measure
    int height = 0;
    {
        wxClientDC dc(this);
        height = render(dc, false);
    }
    const int width = FromDIP(CARD_W_DIP);

    int disp_idx = wxDisplay::GetFromPoint(m_label_rect.GetPosition());
    if (disp_idx < 0)
        disp_idx = 0;
    const wxRect area = wxDisplay(static_cast<unsigned int>(disp_idx)).GetClientArea();

    int x = m_label_rect.GetLeft();
    int y = m_label_rect.GetBottom() + 1;
    if (y + height > area.GetBottom())
        y = m_label_rect.GetTop() - height - 1;
    if (y < area.GetTop())
        y = area.GetTop();
    if (x + width > area.GetRight())
        x = area.GetRight() - width;
    if (x < area.GetLeft())
        x = area.GetLeft();

    SetSize(x, y, width, height);
    apply_shape();
    m_visible  = true;
    m_shown_at = wxGetLocalTimeMillis();
    Show();
    Refresh();
    m_poll_timer.Start(150);
}

void ParamTipWindow::apply_shape()
{
#ifndef __WXOSX__
    const wxSize size = GetClientSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return;
    wxBitmap bmp(size.GetWidth(), size.GetHeight(), 32);
    if (!bmp.IsOk())
        return;
    {
        wxMemoryDC dc;
        dc.SelectObject(bmp);
        dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
        dc.Clear();
        dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), FromDIP(8));
        dc.SelectObject(wxNullBitmap);
    }
    wxRegion region(bmp, wxColour(0, 0, 0));
    if (region.IsOk())
        SetShape(region);
#endif
}

// Lays out (and optionally paints) the card. Returns the total height in pixels.
int ParamTipWindow::render(wxDC& dc, bool paint)
{
    const Palette pal   = make_palette();
    const int     W     = FromDIP(CARD_W_DIP);
    const int     pad   = FromDIP(PAD_DIP);
    const int     inner = W - 2 * pad;
    const int     gap   = FromDIP(8);
    int           y     = pad;

    if (paint) {
        dc.SetBrush(wxBrush(pal.bg));
        dc.SetPen(wxPen(pal.border, 1));
        dc.DrawRoundedRectangle(0, 0, W, GetClientSize().GetHeight(), FromDIP(8));
    }

    std::vector<wxString> lines;
    auto text_block = [&](const wxFont& font, const wxColour& color, const wxString& text, int x, int width, int line_gap) {
        dc.SetFont(font);
        dc.SetTextForeground(color);
        lines.clear();
        wrap_text(dc, text, width, lines);
        const int lh = dc.GetCharHeight() + line_gap;
        for (const wxString& l : lines) {
            if (paint && !l.IsEmpty())
                dc.DrawText(l, x, y);
            y += lh;
        }
        return lh;
    };

    // title
    if (!m_title.IsEmpty()) {
        text_block(Label::Head_14, pal.text, m_title, pad, inner, 2);
        y += FromDIP(2);
    }
    // description
    if (!m_desc.IsEmpty()) {
        text_block(Label::Body_13, pal.text, m_desc, pad, inner, 2);
        y += gap;
    }
    // illustration
    if (m_bitmap.IsOk()) {
        const int bw = m_bitmap.GetWidth();
        const int bh = m_bitmap.GetHeight();
        if (paint)
            dc.DrawBitmap(m_bitmap, pad + std::max(0, (inner - bw) / 2), y, true);
        y += bh + gap;
    }
    // notes
    if (!m_notes.empty()) {
        const int indent = FromDIP(12);
        for (const wxString& n : m_notes) {
            if (paint) {
                dc.SetFont(Label::Body_13);
                dc.SetTextForeground(pal.muted);
                dc.DrawText(wxString(wchar_t(0x2022)), pad, y);
            }
            text_block(Label::Body_13, pal.muted, n, pad + indent, inner - indent, 2);
            y += FromDIP(2);
        }
        y += FromDIP(4);
    }
    // recommendation
    if (!m_recommend.IsEmpty()) {
        const wxColour col = m_recommend_warn ? pal.warn : pal.accent;
        const int      ip  = FromDIP(8);
        // measure first
        dc.SetFont(Label::Body_13);
        std::vector<wxString> rl;
        wrap_text(dc, m_recommend, inner - 2 * ip - FromDIP(3), rl);
        const int lh = dc.GetCharHeight() + 2;
        const int bh = int(rl.size()) * lh + 2 * ip - 2;
        if (paint) {
            // flat tint blended with the background (no alpha on plain wxDC)
            const double   a = wxGetApp().dark_mode() ? 0.22 : 0.12;
            const wxColour tint(int(pal.bg.Red() * (1 - a) + col.Red() * a), int(pal.bg.Green() * (1 - a) + col.Green() * a),
                            int(pal.bg.Blue() * (1 - a) + col.Blue() * a));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(tint));
            dc.DrawRoundedRectangle(pad, y, inner, bh, FromDIP(5));
            dc.SetBrush(wxBrush(col));
            dc.DrawRectangle(pad, y + FromDIP(4), FromDIP(3), bh - FromDIP(8));
            dc.SetTextForeground(col);
            int ty = y + ip - 1;
            for (const wxString& l : rl) {
                dc.DrawText(l, pad + ip + FromDIP(3), ty);
                ty += lh;
            }
        }
        y += bh + gap;
    }
    // default / range
    if (!m_meta.IsEmpty()) {
        text_block(Label::Body_12, pal.muted, m_meta, pad, inner, 1);
        y += FromDIP(6);
    }

    // footer: key chip + copy button
    {
        dc.SetFont(Label::Body_12);
        const int      cp       = FromDIP(6);
        const wxString key_text = wxString::FromUTF8(m_key.c_str());
        const wxString copy_txt = m_copied ? _L("Copied") : _L("Copy");
        const wxSize   ks       = dc.GetTextExtent(key_text);
        const wxSize   cs       = dc.GetTextExtent(copy_txt);
        const int      ch       = ks.y + 2 * FromDIP(3);
        const int      key_w    = std::min(ks.x + 2 * cp, inner - cs.x - 2 * cp - FromDIP(4));
        const int      copy_w   = cs.x + 2 * cp;

        const wxRect key_rect(pad, y, key_w, ch);
        m_copy_rect = wxRect(pad + key_w + FromDIP(4), y, copy_w, ch);
        if (paint) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(pal.chip));
            dc.DrawRoundedRectangle(key_rect, FromDIP(4));
            dc.SetTextForeground(pal.chip_text);
            dc.SetClippingRegion(key_rect.x + cp / 2, key_rect.y, key_rect.width - cp, key_rect.height);
            dc.DrawText(key_text, key_rect.x + cp, key_rect.y + FromDIP(3));
            dc.DestroyClippingRegion();

            dc.SetBrush(wxBrush(pal.chip));
            dc.DrawRoundedRectangle(m_copy_rect, FromDIP(4));
            dc.SetTextForeground(m_copied ? pal.accent : pal.chip_text);
            dc.DrawText(copy_txt, m_copy_rect.x + cp, m_copy_rect.y + FromDIP(3));
        }
        y += ch + gap;
    }

    // wiki link
    {
        wxFont f = Label::Body_12;
        f.SetUnderlined(true);
        dc.SetFont(f);
        const wxString txt = _L("View details on the Wiki");
        const wxSize   ts  = dc.GetTextExtent(txt);
        m_link_rect        = wxRect(pad, y, std::min(ts.x, inner), ts.y);
        if (paint) {
            dc.SetTextForeground(pal.accent);
            dc.DrawText(txt, pad, y);
        }
        y += ts.y;
    }

    return y + pad;
}

void ParamTipWindow::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(make_palette().bg));
    dc.Clear();
    render(dc, true);
}

void ParamTipWindow::OnMotion(wxMouseEvent& e)
{
    const bool hand = m_copy_rect.Contains(e.GetPosition()) || m_link_rect.Contains(e.GetPosition());
    SetCursor(hand ? wxCursor(wxCURSOR_HAND) : wxCursor(wxCURSOR_ARROW));
    e.Skip();
}

void ParamTipWindow::OnLeftDown(wxMouseEvent& e)
{
    const wxPoint pos = e.GetPosition();
    if (m_copy_rect.Contains(pos)) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(m_key.c_str())));
            wxTheClipboard->Close();
        }
        m_copied = true;
        m_copied_timer.StartOnce(1500);
        Refresh();
        return;
    }
    if (m_link_rect.Contains(pos)) {
        const wxString url = m_url;
        cancel();
        wxLaunchDefaultBrowser(url);
        return;
    }
    e.Skip();
}

ParamTipWindow* get_window()
{
    if (s_window == nullptr) {
        MainFrame* frame = wxGetApp().mainframe;
        if (frame == nullptr)
            return nullptr;
        s_window = new ParamTipWindow(frame);
    }
    return s_window;
}

} // namespace

bool ParamTipCard::enabled()
{
    AppConfig* cfg = wxGetApp().app_config;
    return cfg == nullptr || cfg->get("param_tip_cards") != "false";
}

bool ParamTipCard::hover(wxWindow* anchor, const wxRect& label_rect, const wxString& title, const wxString& tooltip,
                         const std::string& wiki_path)
{
    if (anchor == nullptr || tooltip.IsEmpty() || !enabled())
        return false;
    ParamTipWindow* w = get_window();
    if (w == nullptr)
        return false;
    return w->hover(anchor, label_rect, title, tooltip, wiki_path);
}

void ParamTipCard::leave()
{
    if (s_window != nullptr)
        s_window->leave();
}

void ParamTipCard::hide_now()
{
    if (s_window != nullptr)
        s_window->cancel();
}

}} // namespace Slic3r::GUI
