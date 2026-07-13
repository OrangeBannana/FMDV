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
        // Layout renumbers ordered lists itself and resets after a non-ordered
        // block; the source numbers are ignored.
        LayoutResult r = lay("5. a\n6. b\n\npara\n\n9. c");
        check(firstText(r, "1.") != nullptr && firstText(r, "2.") != nullptr,
              "ol: renumbered from 1 regardless of source");
        check(firstText(r, "5.") == nullptr, "ol: source number not drawn");
        int ones = 0;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::Text && ToUtf8(c.text) == "1.") ones++;
        check(ones == 2, "ol: counter resets after a non-ordered block");
        const DrawCommand* marker = firstText(r, "2.");
        check(marker && !marker->selectable, "ol: number marker not selectable");
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
                  && near(box.rect.h, 2 * 24 + 24),
              "code: box geometry (2 lines)");
        const DrawCommand* l1 = firstText(r, "ab");
        const DrawCommand* l2 = firstText(r, "cd");
        check(l1 && l2 && l1->font.role == FontRole::Mono, "code: mono font");
        check(l1 && near(l1->rect.x, 56), "code: text inset 16px into the box");
        check(l1 && l2 && near(l2->rect.y - l1->rect.y, 24), "code: line advance = height + 4");
        check(l1 && l1->selectable, "code: lines are selectable");
    }

    // ---- blockquote ----
    {
        LayoutResult r = lay("> hi there");
        const DrawCommand* t = firstText(r, "hi there");
        check(t && sameColor(t->color, light.text2), "quote: muted text color");
        check(t && near(t->rect.x, 56), "quote: text indented 16px");
        bool bar = false;
        for (const auto& c : r.cmds)
            if (c.kind == DrawCommand::FillRect && near(c.rect.w, 4)
                && sameColor(c.color, light.border)) bar = true;
        check(bar, "quote: 4px border bar");
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
