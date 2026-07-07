#include "mac_render.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <cmath>

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

void PaintLayout(CGContextRef ctx, double height, const LayoutResult& r,
                 const LayoutTheme& th, CoreTextMeasurer& tm) {
    double H = height;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    // background (fills whatever region is being drawn: full bitmap or dirty rect)
    CGColorRef bg = cg(cs, th.bg);
    CGContextSetFillColorWithColor(ctx, bg);
    CGContextFillRect(ctx, CGContextGetClipBoundingBox(ctx));
    CGColorRelease(bg);

    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    auto flipY = [&](double docY) { return H - docY; }; // top-left -> CG bottom-left

    for (const DrawCommand& c : r.cmds) {
        switch (c.kind) {
        case DrawCommand::FillRect: {
            CGColorRef col = cg(cs, c.color);
            CGContextSetFillColorWithColor(ctx, col);
            CGContextFillRect(ctx, CGRectMake(c.rect.x, flipY(c.rect.y + c.rect.h), c.rect.w, c.rect.h));
            CGColorRelease(col);
            break;
        }
        case DrawCommand::FrameRect: {
            CGColorRef col = cg(cs, c.color);
            CGContextSetStrokeColorWithColor(ctx, col);
            CGContextSetLineWidth(ctx, 1.0);
            CGContextStrokeRect(ctx, CGRectMake(c.rect.x + 0.5, flipY(c.rect.y + c.rect.h) + 0.5,
                                                c.rect.w - 1, c.rect.h - 1));
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
        case DrawCommand::Text: {
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
            break;
        }
        }
    }

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

} // namespace fmdv
