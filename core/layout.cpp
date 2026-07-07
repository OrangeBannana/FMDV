#include "layout.h"

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

FontSpec roleFont(FontRole role, bool bold, bool italic) {
    return FontSpec{role, bold, italic, RoleSizePx(role)};
}

struct Word {
    Str text;
    FontSpec font;
    Color color;
    bool code = false;
    bool link = false;
    bool strike = false;
    bool space = false; // a space precedes this word on the line
    Str href;
    double w = 0, h = 0, asc = 0;
};

struct Ctx {
    double left, right;
    const LayoutTheme* th;
    TextMeasurer* tm;
    LayoutResult* out;
};

// Split inline runs into measured words (split on spaces, space flag carried).
std::vector<Word> buildWords(Ctx& cx, const std::vector<InlineRun>& runs, FontRole base) {
    std::vector<Word> words;
    bool pendingSpace = false;
    for (const auto& r : runs) {
        FontRole role = r.code ? FontRole::Mono : base;
        FontSpec f = roleFont(role, r.bold, r.italic);
        Color col = !r.href.empty() ? cx.th->link : (r.code ? cx.th->codeText : cx.th->text);
        double h = cx.tm->lineHeight(f), asc = cx.tm->ascent(f);
        Str cur;
        auto flush = [&]() {
            if (cur.empty()) return;
            Word w;
            w.text = cur; w.font = f; w.color = col; w.code = r.code;
            w.link = !r.href.empty(); w.strike = r.strike; w.href = r.href;
            w.space = pendingSpace; w.h = h; w.asc = asc;
            w.w = cx.tm->textWidth(f, cur);
            words.push_back(std::move(w));
            cur.clear(); pendingSpace = false;
        };
        for (Char c : r.text) {
            if (c == U16(' ')) { flush(); pendingSpace = true; }
            else cur += c;
        }
        flush();
    }
    return words;
}

// Lay out words from x0 with wrapping to cx.right; emit Text commands + link
// hits + inline-code backgrounds. Returns the y just past the last line.
double layoutInline(Ctx& cx, const std::vector<Word>& words, double x0, double y) {
    double spaceW = cx.tm->textWidth(roleFont(FontRole::Body, false, false), U16(" "));
    size_t i = 0, n = words.size();
    while (i < n) {
        // gather a line
        double x = x0, lineH = 0, lineAsc = 0;
        size_t start = i;
        while (i < n) {
            double adv = (i > start && words[i].space) ? spaceW : 0;
            if (i > start && x + adv + words[i].w > cx.right) break;
            x += adv + words[i].w;
            if (words[i].h > lineH) lineH = words[i].h;
            if (words[i].asc > lineAsc) lineAsc = words[i].asc;
            i++;
        }
        if (i == start) i++; // a single word wider than the line: place it anyway
        // emit the line
        double cx0 = x0, baseline = y + lineAsc;
        for (size_t k = start; k < i; k++) {
            const Word& w = words[k];
            if (k > start && w.space) cx0 += spaceW;
            if (w.code) {
                DrawCommand bg; bg.kind = DrawCommand::FillRect;
                bg.rect = {cx0 - 1, y, w.w + 2, lineH}; bg.color = cx.th->bg2;
                cx.out->cmds.push_back(bg);
            }
            DrawCommand t; t.kind = DrawCommand::Text;
            t.rect = {cx0, baseline, w.w, lineH};
            t.color = w.color; t.font = w.font; t.text = w.text;
            t.underline = w.link; t.strike = w.strike;
            cx.out->cmds.push_back(t);
            if (w.link && !w.href.empty())
                cx.out->links.push_back({{cx0, y, w.w, lineH}, w.href});
            cx0 += w.w;
        }
        y += lineH;
    }
    return y;
}

void line(Ctx& cx, double x1, double y1, double x2, double y2, Color c) {
    DrawCommand d; d.kind = DrawCommand::Line; d.rect = {x1, y1, x2, y2}; d.color = c;
    cx.out->cmds.push_back(d);
}
void fill(Ctx& cx, RectF r, Color c) {
    DrawCommand d; d.kind = DrawCommand::FillRect; d.rect = r; d.color = c;
    cx.out->cmds.push_back(d);
}
void frame(Ctx& cx, RectF r, Color c) {
    DrawCommand d; d.kind = DrawCommand::FrameRect; d.rect = r; d.color = c;
    cx.out->cmds.push_back(d);
}
void textAt(Ctx& cx, double x, double baseline, double h, const Str& s, FontSpec f, Color c) {
    DrawCommand d; d.kind = DrawCommand::Text; d.rect = {x, baseline, 0, h};
    d.text = s; d.font = f; d.color = c;
    cx.out->cmds.push_back(d);
}

} // namespace

