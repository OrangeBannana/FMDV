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
// fmdv::TextMeasurer over the font cache above (macOS impl guide, Phase 2 Step
// 2a). Wraps the exact GetTextExtentPoint32W / GetTextMetricsW calls the layout
// has always used, so routing measurement through it cannot move pixels. Font
// px comes from the g_fonts cache (S()-scaled at creation), not FontSpec::px.
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
// Layout builds a list of draw commands in DOCUMENT space (y not scroll-adjusted)
// once; painting culls to the viewport and offsets by scrollY. Scrolling reuses
// the cached list instead of re-measuring/re-laying-out every frame.

enum CmdKind { C_RECT, C_FRAME, C_LINE, C_TEXT };
struct DrawCmd {
    int kind;
    int x, y, w, h;      // RECT/FRAME: x,y,w,h ; LINE: (x,y)->(w,h) ; TEXT: x,y + h=lineHeight
    COLORREF color;
    HFONT font;          // TEXT only
    std::wstring text;   // TEXT only
};
static std::vector<DrawCmd> g_cmds;

// ---------------- layout primitives ----------------

namespace {

const int PAD_X = 40;       // left/right page padding
const int PAD_TOP = 32;

struct Word {
    std::wstring text;
    HFONT font;
    COLORREF color;
    bool code = false;   // inline code background
    bool link = false;   // underline
    bool strike = false; // line-through
    bool space = false;  // a space precedes this word in the source
    std::wstring href;   // link target (for hit-testing)
    int width = 0;
    int height = 0;
};

struct Ctx {
    GdiTextMeasurer* tm;           // all text metrics route through this (Step 2a)
    int contentLeft;
    int contentRight;
    const Theme* th;
    std::vector<LinkHit>* links;   // link hit rects (doc space)
    std::vector<TextFrag>* frags;  // selectable text runs (doc space)
};

// FontSpec for a local role (px is informational; the measurer's font cache
// applies the real S()-scaled size).
fmdv::FontSpec specFor(int role, bool bold, bool italic) {
    fmdv::FontRole r;
    switch (role) {
        case F_Mono: r = fmdv::FontRole::Mono; break;
        case F_H1:   r = fmdv::FontRole::H1; break;
        case F_H2:   r = fmdv::FontRole::H2; break;
        case F_H3:   r = fmdv::FontRole::H3; break;
        case F_H4:   r = fmdv::FontRole::H4; break;
        case F_H5:   r = fmdv::FontRole::H5; break;
        case F_H6:   r = fmdv::FontRole::H6; break;
        default:     r = fmdv::FontRole::Body; break;
    }
    return fmdv::FontSpec{r, bold, italic, fmdv::RoleSizePx(r)};
}

// emit helpers (all coordinates in document space)
void emitRect(int x, int y, int w, int h, COLORREF c)  { g_cmds.push_back({C_RECT,  x, y, w, h, c, nullptr, {}}); }
void emitFrame(int x, int y, int w, int h, COLORREF c) { g_cmds.push_back({C_FRAME, x, y, w, h, c, nullptr, {}}); }
void emitLine(int x1, int y1, int x2, int y2, COLORREF c) { g_cmds.push_back({C_LINE, x1, y1, x2, y2, c, nullptr, {}}); }
void emitTextCmd(int x, int y, int h, const std::wstring& s, HFONT f, COLORREF c) {
    g_cmds.push_back({C_TEXT, x, y, 0, h, c, f, s});
}

// Convert a run sequence into measured words (split on spaces).
void buildWords(Ctx& cx, const std::vector<InlineRun>& runs, int role,
                std::vector<Word>& words) {
    bool pendingSpace = false; // whitespace seen since last emitted word (carries across runs)
    for (const auto& r : runs) {
        int useRole = r.code ? F_Mono : role;
        fmdv::FontSpec spec = specFor(useRole, r.bold, r.italic);
        HFONT f = cx.tm->font(spec);
        COLORREF col = cx.th->text;
        if (!r.href.empty()) col = cx.th->link;
        else if (r.code) col = cx.th->codeText;

        std::wstring cur;
        auto flush = [&]() {
            if (cur.empty()) return;
            Word w; w.text = cur; w.font = f; w.color = col;
            w.code = r.code; w.link = !r.href.empty(); w.strike = r.strike;
            w.href = r.href;
            w.space = pendingSpace;
            w.width = (int)cx.tm->textWidth(spec, cur);
            w.height = (int)cx.tm->lineHeight(spec);
            words.push_back(w);
            cur.clear();
            pendingSpace = false;
        };
        for (wchar_t c : r.text) {
            if (c == L' ' || c == L'\t') { flush(); pendingSpace = true; }
            else cur += c;
        }
        flush();
    }
}

// Emit one text run + record it as a selectable fragment (doc space).
void emitRun(Ctx& cx, int x, int y, int widthPx, int height,
             const std::wstring& s, HFONT font, COLORREF color, bool spaceBefore) {
    emitTextCmd(x, y, height, s, font, color);
    if (cx.frags)
        cx.frags->push_back(TextFrag{ RECT{ x, y, x + widthPx, y + height }, s, font, spaceBefore });
}

// Lay out a wrapped run of words starting at x=indentLeft. Emits cmds. Returns y after.
int layoutWords(Ctx& cx, std::vector<Word>& words, int indentLeft, int y) {
    int spaceW = (int)cx.tm->textWidth(specFor(F_Body, false, false), L" ");
    int x = indentLeft;
    int lineStart = indentLeft;
    int lineH = 0;
    std::vector<Word*> line;

    auto flushLine = [&]() {
        if (line.empty()) { return; }
        size_t n = line.size();
        std::vector<int> dxs(n);
        int dx = lineStart;
        for (size_t i = 0; i < n; i++) {
            if (i > 0 && line[i]->space) dx += spaceW;
            dxs[i] = dx;
            dx += line[i]->width;
        }
        auto topY = [&](Word* w) { return y + (lineH - w->height); };

        // 1. inline-code backgrounds
        for (size_t i = 0; i < n; i++) if (line[i]->code) {
            int ty = topY(line[i]);
            emitRect(dxs[i] - 2, ty, line[i]->width + 4, line[i]->height, cx.th->bg2);
        }
        // 2. text (group consecutive same-font+color words -> one run for natural spacing)
        for (size_t i = 0; i < n; ) {
            size_t j = i;
            std::wstring s = line[i]->text;
            while (j + 1 < n && line[j+1]->font == line[i]->font
                             && line[j+1]->color == line[i]->color) {
                j++;
                s += (line[j]->space ? L" " : L"");
                s += line[j]->text;
            }
            int fx = dxs[i], fy = topY(line[i]);
            int wpx = dxs[j] + line[j]->width - fx;
            emitRun(cx, fx, fy, wpx, line[i]->height, s, line[i]->font, line[i]->color,
                    (i > 0 && line[i]->space));
            i = j + 1;
        }
        // 3. strikethrough
        for (size_t i = 0; i < n; i++) if (line[i]->strike) {
            int my = topY(line[i]) + line[i]->height / 2;
            emitLine(dxs[i], my, dxs[i] + line[i]->width, my, line[i]->color);
        }
        // 4. link underline + hit-rect (span consecutive same-href words)
        for (size_t i = 0; i < n; ) {
            if (line[i]->href.empty()) { i++; continue; }
            size_t j = i;
            while (j + 1 < n && line[j+1]->href == line[i]->href) j++;
            int x0 = dxs[i], x1 = dxs[j] + line[j]->width;
            int ty = topY(line[i]);
            emitLine(x0, ty + line[i]->height - 2, x1, ty + line[i]->height - 2, line[i]->color);
            if (cx.links)
                cx.links->push_back(LinkHit{ RECT{ x0, ty, x1, ty + line[i]->height }, line[i]->href });
            i = j + 1;
        }
        y += lineH;
        line.clear(); lineH = 0; x = lineStart;
    };

    for (auto& w : words) {
        int sp = (!line.empty() && w.space) ? spaceW : 0;
        int need = sp + w.width;
        if (!line.empty() && x + need > cx.contentRight) {
            flushLine();
        } else {
            x += sp;
        }
        line.push_back(&w);
        x += w.width;
        if (w.height > lineH) lineH = w.height;
    }
    flushLine();
    return y;
}

void hline(Ctx&, int l, int r, int y, COLORREF c) {
    emitRect(l, y, r - l, 1, c);
}

// Greedy word-wrap of plain text to fit maxWidth. No mid-word breaking (matches
// layoutWords' paragraph wrapping, which also never splits a word).
std::vector<std::wstring> WrapCellText(GdiTextMeasurer& tm, const fmdv::FontSpec& f,
                                       const std::wstring& text, int maxWidth) {
    std::vector<std::wstring> lines;
    std::vector<std::wstring> words; std::wstring cur;
    for (wchar_t c : text) {
        if (c == L' ' || c == L'\t') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty()) { lines.push_back(L""); return lines; }

    int spaceW = (int)tm.textWidth(f, L" ");
    std::wstring line = words[0];
    int lineW = (int)tm.textWidth(f, line);
    for (size_t i = 1; i < words.size(); i++) {
        int wW = (int)tm.textWidth(f, words[i]);
        if (lineW + spaceW + wW <= maxWidth) {
            line += L" " + words[i];
            lineW += spaceW + wW;
        } else {
            lines.push_back(line);
            line = words[i];
            lineW = wW;
        }
    }
    lines.push_back(line);
    return lines;
}

} // namespace

