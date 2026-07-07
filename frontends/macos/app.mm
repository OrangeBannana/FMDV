// FMDV macOS AppKit shell: a scrollable preview window (macOS guide,
// macos/skeleton + interactions). The preview view lays out via core/layout and
// draws the display list with the shared CoreGraphics painter. Supports
// scrolling (NSScrollView), dark-mode toggle (Cmd-D), zoom (Cmd +/-/0), live
// file reload, and clickable links.
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <string>
#include "mac_render.h"
#include "markdown.h"
#include "str.h"

static std::string ReadFileUtf8(const char* path) {
    std::string out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (sz > 0) { out.resize((size_t)sz); out.resize(std::fread(&out[0], 1, (size_t)sz, f)); }
    std::fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF) out.erase(0, 3);
    return out;
}
static Str LoadDoc(std::string u8) {
    u8.erase(std::remove(u8.begin(), u8.end(), '\r'), u8.end());
    return FromUtf8(u8);
}

// ---------------- preview view ----------------

@interface FMDVPreviewView : NSView {
    Document _doc;
    fmdv::LayoutResult _layout;
    fmdv::CoreTextMeasurer _tm;
    bool _dark;
    double _zoom;
    double _laidOutWidth;
}
- (instancetype)initWithDoc:(const Document&)doc dark:(bool)dark;
- (void)setDoc:(const Document&)doc;
- (void)toggleDark;
- (void)zoomBy:(double)factor;
- (void)zoomReset;
@end

@implementation FMDVPreviewView
- (instancetype)initWithDoc:(const Document&)doc dark:(bool)dark {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 900, 100)])) {
        _doc = doc; _dark = dark; _zoom = 1.0; _laidOutWidth = -1;
    }
    return self;
}
- (BOOL)isFlipped { return YES; }          // top-left origin -> scrolls from the top
- (BOOL)acceptsFirstResponder { return YES; }

- (fmdv::LayoutTheme)theme {
    return _dark ? fmdv::DarkLayoutTheme() : fmdv::LightLayoutTheme();
}

- (void)relayout {
    double viewW = self.superview ? self.superview.bounds.size.width : self.bounds.size.width;
    if (viewW < 1) viewW = 900;
    double logicalW = viewW / _zoom;
    _layout = fmdv::LayoutDocument(_doc, logicalW, [self theme], _tm);
    _laidOutWidth = viewW;
    double h = _layout.contentHeight * _zoom;
    [self setFrameSize:NSMakeSize(viewW, h)];
    self.needsDisplay = YES;
}

- (void)setDoc:(const Document&)doc { _doc = doc; [self relayout]; }
- (void)toggleDark { _dark = !_dark; [self relayout]; }
- (void)zoomBy:(double)f { _zoom = std::min(3.0, std::max(0.5, _zoom * f)); [self relayout]; }
- (void)zoomReset { _zoom = 1.0; [self relayout]; }

- (void)viewDidMoveToSuperview { [self relayout]; }
- (void)resizeWithOldSuperviewSize:(NSSize)old {
    [super resizeWithOldSuperviewSize:old];
    if (self.superview && self.superview.bounds.size.width != _laidOutWidth) [self relayout];
}

- (void)drawRect:(NSRect)dirty {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    double h = self.bounds.size.height;
    CGContextSaveGState(ctx);
    // Undo the flipped-view transform so CoreText draws upright and the shared
    // bottom-left painter (verified by the PNG path) applies unchanged.
    CGContextTranslateCTM(ctx, 0, h);
    CGContextScaleCTM(ctx, _zoom, -_zoom);
    fmdv::PaintLayout(ctx, _layout.contentHeight, _layout, [self theme], _tm);
    CGContextRestoreGState(ctx);
}

- (BOOL)performKeyEquivalent:(NSEvent*)ev {
    if (ev.modifierFlags & NSEventModifierFlagCommand) {
        NSString* c = ev.charactersIgnoringModifiers;
        if ([c isEqualToString:@"d"]) { [self toggleDark]; return YES; }
        if ([c isEqualToString:@"="] || [c isEqualToString:@"+"]) { [self zoomBy:1.1]; return YES; }
        if ([c isEqualToString:@"-"]) { [self zoomBy:1.0 / 1.1]; return YES; }
        if ([c isEqualToString:@"0"]) { [self zoomReset]; return YES; }
    }
    return [super performKeyEquivalent:ev];
}

// Click a link: hit-test the point (in logical coords) against link rects.
- (void)mouseUp:(NSEvent*)ev {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    double lx = p.x / _zoom, ly = p.y / _zoom;
    for (const auto& lk : _layout.links) {
        const auto& r = lk.rect;
        if (lx >= r.x && lx <= r.x + r.w && ly >= r.y && ly <= r.y + r.h) {
            std::string u = ToUtf8(lk.href);
            NSString* s = [NSString stringWithUTF8String:u.c_str()];
            NSURL* url = [NSURL URLWithString:s];
            if (url) [[NSWorkspace sharedWorkspace] openURL:url];
            return;
        }
    }
}
@end

// ---------------- app delegate ----------------

@interface FMDVAppDelegate : NSObject <NSApplicationDelegate> {
    NSWindow* _window;
    FMDVPreviewView* _preview;
    std::string _file;
}
- (instancetype)initWithFile:(std::string)file preview:(FMDVPreviewView*)pv window:(NSWindow*)w;
@end

@implementation FMDVAppDelegate
- (instancetype)initWithFile:(std::string)file preview:(FMDVPreviewView*)pv window:(NSWindow*)w {
    if ((self = [super init])) { _file = std::move(file); _preview = pv; _window = w; }
    return self;
}
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    (void)n;
    [NSApp activateIgnoringOtherApps:YES];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_preview];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { (void)a; return YES; }

- (void)reload {
    Document doc = ParseMarkdown(LoadDoc(ReadFileUtf8(_file.c_str())));
    [_preview setDoc:doc];
}
@end

namespace fmdv {

int RunApp(const char* file, bool dark) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Minimal main menu so Cmd-Q (and the standard app menu) work.
        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [menubar addItem:appItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit FMDV" action:@selector(terminate:) keyEquivalent:@"q"];
        [appItem setSubmenu:appMenu];
        [NSApp setMainMenu:menubar];

        Document doc = ParseMarkdown(LoadDoc(ReadFileUtf8(file)));

        NSRect frame = NSMakeRect(0, 0, 940, 760);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        NSString* title = [NSString stringWithUTF8String:file];
        [window setTitle:[title lastPathComponent]];

        NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
        [scroll setHasVerticalScroller:YES];
        [scroll setAutohidesScrollers:YES];
        [scroll setDrawsBackground:NO];

        FMDVPreviewView* preview = [[FMDVPreviewView alloc] initWithDoc:doc dark:dark];
        [scroll setDocumentView:preview];
        [window setContentView:scroll];

        FMDVAppDelegate* delegate =
            [[FMDVAppDelegate alloc] initWithFile:std::string(file) preview:preview window:window];
        [NSApp setDelegate:delegate];

        [NSApp run];
    }
    return 0;
}

} // namespace fmdv
