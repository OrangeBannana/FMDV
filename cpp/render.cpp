#include "render.h"
#include "layout.h"
#include <map>
#include <vector>
#include <cwctype>
#include <cmath>

// ---------------- render scale (zoom * DPI) ----------------

static double g_scale = 1.0;
static int S(int v) { return (int)std::lround(v * g_scale); } // scale a layout constant
static BYTE g_fontQuality = CLEARTYPE_QUALITY; // window uses ClearType; PNG dump uses grayscale

// ---------------- font cache ----------------

enum FontRole { F_Body, F_Mono, F_H1, F_H2, F_H3, F_H4, F_H5, F_H6 };

struct FontKey {
    int role; bool bold; bool italic;
    bool operator<(const FontKey& o) const {
        if (role != o.role) return role < o.role;
        if (bold != o.bold) return bold < o.bold;
        return italic < o.italic;
    }
};

static std::map<FontKey, HFONT> g_fonts;
static std::map<HFONT, int> g_fontHeight; // cached tmHeight per font
static std::map<HFONT, int> g_fontAscent; // cached tmAscent per font

static void roleInfo(int role, const wchar_t** family, int* px, bool* forceBold) {
    *forceBold = false;
    switch (role) {
        case F_Mono: *family = L"Consolas";  *px = 14; break;
        case F_H1:   *family = L"Segoe UI";  *px = 30; *forceBold = true; break;
        case F_H2:   *family = L"Segoe UI";  *px = 24; *forceBold = true; break;
        case F_H3:   *family = L"Segoe UI";  *px = 20; *forceBold = true; break;
        case F_H4:   *family = L"Segoe UI";  *px = 16; *forceBold = true; break;
        case F_H5:   *family = L"Segoe UI";  *px = 14; *forceBold = true; break;
        case F_H6:   *family = L"Segoe UI";  *px = 13; *forceBold = true; break;
        default:     *family = L"Segoe UI";  *px = 16; break; // F_Body
    }
}

static HFONT GetFont(int role, bool bold, bool italic) {
    FontKey k{role, bold, italic};
    auto it = g_fonts.find(k);
    if (it != g_fonts.end()) return it->second;

    const wchar_t* family; int px; bool forceBold;
    roleInfo(role, &family, &px, &forceBold);

    LOGFONTW lf = {};
    lf.lfHeight = -S(px);              // negative = character height in pixels (scaled by zoom/DPI)
    lf.lfWeight = (bold || forceBold) ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfItalic = italic ? TRUE : FALSE;
    lf.lfQuality = g_fontQuality;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, family);

    HFONT f = CreateFontIndirectW(&lf);
    g_fonts[k] = f;
    return f;
}

void FreeFontCache() {
    for (auto& kv : g_fonts) DeleteObject(kv.second);
    g_fonts.clear();
    g_fontHeight.clear();
    g_fontAscent.clear();
}

void SetFontQuality(int quality) {
    if ((BYTE)quality == g_fontQuality) return;
    g_fontQuality = (BYTE)quality;
    FreeFontCache();
}

void SetRenderScale(double scale) {
    if (scale < 0.4) scale = 0.4;
    if (scale > 4.0) scale = 4.0;
    if (scale == g_scale) return;
    g_scale = scale;
    FreeFontCache(); // fonts are sized at creation; rebuild at new scale
}

static int FontHeight(HDC hdc, HFONT f) {
    auto it = g_fontHeight.find(f);
    if (it != g_fontHeight.end()) return it->second;
    HFONT old = (HFONT)SelectObject(hdc, f);
    TEXTMETRICW tm; GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    g_fontHeight[f] = tm.tmHeight;
    g_fontAscent[f] = tm.tmAscent;
    return tm.tmHeight;
}

static int FontAscent(HDC hdc, HFONT f) {
    auto it = g_fontAscent.find(f);
    if (it != g_fontAscent.end()) return it->second;
    FontHeight(hdc, f); // fills both caches
    return g_fontAscent[f];
}

