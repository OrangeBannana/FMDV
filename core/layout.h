#pragma once
// Platform-neutral layout: Document -> a display list of draw commands in
// document space (macOS impl guide, Phase 2 "core/layout"). The frontend
// supplies text metrics via a TextMeasurer and paints the returned commands
// with its own graphics API (CoreGraphics on macOS, GDI on Windows), so the
// layout/wrapping logic is shared and only measurement + drawing are per-platform.
#include <cstdint>
#include <vector>
#include "str.h"
#include "markdown.h"

namespace fmdv {

struct Color { uint8_t r = 0, g = 0, b = 0, a = 255; };
struct RectF { double x = 0, y = 0, w = 0, h = 0; };

enum class FontRole { Body, Mono, H1, H2, H3, H4, H5, H6 };

struct FontSpec {
    FontRole role = FontRole::Body;
    bool bold = false;
    bool italic = false;
    double px = 16.0;
};

// Default point size for a role (matches the Windows frontend: Body 16, Mono 14,
// H1 30 … H6 13). The frontend may scale by zoom/DPI on top of this.
inline double RoleSizePx(FontRole r) {
    switch (r) {
        case FontRole::H1:   return 30;
        case FontRole::H2:   return 24;
        case FontRole::H3:   return 20;
        case FontRole::H4:   return 16;
        case FontRole::H5:   return 14;
        case FontRole::H6:   return 13;
        case FontRole::Mono: return 14;
        case FontRole::Body: default: return 16;
    }
}

// Frontends implement this over their font system.
class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;
    virtual double textWidth(const FontSpec& f, StrView s) = 0; // advance width in px
    virtual double lineHeight(const FontSpec& f) = 0;           // full line box height
    virtual double ascent(const FontSpec& f) = 0;               // baseline from box top
};

// One positioned draw command, document space (y is not scroll-adjusted).
struct DrawCommand {
    enum Kind { FillRect, FrameRect, Line, Text };
    Kind kind = FillRect;
    RectF rect;        // FillRect/FrameRect: the box. Line: (x,y)->(w,h) as (x2,y2).
                       // Text: x = rect.x (left), rect.y = baseline y,
                       // rect.w = run advance width, rect.h = the run's own font
                       // height (frontends recover the top as baseline - ascent).
    Color color;
    FontSpec font;     // Text only
    Str text;          // Text only
    bool underline = false; // Text: link underline (links also get explicit Line cmds)
    bool strike = false;    // Text: strikethrough (also emitted as a Line cmd)
    bool spaceBefore = false; // Text: a space separated this run from the previous
                              // one on the line (drives copy spacing)
    bool selectable = true;   // Text: false for list markers (bullet/number),
                              // which are drawn but not selectable/copyable
};

// A clickable link rectangle (document space) recorded during layout.
struct LinkHit {
    RectF rect;
    Str href;
};

// A clickable task-list checkbox (document space). `srcLine` is the 0-based
// source line of the item (Block::srcStartLine) so the frontend can toggle the
// "[ ]" / "[x]" marker in the raw markdown; `state` is 0 unchecked, 1 checked.
struct TaskHit {
    RectF rect;
    int srcLine = -1;
    int state = 0;
};

struct LayoutTheme {
    Color bg, bg2, bg3, text, text2, border, link, codeText, sel;
};
LayoutTheme LightLayoutTheme();
LayoutTheme DarkLayoutTheme();

// A heading occurrence, for a table-of-contents sidebar (document-space top y).
struct HeadingRef {
    int level;
    Str text;
    double y;
};

struct LayoutResult {
    std::vector<DrawCommand> cmds;
    std::vector<LinkHit> links;
    std::vector<TaskHit> taskHits;
    std::vector<HeadingRef> headings;
    std::vector<double> blockTops; // document-space top y per doc.blocks[i]
                                   // (TOC scroll anchors)
    double contentHeight = 0;
};

// Lay out `doc` at content width `width` (px) with `th`, measuring via `tm`.
// `scale` multiplies the layout constants (padding/margins), rounded to whole
// px the way the Win32 frontend always has (zoom * DPI); the measurer is
// expected to return metrics for correspondingly scaled fonts. Frontends that
// scale at paint time (macOS: CTM) pass 1.0.
LayoutResult LayoutDocument(const Document& doc, double width,
                            const LayoutTheme& th, TextMeasurer& tm,
                            double scale = 1.0);

} // namespace fmdv
