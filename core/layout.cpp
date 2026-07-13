#include "layout.h"
#include <cmath>

// This is a faithful port of the Win32 frontend's original GDI layout
// (cpp/render.cpp before the render->core/layout migration). The Windows
// renderer is the behavior reference, and its PNG output is the migration's
// regression gate, so the arithmetic here mirrors the original integer math
// exactly: layout constants are scaled with round-half-up (the old S() macro)
// and every division the old code did in ints is floored. When the measurer
// returns whole-pixel metrics (GDI does), every coordinate this produces is a
// whole number and the Win32 frontend's output is pixel-identical to the old
// engine. Frontends with fractional metrics (CoreText) get the same structure
// with fractional positions.

namespace fmdv {

LayoutTheme LightLayoutTheme() {
    return LayoutTheme{
        {0xff,0xff,0xff}, {0xf6,0xf8,0xfa}, {0xfa,0xfb,0xfc},
        {0x24,0x29,0x2f}, {0x57,0x60,0x6a}, {0xd0,0xd7,0xde},
        {0x09,0x69,0xda}, {0x24,0x29,0x2f}, {0xae,0xd4,0xfb},
    };
}
LayoutTheme DarkLayoutTheme() {
    return LayoutTheme{
        {0x0d,0x11,0x17}, {0x16,0x1b,0x22}, {0x16,0x1b,0x22},
        {0xe6,0xed,0xf3}, {0x8b,0x94,0x9e}, {0x30,0x36,0x3d},
        {0x58,0xa6,0xff}, {0xe6,0xed,0xf3}, {0x26,0x4f,0x78},
    };
}

namespace {

const double PAD_X = 40, PAD_TOP = 32;

inline bool sameColor(const Color& a, const Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

FontSpec roleFont(FontRole role, bool bold, bool italic) {
    bool heading = role >= FontRole::H1 && role <= FontRole::H6;
    return FontSpec{role, bold || heading, italic, RoleSizePx(role)};
}

struct Word {
    Str text;
    FontSpec font;   // emitted font (heading bold is forced on)
    // Grouping key: the run's *unforced* attributes. Two words merge into one
    // text run only when these match — the same condition the original Win32
    // code expressed as HFONT-handle equality, preserved so run grouping (and
    // with it selection/copy/find granularity) is unchanged.
    FontRole gRole = FontRole::Body;
    bool gBold = false, gItalic = false;
    Color color;
    bool code = false;
    bool link = false;
    bool strike = false;
    bool space = false; // a space precedes this word in the source
    Str href;
    double w = 0, h = 0, asc = 0;
};

struct Ctx {
    double left, right;
    const LayoutTheme* th;
    TextMeasurer* tm;
    LayoutResult* out;
    double scale;
};

// Scale a layout constant; round-half-up like the Win32 S() always did.
inline double Sc(const Ctx& cx, double v) { return std::floor(v * cx.scale + 0.5); }

// emit helpers (all coordinates in document space)
void fill(Ctx& cx, RectF r, Color c) {
    DrawCommand d; d.kind = DrawCommand::FillRect; d.rect = r; d.color = c;
    cx.out->cmds.push_back(d);
}
void frame(Ctx& cx, RectF r, Color c) {
    DrawCommand d; d.kind = DrawCommand::FrameRect; d.rect = r; d.color = c;
    cx.out->cmds.push_back(d);
}
void drawLine(Ctx& cx, double x1, double y1, double x2, double y2, Color c) {
    DrawCommand d; d.kind = DrawCommand::Line; d.rect = {x1, y1, x2, y2}; d.color = c;
    cx.out->cmds.push_back(d);
}
// A 1-px-high horizontal rule (the old code drew rules as fills, not lines).
void hline(Ctx& cx, double l, double r, double y, Color c) {
    fill(cx, {l, y, r - l, 1}, c);
}
void textCmd(Ctx& cx, double x, double baseline, double w, double h, const Str& s,
             const FontSpec& f, Color c, bool spaceBefore, bool selectable) {
    DrawCommand d; d.kind = DrawCommand::Text;
    d.rect = {x, baseline, w, h};
    d.text = s; d.font = f; d.color = c;
    d.spaceBefore = spaceBefore; d.selectable = selectable;
    cx.out->cmds.push_back(d);
}

// Convert a run sequence into measured words (split on spaces/tabs).
std::vector<Word> buildWords(Ctx& cx, const std::vector<InlineRun>& runs, FontRole base) {
    std::vector<Word> words;
    bool pendingSpace = false; // whitespace seen since last emitted word (carries across runs)
    for (const auto& r : runs) {
        FontRole useRole = r.code ? FontRole::Mono : base;
        FontSpec f = roleFont(useRole, r.bold, r.italic);
        Color col = cx.th->text;
        if (!r.href.empty()) col = cx.th->link;
        else if (r.code) col = cx.th->codeText;
        double h = cx.tm->lineHeight(f), asc = cx.tm->ascent(f);

        Str cur;
        auto flush = [&]() {
            if (cur.empty()) return;
            Word w;
            w.text = cur; w.font = f;
            w.gRole = useRole; w.gBold = r.bold; w.gItalic = r.italic;
            w.color = col; w.code = r.code; w.link = !r.href.empty();
            w.strike = r.strike; w.href = r.href;
            w.space = pendingSpace;
            w.w = cx.tm->textWidth(f, cur);
            w.h = h; w.asc = asc;
            words.push_back(std::move(w));
            cur.clear();
            pendingSpace = false;
        };
        for (Char c : r.text) {
            if (c == U16(' ') || c == U16('\t')) { flush(); pendingSpace = true; }
            else cur += c;
        }
        flush();
    }
    return words;
}

// Lay out a wrapped run of words starting at x=indentLeft. Emits cmds/links.
// Returns y after. Words on a line are bottom-aligned (top = y + lineH - h),
// exactly like the original Win32 layout.
double layoutWords(Ctx& cx, std::vector<Word>& words, double indentLeft, double y) {
    double spaceW = cx.tm->textWidth(roleFont(FontRole::Body, false, false), U16(" "));
    double x = indentLeft;
    double lineStart = indentLeft;
    double lineH = 0;
    std::vector<Word*> line;

    auto flushLine = [&]() {
        if (line.empty()) return;
        size_t n = line.size();
        std::vector<double> dxs(n);
        double dx = lineStart;
        for (size_t i = 0; i < n; i++) {
            if (i > 0 && line[i]->space) dx += spaceW;
            dxs[i] = dx;
            dx += line[i]->w;
        }
        auto topY = [&](const Word* w) { return y + (lineH - w->h); };

        // 1. inline-code backgrounds
        for (size_t i = 0; i < n; i++) if (line[i]->code) {
            double ty = topY(line[i]);
            fill(cx, {dxs[i] - 2, ty, line[i]->w + 4, line[i]->h}, cx.th->bg2);
        }
        // 2. text (group consecutive same-font+color words -> one run for natural spacing)
        for (size_t i = 0; i < n; ) {
            size_t j = i;
            Str s = line[i]->text;
            while (j + 1 < n && line[j+1]->gRole == line[i]->gRole
                             && line[j+1]->gBold == line[i]->gBold
                             && line[j+1]->gItalic == line[i]->gItalic
                             && sameColor(line[j+1]->color, line[i]->color)) {
                j++;
                if (line[j]->space) s += U16(' ');
                s += line[j]->text;
            }
            double fx = dxs[i], ty = topY(line[i]);
            double wpx = dxs[j] + line[j]->w - fx;
            textCmd(cx, fx, ty + line[i]->asc, wpx, line[i]->h, s, line[i]->font,
                    line[i]->color, (i > 0 && line[i]->space), true);
            i = j + 1;
        }
        // 3. strikethrough
        for (size_t i = 0; i < n; i++) if (line[i]->strike) {
            double my = topY(line[i]) + std::floor(line[i]->h / 2);
            drawLine(cx, dxs[i], my, dxs[i] + line[i]->w, my, line[i]->color);
        }
        // 4. link underline + hit-rect (span consecutive same-href words)
        for (size_t i = 0; i < n; ) {
            if (line[i]->href.empty()) { i++; continue; }
            size_t j = i;
            while (j + 1 < n && line[j+1]->href == line[i]->href) j++;
            double x0 = dxs[i], x1 = dxs[j] + line[j]->w;
            double ty = topY(line[i]);
            drawLine(cx, x0, ty + line[i]->h - 2, x1, ty + line[i]->h - 2, line[i]->color);
            cx.out->links.push_back({{x0, ty, x1 - x0, line[i]->h}, line[i]->href});
            i = j + 1;
        }
        y += lineH;
        line.clear(); lineH = 0; x = lineStart;
    };

    for (auto& w : words) {
        double sp = (!line.empty() && w.space) ? spaceW : 0;
        double need = sp + w.w;
        if (!line.empty() && x + need > cx.right) {
            flushLine();
        } else {
            x += sp;
        }
        line.push_back(&w);
        x += w.w;
        if (w.h > lineH) lineH = w.h;
    }
    flushLine();
    return y;
}

// Greedy word-wrap of plain text to fit maxWidth. No mid-word breaking (matches
// layoutWords' paragraph wrapping, which also never splits a word).
std::vector<Str> WrapCellText(TextMeasurer& tm, const FontSpec& f, const Str& text,
                              double maxWidth) {
    std::vector<Str> lines;
    std::vector<Str> words; Str cur;
    for (Char c : text) {
        if (c == U16(' ') || c == U16('\t')) { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty()) { lines.push_back(Str()); return lines; }

    double spaceW = tm.textWidth(f, U16(" "));
    Str line = words[0];
    double lineW = tm.textWidth(f, line);
    for (size_t i = 1; i < words.size(); i++) {
        double wW = tm.textWidth(f, words[i]);
        if (lineW + spaceW + wW <= maxWidth) {
            line += U16(' ');
            line += words[i];
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

LayoutResult LayoutDocument(const Document& doc, double width,
                            const LayoutTheme& th, TextMeasurer& tm,
                            double scale) {
    LayoutResult res;
    Ctx cx{0, 0, &th, &tm, &res, scale};
    cx.left = Sc(cx, PAD_X);
    cx.right = width - Sc(cx, PAD_X);

    double y = Sc(cx, PAD_TOP);
    int olCounter = 0; // running number for ordered lists

    for (size_t bi = 0; bi < doc.blocks.size(); bi++) {
        const Block& b = doc.blocks[bi];
        res.blockTops.push_back(y);
        if (b.type != BlockType::ListItem || !b.ordered) olCounter = 0;

        switch (b.type) {
        case BlockType::Heading: {
            int lvl = (int)FontRole::H1 + (b.level - 1);
            if (lvl > (int)FontRole::H6) lvl = (int)FontRole::H6;
            FontRole role = (FontRole)lvl;
            y += (b.level <= 2) ? Sc(cx, 24) : Sc(cx, 18); // top margin
            Str htext; for (const auto& rn : b.runs) htext += rn.text;
            res.headings.push_back({b.level, htext, y});
            auto words = buildWords(cx, b.runs, role);
            y = layoutWords(cx, words, cx.left, y);
            if (b.level <= 2) { // underline rule
                y += Sc(cx, 6);
                hline(cx, cx.left, cx.right, y, th.border);
                y += Sc(cx, 10);
            } else {
                y += Sc(cx, 8);
            }
            break;
        }
        case BlockType::Paragraph: {
            auto words = buildWords(cx, b.runs, FontRole::Body);
            y = layoutWords(cx, words, cx.left, y);
            y += Sc(cx, 16);
            break;
        }
        case BlockType::BlockQuote: {
            auto words = buildWords(cx, b.runs, FontRole::Body);
            for (auto& w : words) if (!w.link) w.color = th.text2;
            double top = y;
            double yy = layoutWords(cx, words, cx.left + Sc(cx, 16), y);
            // left border bar
            fill(cx, {cx.left, top - 2, Sc(cx, 4), (yy + 2) - (top - 2)}, th.border);
            y = yy + Sc(cx, 16);
            break;
        }
        case BlockType::ListItem: {
            double bulletX = cx.left + Sc(cx, 8) + b.level * Sc(cx, 24);
            double indent = cx.left + Sc(cx, 24) + b.level * Sc(cx, 24);
            if (b.taskState >= 0) indent = bulletX + Sc(cx, 24); // extra gap after checkbox
            // bullet / number / checkbox
            auto words = buildWords(cx, b.runs, FontRole::Body);
            FontSpec body = roleFont(FontRole::Body, false, false);
            double lineH = words.empty() ? tm.lineHeight(body) : 0;
            for (auto& w : words) if (w.h > lineH) lineH = w.h;
            double bodyAsc = tm.ascent(body);
            if (b.taskState >= 0) {
                RectF boxR{bulletX, y + Sc(cx, 3), Sc(cx, 14), Sc(cx, 14)};
                frame(cx, boxR, th.text2);
                if (b.taskState == 1) { // checkmark
                    drawLine(cx, bulletX + Sc(cx, 3), y + Sc(cx, 10), bulletX + Sc(cx, 6), y + Sc(cx, 13), th.text2);
                    drawLine(cx, bulletX + Sc(cx, 6), y + Sc(cx, 13), bulletX + Sc(cx, 11), y + Sc(cx, 6), th.text2);
                }
                // Clickable hit area, padded a little beyond the 14px box for easier
                // targeting; the frontend toggles the marker on b's source line.
                double pad = Sc(cx, 4);
                cx.out->taskHits.push_back(
                    {{boxR.x - pad, boxR.y - pad, boxR.w + 2 * pad, boxR.h + 2 * pad},
                     b.srcStartLine, b.taskState});
            } else if (b.ordered) {
                olCounter++;
                Str num = toStr(olCounter) + U16(".");
                textCmd(cx, bulletX, y + bodyAsc, tm.textWidth(body, num), lineH,
                        num, body, th.text, false, false);
            } else {
                Str bullet(1, (Char)0x2022);
                textCmd(cx, bulletX, y + bodyAsc, tm.textWidth(body, bullet), lineH,
                        bullet, body, th.text, false, false);
            }
            // An empty item (marker with no text) still reserves its marker's
            // line height; layoutWords returns y unchanged for zero words, which
            // would let the checkbox/bullet overlap the next block.
            if (words.empty()) y += lineH;
            else y = layoutWords(cx, words, indent, y);
            y += Sc(cx, 6);
            break;
        }
        case BlockType::CodeBlock: {
            FontSpec mono = roleFont(FontRole::Mono, false, false);
            double fh = tm.lineHeight(mono), asc = tm.ascent(mono);
            // split code into lines
            std::vector<Str> lines; Str cur;
            for (Char c : b.codeText) { if (c == U16('\n')) { lines.push_back(cur); cur.clear(); } else cur += c; }
            lines.push_back(cur);
            double lineH = fh + Sc(cx, 4);
            double boxTop = y;
            double boxH = (double)lines.size() * lineH + Sc(cx, 24);
            fill(cx, {cx.left, boxTop, cx.right - cx.left, boxH}, th.bg2);
            double ty = boxTop + Sc(cx, 12);
            for (const auto& ln : lines) {
                double wpx = tm.textWidth(mono, ln);
                textCmd(cx, cx.left + Sc(cx, 16), ty + asc, wpx, fh, ln, mono,
                        th.codeText, false, true);
                ty += lineH;
            }
            y = boxTop + boxH + Sc(cx, 16);
            break;
        }
        case BlockType::Table: {
            int cols = (int)b.headers.size();
            if (cols == 0) { break; }
            double avail = cx.right - cx.left;
            FontSpec bodySpec = roleFont(FontRole::Body, false, false);
            FontSpec boldSpec = roleFont(FontRole::Body, true, false);
            double fh = tm.lineHeight(bodySpec);
            double asc = tm.ascent(bodySpec), ascBold = tm.ascent(boldSpec);
            double cellPadX = Sc(cx, 8), cellPadY = Sc(cx, 7);
            double lineGap = Sc(cx, 4), minColW = Sc(cx, 60);

            auto cellText = [](const std::vector<TableCell>& cells, int c) {
                Str s;
                if (c < (int)cells.size()) for (const auto& r : cells[c].runs) s += r.text;
                return s;
            };

            // 1. natural (unwrapped) content width per column, from header + all rows
            std::vector<double> naturalPad(cols);
            for (int c = 0; c < cols; c++)
                naturalPad[c] = tm.textWidth(boldSpec, cellText(b.headers, c)) + 2 * cellPadX;
            for (const auto& row : b.rows)
                for (int c = 0; c < cols && c < (int)row.cells.size(); c++) {
                    double w = tm.textWidth(bodySpec, cellText(row.cells, c)) + 2 * cellPadX;
                    if (w > naturalPad[c]) naturalPad[c] = w;
                }

            // 2. column widths: stretch proportionally to fill available width when
            // content fits; otherwise shrink proportionally (floor at minColW) and
            // wrap cell text that no longer fits.
            double totalNatural = 0; for (double w : naturalPad) totalNatural += w;
            if (totalNatural < 1) totalNatural = 1;
            std::vector<double> colW(cols);
            if (totalNatural <= avail) {
                double surplus = avail - totalNatural, used = 0;
                for (int c = 0; c < cols; c++) {
                    double share = (c == cols - 1) ? (surplus - used)
                                                   : std::floor(surplus * naturalPad[c] / totalNatural);
                    used += share;
                    colW[c] = naturalPad[c] + share;
                }
            } else {
                // Iteratively floor columns that would go below minColW and
                // redistribute the remaining width among the rest so the total
                // never exceeds avail (CSS-flexbox-style min-width negotiation).
                std::vector<bool> floored(cols, false);
                double remainingAvail = avail;
                double remainingNatural = totalNatural;
                for (int iter = 0; iter < cols; iter++) {
                    bool changed = false;
                    for (int c = 0; c < cols; c++) {
                        if (floored[c]) continue;
                        double w = (remainingNatural > 0)
                            ? std::floor(naturalPad[c] * remainingAvail / remainingNatural)
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
                            ? std::floor(naturalPad[c] * remainingAvail / remainingNatural)
                            : minColW;
            }
            std::vector<double> colX(cols + 1); colX[0] = cx.left;
            for (int c = 0; c < cols; c++) colX[c+1] = colX[c] + colW[c];

            // 3. wrap a row's cells to their column width; row height follows the tallest cell
            auto wrapRow = [&](const std::vector<TableCell>& cells, const FontSpec& f,
                               std::vector<std::vector<Str>>& outLines) {
                int lineCount = 1;
                outLines.assign(cols, {});
                for (int c = 0; c < cols; c++) {
                    Str s = cellText(cells, c);
                    double maxW = colW[c] - 2 * cellPadX;
                    if (maxW < Sc(cx, 10)) maxW = Sc(cx, 10);
                    outLines[c] = WrapCellText(tm, f, s, maxW);
                    if ((int)outLines[c].size() > lineCount) lineCount = (int)outLines[c].size();
                }
                return lineCount;
            };
            auto drawRow = [&](std::vector<std::vector<Str>>& linesPerCol, int lineCount,
                               double ry, bool bold, bool stripe) {
                double rh = lineCount * fh + (lineCount - 1) * lineGap + 2 * cellPadY;
                if (stripe) fill(cx, {cx.left, ry, colX[cols] - cx.left, rh}, th.bg2);
                const FontSpec& f = bold ? boldSpec : bodySpec;
                double fasc = bold ? ascBold : asc;
                for (int c = 0; c < cols; c++) {
                    int align = (c < (int)b.aligns.size()) ? b.aligns[c] : AlignLeft;
                    for (int li = 0; li < (int)linesPerCol[c].size(); li++) {
                        const Str& s = linesPerCol[c][li];
                        double tw = tm.textWidth(f, s);
                        double tx = colX[c] + cellPadX;
                        if (align == AlignCenter) tx = colX[c] + std::floor((colW[c] - tw) / 2);
                        else if (align == AlignRight) tx = colX[c+1] - cellPadX - tw;
                        // cells are selectable; spaceBefore separates columns on
                        // copy, wrapped continuation lines get their own row
                        textCmd(cx, tx, ry + cellPadY + li * (fh + lineGap) + fasc, tw, fh,
                                s, f, th.text, c > 0 && li == 0, true);
                    }
                }
                return rh;
            };

            double tableTop = y;
            std::vector<std::vector<Str>> hdrLines;
            int hdrLineCount = wrapRow(b.headers, boldSpec, hdrLines);
            y += drawRow(hdrLines, hdrLineCount, y, true, true);

            std::vector<double> rowYs{ tableTop, y };
            for (size_t ri = 0; ri < b.rows.size(); ri++) {
                std::vector<std::vector<Str>> lines;
                int lc = wrapRow(b.rows[ri].cells, bodySpec, lines);
                y += drawRow(lines, lc, y, false, (ri % 2 == 1));
                rowYs.push_back(y);
            }
            double tableBottom = y;

            // grid lines
            for (int c = 0; c <= cols; c++)
                fill(cx, {colX[c], tableTop, 1, tableBottom - tableTop}, th.border);
            for (double ry : rowYs) hline(cx, cx.left, colX[cols], ry, th.border);

            y = tableBottom + Sc(cx, 16);
            break;
        }
        case BlockType::HRule: {
            y += Sc(cx, 8);
            hline(cx, cx.left, cx.right, y, th.border);
            y += Sc(cx, 16);
            break;
        }
        }
    }

    res.contentHeight = y + Sc(cx, PAD_TOP);
    return res;
}

} // namespace fmdv