static int TextW(HDC hdc, HFONT f, StrView s) {
    HFONT old = (HFONT)SelectObject(hdc, f);
    SIZE sz; GetTextExtentPoint32W(hdc, s.data(), (int)s.size(), &sz);
    SelectObject(hdc, old);
    return sz.cx;
}

// ---------------- GDI text measurer ----------------
// fmdv::TextMeasurer over the font cache above (macOS impl guide, Phase 2).
// Wraps the exact GetTextExtentPoint32W / GetTextMetricsW calls the layout has
// always used, so core layout driven by it reproduces the old engine's pixels.
// Font px comes from the g_fonts cache (S()-scaled at creation), not FontSpec::px.
static int RoleIdx(fmdv::FontRole r) {
    switch (r) {
        case fmdv::FontRole::Mono: return F_Mono;
        case fmdv::FontRole::H1:   return F_H1;
        case fmdv::FontRole::H2:   return F_H2;
        case fmdv::FontRole::H3:   return F_H3;
        case fmdv::FontRole::H4:   return F_H4;
        case fmdv::FontRole::H5:   return F_H5;
        case fmdv::FontRole::H6:   return F_H6;
        case fmdv::FontRole::Body: default: return F_Body;
    }
}

class GdiTextMeasurer : public fmdv::TextMeasurer {
public:
    explicit GdiTextMeasurer(HDC hdc) : hdc_(hdc) {}
    HFONT font(const fmdv::FontSpec& f) { return GetFont(RoleIdx(f.role), f.bold, f.italic); }
    double textWidth(const fmdv::FontSpec& f, StrView s) override { return TextW(hdc_, font(f), s); }
    double lineHeight(const fmdv::FontSpec& f) override { return FontHeight(hdc_, font(f)); }
    double ascent(const fmdv::FontSpec& f) override { return FontAscent(hdc_, font(f)); }
private:
    HDC hdc_;
};

// ---------------- selection char/x mapping ----------------

int FragCharAtX(HDC hdc, const TextFrag& f, int x) {
    int n = (int)f.text.size();
    if (n == 0) return 0;
    int xoff = x - f.rc.left;
    if (xoff <= 0) return 0;
    HFONT old = (HFONT)SelectObject(hdc, f.font);
    int fit = 0; SIZE sz;
    GetTextExtentExPointW(hdc, f.text.c_str(), n, xoff, &fit, nullptr, &sz);
    // round to the nearer char edge
    if (fit < n) {
        int wA = 0, wB = 0; SIZE s2;
        GetTextExtentPoint32W(hdc, f.text.c_str(), fit, &s2); wA = s2.cx;
        GetTextExtentPoint32W(hdc, f.text.c_str(), fit + 1, &s2); wB = s2.cx;
        if (xoff > (wA + wB) / 2) fit++;
    }
    SelectObject(hdc, old);
    return fit;
}

int FragXAtChar(HDC hdc, const TextFrag& f, int ch) {
    if (ch <= 0) return f.rc.left;
    int n = (int)f.text.size();
    if (ch > n) ch = n;
    HFONT old = (HFONT)SelectObject(hdc, f.font);
    SIZE sz; GetTextExtentPoint32W(hdc, f.text.c_str(), ch, &sz);
    SelectObject(hdc, old);
    return f.rc.left + sz.cx;
}

// ---------------- cached display list ----------------
// Layout (in core/layout) produces draw commands in DOCUMENT space (y not
// scroll-adjusted) once; they are translated to GDI-typed commands here, and
// painting culls to the viewport and offsets by scrollY. Scrolling reuses the
// cached list instead of re-measuring/re-laying-out every frame.

enum CmdKind { C_RECT, C_FRAME, C_LINE, C_TEXT };
struct DrawCmd {
    int kind;
    int x, y, w, h;      // RECT/FRAME: x,y,w,h ; LINE: (x,y)->(w,h) ; TEXT: x,y + h=run height
    COLORREF color;
    HFONT font;          // TEXT only
    std::wstring text;   // TEXT only
};
static std::vector<DrawCmd> g_cmds;

