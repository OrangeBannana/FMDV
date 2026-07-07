// FMDV macOS AppKit shell: a scrollable preview window with an optional split
// editor (macOS guide, macos/skeleton + interactions + editor). The preview
// lays out via core/layout and draws with the shared CoreGraphics painter; the
// editor is a native NSTextView whose edits re-parse into the live preview.
// Editor decisions (list continuation, table markdown) reuse core/edit_assist.
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <string>
#include "mac_render.h"
#include "markdown.h"
#include "edit_assist.h"
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
static Str NSStringToStr(NSString* s) { return FromUtf8(s.UTF8String ? s.UTF8String : ""); }
static NSString* StrToNS(const Str& s) { return [NSString stringWithUTF8String:ToUtf8(s).c_str()]; }

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
- (bool)dark;
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
- (bool)dark { return _dark; }
- (fmdv::LayoutTheme)theme { return _dark ? fmdv::DarkLayoutTheme() : fmdv::LightLayoutTheme(); }

- (void)relayout {
    double viewW = self.superview ? self.superview.bounds.size.width : self.bounds.size.width;
    if (viewW < 1) viewW = 900;
    double logicalW = viewW / _zoom;
    _layout = fmdv::LayoutDocument(_doc, logicalW, [self theme], _tm);
    _laidOutWidth = viewW;
    [self setFrameSize:NSMakeSize(viewW, _layout.contentHeight * _zoom)];
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
    (void)dirty;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    // Un-flip so CoreText draws upright and the shared bottom-left painter
    // (verified by the PNG path) applies unchanged; also applies zoom.
    CGContextTranslateCTM(ctx, 0, self.bounds.size.height);
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
- (void)mouseUp:(NSEvent*)ev {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    double lx = p.x / _zoom, ly = p.y / _zoom;
    for (const auto& lk : _layout.links) {
        const auto& r = lk.rect;
        if (lx >= r.x && lx <= r.x + r.w && ly >= r.y && ly <= r.y + r.h) {
            NSURL* url = [NSURL URLWithString:StrToNS(lk.href)];
            if (url) [[NSWorkspace sharedWorkspace] openURL:url];
            return;
        }
    }
}
@end

// ---------------- source editor (list continuation via core/edit_assist) ----

@interface FMDVTextView : NSTextView
@end

@implementation FMDVTextView
- (void)insertNewline:(id)sender {
    NSString* s = self.string;
    NSUInteger caret = self.selectedRange.location;
    NSUInteger ls = caret;
    while (ls > 0 && [s characterAtIndex:ls - 1] != '\n') ls--;
    NSString* line = [s substringWithRange:NSMakeRange(ls, caret - ls)];
    fmdv::ListEnter le = fmdv::DecideListEnter(NSStringToStr(line));
    if (!le.handled) { [super insertNewline:sender]; return; }
    if (le.endList) {
        if ([self shouldChangeTextInRange:NSMakeRange(ls, caret - ls) replacementString:@""]) {
            [self replaceCharactersInRange:NSMakeRange(ls, caret - ls) withString:@""];
            [self didChangeText];
        }
        return;
    }
    NSString* ins = [@"\n" stringByAppendingString:StrToNS(le.continuation)];
    NSRange sel = self.selectedRange;
    if ([self shouldChangeTextInRange:sel replacementString:ins]) {
        [self replaceCharactersInRange:sel withString:ins];
        [self didChangeText];
    }
}
- (void)insertTableMarkdown {
    NSString* tbl = StrToNS(fmdv::MakeTableMarkdown(3, 3));
    NSRange sel = self.selectedRange;
    if ([self shouldChangeTextInRange:sel replacementString:tbl]) {
        [self replaceCharactersInRange:sel withString:tbl];
        [self didChangeText];
    }
}
@end

// ---------------- window controller / app delegate ----------------

@interface FMDVAppDelegate : NSObject <NSApplicationDelegate, NSTextViewDelegate> {
    NSWindow* _window;
    FMDVPreviewView* _preview;
    NSScrollView* _previewScroll;
    FMDVTextView* _textView;
    NSSplitView* _split;
    std::string _file;
    bool _editing;
    bool _dark;
    bool _opened;
}
- (instancetype)initWithFile:(const char*)file dark:(bool)dark;
- (void)openPath:(NSString*)path;
@end

@implementation FMDVAppDelegate
- (instancetype)initWithFile:(const char*)file dark:(bool)dark {
    if ((self = [super init])) {
        _file = file ? file : ""; _dark = dark; _editing = false; _opened = false;
    }
    return self;
}

// Create the window + preview on demand (a bare .app launch has no document yet).
- (void)ensureWindow {
    if (_window) return;
    NSRect frame = NSMakeRect(0, 0, 940, 760);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered defer:NO];
    [_window setTitle:@"FMDV"];
    [_window center];
    _previewScroll = [[NSScrollView alloc] initWithFrame:frame];
    [_previewScroll setHasVerticalScroller:YES];
    [_previewScroll setAutohidesScrollers:YES];
    [_previewScroll setDrawsBackground:NO];
    _preview = [[FMDVPreviewView alloc] initWithDoc:Document() dark:_dark];
    [_previewScroll setDocumentView:_preview];
    [_window setContentView:_previewScroll];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_preview];
}

