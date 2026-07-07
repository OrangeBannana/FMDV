#pragma once
// macOS rendering: a CoreText TextMeasurer and a headless CoreGraphics painter
// that draws core/layout's platform-neutral display list into a PNG. Shared by
// the headless --dump path and (later) the AppKit preview view.
#include <CoreText/CoreText.h>
#include <map>
#include "layout.h"
#include "markdown.h"

namespace fmdv {

// Measures text with CoreText. Fonts are cached by (role, bold, italic, px).
class CoreTextMeasurer : public TextMeasurer {
public:
    ~CoreTextMeasurer() override;
    double textWidth(const FontSpec& f, StrView s) override;
    double lineHeight(const FontSpec& f) override;
    double ascent(const FontSpec& f) override;
    CTFontRef font(const FontSpec& f); // shared with the painter; not owned by caller
private:
    std::map<long, CTFontRef> cache_;
};

// Paint a display list into a CGContext whose height is `height` px (document
// space is top-left/y-down; this flips to CoreGraphics' bottom-left). Fills the
// background first. Shared by the headless PNG path and the AppKit preview view.
void PaintLayout(CGContextRef ctx, double height, const LayoutResult& r,
                 const LayoutTheme& th, CoreTextMeasurer& tm);

// Render a laid-out document to a PNG at `outPath`. Returns false on failure.
bool RenderMarkdownToPng(const Document& doc, double width, bool dark, const char* outPath);

// Open the AppKit window showing `file` and run the app loop (app.mm).
int RunApp(const char* file, bool dark);

} // namespace fmdv