// ---------------- layout (shared core engine + GDI translation) ----------------

static fmdv::Color ToColor(COLORREF c) {
    return fmdv::Color{ GetRValue(c), GetGValue(c), GetBValue(c), 255 };
}
static COLORREF ToColorRef(const fmdv::Color& c) { return RGB(c.r, c.g, c.b); }
static int Px(double v) { return (int)std::llround(v); }

int LayoutDocument(HDC hdc, int width, const Document& doc, const Theme& th,
                   std::vector<LinkHit>* links, std::vector<TextFrag>* frags,
                   std::vector<int>* blockTops, std::vector<TaskHit>* taskHits) {
    g_cmds.clear();
    if (links) links->clear();
    if (frags) frags->clear();
    if (blockTops) blockTops->clear();
    if (taskHits) taskHits->clear();

    fmdv::LayoutTheme lth;
    lth.bg = ToColor(th.bg);     lth.bg2 = ToColor(th.bg2);       lth.bg3 = ToColor(th.bg3);
    lth.text = ToColor(th.text); lth.text2 = ToColor(th.text2);   lth.border = ToColor(th.border);
    lth.link = ToColor(th.link); lth.codeText = ToColor(th.codeText); lth.sel = ToColor(th.sel);

    // The measurer returns metrics for S()-scaled fonts, and core scales its
    // layout constants by the same factor, so document space stays in device
    // pixels exactly as the old GDI layout's did.
    GdiTextMeasurer tm(hdc);
    fmdv::LayoutResult res = fmdv::LayoutDocument(doc, width, lth, tm, g_scale);

    g_cmds.reserve(res.cmds.size());
    for (const auto& c : res.cmds) {
        switch (c.kind) {
        case fmdv::DrawCommand::FillRect:
            g_cmds.push_back({C_RECT, Px(c.rect.x), Px(c.rect.y), Px(c.rect.w), Px(c.rect.h),
                              ToColorRef(c.color), nullptr, {}});
            break;
        case fmdv::DrawCommand::FrameRect:
            g_cmds.push_back({C_FRAME, Px(c.rect.x), Px(c.rect.y), Px(c.rect.w), Px(c.rect.h),
                              ToColorRef(c.color), nullptr, {}});
            break;
        case fmdv::DrawCommand::Line:
            g_cmds.push_back({C_LINE, Px(c.rect.x), Px(c.rect.y), Px(c.rect.w), Px(c.rect.h),
                              ToColorRef(c.color), nullptr, {}});
            break;
        case fmdv::DrawCommand::Text: {
            HFONT f = tm.font(c.font);
            int x = Px(c.rect.x);
            int top = Px(c.rect.y) - FontAscent(hdc, f); // rect.y is the baseline
            int h = Px(c.rect.h);
            g_cmds.push_back({C_TEXT, x, top, 0, h, ToColorRef(c.color), f, c.text});
            if (frags && c.selectable)
                frags->push_back(TextFrag{ RECT{ x, top, x + Px(c.rect.w), top + h },
                                           c.text, f, c.spaceBefore });
            break;
        }
        }
    }
    if (links)
        for (const auto& l : res.links)
            links->push_back(LinkHit{ RECT{ Px(l.rect.x), Px(l.rect.y),
                                            Px(l.rect.x + l.rect.w), Px(l.rect.y + l.rect.h) },
                                      l.href });
    if (taskHits)
        for (const auto& t : res.taskHits)
            taskHits->push_back(TaskHit{ RECT{ Px(t.rect.x), Px(t.rect.y),
                                               Px(t.rect.x + t.rect.w), Px(t.rect.y + t.rect.h) },
                                         t.srcLine, t.state });
    if (blockTops)
        for (double t : res.blockTops) blockTops->push_back(Px(t));

    return Px(res.contentHeight);
}

// ---------------- paint (cull cached list to viewport + selection) ----------------

