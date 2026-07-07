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
                       // Text: x = rect.x (left), rect.y = baseline y, rect.h = line height.
    Color color;
    FontSpec font;     // Text only
    Str text;          // Text only
    bool underline = false; // Text: link underline
    bool strike = false;    // Text: strikethrough
};

// A clickable link rectangle (document space) recorded during layout.
struct LinkHit {
    RectF rect;
    Str href;
};

struct LayoutTheme {
    Color bg, bg2, bg3, text, text2, border, link, codeText, sel;
};
LayoutTheme LightLayoutTheme();
LayoutTheme DarkLayoutTheme();

struct LayoutResult {
    std::vector<DrawCommand> cmds;
    std::vector<LinkHit> links;
    double contentHeight = 0;
};

// Lay out `doc` at content width `width` (px) with `th`, measuring via `tm`.
LayoutResult LayoutDocument(const Document& doc, double width,
                            const LayoutTheme& th, TextMeasurer& tm);

} // namespace fmdv
