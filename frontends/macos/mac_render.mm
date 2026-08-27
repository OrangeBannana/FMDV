#include "mac_render.h"
#include "bench_log.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace fmdv {

// ---------------- CoreTextMeasurer ----------------

static long fontKey(const FontSpec& f) {
    return ((long)f.role << 20) | ((long)llround(f.px) << 4) | (f.bold ? 2 : 0) | (f.italic ? 1 : 0);
}

CoreTextMeasurer::~CoreTextMeasurer() {
    for (auto& kv : cache_) if (kv.second) CFRelease(kv.second);
}

CTFontRef CoreTextMeasurer::font(const FontSpec& f) {
    long k = fontKey(f);
    auto it = cache_.find(k);
    if (it != cache_.end()) return it->second;

    CTFontRef base;
    if (f.role == FontRole::Mono)
        base = CTFontCreateWithName(CFSTR("Menlo"), f.px, nullptr);
    else
        base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, f.px, nullptr);

    CTFontSymbolicTraits tr = 0;
    if (f.bold) tr |= kCTFontTraitBold;
    if (f.italic) tr |= kCTFontTraitItalic;
    if (tr) {
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
            base, f.px, nullptr, tr, kCTFontTraitBold | kCTFontTraitItalic);
        if (styled) { CFRelease(base); base = styled; }
    }
    cache_[k] = base;
    return base;
}

static CFStringRef makeCFString(StrView s) {
    return CFStringCreateWithCharacters(nullptr, (const UniChar*)s.data(), (CFIndex)s.size());
}

// Build a CTLine for a string in a font, optionally with a foreground color.
static CTLineRef makeLine(CTFontRef ft, StrView s, CGColorRef color) {
    CFStringRef str = makeCFString(s);
    CFStringRef keys[2] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    CFTypeRef vals[2] = { ft, color };
    CFIndex n = color ? 2 : 1;
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, (const void**)keys, (const void**)vals, n,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as); CFRelease(attrs); CFRelease(str);
    return line;
}

double CoreTextMeasurer::textWidth(const FontSpec& f, StrView s) {
    if (s.empty()) return 0;
    CTLineRef line = makeLine(font(f), s, nullptr);
    double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line);
    return w;
}
double CoreTextMeasurer::lineHeight(const FontSpec& f) {
    CTFontRef ft = font(f);
    return std::ceil(CTFontGetAscent(ft) + CTFontGetDescent(ft) + CTFontGetLeading(ft));
}
double CoreTextMeasurer::ascent(const FontSpec& f) {
    return std::ceil(CTFontGetAscent(font(f)));
}

// ---------------- headless CoreGraphics painter ----------------

static CGColorRef cg(CGColorSpaceRef cs, Color c) {
    CGFloat comps[4] = { c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0 };
    return CGColorCreate(cs, comps);
}

// Adds a rounded-rect path (clamped so the radius never exceeds half the
// shorter side) to the current path; caller fills or strokes it.
static void addRoundedRectPath(CGContextRef ctx, CGRect rect, double radius) {
    double r = std::max(0.0, std::min(radius, std::min(rect.size.width, rect.size.height) / 2.0));
    CGPathRef path = CGPathCreateWithRoundedRect(rect, r, r, nullptr);
    CGContextAddPath(ctx, path);
    CGPathRelease(path);
}