void PaintDocument(HDC hdc, int scrollY, int clientW, int clientH, const Theme& th,
                   const Selection* sel, const std::vector<TextFrag>& frags,
                   const std::vector<FindMatch>* findMatches, int currentMatch) {
    (void)clientW;
    int top = scrollY, bot = scrollY + clientH;
    auto vis = [&](int t, int b) { return b >= top && t <= bot; };

    // phase 1: backgrounds, frames, lines (in document order)
    for (const auto& c : g_cmds) {
        if (c.kind == C_TEXT) continue;
        if (c.kind == C_LINE) {
            int t = (c.y < c.h ? c.y : c.h), b = (c.y < c.h ? c.h : c.y);
            if (!vis(t, b)) continue;
            HPEN pen = CreatePen(PS_SOLID, 1, c.color);
            HPEN op = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, c.x, c.y - scrollY, nullptr);
            LineTo(hdc, c.w, c.h - scrollY);
            SelectObject(hdc, op); DeleteObject(pen);
        } else {
            if (!vis(c.y, c.y + c.h)) continue;
            RECT rc{ c.x, c.y - scrollY, c.x + c.w, c.y + c.h - scrollY };
            HBRUSH br = CreateSolidBrush(c.color);
            if (c.kind == C_FRAME) FrameRect(hdc, &rc, br);
            else                   FillRect(hdc, &rc, br);
            DeleteObject(br);
        }
    }

    // phase 1b: find-in-doc match highlights (behind text, drawn before
    // selection so an active selection still shows on top if they overlap)
    if (findMatches && !findMatches->empty()) {
        HBRUSH hbAll = CreateSolidBrush(th.findHi);
        HBRUSH hbCur = CreateSolidBrush(th.findCur);
        for (size_t i = 0; i < findMatches->size(); i++) {
            const FindMatch& m = (*findMatches)[i];
            if (m.frag < 0 || m.frag >= (int)frags.size()) continue;
            const TextFrag& f = frags[m.frag];
            if (!vis(f.rc.top, f.rc.bottom)) continue;
            int hx0 = FragXAtChar(hdc, f, m.chStart);
            int hx1 = FragXAtChar(hdc, f, m.chEnd);
            if (hx1 <= hx0) continue;
            RECT hr{ hx0, f.rc.top - scrollY, hx1, f.rc.bottom - scrollY };
            FillRect(hdc, &hr, (int)i == currentMatch ? hbCur : hbAll);
        }
        DeleteObject(hbAll); DeleteObject(hbCur);
    }

    // phase 2: selection highlight (behind text)
    if (sel && sel->active && !frags.empty()) {
        HBRUSH hb = CreateSolidBrush(th.sel);
        for (int i = sel->a.frag; i <= sel->b.frag && i < (int)frags.size(); i++) {
            const TextFrag& f = frags[i];
            if (!vis(f.rc.top, f.rc.bottom)) continue;
            int c0 = (i == sel->a.frag) ? sel->a.ch : 0;
            int c1 = (i == sel->b.frag) ? sel->b.ch : (int)f.text.size();
            int hx0 = FragXAtChar(hdc, f, c0);
            int hx1 = FragXAtChar(hdc, f, c1);
            if (i != sel->b.frag && hx1 <= hx0) hx1 = hx0 + 4; // whole-line marker
            if (hx1 > hx0) {
                RECT hr{ hx0, f.rc.top - scrollY, hx1, f.rc.bottom - scrollY };
                FillRect(hdc, &hr, hb);
            }
        }
        DeleteObject(hb);
    }

    // phase 3: text
    SetBkMode(hdc, TRANSPARENT);
    for (const auto& c : g_cmds) {
        if (c.kind != C_TEXT) continue;
        if (!vis(c.y, c.y + c.h)) continue;
        HFONT old = (HFONT)SelectObject(hdc, c.font);
        SetTextColor(hdc, c.color);
        TextOutW(hdc, c.x, c.y - scrollY, c.text.c_str(), (int)c.text.size());
        SelectObject(hdc, old);
    }
}
