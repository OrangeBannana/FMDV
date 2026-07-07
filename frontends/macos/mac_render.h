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

// Render a laid-out document to a PNG at `outPath`. Returns false on failure.
bool RenderMarkdownToPng(const Document& doc, double width, bool dark, const char* outPath);

} // namespace fmdv