LayoutResult LayoutDocument(const Document& doc, double width,
                            const LayoutTheme& th, TextMeasurer& tm) {
    LayoutResult res;
    Ctx cx{PAD_X, width - PAD_X, &th, &tm, &res};
    double y = PAD_TOP;
    int orderedCounter = 0; // sequential numbering for runs of ordered items

    for (size_t bi = 0; bi < doc.blocks.size(); bi++) {
        const Block& b = doc.blocks[bi];
        if (b.type != BlockType::ListItem || !b.ordered) orderedCounter = 0;

        switch (b.type) {
        case BlockType::Heading: {
            y += (b.level <= 2) ? 24 : 18;
            FontRole role = (FontRole)((int)FontRole::H1 + (b.level - 1));
            if ((int)role > (int)FontRole::H6) role = FontRole::H6;
            y = layoutInline(cx, buildWords(cx, b.runs, role), cx.left, y);
            if (b.level <= 2) { y += 6; line(cx, cx.left, y, cx.right, y, th.border); y += 2; }
            y += 6;
            break;
        }
        case BlockType::Paragraph: {
            y = layoutInline(cx, buildWords(cx, b.runs, FontRole::Body), cx.left, y);
            y += 16;
            break;
        }
        case BlockType::CodeBlock: {
            FontSpec mono = roleFont(FontRole::Mono, false, false);
            double lh = tm.lineHeight(mono), asc = tm.ascent(mono);
            std::vector<Str> lines; Str curl;
            for (Char c : b.codeText) { if (c == U16('\n')) { lines.push_back(curl); curl.clear(); } else curl += c; }
            lines.push_back(curl);
            double pad = 10, boxH = lines.size() * lh + 2 * pad;
            fill(cx, {cx.left, y, cx.right - cx.left, boxH}, th.bg2);
            double ty = y + pad;
            for (const auto& ln : lines) { textAt(cx, cx.left + 12, ty + asc, lh, ln, mono, th.codeText); ty += lh; }
            y += boxH + 14;
            break;
        }
        case BlockType::BlockQuote: {
            double yStart = y + 2;
            Ctx qcx = cx; qcx.left = cx.left + 16;
            auto words = buildWords(qcx, b.runs, FontRole::Body);
            for (auto& w : words) w.color = th.text2;
            double yEnd = layoutInline(qcx, words, qcx.left, yStart);
            fill(cx, {cx.left, yStart, 4, yEnd - yStart}, th.border);
            y = yEnd + 16;
            break;
        }
        case BlockType::ListItem: {
            double indent = b.level * 22.0;
            double markerX = cx.left + indent;
            double contentX = markerX + 22;
            FontSpec body = roleFont(FontRole::Body, false, false);
            double lh = tm.lineHeight(body), asc = tm.ascent(body);
            double baseline = y + asc;
            if (b.taskState >= 0) {
                double bx = markerX, by = y + (lh - 14) / 2;
                frame(cx, {bx, by, 14, 14}, th.text2);
                if (b.taskState == 1) {
                    line(cx, bx + 3, by + 7, bx + 6, by + 11, th.text2);
                    line(cx, bx + 6, by + 11, bx + 11, by + 4, th.text2);
                }
            } else if (b.ordered) {
                orderedCounter++;
                Str num = toStr(orderedCounter) + U16(".");
                textAt(cx, markerX, baseline, lh, num, body, th.text);
            } else {
                textAt(cx, markerX + 2, baseline, lh, U16("•"), body, th.text);
            }
            Ctx lcx = cx; lcx.left = contentX;
            y = layoutInline(lcx, buildWords(lcx, b.runs, FontRole::Body), contentX, y);
            y += 6;
            break;
        }
        case BlockType::Table: {
            size_t cols = b.headers.size();
            if (cols == 0) { y += 8; break; }
            FontSpec body = roleFont(FontRole::Body, false, false);
            FontSpec bold = roleFont(FontRole::Body, true, false);
            double lh = tm.lineHeight(body), asc = tm.ascent(body);
            double cellPad = 8, rowH = lh + 2 * cellPad;
            double tableW = cx.right - cx.left;
            double colW = tableW / (double)cols;

            auto cellText = [&](const std::vector<InlineRun>& runs) {
                Str s; for (const auto& r : runs) s += r.text; return s;
            };
            auto drawRow = [&](const std::vector<TableCell>& cells, double ry, FontSpec f, Color bgc) {
                if (bgc.a) fill(cx, {cx.left, ry, tableW, rowH}, bgc);
                for (size_t c = 0; c < cols; c++) {
                    double cxl = cx.left + c * colW;
                    frame(cx, {cxl, ry, colW, rowH}, th.border);
                    if (c >= cells.size()) continue;
                    Str s = cellText(cells[c].runs);
                    double tw = tm.textWidth(f, s);
                    int align = (c < b.aligns.size()) ? b.aligns[c] : AlignLeft;
                    double tx = cxl + cellPad;
                    if (align == AlignCenter) tx = cxl + (colW - tw) / 2;
                    else if (align == AlignRight) tx = cxl + colW - cellPad - tw;
                    textAt(cx, tx, ry + cellPad + asc, lh, s, f, th.text);
                }
            };
            drawRow(b.headers, y, bold, th.bg2);
            y += rowH;
            for (size_t r = 0; r < b.rows.size(); r++) {
                Color stripe = (r % 2) ? th.bg2 : Color{0,0,0,0};
                drawRow(b.rows[r].cells, y, body, stripe);
                y += rowH;
            }
            y += 16;
            break;
        }
        case BlockType::HRule: {
            y += 8; line(cx, cx.left, y, cx.right, y, th.border); y += 8 + 8;
            break;
        }
        }
    }
    res.contentHeight = y + PAD_TOP;
    return res;
}

} // namespace fmdv
