// Unit tests for the shared layout engine (core/layout), which turns a parsed
// Document into draw commands for both frontends. A fixed-metrics measurer
// (every char 10px wide, 20px line height, 15px ascent, any font) makes every
// coordinate exactly computable: at scale 1 the content box starts at
// x=40 (PAD_X) / y=32 (PAD_TOP).
#include "layout.h"
#include "markdown.h"
#include "test_check.h"
#include <cmath>
#include <string>

using namespace fmdv;

struct FixedMeasurer : TextMeasurer {
    double textWidth(const FontSpec&, StrView s) override { return (double)s.size() * 10; }
    double lineHeight(const FontSpec&) override { return 20; }
    double ascent(const FontSpec&) override { return 15; }
};

static LayoutResult lay(const char* md, double width = 900, double scale = 1.0,
                        bool dark = false) {
    FixedMeasurer tm;
    Document doc = ParseMarkdown(FromUtf8(md));
    return LayoutDocument(doc, width, dark ? DarkLayoutTheme() : LightLayoutTheme(), tm, scale);
}

static int countKind(const LayoutResult& r, DrawCommand::Kind k) {
    int n = 0;
    for (const auto& c : r.cmds) if (c.kind == k) n++;
    return n;
}
static const DrawCommand* firstText(const LayoutResult& r, const char* text) {
    for (const auto& c : r.cmds)
        if (c.kind == DrawCommand::Text && ToUtf8(c.text) == text) return &c;
    return nullptr;
}
static bool near(double a, double b) { return std::fabs(a - b) < 0.001; }
static bool sameColor(const Color& a, const Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

int main() {
    const LayoutTheme light = LightLayoutTheme();
    const LayoutTheme dark = DarkLayoutTheme();

    // ---- themes ----
    check(sameColor(light.bg, {0xff, 0xff, 0xff}), "theme: light bg is white");
    check(sameColor(dark.bg, {0x0d, 0x11, 0x17}), "theme: dark bg");
    check(!sameColor(light.link, dark.link), "theme: link colors differ per theme");

    // ---- role sizes (matches the Windows frontend's font ramp) ----
    check(RoleSizePx(FontRole::H1) == 30 && RoleSizePx(FontRole::H2) == 24
              && RoleSizePx(FontRole::H3) == 20 && RoleSizePx(FontRole::H4) == 16
              && RoleSizePx(FontRole::H5) == 14 && RoleSizePx(FontRole::H6) == 13
              && RoleSizePx(FontRole::Mono) == 14 && RoleSizePx(FontRole::Body) == 16,
          "roles: H1 30 ... H6 13, Mono 14, Body 16");

    // ---- empty document ----
    {
        LayoutResult r = lay("");
        check(r.cmds.empty() && r.blockTops.empty(), "empty: no commands, no block tops");
        check(near(r.contentHeight, 64), "empty: content height is 2x top padding");
    }

    // ---- paragraph ----
    {
        LayoutResult r = lay("hello world");
        check(countKind(r, DrawCommand::Text) == 1, "para: adjacent plain words merge into one run");
        const DrawCommand* t = firstText(r, "hello world");
        check(t != nullptr, "para: merged run text includes the space");
        if (t) {
            check(near(t->rect.x, 40), "para: text starts at left padding");
            check(near(t->rect.y, 47), "para: baseline = top pad + ascent");
            check(near(t->rect.w, 110), "para: run advance covers both words + space");
            check(t->font.role == FontRole::Body && !t->font.bold, "para: body font");
            check(t->selectable && !t->spaceBefore, "para: selectable, no leading space");
        }
        check(r.blockTops.size() == 1 && near(r.blockTops[0], 32), "para: block top recorded");
        check(near(r.contentHeight, 100), "para: content height (pad+line+margin+pad)");
    }

    // ---- word wrap ----
    {
        // usable width 40..90 fits one 4-char word (40px) per line
        LayoutResult r = lay("aaaa bbbb", 130);
        check(countKind(r, DrawCommand::Text) == 2, "wrap: narrow width wraps to two lines");
        const DrawCommand* a = firstText(r, "aaaa");
        const DrawCommand* b = firstText(r, "bbbb");
        check(a && b && near(a->rect.x, 40) && near(b->rect.x, 40),
              "wrap: both lines start at the left edge");
        check(a && b && near(b->rect.y - a->rect.y, 20), "wrap: second line one line-height down");
    }

    // ---- style runs break grouping ----
    {
        LayoutResult r = lay("a **b** c");
        check(countKind(r, DrawCommand::Text) == 3, "runs: bold word splits into three commands");
        const DrawCommand* b = firstText(r, "b");
        check(b && b->font.bold, "runs: middle command is bold");
        check(b && b->spaceBefore, "runs: bold word remembers the space before it");
    }
    // A code span with no source space on either side (e.g. "(`x`)") must not
    // have its background box crowd the adjacent punctuation -- the padding
    // is reserved as real layout space, not just painted overlap.
    {
        LayoutResult r = lay("(`x`)");
        const DrawCommand* open = firstText(r, "(");
        const DrawCommand* code = firstText(r, "x");
        const DrawCommand* close = firstText(r, ")");
        check(open && code && close, "code-adjacent: all three runs present");
        const DrawCommand* bg = nullptr;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::FillRect && sameColor(c.color, light.bg2)) bg = &c;
        check(bg != nullptr, "code-adjacent: background box drawn");
        check(bg && open && bg->rect.x >= open->rect.x + open->rect.w,
              "code-adjacent: background box doesn't overlap the preceding '('");
        check(bg && close && close->rect.x >= bg->rect.x + bg->rect.w,
              "code-adjacent: following ')' doesn't overlap the background box");
        // The leading inset (box edge to code text) is deliberately larger
        // than the trailing one: a leading neighbor with no space reads as
        // crowded at the same gap that looks fine trailing, since glyphs
        // like "(" carry very little of their own right-side bearing.
        check(bg && code &&
                  (code->rect.x - bg->rect.x) > ((bg->rect.x + bg->rect.w) - (code->rect.x + code->rect.w)),
              "code-adjacent: leading inset is larger than the trailing inset");
    }

    // ---- headings ----
    {
        LayoutResult r = lay("# Title");
        check(r.headings.size() == 1 && r.headings[0].level == 1
                  && ToUtf8(r.headings[0].text) == "Title",
              "h1: heading recorded for TOC");
        check(r.headings.size() == 1 && near(r.headings[0].y, 56),
              "h1: TOC y is below the top margin");
        const DrawCommand* t = firstText(r, "Title");
        check(t && t->font.role == FontRole::H1 && t->font.bold, "h1: heading font, bold forced");
        check(countKind(r, DrawCommand::FillRect) == 1, "h1: underline rule emitted");
        check(near(r.contentHeight, 124), "h1: content height");
    }
    // Inline code inside a heading must scale up with the heading instead of
    // staying at Mono's fixed 14px, which looked tiny/subscript-like next to
    // much larger heading text.
    {
        LayoutResult r = lay("# a `code` b");
        const DrawCommand* c = firstText(r, "code");
        check(c && c->font.role == FontRole::Mono, "h1 code: still uses the Mono font family");
        check(c && c->font.px > RoleSizePx(FontRole::Mono),
              "h1 code: scaled above Mono's fixed default size");
        check(c && near(c->font.px, RoleSizePx(FontRole::H1) *
                             (RoleSizePx(FontRole::Mono) / RoleSizePx(FontRole::Body))),
              "h1 code: scaled proportionally to the heading's own size");
    }
    {
        // outside a heading, inline code keeps its normal fixed size
        LayoutResult r = lay("a `code` b");
        const DrawCommand* c = firstText(r, "code");
        check(c && near(c->font.px, RoleSizePx(FontRole::Mono)),
              "body code: unscaled outside a heading");
    }
    {
        LayoutResult r = lay("### Sub");
        check(countKind(r, DrawCommand::FillRect) == 0, "h3: no underline rule");
        check(r.headings.size() == 1 && r.headings[0].level == 3, "h3: TOC level 3");
    }

    // ---- lists ----
    {
        LayoutResult r = lay("- a");
        const DrawCommand* bullet = firstText(r, "\xE2\x80\xA2"); // U+2022
        check(bullet != nullptr, "ul: bullet glyph emitted");
        if (bullet) {
            check(!bullet->selectable, "ul: bullet is not selectable");
            check(near(bullet->rect.x, 48), "ul: bullet x = left + 8");
        }
        const DrawCommand* item = firstText(r, "a");
        check(item && near(item->rect.x, 64), "ul: item text indented to left + 24");
    }
    {
        LayoutResult r = lay("  - nested");
        const DrawCommand* bullet = firstText(r, "\xE2\x80\xA2");
        check(bullet && near(bullet->rect.x, 72), "ul: nested bullet shifts 24px per level");
    }
    {
        // Ordered lists honor the author's start number (GFM <ol start="N">):
        // a list renders from its first item's source number and continues
        // sequentially; an interrupted list (paragraph in between) starts a NEW
        // list at its own source number (previously everything reset to 1).
        LayoutResult r = lay("5. a\n6. b\n\npara\n\n9. c\n10. d");
        check(firstText(r, "5.") != nullptr && firstText(r, "6.") != nullptr,
              "ol: source start number honoured, then continues");
        check(firstText(r, "9.") != nullptr && firstText(r, "10.") != nullptr,
              "ol: interrupted list restarts at its own source number");
        check(firstText(r, "1.") == nullptr && firstText(r, "2.") == nullptr,
              "ol: no reset to 1 anywhere");
        const DrawCommand* marker = firstText(r, "9.");
        check(marker && !marker->selectable, "ol: number marker not selectable");
    }
    {
        // A single non-1-start item keeps its own number.
        LayoutResult r = lay("5. only");
        check(firstText(r, "5.") != nullptr && firstText(r, "1.") == nullptr,
              "ol: single item numbered 5 in source renders as 5.");
    }
    {
        // Continuation runs sequentially from the start number; a skip in the
        // source (8. instead of 7.) is ignored, per GFM.
        LayoutResult r = lay("5. a\n6. b\n8. c");
        check(firstText(r, "5.") != nullptr && firstText(r, "6.") != nullptr
                  && firstText(r, "7.") != nullptr && firstText(r, "8.") == nullptr,
              "ol: sequential from start number, source gaps ignored");
    }
    {
        // Nested ordered lists number independently; the outer list continues
        // after the nested one ends ("1.a / 1.x / 2.y / 2.b" -> 1, 1, 2, 2).
        LayoutResult r = lay("1. a\n   1. x\n   2. y\n2. b");
        auto countText = [&](const char* t) {
            int n = 0;
            for (const auto& c : r.cmds)
                if (c.kind == DrawCommand::Text && ToUtf8(c.text) == t) n++;
            return n;
        };
        check(countText("1.") == 2 && countText("2.") == 2 && countText("3.") == 0,
              "ol: nested ordered list independent, outer list resumes");
    }
    {
        // A bullet nested inside an ordered list must not reset the outer counter.
        LayoutResult r = lay("1. a\n  - sub\n2. b");
        check(firstText(r, "1.") != nullptr && firstText(r, "2.") != nullptr
                  && firstText(r, "3.") == nullptr,
              "ol: outer list survives a nested bullet item");
    }
    {
        LayoutResult r = lay("- [ ] todo");
        check(countKind(r, DrawCommand::FrameRect) == 1, "task: checkbox frame");
        check(countKind(r, DrawCommand::Line) == 0, "task: unchecked has no checkmark");
    }
    {
        LayoutResult r = lay("- [x] done");
        check(countKind(r, DrawCommand::FrameRect) == 1, "task: checked frame");
        check(countKind(r, DrawCommand::Line) == 2, "task: checkmark is two lines");
    }
    {
        // Empty items (marker, no text) must still reserve a line so their
        // checkbox/bullet doesn't overlap following content (regression).
        LayoutResult r = lay("- [x]\n- [x]\n\nAfter");
        check(r.taskHits.size() == 2, "empty-task: two checkboxes emitted");
        if (r.taskHits.size() == 2)
            check(r.taskHits[1].rect.y >= r.taskHits[0].rect.y + r.taskHits[0].rect.h,
                  "empty-task: second checkbox is below the first (no overlap)");
        const DrawCommand* after = firstText(r, "After");
        check(after && r.taskHits.size() == 2 &&
                  after->rect.y > r.taskHits[1].rect.y + r.taskHits[1].rect.h,
              "empty-task: following paragraph sits below the checkboxes");
    }

    // ---- code block ----
    {
        LayoutResult r = lay("```\nab\ncd\n```");
        check(countKind(r, DrawCommand::FillRect) == 1, "code: one background box");
        const DrawCommand& box = r.cmds[0];
        check(box.kind == DrawCommand::FillRect && sameColor(box.color, light.bg2),
              "code: box uses bg2 and is drawn first");
        check(near(box.rect.x, 40) && near(box.rect.y, 32) && near(box.rect.w, 820)
                  && near(box.rect.h, 28 + 2 * 24 + 12), // header strip + 2 lines + bottom pad
              "code: box geometry (2 lines)");
        const DrawCommand* l1 = firstText(r, "ab");
        const DrawCommand* l2 = firstText(r, "cd");
        check(l1 && l2 && l1->font.role == FontRole::Mono, "code: mono font");
        check(l1 && near(l1->rect.x, 56), "code: text inset 16px into the box");
        check(l1 && l2 && near(l2->rect.y - l1->rect.y, 24), "code: line advance = height + 4");
        check(l1 && l1->selectable, "code: lines are selectable");
    }
    // ---- code block: copy-to-clipboard button ----
    {
        LayoutResult r = lay("```\nab\ncd\n```");
        check(r.codeCopyHits.size() == 1, "code copy: one hit per code block");
        const auto& h = r.codeCopyHits[0];
        check(ToUtf8(h.text) == "ab\ncd", "code copy: hit carries the raw code text verbatim");
        const DrawCommand& box = r.cmds[0];
        check(h.rect.x > box.rect.x && h.rect.x + h.rect.w < box.rect.x + box.rect.w,
              "code copy: button sits inside the box horizontally");
        check(h.rect.x + h.rect.w > box.rect.x + box.rect.w * 0.5,
              "code copy: button is in the right half of the box");
        check(h.rect.y >= box.rect.y && h.rect.y < box.rect.y + 28,
              "code copy: button sits in the header strip, not over the code text");
        check(h.rect.y + h.rect.h <= box.rect.y + 28,
              "code copy: button stays within the header strip, never over the code text");
        check(countKind(r, DrawCommand::FrameRect) == 2, "code copy: icon is two outlined squares");
    }
    {
        // two code blocks each get their own button, carrying their own text
        LayoutResult r = lay("```\nfirst\n```\n\n```\nsecond\n```");
        check(r.codeCopyHits.size() == 2, "code copy: one hit per block, not shared");
        check(ToUtf8(r.codeCopyHits[0].text) == "first" && ToUtf8(r.codeCopyHits[1].text) == "second",
              "code copy: each hit carries its own block's text");
    }
    // A code line with no spaces, wider than the box, must wrap character by
    // character instead of spilling past the box's right edge uncut.
    {
        std::string longLine(100, 'a');
        std::string md = "```\n" + longLine + "\n```";
        LayoutResult r = lay(md.c_str());
        int textCount = 0;
        std::string joined;
        double maxTextW = 0;
        for (const auto& c : r.cmds) {
            if (c.kind != DrawCommand::Text) continue;
            textCount++;
            joined += ToUtf8(c.text);
            if (c.rect.w > maxTextW) maxTextW = c.rect.w;
        }
        check(textCount > 1, "code: a too-long line wraps onto more than one display line");
        check(maxTextW <= 820 - 32 + 0.001, "code: no wrapped piece overflows the box width");
        check(joined == longLine, "code: wrapping loses or duplicates no characters");
    }
    // Leading indentation on a line that already fits must survive untouched
    // -- the code wrapper must not rebuild/collapse whitespace the way the
    // table-cell word-wrapper does for prose.
    {
        LayoutResult r = lay("```\n    indented\n```");
        check(firstText(r, "    indented") != nullptr,
              "code: leading indentation preserved on a short line");
    }

    // ---- blockquote ----
    {
        LayoutResult r = lay("> hi there");
        const DrawCommand* t = firstText(r, "hi there");
        check(t && sameColor(t->color, light.text2), "quote: muted text color");
        check(t && near(t->rect.x, 56), "quote: text indented 16px");
        bool bar = false;
        double barY = -1;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::FillRect && near(c.rect.w, 4)
                && sameColor(c.color, light.border)) { bar = true; barY = c.rect.y; }
        check(bar, "quote: 4px border bar");
        // Own 16px top margin: 32 pad + 16 margin + 15 ascent (regression pin
        // for the crowding fix — previously the first block's pad was its only gap).
        check(t && near(t->rect.y, 63), "quote: own top margin as first block (baseline 32+16+15)");
        check(bar && near(barY, 46), "quote: bar overhang stays in the margin band");
    }
    {
        // The reported bug: a quote after list items previously sat only 6px
        // below the previous line. It must now keep >=16px, and its bar must
        // not touch the previous text.
        LayoutResult r = lay("- a\n- b\n\n> hi there");
        const DrawCommand* t = firstText(r, "hi there");
        const DrawCommand* lastItem = firstText(r, "b");
        double gapTop = t->rect.y - 15.0 - (lastItem->rect.y + lastItem->rect.h - 15.0);
        check(t && lastItem && gapTop >= 16.0 - 1e-9,
              "quote: >=16px clearance after list items (was 6px)");
        bool barClear = true;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::FillRect && near(c.rect.w, 4)
                && sameColor(c.color, light.border))
                if (c.rect.y <= lastItem->rect.y + lastItem->rect.h - 15.0) barClear = false;
        check(barClear, "quote: border bar clear of adjacent list text");
    }
    {
        // Paragraph bottom margin (16) + quote's own top margin (16) = 32,
        // matching GitHub's 1em/1em blockquote rhythm.
        LayoutResult r = lay("hello\n\n> quoted");
        const DrawCommand* t = firstText(r, "quoted");
        const DrawCommand* p = firstText(r, "hello");
        double gapTop = t->rect.y - 15.0 - (p->rect.y + p->rect.h - 15.0);
        check(t && p && near(gapTop, 32.0), "quote: 32px below a paragraph (16+16)");
    }
    // A quote with a bare ">" paragraph break parses into multiple BlockQuote
    // blocks (core/markdown.cpp); layout must still draw one continuous
    // border bar spanning all of them, not one short bar per paragraph.
    {
        LayoutResult r = lay("> first\n>\n> second");
        int bars = 0; double barH = 0;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::FillRect && near(c.rect.w, 4)
                && sameColor(c.color, light.border)) { bars++; barH = c.rect.h; }
        check(bars == 1, "quote: multi-paragraph quote has one border bar, not one per paragraph");
        check(barH > 20 + 16, "quote: border bar spans both paragraphs plus the gap");
        check(r.blockTops.size() == 2, "quote: blockTops has one entry per paragraph block");
        check(r.blockTops[1] > r.blockTops[0], "quote: second paragraph's top is below the first's");
    }

    // ---- links ----
    {
        LayoutResult r = lay("[x](https://e)");
        check(r.links.size() == 1 && ToUtf8(r.links[0].href) == "https://e",
              "link: hit rect recorded with href");
        check(r.links.size() == 1 && near(r.links[0].rect.x, 40)
                  && near(r.links[0].rect.w, 10) && near(r.links[0].rect.h, 20),
              "link: hit rect geometry");
        const DrawCommand* t = firstText(r, "x");
        check(t && sameColor(t->color, light.link), "link: link color");
        check(countKind(r, DrawCommand::Line) == 1, "link: underline line emitted");
    }

    // ---- strikethrough ----
    {
        LayoutResult r = lay("~~s~~");
        check(countKind(r, DrawCommand::Line) == 1, "strike: line emitted");
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::Line)
                check(near(c.rect.y, 42), "strike: line at mid line-height");
    }

    // ---- horizontal rule ----
    {
        LayoutResult r = lay("---");
        check(countKind(r, DrawCommand::FillRect) == 1, "hrule: one 1px fill");
        check(near(r.cmds[0].rect.h, 1) && near(r.cmds[0].rect.y, 40),
              "hrule: geometry (8px top margin)");
        check(near(r.contentHeight, 88), "hrule: content height");
    }

    // ---- table: columns stretch to fill when content fits ----
    {
        LayoutResult r = lay("| A | B |\n| --- | --- |\n| 1 | 2 |");
        check(countKind(r, DrawCommand::Text) == 4, "table: header + body cell texts");
        // equal natural widths -> both columns stretch to 410px: 40..450..860
        const DrawCommand* a = firstText(r, "A");
        const DrawCommand* b = firstText(r, "B");
        check(a && near(a->rect.x, 48) && a->font.bold, "table: header cell padded 8px, bold");
        check(b && near(b->rect.x, 458), "table: equal columns split the width");
        check(b && b->spaceBefore, "table: later columns copy with a separating space");
        // grid: 3 vertical fills + 3 horizontal rules + 1 header stripe
        check(countKind(r, DrawCommand::FillRect) == 7, "table: grid lines + header stripe");
        const DrawCommand* one = firstText(r, "1");
        check(one && !one->font.bold && one->selectable, "table: body cells plain + selectable");
        check(near(r.contentHeight, 148), "table: content height (two 34px rows)");
    }

    // ---- table: alignment ----
    {
        LayoutResult r = lay("| A | B |\n| --- | ---: |\n| x | y |");
        const DrawCommand* y = firstText(r, "y");
        check(y && near(y->rect.x, 842), "table: right-aligned cell hugs column right edge");
        LayoutResult c = lay("| H |\n| :---: |\n| x |");
        const DrawCommand* x = firstText(c, "x");
        check(x && near(x->rect.x, 445), "table: centered cell");
    }

    // ---- table: shrink + wrap when content overflows ----
    {
        // avail=120 < natural 182. The prose column shrinks (flooring at its
        // min-content) and its words wrap to fit; the narrow column keeps its token.
        LayoutResult r = lay("| Desc | N |\n| --- | --- |\n| aa bb cc dd ee | x |", 200);
        bool inBounds = true;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::Text && c.rect.x + c.rect.w > 160.001) inBounds = false;
        check(inBounds, "table: wrapped cells stay inside the content width");
        const DrawCommand* l1 = firstText(r, "aa bb");
        const DrawCommand* l2 = firstText(r, "cc dd");
        check(l1 && l2 && near(l2->rect.y - l1->rect.y, 24),
              "table: wrapped lines advance by line height + gap");
    }

    // ---- table: <br> forces a line break inside a cell ----
    {
        // Plenty of width for "one two" on one line -- <br> must force the
        // break regardless, unlike ordinary word-wrap (which only breaks on
        // overflow). This is GFM's only way to hard-break inside a pipe-table
        // cell (the syntax can't hold a literal newline).
        LayoutResult r = lay("| A |\n| --- |\n| one<br>two |", 900);
        const DrawCommand* l1 = firstText(r, "one");
        const DrawCommand* l2 = firstText(r, "two");
        check(l1 && l2 && near(l1->rect.x, l2->rect.x) && near(l2->rect.y - l1->rect.y, 24),
              "table: <br> starts a new line at the same column x, one line-height down");
        check(firstText(r, "one two") == nullptr, "table: <br> content isn't joined onto one line");
    }
    {
        // Two <br> in a row -> a blank line between "one" and "two".
        LayoutResult r = lay("| A |\n| --- |\n| one<br><br>two |", 900);
        const DrawCommand* l1 = firstText(r, "one");
        const DrawCommand* l2 = firstText(r, "two");
        check(l1 && l2 && near(l2->rect.y - l1->rect.y, 48),
              "table: <br><br> leaves a blank line (two line-heights down)");
    }

    // ---- table: a short unbreakable token keeps its column (min-content floor) ----
    {
        // The prose column forces the table to shrink, but "SENSE_FWD" stays on
        // one line instead of overflowing into the next column (the reported bug).
        LayoutResult r = lay("| Sig | Note |\n| --- | --- |\n| SENSE_FWD | this is a "
                             "long note with many words that must wrap across lines to fit |", 900);
        check(firstText(r, "SENSE_FWD") != nullptr,
              "table: short token stays intact while a sibling column wraps");
        bool inBounds = true;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::Text && c.rect.x + c.rect.w > 860.001) inBounds = false;
        check(inBounds, "table: no cell overflows the table width");
    }

    // ---- table: an over-long token breaks rather than overflowing ----
    {
        // Window too narrow to fit the token's min-content -> it breaks char-wise.
        LayoutResult r = lay("| Sig | N |\n| --- | --- |\n| SUPERLONGIDENTIFIER | x |", 160);
        check(firstText(r, "SUPERLONGIDENTIFIER") == nullptr, "table: over-long token is broken up");
        bool inBounds = true;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::Text && c.rect.x + c.rect.w > 120.001) inBounds = false;
        check(inBounds, "table: broken token stays inside its column");
    }

    // ---- block tops track every block ----
    {
        LayoutResult r = lay("# H\n\npara\n\n- item");
        check(r.blockTops.size() == 3, "tops: one entry per block");
        check(r.blockTops.size() == 3 && r.blockTops[0] < r.blockTops[1]
                  && r.blockTops[1] < r.blockTops[2],
              "tops: strictly increasing");
    }

    // ---- scale ----
    {
        LayoutResult r = lay("hello", 900, 2.0);
        check(r.blockTops.size() == 1 && near(r.blockTops[0], 64),
              "scale: 2x doubles the top padding");
        const DrawCommand* t = firstText(r, "hello");
        check(t && near(t->rect.x, 80), "scale: 2x doubles the left padding");
    }
    {
        // round-half-up like the Win32 S() macro: 32 * 1.25 = 40 exactly,
        // and PAD_X 40 * 1.25 = 50
        LayoutResult r = lay("hello", 900, 1.25);
        const DrawCommand* t = firstText(r, "hello");
        check(t && near(t->rect.x, 50) && r.blockTops.size() == 1
                  && near(r.blockTops[0], 40),
              "scale: fractional scale rounds like Win32");
    }

    return summary();
}