// ---------------- layout (build cached display list) ----------------

int LayoutDocument(HDC hdc, int width, const Document& doc, const Theme& th,
                   std::vector<LinkHit>* links, std::vector<TextFrag>* frags,
                   std::vector<int>* blockTops) {
    g_cmds.clear();
    if (links) links->clear();
    if (frags) frags->clear();
    if (blockTops) blockTops->clear();

    GdiTextMeasurer tm(hdc);
    Ctx cx;
    cx.tm = &tm;
    cx.contentLeft = S(PAD_X);
    cx.contentRight = width - S(PAD_X);
    cx.th = &th;
    cx.links = links;
    cx.frags = frags;

    int y = S(PAD_TOP);
    int olCounter = 0; // running number for ordered lists

    for (size_t bi = 0; bi < doc.blocks.size(); bi++) {
        const Block& b = doc.blocks[bi];
        if (blockTops) blockTops->push_back(y);
        // maintain ordered-list counter
        if (b.type == BlockType::ListItem && b.ordered) {
            const Block* prev = (bi > 0) ? &doc.blocks[bi-1] : nullptr;
            if (!prev || prev->type != BlockType::ListItem || !prev->ordered) olCounter = 1;
            else olCounter++;
        }
        switch (b.type) {
        case BlockType::Heading: {
            int role = F_H1 + (b.level - 1);
            if (role > F_H6) role = F_H6;
            y += (b.level <= 2) ? S(24) : S(18); // top margin
            std::vector<Word> words; buildWords(cx, b.runs, role, words);
            y = layoutWords(cx, words, cx.contentLeft, y);
            if (b.level <= 2) { // underline rule
                y += S(6);
                hline(cx, cx.contentLeft, cx.contentRight, y, th.border);
                y += S(10);
            } else {
                y += S(8);
            }
            break;
        }
        case BlockType::Paragraph: {
            std::vector<Word> words; buildWords(cx, b.runs, F_Body, words);
            y = layoutWords(cx, words, cx.contentLeft, y);
            y += S(16);
            break;
        }
        case BlockType::BlockQuote: {
            std::vector<Word> words;
            buildWords(cx, b.runs, F_Body, words);
            for (auto& w : words) if (!w.link) w.color = th.text2;
            int top = y;
            int yy = layoutWords(cx, words, cx.contentLeft + S(16), y);
            // left border bar
            emitRect(cx.contentLeft, top - 2, S(4), (yy + 2) - (top - 2), th.border);
            y = yy + S(16);
            break;
        }
        case BlockType::ListItem: {
            int bulletX = cx.contentLeft + S(8) + b.level * S(24);
            int indent = cx.contentLeft + S(24) + b.level * S(24);
            if (b.taskState >= 0) indent = bulletX + S(24); // extra gap after checkbox
            // bullet / number / checkbox
            std::vector<Word> words; buildWords(cx, b.runs, F_Body, words);
            fmdv::FontSpec bodySpec = specFor(F_Body, false, false);
            HFONT bf = tm.font(bodySpec);
            int lineH = words.empty() ? (int)tm.lineHeight(bodySpec) : 0;
            for (auto& w : words) if (w.height > lineH) lineH = w.height;
            if (b.taskState >= 0) {
                emitFrame(bulletX, y + S(3), S(14), S(14), th.text2);
                if (b.taskState == 1) { // checkmark
                    emitLine(bulletX + S(3), y + S(10), bulletX + S(6), y + S(13), th.text2);
                    emitLine(bulletX + S(6), y + S(13), bulletX + S(11), y + S(6), th.text2);
                }
            } else if (b.ordered) {
                wchar_t num[16]; _snwprintf_s(num, 16, _TRUNCATE, L"%d.", olCounter);
                emitTextCmd(bulletX, y, lineH, num, bf, th.text);
            } else {
                emitTextCmd(bulletX, y, lineH, L"\x2022", bf, th.text);
            }
            y = layoutWords(cx, words, indent, y);
            y += S(6);
            break;
        }
        case BlockType::CodeBlock: {
            fmdv::FontSpec monoSpec = specFor(F_Mono, false, false);
            HFONT mf = tm.font(monoSpec);
            int fh = (int)tm.lineHeight(monoSpec);
            // split code into lines
            std::vector<std::wstring> lines; std::wstring cur;
            for (wchar_t c : b.codeText) { if (c == L'\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
            lines.push_back(cur);
            int lineH = fh + S(4);
            int boxTop = y;
            int boxH = (int)lines.size() * lineH + S(24);
            emitRect(cx.contentLeft, boxTop, cx.contentRight - cx.contentLeft, boxH, th.bg2);
            int ty = boxTop + S(12);
            for (auto& ln : lines) {
                int wpx = (int)tm.textWidth(monoSpec, ln);
                emitRun(cx, cx.contentLeft + S(16), ty, wpx, fh, ln, mf, th.codeText, false);
                ty += lineH;
            }
            y = boxTop + boxH + S(16);
            break;
        }
        case BlockType::Table: {
            int cols = (int)b.headers.size();
            if (cols == 0) { break; }
            int avail = cx.contentRight - cx.contentLeft;
            fmdv::FontSpec bodySpec = specFor(F_Body, false, false);
            fmdv::FontSpec boldSpec = specFor(F_Body, true, false);
            HFONT bf = tm.font(bodySpec);
            HFONT bfBold = tm.font(boldSpec);
            int fh = (int)tm.lineHeight(bodySpec);
            int cellPadX = S(8), cellPadY = S(7), lineGap = S(4), minColW = S(60);

            auto cellText = [](const std::vector<InlineRun>& runs) {
                std::wstring s; for (auto& r : runs) s += r.text; return s;
            };

            // 1. natural (unwrapped) content width per column, from header + all rows
            std::vector<int> naturalPad(cols);
            for (int c = 0; c < cols; c++) naturalPad[c] = (int)tm.textWidth(boldSpec, cellText(b.headers[c].runs)) + 2 * cellPadX;
            for (auto& row : b.rows)
                for (int c = 0; c < cols && c < (int)row.cells.size(); c++) {
                    int w = (int)tm.textWidth(bodySpec, cellText(row.cells[c].runs)) + 2 * cellPadX;
                    if (w > naturalPad[c]) naturalPad[c] = w;
                }

            // 2. column widths: stretch proportionally to fill available width when
            // content fits; otherwise shrink proportionally (floor at minColW) and
            // wrap cell text that no longer fits.
            long long totalNatural = 0; for (int w : naturalPad) totalNatural += w;
            if (totalNatural < 1) totalNatural = 1;
            std::vector<int> colW(cols);
            if (totalNatural <= avail) {
                int surplus = avail - (int)totalNatural, used = 0;
                for (int c = 0; c < cols; c++) {
                    int share = (c == cols - 1) ? (surplus - used)
                                                 : (int)(surplus * (long long)naturalPad[c] / totalNatural);
                    used += share;
                    colW[c] = naturalPad[c] + share;
                }
            } else {
                // Shrink proportionally, but a naive single pass can push several
                // columns below minColW and, once floored, their combined width can
                // exceed avail (the exact overflow this feature exists to prevent).
                // Iteratively floor columns that would go below minColW and
                // redistribute the remaining width among the rest (same idea as
                // CSS flexbox min-width negotiation) so the total never exceeds avail.
                std::vector<bool> floored(cols, false);
                int remainingAvail = avail;
                long long remainingNatural = totalNatural;
                for (int iter = 0; iter < cols; iter++) {
                    bool changed = false;
                    for (int c = 0; c < cols; c++) {
                        if (floored[c]) continue;
                        int w = (remainingNatural > 0)
                            ? (int)((long long)naturalPad[c] * remainingAvail / remainingNatural)
                            : minColW;
                        if (w < minColW) {
                            floored[c] = true;
                            colW[c] = minColW;
                            remainingAvail -= minColW;
                            remainingNatural -= naturalPad[c];
                            changed = true;
                        }
                    }
                    if (!changed) break;
                }
                for (int c = 0; c < cols; c++)
                    if (!floored[c])
                        colW[c] = (remainingNatural > 0)
                            ? (int)((long long)naturalPad[c] * remainingAvail / remainingNatural)
                            : minColW;
            }
            std::vector<int> colX(cols + 1); colX[0] = cx.contentLeft;
            for (int c = 0; c < cols; c++) colX[c+1] = colX[c] + colW[c];

            // 3. wrap a row's cells to their column width; row height follows the tallest cell
            auto wrapRow = [&](const std::vector<TableCell>& cells, const fmdv::FontSpec& f,
                               std::vector<std::vector<std::wstring>>& outLines) {
                int lineCount = 1;
                outLines.assign(cols, {});
                for (int c = 0; c < cols; c++) {
                    std::wstring s = (c < (int)cells.size()) ? cellText(cells[c].runs) : L"";
                    int maxW = colW[c] - 2 * cellPadX;
                    if (maxW < S(10)) maxW = S(10);
                    outLines[c] = WrapCellText(tm, f, s, maxW);
                    if ((int)outLines[c].size() > lineCount) lineCount = (int)outLines[c].size();
                }
                return lineCount;
            };
            auto drawRow = [&](std::vector<std::vector<std::wstring>>& linesPerCol, int lineCount,
                               int ry, bool bold, bool stripe) {
                int rh = lineCount * fh + (lineCount - 1) * lineGap + 2 * cellPadY;
                if (stripe) emitRect(cx.contentLeft, ry, colX[cols] - cx.contentLeft, rh, th.bg2);
                const fmdv::FontSpec& fs = bold ? boldSpec : bodySpec;
                HFONT f = bold ? bfBold : bf;
                for (int c = 0; c < cols; c++) {
                    int align = (c < (int)b.aligns.size()) ? b.aligns[c] : AlignLeft;
                    for (int li = 0; li < (int)linesPerCol[c].size(); li++) {
                        const std::wstring& s = linesPerCol[c][li];
                        int tw = (int)tm.textWidth(fs, s);
                        int tx = colX[c] + cellPadX;
                        if (align == AlignCenter) tx = colX[c] + (colW[c] - tw) / 2;
                        else if (align == AlignRight) tx = colX[c+1] - cellPadX - tw;
                        // route through emitRun so cells are selectable; spaceBefore
                        // separates columns on copy, wrapped continuation lines get
                        // their own row (CopySelection inserts \n on a rc.top change)
                        emitRun(cx, tx, ry + cellPadY + li * (fh + lineGap), tw, fh, s, f, th.text, c > 0 && li == 0);
                    }
                }
                return rh;
            };

            int tableTop = y;
            std::vector<std::vector<std::wstring>> hdrLines;
            int hdrLineCount = wrapRow(b.headers, boldSpec, hdrLines);
            y += drawRow(hdrLines, hdrLineCount, y, true, true);

            std::vector<int> rowYs{ tableTop, y };
            for (size_t ri = 0; ri < b.rows.size(); ri++) {
                std::vector<std::vector<std::wstring>> lines;
                int lc = wrapRow(b.rows[ri].cells, bodySpec, lines);
                y += drawRow(lines, lc, y, false, (ri % 2 == 1));
                rowYs.push_back(y);
            }
            int tableBottom = y;

            // grid lines
            for (int c = 0; c <= cols; c++) emitRect(colX[c], tableTop, 1, tableBottom - tableTop, th.border);
            for (int ry : rowYs) hline(cx, cx.contentLeft, colX[cols], ry, th.border);

            y = tableBottom + S(16);
            break;
        }
        case BlockType::HRule: {
            y += S(8);
            hline(cx, cx.contentLeft, cx.contentRight, y, th.border);
            y += S(16);
            break;
        }
        }
    }

    return y + S(PAD_TOP);
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