- (void)openPath:(NSString*)path {
    [self ensureWindow];
    _opened = true;
    _file = path.UTF8String ? path.UTF8String : "";
    Document doc = ParseMarkdown(LoadDoc(ReadFileUtf8(_file.c_str())));
    [_preview setDoc:doc];
    if (_textView) [_textView setString:StrToNS(LoadDoc(ReadFileUtf8(_file.c_str())))];
    [_window setTitle:path.lastPathComponent];
}

- (BOOL)application:(NSApplication*)app openFile:(NSString*)filename {
    (void)app; [self openPath:filename]; return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    (void)n;
    [NSApp activateIgnoringOtherApps:YES];
    if (_opened) return;                         // openFile: already loaded a document
    if (!_file.empty()) { [self openPath:[NSString stringWithUTF8String:_file.c_str()]]; return; }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:NO];
    if ([panel runModal] == NSModalResponseOK && panel.URLs.count > 0)
        [self openPath:panel.URLs.firstObject.path];
    else
        [self ensureWindow];                     // empty window if nothing chosen
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { (void)a; return YES; }

- (void)reparseFromEditor {
    Document doc = ParseMarkdown(NSStringToStr(_textView.string));
    [_preview setDoc:doc];
}
- (void)textDidChange:(NSNotification*)n { (void)n; [self reparseFromEditor]; }

- (void)toggleEditor:(id)sender {
    (void)sender;
    if (!_editing) {
        NSRect b = _window.contentView.bounds;
        if (!_textView) {
            NSScrollView* tvScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, b.size.width / 2, b.size.height)];
            [tvScroll setHasVerticalScroller:YES];
            _textView = [[FMDVTextView alloc] initWithFrame:tvScroll.bounds];
            [_textView setFont:[NSFont userFixedPitchFontOfSize:13]];
            [_textView setAutomaticQuoteSubstitutionEnabled:NO];
            [_textView setAutomaticDashSubstitutionEnabled:NO];
            [_textView setDelegate:self];
            [_textView setString:StrToNS(LoadDoc(ReadFileUtf8(_file.c_str())))];
            [tvScroll setDocumentView:_textView];

            _split = [[NSSplitView alloc] initWithFrame:b];
            [_split setVertical:YES];
            [_split setDividerStyle:NSSplitViewDividerStyleThin];
            [_split addSubview:tvScroll];
            [_split addSubview:_previewScroll];
        }
        [_window setContentView:_split];
        [_window makeFirstResponder:_textView];
        _editing = true;
        [self reparseFromEditor];
    } else {
        [_previewScroll removeFromSuperview];
        [_window setContentView:_previewScroll];
        [_window makeFirstResponder:_preview];
        _editing = false;
    }
}

- (void)saveDoc:(id)sender {
    (void)sender;
    if (_file.empty()) return;
    std::string text = _editing ? std::string(_textView.string.UTF8String ?: "") : ToUtf8(LoadDoc(ReadFileUtf8(_file.c_str())));
    // NSTextView already uses LF; write as UTF-8.
    FILE* f = std::fopen(_file.c_str(), "wb");
    if (f) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
}
- (void)insertTable:(id)sender { (void)sender; if (_editing && _textView) [_textView insertTableMarkdown]; }
@end

// ---------------- menu ----------------

static void buildMenu(id target) {
    NSMenu* menubar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menubar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit FMDV" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];

    NSMenuItem* fileItem = [[NSMenuItem alloc] init];
    [menubar addItem:fileItem];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [[fileMenu addItemWithTitle:@"Save" action:@selector(saveDoc:) keyEquivalent:@"s"] setTarget:target];
    [fileItem setSubmenu:fileMenu];

    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [[viewMenu addItemWithTitle:@"Toggle Editor" action:@selector(toggleEditor:) keyEquivalent:@"e"] setTarget:target];
    [[viewMenu addItemWithTitle:@"Insert Table" action:@selector(insertTable:) keyEquivalent:@"t"] setTarget:target];
    [viewItem setSubmenu:viewMenu];

    [NSApp setMainMenu:menubar];
}

namespace fmdv {

int RunApp(const char* file, bool dark) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        FMDVAppDelegate* delegate = [[FMDVAppDelegate alloc] initWithFile:file dark:dark];
        [NSApp setDelegate:delegate];
        buildMenu(delegate);
        [NSApp run];
    }
    return 0;
}

} // namespace fmdv