void PaintLayout(CGContextRef ctx, double height, const LayoutResult& r,
                 const LayoutTheme& th, CoreTextMeasurer& tm,
                 const std::vector<ColoredRect>* highlights) {
    double H = height;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    // background (fills whatever region is being drawn: full bitmap or dirty rect)
    CGColorRef bg = cg(cs, th.bg);
    CGContextSetFillColorWithColor(ctx, bg);
    CGContextFillRect(ctx, CGContextGetClipBoundingBox(ctx));
    CGColorRelease(bg);

    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    auto flipY = [&](double docY) { return H - docY; }; // top-left -> CG bottom-left

    // Paints one FillRect/FrameRect/Line command (Text is handled separately).
    auto paintNonText = [&](const DrawCommand& c) {
        switch (c.kind) {
        case DrawCommand::FillRect: {
            CGColorRef col = cg(cs, c.color);
            CGContextSetFillColorWithColor(ctx, col);
            CGRect rr = CGRectMake(c.rect.x, flipY(c.rect.y + c.rect.h), c.rect.w, c.rect.h);
            if (c.radius > 0) { addRoundedRectPath(ctx, rr, c.radius); CGContextFillPath(ctx); }
            else CGContextFillRect(ctx, rr);
            CGColorRelease(col);
            break;
        }
        case DrawCommand::FrameRect: {
            CGColorRef col = cg(cs, c.color);
            CGContextSetStrokeColorWithColor(ctx, col);
            CGContextSetLineWidth(ctx, 1.0);
            CGRect rr = CGRectMake(c.rect.x + 0.5, flipY(c.rect.y + c.rect.h) + 0.5,
                                    c.rect.w - 1, c.rect.h - 1);
            if (c.radius > 0) { addRoundedRectPath(ctx, rr, c.radius); CGContextStrokePath(ctx); }
            else CGContextStrokeRect(ctx, rr);
            CGColorRelease(col);
            break;
        }
        case DrawCommand::Line: {
            CGColorRef col = cg(cs, c.color);
            CGContextSetStrokeColorWithColor(ctx, col);
            CGContextSetLineWidth(ctx, 1.0);
            CGContextBeginPath(ctx);
            CGContextMoveToPoint(ctx, c.rect.x, flipY(c.rect.y) - 0.5);
            CGContextAddLineToPoint(ctx, c.rect.w, flipY(c.rect.h) - 0.5);
            CGContextStrokePath(ctx);
            CGColorRelease(col);
            break;
        }
        case DrawCommand::Text:
            break;
        }
    };

    // Paint in four passes so layering is correct regardless of command order:
    //   1. backgrounds (FillRect/FrameRect/Line not marked afterText)
    //   2. highlights (selection, find matches) — over the block backgrounds
    //   3. text — over the highlights
    //   4. decorations marked afterText (table grid lines, strikethrough,
    //      link underlines) — these are emitted right after their own text
    //      specifically so they stay on top of it.
    // Previously highlights were drawn before the command loop, so a code
    // block's background fill (emitted as a FillRect) painted over the
    // selection, hiding word/line selection inside code blocks.
    for (const DrawCommand& c : r.cmds)
        if (!c.afterText) paintNonText(c);

    // highlights (selection, find matches, ...), over the backgrounds, behind text
    if (highlights) {
        for (const ColoredRect& h : *highlights) {
            CGColorRef hc = cg(cs, h.color);
            CGContextSetFillColorWithColor(ctx, hc);
            CGRect rr = CGRectMake(h.rect.x, flipY(h.rect.y + h.rect.h), h.rect.w, h.rect.h);
            if (h.radius > 0) { addRoundedRectPath(ctx, rr, h.radius); CGContextFillPath(ctx); }
            else CGContextFillRect(ctx, rr);
            CGColorRelease(hc);
        }
    }

    for (const DrawCommand& c : r.cmds) {
        if (c.kind != DrawCommand::Text) continue;
        CGColorRef col = cg(cs, c.color);
        CTLineRef line = makeLine(tm.font(c.font), c.text, col);
        CGContextSetTextPosition(ctx, c.rect.x, flipY(c.rect.y));
        CTLineDraw(line, ctx);
        CFRelease(line);
        if (c.underline || c.strike) {
            CGContextSetStrokeColorWithColor(ctx, col);
            CGContextSetLineWidth(ctx, 1.0);
            double uy = c.underline ? (c.rect.y + 2) : (c.rect.y - tm.ascent(c.font) * 0.35);
            CGContextBeginPath(ctx);
            CGContextMoveToPoint(ctx, c.rect.x, flipY(uy) - 0.5);
            CGContextAddLineToPoint(ctx, c.rect.x + c.rect.w, flipY(uy) - 0.5);
            CGContextStrokePath(ctx);
        }
        CGColorRelease(col);
    }

    for (const DrawCommand& c : r.cmds)
        if (c.afterText) paintNonText(c);

    CGColorSpaceRelease(cs);
}

bool RenderMarkdownToPng(const Document& doc, double width, bool dark, const char* outPath) {
    LayoutTheme th = dark ? DarkLayoutTheme() : LightLayoutTheme();
    CoreTextMeasurer tm;
    LayoutResult r = LayoutDocument(doc, width, th, tm);

    int W = (int)std::ceil(width);
    int H = (int)std::ceil(r.contentHeight);
    if (H < 1) H = 1;

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(nullptr, W, H, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) return false;

    PaintLayout(ctx, H, r, th, tm);

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return false;

    CFStringRef path = CFStringCreateWithCString(nullptr, outPath, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, path, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url); CFRelease(path); CGImageRelease(img);
    return ok;
}

// Render a laid-out display list into a WxH RGBA bitmap with the given
// highlights; returns the buffer (caller frees). Used by RenderSelProbe.
static bool renderToBuffer(const LayoutResult& r, CoreTextMeasurer& tm,
                           const LayoutTheme& th, int W, int H,
                           const std::vector<ColoredRect>* hl,
                           unsigned char** outBuf) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(nullptr, W, H, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) return false;
    PaintLayout(ctx, H, r, th, tm, hl);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return false;
    CGColorSpaceRef bcs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    unsigned char* buf = (unsigned char*)calloc((size_t)W * H * 4, 1);
    CGContextRef bctx = CGBitmapContextCreate(buf, W, H, 8, W * 4, bcs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(bcs);
    if (!bctx) { CFRelease(img); return false; }
    CGContextDrawImage(bctx, CGRectMake(0, 0, W, H), img);
    CGContextRelease(bctx);
    CFRelease(img);
    *outBuf = buf;
    return true;
}

bool RenderSelProbe(const Document& doc, double width, bool dark,
                    int* selR, int* selG, int* selB) {
    LayoutTheme th = dark ? DarkLayoutTheme() : LightLayoutTheme();
    CoreTextMeasurer tm;
    LayoutResult r = LayoutDocument(doc, width, th, tm);

    // Find the first monospace (code-block) text line to highlight.
    const DrawCommand* code = nullptr;
    for (const DrawCommand& c : r.cmds)
        if (c.kind == DrawCommand::Text && c.font.role == FontRole::Mono) { code = &c; break; }
    if (!code) return false;

    int W = (int)std::ceil(width);
    int H = (int)std::ceil(r.contentHeight);
    if (H < 1) H = 1;

    // The selection highlight rect is top-origin (like FMDVPreviewView's
    // selectionRects): top = baseline - ascent, height = lineHeight.
    double asc = tm.ascent(code->font), lh = tm.lineHeight(code->font);
    RectF lineBox = {code->rect.x, code->rect.y - asc, code->rect.w, lh};

    // Highlight the whole code line, like a triple-click selection.
    std::vector<ColoredRect> hl;
    hl.push_back({lineBox, th.sel});

    // Differential probe: render the doc with and without the highlight and
    // count pixels that differ. The two renders are identical except for the
    // highlight, so any difference means the highlight is visible. A correct
    // painter draws it over the code-block background (differs); the old
    // painter drew it behind the block background (identical, selection
    // invisible). Comparing renders sidesteps CoreGraphics color management,
    // which shifts exact sRGB byte values and would make a direct color-match
    // flaky.
    unsigned char* bufSel = nullptr, *bufNone = nullptr;
    if (!renderToBuffer(r, tm, th, W, H, &hl, &bufSel)) return false;
    if (!renderToBuffer(r, tm, th, W, H, nullptr, &bufNone)) { free(bufSel); return false; }

    // Restrict the scan to the code line's x-column (its x-range is known and
    // unambiguous) across all rows, so the signal is specific to the code block
    // and immune to bitmap row-orientation differences.
    int x0 = (int)std::floor(lineBox.x), x1 = (int)std::ceil(lineBox.x + lineBox.w);
    if (x0 < 0) x0 = 0; if (x1 > W) x1 = W;
    int differing = 0;
    for (int y = 0; y < H; y++) {
        const unsigned char* a = bufSel + ((size_t)y * W) * 4;
        const unsigned char* b = bufNone + ((size_t)y * W) * 4;
        for (int x = x0; x < x1; x++) {
            const unsigned char* pa = a + (size_t)x * 4;
            const unsigned char* pb = b + (size_t)x * 4;
            if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) differing++;
        }
    }
    bool visible = differing > 0;
    if (selR) { *selR = visible ? th.sel.r : -1; *selG = visible ? th.sel.g : -1; *selB = visible ? th.sel.b : -1; }
    free(bufSel); free(bufNone);
    return visible;
}

void BenchLayoutRender(const Document& doc, double width, bool dark, int runs,
                       double& layoutMedianMs, double& renderMedianMs) {
    if (runs < 1) runs = 1;
    LayoutTheme th = dark ? DarkLayoutTheme() : LightLayoutTheme();
    CoreTextMeasurer tm;
    std::vector<double> lt, rt;

    LayoutResult r;
    for (int i = 0; i < runs; i++) {
        double t0 = NowMonotonicMs();
        r = LayoutDocument(doc, width, th, tm);
        lt.push_back(NowMonotonicMs() - t0);
    }

    int W = (int)std::ceil(width), H = (int)std::ceil(r.contentHeight);
    if (H < 1) H = 1;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(nullptr, W, H, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    // A degenerate size (e.g. --width 0) yields a null context; don't paint into
    // it. Report the layout timing and a zero render median.
    if (!ctx) {
        std::sort(lt.begin(), lt.end());
        layoutMedianMs = lt[lt.size() / 2];
        renderMedianMs = 0;
        return;
    }
    for (int i = 0; i < runs; i++) {
        double t0 = NowMonotonicMs();
        PaintLayout(ctx, H, r, th, tm);
        rt.push_back(NowMonotonicMs() - t0);
    }
    if (ctx) CGContextRelease(ctx);

    std::sort(lt.begin(), lt.end());
    std::sort(rt.begin(), rt.end());
    layoutMedianMs = lt[lt.size() / 2];
    renderMedianMs = rt[rt.size() / 2];
}

} // namespace fmdv
