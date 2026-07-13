// FMDV macOS AppKit shell: a scrollable preview window with an optional split
// editor (macOS guide, macos/skeleton + interactions + editor). The preview
// lays out via core/layout and draws with the shared CoreGraphics painter; the
// editor is a native NSTextView whose edits re-parse into the live preview.
// Editor decisions (list continuation, table markdown) reuse core/edit_assist.
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "mac_render.h"
#include "markdown.h"
#include "edit_assist.h"
#include "release_info.h"
#include "text_select.h"
#include "str.h"
#include "../../cpp/version.h" // FMDV_VERSION_STR — single source of truth

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

// Persisted UI preferences. macOS uses NSUserDefaults; the Win32 app stores the
// same fields in %APPDATA%\fmdv\prefs.txt (dark / zoom / split / update mode+pin).
static NSString* const kPrefDark         = @"FMDVDark";
static NSString* const kPrefZoomPct      = @"FMDVZoomPct";
static NSString* const kPrefSplitPct     = @"FMDVSplitPct";
static NSString* const kPrefUpdateNotify = @"FMDVUpdateNotify"; // check on launch (default on)
static NSString* const kPrefUpdateMode   = @"FMDVUpdateMode";   // "notify" (default) / "auto" / "pin"
static NSString* const kPrefPinTag       = @"FMDVPinTag";       // tag held by "pin" mode

// File modification time (seconds since reference date; 0 if missing). Backs the
// live-reload poll — the Win32 app watches the same mtime via a 500ms WM_TIMER.
static NSTimeInterval FileModTime(const std::string& path) {
    if (path.empty()) return 0;
    NSString* p = [NSString stringWithUTF8String:path.c_str()];
    NSDictionary* a = [[NSFileManager defaultManager] attributesOfItemAtPath:p error:nil];
    NSDate* d = a[NSFileModificationDate];
    return d ? d.timeIntervalSinceReferenceDate : 0;
}

// ---------------- preview view ----------------

// A selectable text fragment: one laid-out Text command, with its box in
// document space (top-left, y-down) for hit-testing and highlighting.
struct Frag {
    fmdv::RectF box;
    fmdv::FontSpec font;
    Str text;
    double baseline;
};

// If char index `ch` sits inside a double-quoted phrase in `t`, set [s,e) to the
// span between the quotes (exclusive of the quote marks) and return true; else
// false. Handles straight ("...") and curly (“...”) double quotes. A missing
// closing quote returns false so the caller falls back to single-word selection.
static bool QuotedSpan(const Str& t, long ch, long& s, long& e) {
    long n = (long)t.size();
    if (ch < 0) ch = 0; else if (ch > n) ch = n;
    const Char kOpen = (Char)0x201C, kClose = (Char)0x201D, kQuote = (Char)0x22;
    // Curly quotes pair by direction: nearest “ to the left, matching ” to the right.
    for (long i = ch - 1; i >= 0; i--) {
        if (t[i] == kClose) break;              // a closer to our left => not inside
        if (t[i] == kOpen) {
            for (long j = ch; j < n; j++) {
                if (t[j] == kOpen) break;        // another opener before a closer => bail
                if (t[j] == kClose) { s = i + 1; e = j; return true; }
            }
            break;
        }
    }
    // Straight quotes: inside iff an odd number of " precede ch AND a closing " follows.
    long before = 0; for (long i = 0; i < ch; i++) if (t[i] == kQuote) before++;
    if (before % 2 == 1) {
        long L = -1; for (long i = ch - 1; i >= 0; i--) if (t[i] == kQuote) { L = i; break; }
        long R = -1; for (long j = ch; j < n; j++)   if (t[j] == kQuote) { R = j; break; }
        if (L >= 0 && R >= 0 && L < R) { s = L + 1; e = R; return true; }
    }
    return false;
}

@interface FMDVPreviewView : NSView <NSTextFieldDelegate> {
    Document _doc;
    fmdv::LayoutResult _layout;
    fmdv::CoreTextMeasurer _tm;
    bool _dark;
    double _zoom;
    double _laidOutWidth;
    std::vector<Frag> _frags;               // selectable text, document order
    long _selA, _selACh, _selB, _selBCh;    // selection endpoints (frag index + char)
    bool _hasSel;
    bool _dragging;
    // find in doc
    NSView* _findBar;
    NSTextField* _findField;
    NSTextField* _findLabel;
    std::vector<fmdv::FindMatch> _matches;
    long _curMatch;
}
// Directory of the open document; scheme-less link hrefs resolve against it.
@property (nonatomic, copy) NSString* baseDir;
- (instancetype)initWithDoc:(const Document&)doc dark:(bool)dark;
- (void)setDoc:(const Document&)doc;
- (bool)dark;
- (void)toggleDark;
- (void)zoomBy:(double)factor;
- (void)zoomReset;
- (void)scrollToDocY:(double)docY;
- (std::vector<fmdv::HeadingRef>)headings;
// test-driver introspection (--test-drive)
- (bool)findBarVisible;
- (NSString*)findLabelText;
- (void)stepFind:(int)dir;
- (NSString*)laidOutInfo; // "<laidOutWidth> <contentHeight>", to probe reflow
@end

@implementation FMDVPreviewView
- (instancetype)initWithDoc:(const Document&)doc dark:(bool)dark {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 900, 100)])) {
        // Restore persisted dark/zoom so the very first paint uses them; an
        // explicit --dark flag still forces dark for this launch.
        NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
        NSInteger zp = [ud integerForKey:kPrefZoomPct]; // 0 if never set
        _doc = doc;
        _dark = dark || [ud boolForKey:kPrefDark];
        _zoom = zp > 0 ? std::min(3.0, std::max(0.5, zp / 100.0)) : 1.0;
        _laidOutWidth = -1;
        _hasSel = false; _dragging = false;
    }
    return self;
}
- (void)savePrefs {
    NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
    [ud setBool:_dark forKey:kPrefDark];
    [ud setInteger:(NSInteger)std::llround(_zoom * 100) forKey:kPrefZoomPct];
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
    // rebuild selectable fragments from the Text draw commands (document order)
    _frags.clear();
    for (const auto& c : _layout.cmds) {
        if (c.kind != fmdv::DrawCommand::Text || c.text.empty()) continue;
        double asc = _tm.ascent(c.font), lh = _tm.lineHeight(c.font);
        Frag f;
        f.box = {c.rect.x, c.rect.y - asc, c.rect.w, lh};
        f.font = c.font; f.text = c.text; f.baseline = c.rect.y;
        _frags.push_back(std::move(f));
    }
    _hasSel = false;
    _matches.clear(); _curMatch = -1;
    [self setFrameSize:NSMakeSize(viewW, _layout.contentHeight * _zoom)];
    if (_findBar && !_findBar.hidden) [self updateMatches];
    self.needsDisplay = YES;
}
- (void)setDoc:(const Document&)doc { _doc = doc; [self relayout]; }
- (void)toggleDark { _dark = !_dark; [self savePrefs]; [self relayout]; }
- (void)zoomBy:(double)f { _zoom = std::min(3.0, std::max(0.5, _zoom * f)); [self savePrefs]; [self relayout]; }
- (void)zoomReset { _zoom = 1.0; [self savePrefs]; [self relayout]; }
- (void)viewDidMoveToSuperview {
    // Reflow when the enclosing NSClipView resizes (window resize, scroller
    // show/hide), matching the Windows frontend's WM_SIZE relayout. NSClipView
    // doesn't reliably forward resizeWithOldSuperviewSize: to its document view,
    // so observe the clip view's frame changes directly.
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    [nc removeObserver:self name:NSViewFrameDidChangeNotification object:nil];
    NSView* clip = self.superview; // the enclosing NSClipView
    if (clip) {
        clip.postsFrameChangedNotifications = YES;
        [nc addObserver:self selector:@selector(clipViewFrameChanged:)
                   name:NSViewFrameDidChangeNotification object:clip];
    }
    [self relayout];
}
- (void)clipViewFrameChanged:(NSNotification*)n {
    (void)n;
    if (self.superview && self.superview.bounds.size.width != _laidOutWidth) [self relayout];
    // Keep the floating find bar glued to its top-right anchor on any resize
    // (a height-only change doesn't relayout but still moves the anchor).
    if (_findBar && !_findBar.hidden) [self positionFindBar];
}
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
    std::vector<fmdv::ColoredRect> hl = [self allHighlights];
    fmdv::PaintLayout(ctx, _layout.contentHeight, _layout, [self theme], _tm, &hl);
    CGContextRestoreGState(ctx);
}

// ---- text selection (Windows: drag-select, double/triple click, Ctrl+A/C) ----

// Insertion char index in frag `fi` nearest logical x (document space).
- (long)charInFrag:(long)fi atX:(double)x {
    if (fi < 0 || fi >= (long)_frags.size()) return 0;
    const Frag& f = _frags[fi];
    double xr = x - f.box.x, prev = 0;
    long n = (long)f.text.size();
    for (long ch = 1; ch <= n; ch++) {
        double w = _tm.textWidth(f.font, StrView(f.text.data(), ch));
        if (xr < (prev + w) / 2) return ch - 1;
        prev = w;
    }
    return n;
}
// Map a logical point to (frag, char). Returns false if there are no frags.
- (bool)hitPoint:(NSPoint)p frag:(long*)fi ch:(long*)ch {
    if (_frags.empty()) return false;
    long best = -1; double bestDy = 1e18;
    for (long i = 0; i < (long)_frags.size(); i++) {
        const fmdv::RectF& b = _frags[i].box;
        bool inRow = p.y >= b.y && p.y <= b.y + b.h;
        if (inRow && p.x >= b.x && p.x <= b.x + b.w) { *fi = i; *ch = [self charInFrag:i atX:p.x]; return true; }
        double dy = inRow ? 0 : std::min(std::abs(p.y - b.y), std::abs(p.y - (b.y + b.h)));
        double dx = (p.x < b.x) ? (b.x - p.x) : (p.x > b.x + b.w ? p.x - (b.x + b.w) : 0);
        double d = dy * 1000 + dx; // prefer same row, then nearest x
        if (d < bestDy) { bestDy = d; best = i; }
    }
    *fi = best; *ch = [self charInFrag:best atX:p.x];
    return true;
}
- (void)normSelA:(long*)a aCh:(long*)aCh b:(long*)b bCh:(long*)bCh {
    if (_selA < _selB || (_selA == _selB && _selACh <= _selBCh)) {
        *a = _selA; *aCh = _selACh; *b = _selB; *bCh = _selBCh;
    } else { *a = _selB; *aCh = _selBCh; *b = _selA; *bCh = _selACh; }
}
- (double)xInFrag:(const Frag&)f upto:(long)ch {
    if (ch <= 0) return 0;
    if (ch >= (long)f.text.size()) return f.box.w;
    return _tm.textWidth(f.font, StrView(f.text.data(), ch));
}
- (std::vector<fmdv::RectF>)selectionRects {
    std::vector<fmdv::RectF> out;
    if (!_hasSel) return out;
    long a, aCh, b, bCh; [self normSelA:&a aCh:&aCh b:&b bCh:&bCh];
    for (long i = a; i <= b && i < (long)_frags.size(); i++) {
        const Frag& f = _frags[i];
        double x0 = (i == a) ? [self xInFrag:f upto:aCh] : 0;
        double x1 = (i == b) ? [self xInFrag:f upto:bCh] : f.box.w;
        if (x1 > x0) out.push_back({f.box.x + x0, f.box.y, x1 - x0, f.box.h});
    }
    return out;
}
- (fmdv::RectF)matchRect:(const fmdv::FindMatch&)m {
    const Frag& f = _frags[m.frag];
    double x0 = [self xInFrag:f upto:m.start];
    double x1 = [self xInFrag:f upto:m.start + m.len];
    return {f.box.x + x0, f.box.y, x1 - x0, f.box.h};
}
// Fragment text + geometry for the pure find/selection helpers.
- (std::vector<fmdv::SelFrag>)selFrags {
    std::vector<fmdv::SelFrag> v;
    v.reserve(_frags.size());
    for (const Frag& f : _frags) v.push_back({f.text, f.box.x, f.box.w, f.baseline});
    return v;
}
// All highlights drawn behind text: selection (theme colour) + find matches.
- (std::vector<fmdv::ColoredRect>)allHighlights {
    std::vector<fmdv::ColoredRect> out;
    fmdv::LayoutTheme th = [self theme];
    for (const auto& r : [self selectionRects]) out.push_back({r, th.sel});
    fmdv::Color hit{250, 220, 90}, cur{255, 168, 40};
    for (long i = 0; i < (long)_matches.size(); i++)
        out.push_back({[self matchRect:_matches[i]], (i == _curMatch) ? cur : hit});
    return out;
}
- (NSString*)selectedString {
    if (!_hasSel) return @"";
    long a, aCh, b, bCh; [self normSelA:&a aCh:&aCh b:&b bCh:&bCh];
    return StrToNS(fmdv::SelectionText([self selFrags], a, aCh, b, bCh));
}
- (void)copySelection {
    NSString* s = [self selectedString];
    if (s.length == 0) return;
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:s forType:NSPasteboardTypeString];
}
- (void)selectAll:(id)sender {
    (void)sender;
    if (_frags.empty()) return;
    _selA = 0; _selACh = 0;
    _selB = (long)_frags.size() - 1; _selBCh = (long)_frags.back().text.size();
    _hasSel = true; self.needsDisplay = YES;
}
- (void)clearSelection { if (_hasSel) { _hasSel = false; self.needsDisplay = YES; } }

// Standard responder-chain action so the Edit menu, ⌘C, right-click Copy, and
// Services all copy the preview selection when the preview is first responder.
- (void)copy:(id)sender { (void)sender; [self copySelection]; }
// Enable Copy only with a live selection; Select All whenever there's text.
- (BOOL)validateMenuItem:(NSMenuItem*)mi {
    SEL a = mi.action;
    if (a == @selector(copy:))      return _hasSel;
    if (a == @selector(selectAll:)) return !_frags.empty();
    return YES;
}
// Right-click over a selection offers Copy (matches every native macOS text view).
- (NSMenu*)menuForEvent:(NSEvent*)ev {
    (void)ev;
    if (!_hasSel) return nil;
    NSMenu* m = [[NSMenu alloc] initWithTitle:@""];
    [m addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@""];
    return [m autorelease];
}

- (NSPoint)logicalPoint:(NSEvent*)ev {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    return NSMakePoint(p.x / _zoom, p.y / _zoom);
}
- (void)mouseDown:(NSEvent*)ev {
    // Clicking to select must focus the preview, so the Edit menu / ⌘C route
    // Copy here (the menu sends copy: to the window's first responder).
    [self.window makeFirstResponder:self];
    _dragging = false;
    long fi, ch;
    if (![self hitPoint:[self logicalPoint:ev] frag:&fi ch:&ch]) { [self clearSelection]; return; }
    if (ev.clickCount >= 3) {                 // triple click: select the line
        double base = _frags[fi].baseline; long lo = fi, hi = fi;
        while (lo > 0 && std::abs(_frags[lo - 1].baseline - base) < 1) lo--;
        while (hi + 1 < (long)_frags.size() && std::abs(_frags[hi + 1].baseline - base) < 1) hi++;
        _selA = lo; _selACh = 0; _selB = hi; _selBCh = (long)_frags[hi].text.size(); _hasSel = true;
    } else if (ev.clickCount == 2) {          // double click: word, or a quoted phrase
        // A fragment is a whole run of same-styled words (often the entire line).
        // Inside a double-quoted phrase, select all the words between the quotes;
        // otherwise expand from the hit char out to the surrounding whitespace to
        // get just the word. Triple-click (above) selects the whole line.
        const Str& t = _frags[fi].text;
        long n = (long)t.size();
        long qs, qe;
        if (QuotedSpan(t, ch, qs, qe)) {
            _selA = fi; _selACh = qs; _selB = fi; _selBCh = qe;
        } else {
            auto isWS = [](Char c) { return c == U16(' ') || c == U16('\t'); };
            long ws = ch, we = ch;
            while (ws > 0 && !isWS(t[ws - 1])) ws--;
            while (we < n && !isWS(t[we])) we++;
            _selA = fi; _selACh = ws; _selB = fi; _selBCh = we;
        }
        _hasSel = true;
    } else {
        _selA = _selB = fi; _selACh = _selBCh = ch; _hasSel = true;
    }
    self.needsDisplay = YES;
}
- (void)mouseDragged:(NSEvent*)ev {
    _dragging = true;
    long fi, ch;
    if ([self hitPoint:[self logicalPoint:ev] frag:&fi ch:&ch]) {
        _selB = fi; _selBCh = ch; self.needsDisplay = YES;
    }
}

// ---- find in doc (Windows Ctrl+F): highlight all matches, step, wraparound ----
- (void)positionFindBar {
    NSScrollView* sv = self.enclosingScrollView;
    if (!sv || !_findBar) return;
    _findBar.frame = NSMakeRect(sv.bounds.size.width - 320, sv.bounds.size.height - 44, 300, 36);
}
- (void)showFind {
    NSScrollView* sv = self.enclosingScrollView;
    if (!sv) return;
    if (!_findBar) {
        _findBar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 36)];
        _findBar.wantsLayer = YES;
        _findBar.layer.backgroundColor = [[NSColor windowBackgroundColor] CGColor];
        _findBar.layer.borderColor = [[NSColor separatorColor] CGColor];
        _findBar.layer.borderWidth = 1;
        _findBar.layer.cornerRadius = 6;
        _findBar.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
        _findField = [[NSTextField alloc] initWithFrame:NSMakeRect(8, 7, 202, 22)];
        _findField.placeholderString = @"Find";
        _findField.bezelStyle = NSTextFieldRoundedBezel;
        _findField.delegate = self;
        [_findBar addSubview:_findField];
        _findLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(216, 8, 76, 20)];
        _findLabel.editable = NO; _findLabel.bordered = NO; _findLabel.drawsBackground = NO;
        _findLabel.textColor = [NSColor secondaryLabelColor];
        [_findBar addSubview:_findLabel];
    }
    if (_findBar.superview != sv) [sv addFloatingSubview:_findBar forAxis:NSEventGestureAxisVertical];
    _findBar.hidden = NO;
    [self positionFindBar];
    [self updateMatches];
    [self.window makeFirstResponder:_findField];
}
- (void)closeFind {
    if (_findBar) _findBar.hidden = YES;
    _matches.clear(); _curMatch = -1;
    [self.window makeFirstResponder:self];
    self.needsDisplay = YES;
}
- (bool)findBarVisible { return _findBar && !_findBar.hidden; }
- (NSString*)findLabelText { return _findLabel ? _findLabel.stringValue : @""; }
- (void)updateFindLabel {
    if (!_findLabel) return;
    if (!_matches.empty())
        _findLabel.stringValue = [NSString stringWithFormat:@"%ld/%zu", _curMatch + 1, _matches.size()];
    else
        _findLabel.stringValue = _findField.stringValue.length ? @"0/0" : @"";
}
- (void)scrollToCurrent {
    if (_curMatch < 0 || _curMatch >= (long)_matches.size()) return;
    fmdv::RectF r = [self matchRect:_matches[_curMatch]];
    [self scrollRectToVisible:NSMakeRect(r.x * _zoom, (r.y - 60) * _zoom, r.w * _zoom, (r.h + 120) * _zoom)];
}
- (void)updateMatches {
    _matches = fmdv::FindMatches([self selFrags], NSStringToStr(_findField.stringValue));
    _curMatch = _matches.empty() ? -1 : 0;
    if (_curMatch == 0) [self scrollToCurrent];
    [self updateFindLabel];
    self.needsDisplay = YES;
}
- (void)stepFind:(int)dir {
    if (_matches.empty()) return;
    _curMatch = (_curMatch + dir + (long)_matches.size()) % (long)_matches.size();
    [self scrollToCurrent];
    [self updateFindLabel];
    self.needsDisplay = YES;
}
- (void)controlTextDidChange:(NSNotification*)n { (void)n; [self updateMatches]; }
- (BOOL)control:(NSControl*)ctl textView:(NSTextView*)tv doCommandBySelector:(SEL)sel {
    (void)ctl; (void)tv;
    if (sel == @selector(insertNewline:)) {
        BOOL shift = ([NSApp currentEvent].modifierFlags & NSEventModifierFlagShift) != 0;
        [self stepFind:shift ? -1 : 1];
        return YES;
    }
    if (sel == @selector(cancelOperation:)) { [self closeFind]; return YES; }
    return NO;
}

- (BOOL)performKeyEquivalent:(NSEvent*)ev {
    if (ev.modifierFlags & NSEventModifierFlagCommand) {
        NSString* c = ev.charactersIgnoringModifiers;
        if ([c isEqualToString:@"d"]) { [self toggleDark]; return YES; }
        if ([c isEqualToString:@"="] || [c isEqualToString:@"+"]) { [self zoomBy:1.1]; return YES; }
        if ([c isEqualToString:@"-"]) { [self zoomBy:1.0 / 1.1]; return YES; }
        if ([c isEqualToString:@"0"]) { [self zoomReset]; return YES; }
        if ([c isEqualToString:@"f"]) { [self showFind]; return YES; }
        // ⌘C (Copy) and ⌘A (Select All) are handled by the Edit menu, which
        // routes copy:/selectAll: through the responder chain to whichever view
        // is focused (this preview, or the source editor's text view).
    }
    return [super performKeyEquivalent:ev];
}
// Ctrl+scroll zooms on Windows; Cmd+scroll is the macOS equivalent.
- (void)scrollWheel:(NSEvent*)ev {
    if (ev.modifierFlags & NSEventModifierFlagCommand) {
        double d = ev.scrollingDeltaY;
        if (d != 0) [self zoomBy:(d > 0 ? 1.06 : 1.0 / 1.06)];
        return;
    }
    [super scrollWheel:ev];
}

- (void)scrollByPoints:(double)dy {
    NSScrollView* sv = self.enclosingScrollView;
    if (!sv) return;
    NSClipView* clip = sv.contentView;
    NSPoint o = clip.bounds.origin;                     // flipped doc view: o.y = offset from top
    double maxY = self.bounds.size.height - clip.bounds.size.height;
    if (maxY < 0) maxY = 0;
    o.y += dy;
    if (o.y < 0) o.y = 0; else if (o.y > maxY) o.y = maxY;
    [clip scrollToPoint:o];
    [sv reflectScrolledClipView:clip];
}
- (std::vector<fmdv::HeadingRef>)headings { return _layout.headings; }
- (NSString*)laidOutInfo {
    return [NSString stringWithFormat:@"%d %d", (int)std::llround(_laidOutWidth),
                                               (int)std::llround(_layout.contentHeight)];
}
- (void)scrollToDocY:(double)docY {
    NSScrollView* sv = self.enclosingScrollView;
    if (!sv) return;
    NSClipView* clip = sv.contentView;
    NSPoint o = clip.bounds.origin;
    o.y = docY * _zoom - 20;
    double maxY = self.bounds.size.height - clip.bounds.size.height;
    if (maxY < 0) maxY = 0;
    if (o.y < 0) o.y = 0; else if (o.y > maxY) o.y = maxY;
    [clip scrollToPoint:o];
    [sv reflectScrolledClipView:clip];
}

// Keyboard scrolling parity with Windows: arrows, PgUp/PgDn, Home/End, Space.
- (void)keyDown:(NSEvent*)ev {
    NSString* c = ev.charactersIgnoringModifiers;
    unichar k = c.length ? [c characterAtIndex:0] : 0;
    double page = self.enclosingScrollView.contentView.bounds.size.height - 40;
    switch (k) {
        case NSUpArrowFunctionKey:   [self scrollByPoints:-60];    return;
        case NSDownArrowFunctionKey: [self scrollByPoints:60];     return;
        case NSPageUpFunctionKey:    [self scrollByPoints:-page];  return;
        case NSPageDownFunctionKey:  [self scrollByPoints:page];   return;
        case NSHomeFunctionKey:      [self scrollByPoints:-1e9];   return;
        case NSEndFunctionKey:       [self scrollByPoints:1e9];    return;
        case ' ':                    [self scrollByPoints:page];   return;
    }
    [super keyDown:ev];
}

// Open a link target. Absolute URLs (http:, mailto:, ...) go straight to the
// workspace. A scheme-less href (docs/guide.md, /tmp/x.md) used to be fed to
// URLWithString:, which yields a schemeless NSURL that openURL: silently
// drops — treat those as file paths, resolved against the open document's
// directory (Windows gets the same case for free via ShellExecute).
- (void)openLink:(NSString*)href {
    if (href.length == 0) return;
    NSURL* url = [NSURL URLWithString:href];
    if (url && url.scheme.length) { [[NSWorkspace sharedWorkspace] openURL:url]; return; }
    NSString* path = href;
    NSRange hash = [path rangeOfString:@"#"];
    if (hash.location != NSNotFound) path = [path substringToIndex:hash.location];
    if (path.length == 0) return; // pure in-page anchor: no jump support yet
    path = [path stringByRemovingPercentEncoding] ?: path;
    if (!path.absolutePath && self.baseDir.length)
        path = [self.baseDir stringByAppendingPathComponent:path];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path])
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}

- (void)mouseUp:(NSEvent*)ev {
    if (_dragging) { _dragging = false; return; } // a drag made a selection; keep it
    // A plain click: follow a link if one was hit, and drop any selection.
    NSPoint p = [self logicalPoint:ev];
    for (const auto& lk : _layout.links) {
        const auto& r = lk.rect;
        if (p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h) {
            [self clearSelection];
            [self openLink:StrToNS(lk.href)];
            return;
        }
    }
    if (ev.clickCount == 1) [self clearSelection];
}
- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [super dealloc];
}
@end

// ---------------- source editor (list continuation via core/edit_assist) ----

@interface FMDVTextView : NSTextView
- (NSString*)ghostText; // test-driver introspection (--test-drive)
@end

@implementation FMDVTextView {
    NSString* _ghost;       // autocomplete overlay (not in the text buffer); nil = none
    NSInteger _ghostCaret;  // caret offset within _ghost after Tab-commit
}
- (void)dealloc { [_ghost release]; [super dealloc]; }
- (NSString*)ghostText { return _ghost ?: @""; }

// ---- markdown autocomplete ghost text (Windows: gray overlay, Tab commits) ----
- (void)setGhost:(NSString*)g caret:(NSInteger)c {
    if (g != _ghost) { [_ghost release]; _ghost = [g retain]; }
    _ghostCaret = c;
    self.needsDisplay = YES;
}
- (void)updateGhost {
    NSRange sel = self.selectedRange;
    NSString* g = nil; NSInteger gc = 0;
    if (sel.length == 0) {
        NSString* s = self.string;
        NSUInteger caret = sel.location;
        BOOL atLineEnd = (caret >= s.length) || [s characterAtIndex:caret] == '\n';
        if (atLineEnd) {
            NSUInteger ls = caret;
            while (ls > 0 && [s characterAtIndex:ls - 1] != '\n') ls--;
            fmdv::Suggestion sg = fmdv::SuggestClose(NSStringToStr([s substringWithRange:NSMakeRange(ls, caret - ls)]));
            if (!sg.text.empty()) { g = StrToNS(sg.text); gc = sg.caret; }
        }
    }
    [self setGhost:g caret:gc];
}
- (void)setSelectedRanges:(NSArray<NSValue*>*)ranges affinity:(NSSelectionAffinity)aff stillSelecting:(BOOL)flag {
    [super setSelectedRanges:ranges affinity:aff stillSelecting:flag];
    [self updateGhost]; // fires on every caret move / edit
}
- (void)drawRect:(NSRect)r {
    [super drawRect:r];
    if (_ghost.length == 0) return;
    NSUInteger caret = self.selectedRange.location;
    NSRect scr = [self firstRectForCharacterRange:NSMakeRange(caret, 0) actualRange:NULL];
    if (scr.size.height == 0) return;
    // firstRect is screen coords (y-up); take the caret line's TOP-LEFT and map
    // into this flipped view so drawAtPoint (top-left) lands inline with the caret.
    NSPoint topLeft = [self convertPoint:[self.window convertPointFromScreen:NSMakePoint(NSMinX(scr), NSMaxY(scr))]
                                fromView:nil];
    NSDictionary* attrs = @{ NSFontAttributeName: (self.font ?: [NSFont userFixedPitchFontOfSize:13]),
                             NSForegroundColorAttributeName: [NSColor tertiaryLabelColor] };
    [_ghost drawAtPoint:topLeft withAttributes:attrs];
}
- (void)insertTab:(id)sender {
    if (_ghost.length == 0) { [super insertTab:sender]; return; }
    NSString* g = [[_ghost retain] autorelease];
    NSInteger gc = _ghostCaret;
    [self setGhost:nil caret:0];
    NSRange sel = self.selectedRange;
    if ([self shouldChangeTextInRange:sel replacementString:g]) {
        [self replaceCharactersInRange:sel withString:g];
        [self didChangeText];
        [self setSelectedRange:NSMakeRange(sel.location + gc, 0)];
    }
}
- (void)cancelOperation:(id)sender {
    if (_ghost.length) { [self setGhost:nil caret:0]; return; }
    [super cancelOperation:sender];
}

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

// ---------------- table-of-contents sidebar (Windows Ctrl+Shift+O) ----------------

@interface FMDVTocView : NSView
@property (nonatomic, copy) void (^onPick)(double docY);
- (void)setHeadings:(const std::vector<fmdv::HeadingRef>&)h dark:(bool)dark;
@end

@implementation FMDVTocView {
    std::vector<fmdv::HeadingRef> _items;
    bool _dark;
}
static const double TOC_ROW = 26, TOC_TOP = 12, TOC_PADX = 14;
- (BOOL)isFlipped { return YES; }
- (void)setHeadings:(const std::vector<fmdv::HeadingRef>&)h dark:(bool)dark {
    _items = h; _dark = dark; self.needsDisplay = YES;
}
- (void)drawRect:(NSRect)r {
    (void)r;
    NSColor* bg = _dark ? [NSColor colorWithSRGBRed:0x16/255.0 green:0x1b/255.0 blue:0x22/255.0 alpha:1]
                        : [NSColor colorWithSRGBRed:0xf6/255.0 green:0xf8/255.0 blue:0xfa/255.0 alpha:1];
    [bg set]; NSRectFill(self.bounds);
    NSColor* border = _dark ? [NSColor colorWithWhite:1 alpha:0.10] : [NSColor colorWithWhite:0 alpha:0.10];
    [border set]; NSRectFill(NSMakeRect(self.bounds.size.width - 1, 0, 1, self.bounds.size.height));
    NSColor* fg = _dark ? [NSColor colorWithWhite:0.90 alpha:1]
                        : [NSColor colorWithSRGBRed:0x24/255.0 green:0x29/255.0 blue:0x2f/255.0 alpha:1];
    for (long i = 0; i < (long)_items.size(); i++) {
        const auto& it = _items[i];
        double fs = it.level <= 1 ? 13 : (it.level == 2 ? 12 : 11);
        NSFont* font = [NSFont systemFontOfSize:fs
                                         weight:(it.level <= 2 ? NSFontWeightSemibold : NSFontWeightRegular)];
        NSColor* c = it.level <= 2 ? fg : [fg colorWithAlphaComponent:0.72];
        NSDictionary* attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: c};
        double indent = TOC_PADX + (it.level - 1) * 12;
        NSString* s = StrToNS(it.text);
        NSRect box = NSMakeRect(indent, TOC_TOP + i * TOC_ROW + 4, self.bounds.size.width - indent - 6, TOC_ROW);
        [s drawWithRect:box options:NSStringDrawingUsesLineFragmentOrigin attributes:attrs];
    }
}
- (void)mouseDown:(NSEvent*)ev {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    long i = (long)std::floor((p.y - TOC_TOP) / TOC_ROW);
    if (i >= 0 && i < (long)_items.size() && self.onPick) self.onPick(_items[i].y);
}
@end

// ---------------- update picker (Windows Ctrl+U popup) ----------------

// Release list inside the updates panel. Same interaction model as the Win32
// picker: ↑/↓ select, Enter (or double-click) installs, 'A' toggles
// auto-update, Esc closes. Rows mark latest / current / "no app" (release
// without a FMDV-macos.zip asset — the macOS analog of "no exe").
@interface FMDVUpdateListView : NSView
@property (nonatomic, copy) void (^onInstall)(long row);
@property (nonatomic, copy) void (^onToggleAuto)(void);
@property (nonatomic, copy) void (^onClose)(void);
- (void)setReleases:(const std::vector<ReleaseInfo>&)rel fetched:(bool)fetched failed:(bool)failed
            current:(const Str&)cur modeLine:(NSString*)modeLine status:(NSString*)status;
- (double)desiredHeight;
@end

@implementation FMDVUpdateListView {
    std::vector<ReleaseInfo> _rel;
    bool _fetched, _failed;
    Str _cur;
    NSString* _modeLine;
    NSString* _status;   // install progress/result line ("" = hidden)
    long _sel;
}
static const double UP_PAD = 12, UP_ROW = 24;
- (void)dealloc { [_modeLine release]; [_status release]; [super dealloc]; }
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (long)rows { return (long)_rel.size() < 8 ? (long)_rel.size() : 8; } // Windows: UpRows()
- (void)setReleases:(const std::vector<ReleaseInfo>&)rel fetched:(bool)fetched failed:(bool)failed
            current:(const Str&)cur modeLine:(NSString*)modeLine status:(NSString*)status {
    _rel = rel; _fetched = fetched; _failed = failed; _cur = cur;
    if (modeLine != _modeLine) { [_modeLine release]; _modeLine = [modeLine copy]; }
    if (status != _status) { [_status release]; _status = [status copy]; }
    if (_sel >= [self rows]) _sel = [self rows] ? [self rows] - 1 : 0;
    self.needsDisplay = YES;
}
- (double)desiredHeight {
    long body = [self rows] ? [self rows] : 1;               // list or one message line
    return UP_PAD * 2 + UP_ROW * (2 + body) + (_status.length ? UP_ROW : 0);
}
- (long)newestInstallable {
    long best = -1;
    for (long i = 0; i < (long)_rel.size(); i++) {
        if (_rel[i].macUrl.empty()) continue;
        if (best < 0 || CompareVersions(_rel[i].tag, _rel[best].tag) > 0) best = i;
    }
    return best;
}
- (void)drawRect:(NSRect)r {
    (void)r;
    [[NSColor controlBackgroundColor] set]; NSRectFill(self.bounds);
    NSColor* fg = [NSColor labelColor];
    NSColor* dim = [NSColor secondaryLabelColor];
    NSDictionary* head = @{NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
                           NSForegroundColorAttributeName: fg};
    NSDictionary* body = @{NSFontAttributeName: [NSFont systemFontOfSize:13], NSForegroundColorAttributeName: fg};
    NSDictionary* mut  = @{NSFontAttributeName: [NSFont systemFontOfSize:13], NSForegroundColorAttributeName: dim};
    double y = UP_PAD;
    [[NSString stringWithFormat:@"FMDV %s — releases", FMDV_VERSION_STR]
        drawAtPoint:NSMakePoint(UP_PAD, y) withAttributes:head]; y += UP_ROW;
    [(_modeLine ?: @"") drawAtPoint:NSMakePoint(UP_PAD, y) withAttributes:mut]; y += UP_ROW;

    if (!_fetched) {
        [(_failed ? @"couldn't reach GitHub" : @"checking…") drawAtPoint:NSMakePoint(UP_PAD, y) withAttributes:mut];
        y += UP_ROW;
    } else if (_rel.empty()) {
        [@"no releases found" drawAtPoint:NSMakePoint(UP_PAD, y) withAttributes:mut];
        y += UP_ROW;
    } else {
        long newest = [self newestInstallable];
        for (long i = 0; i < [self rows]; i++, y += UP_ROW) {
            if (i == _sel) {
                [[NSColor selectedContentBackgroundColor] set];
                NSRectFill(NSMakeRect(UP_PAD / 2, y - 2, self.bounds.size.width - UP_PAD, UP_ROW - 4));
            }
            const ReleaseInfo& rel = _rel[i];
            NSMutableString* line = [NSMutableString stringWithString:StrToNS(rel.tag)];
            if (i == newest) [line appendString:@"  · latest"];
            if (CompareVersions(rel.tag, _cur) == 0) [line appendString:@"  · current"];
            if (rel.macUrl.empty()) [line appendString:@"  · no app"];
            NSDictionary* attrs = rel.macUrl.empty() ? mut
                : (i == _sel ? @{NSFontAttributeName: [NSFont systemFontOfSize:13],
                                 NSForegroundColorAttributeName: [NSColor alternateSelectedControlTextColor]} : body);
            [line drawAtPoint:NSMakePoint(UP_PAD, y) withAttributes:attrs];
        }
    }
    if (_status.length) [_status drawAtPoint:NSMakePoint(UP_PAD, y) withAttributes:mut];
}
- (void)keyDown:(NSEvent*)ev {
    NSString* ch = ev.charactersIgnoringModifiers;
    switch (ch.length ? [ch characterAtIndex:0] : 0) {
        case NSDownArrowFunctionKey: if (_sel + 1 < [self rows]) _sel++; break;
        case NSUpArrowFunctionKey:   if (_sel > 0) _sel--; break;
        case NSCarriageReturnCharacter: if (self.onInstall) self.onInstall(_sel); return;
        case 'a': case 'A': if (self.onToggleAuto) self.onToggleAuto(); return;
        case 0x1B: if (self.onClose) self.onClose(); return; // Esc
        default: [super keyDown:ev]; return;
    }
    self.needsDisplay = YES;
}
- (long)rowAtPoint:(NSPoint)p {
    long i = (long)std::floor((p.y - (UP_PAD + 2 * UP_ROW)) / UP_ROW);
    return (i >= 0 && i < [self rows]) ? i : -1;
}
- (void)mouseDown:(NSEvent*)ev {
    long i = [self rowAtPoint:[self convertPoint:ev.locationInWindow fromView:nil]];
    if (i < 0) return;
    _sel = i; self.needsDisplay = YES;
    if (ev.clickCount >= 2 && self.onInstall) self.onInstall(_sel);
}
@end

// ---------------- window controller / app delegate ----------------

@interface FMDVAppDelegate : NSObject <NSApplicationDelegate, NSTextViewDelegate, NSSplitViewDelegate> {
    NSWindow* _window;
    NSView* _container;
    FMDVPreviewView* _preview;
    NSScrollView* _previewScroll;
    FMDVTextView* _textView;
    NSSplitView* _split;
    FMDVTocView* _tocView;
    bool _tocVisible;
    std::string _file;
    bool _editing;
    bool _dark;
    bool _opened;
    NSTimeInterval _fileMtime;  // last-seen mtime of _file (live reload)
    NSTimer* _watchTimer;       // 500ms file-change poll
    NSView* _updateBanner;      // passive "update available" strip (notify mode)
    NSTextField* _updateLabel;  // banner text
    NSButton* _bannerButton;    // banner action ("View Releases…" / "Relaunch")
    NSPanel* _updPanel;         // Cmd+U release picker (Windows Ctrl+U popup)
    FMDVUpdateListView* _updList;
    std::vector<ReleaseInfo> _updReleases; // picker rows (main thread only)
    bool _updFetched, _updFailed, _updFetchRunning;
    bool _updAutoInstallOnCheck; // pending check should auto-install (auto mode launch check)
    bool _installRunning;       // one install at a time (Windows g_installRunning)
    Str _installTag;            // tag being installed (status/banner text)
}
- (instancetype)initWithFile:(const char*)file dark:(bool)dark;
- (void)openPath:(NSString*)path;
@end

static void StartTestDriver(FMDVAppDelegate* delegate); // --test-drive stdin loop (defined below)

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
    _container = [[NSView alloc] initWithFrame:frame];
    _previewScroll = [[NSScrollView alloc] initWithFrame:frame];
    [_previewScroll setHasVerticalScroller:YES];
    [_previewScroll setAutohidesScrollers:YES];
    [_previewScroll setDrawsBackground:NO];
    _preview = [[FMDVPreviewView alloc] initWithDoc:Document() dark:_dark];
    [_previewScroll setDocumentView:_preview];
    [_container addSubview:_previewScroll];
    [_window setContentView:_container];
    [self layoutPanes];
    // Keep the floating update banner glued to its bottom anchor on resize; like
    // the Windows frontend, which repaints the strip from the live client size
    // every WM_PAINT, it must track the window instead of drifting.
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(windowResized:)
                                                 name:NSWindowDidResizeNotification object:_window];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_preview];
}
- (void)windowResized:(NSNotification*)n {
    (void)n;
    if (_updateBanner && !_updateBanner.hidden) [self positionUpdateBanner];
}

// Position the optional TOC sidebar (left) and the main pane (preview or editor
// split) that fills the rest; autoresizing then tracks window resizes.
- (void)layoutPanes {
    if (!_container) return;
    NSRect b = _container.bounds;
    double tocW = (_tocVisible && _tocView) ? 240 : 0;
    if (_tocView) {
        _tocView.hidden = !_tocVisible;
        _tocView.frame = NSMakeRect(0, 0, tocW, b.size.height);
        _tocView.autoresizingMask = NSViewHeightSizable | NSViewMaxXMargin;
    }
    NSView* main = _editing ? (NSView*)_split : (NSView*)_previewScroll;
    main.frame = NSMakeRect(tocW, 0, b.size.width - tocW, b.size.height);
    main.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
}

- (void)refreshToc {
    if (_tocVisible && _tocView) { auto h = [_preview headings]; [_tocView setHeadings:h dark:[_preview dark]]; }
}

- (void)openPath:(NSString*)path {
    [self ensureWindow];
    _opened = true;
    _file = path.UTF8String ? path.UTF8String : "";
    _preview.baseDir = path.stringByDeletingLastPathComponent; // relative links resolve here
    Document doc = ParseMarkdown(LoadDoc(ReadFileUtf8(_file.c_str())));
    [_preview setDoc:doc];
    if (_textView) [_textView setString:StrToNS(LoadDoc(ReadFileUtf8(_file.c_str())))];
    [_window setTitle:path.lastPathComponent];
    [self refreshToc];
    _fileMtime = FileModTime(_file);
    [self startWatching];
}

// Live reload (Windows: 500ms WM_TIMER polling the file's mtime). Reparse into
// the preview when the file changes on disk. Skip while the split editor is
// open so an external change never clobbers in-progress edits (matches Win32).
- (void)startWatching {
    if (_watchTimer) return;
    _watchTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self
                     selector:@selector(pollFileChange:) userInfo:nil repeats:YES];
}
- (void)pollFileChange:(NSTimer*)t {
    (void)t;
    if (_editing || _file.empty()) return;
    NSTimeInterval m = FileModTime(_file);
    if (m == 0 || m == _fileMtime) return;
    _fileMtime = m;
    Document doc = ParseMarkdown(LoadDoc(ReadFileUtf8(_file.c_str())));
    [_preview setDoc:doc];
    [self refreshToc];
}

- (BOOL)application:(NSApplication*)app openFile:(NSString*)filename {
    (void)app;
    // AppKit also routes stray command-line words here (e.g. the value of an
    // NSUserDefaults argument-domain pair like `-FMDVDark 1`); don't let a
    // non-file clobber the document that's already open.
    if (![[NSFileManager defaultManager] isReadableFileAtPath:filename]) return NO;
    [self openPath:filename];
    return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    (void)n;
    [NSApp activateIgnoringOtherApps:YES];
    if (getenv("FMDV_TEST_DRIVE")) StartTestDriver(self);
    // Passive update check runs after first paint (Windows: async, post-paint);
    // same for sweeping up a previous update's leftover <bundle>.old.
    __unsafe_unretained FMDVAppDelegate* weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [weak cleanupOldBundle]; [weak maybeCheckUpdatesOnLaunch]; });
    if (_opened) return;                         // openFile: already loaded a document
    if (!_file.empty()) {
        NSString* f = [NSString stringWithUTF8String:_file.c_str()];
        if ([[NSFileManager defaultManager] isReadableFileAtPath:f]) { [self openPath:f]; return; }
        // an unreadable path argument would show a blank window; fall through to the panel
    }
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
    [self refreshToc];
}
- (void)textDidChange:(NSNotification*)n { (void)n; [self reparseFromEditor]; }
- (void)scrollPreviewToDocY:(double)y { [_preview scrollToDocY:y]; }

- (void)toggleEditor:(id)sender {
    (void)sender;
    [self ensureWindow];
    if (!_editing) {
        NSRect b = _container.bounds;
        if (!_textView) {
            NSScrollView* tvScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, b.size.width / 2, b.size.height)];
            [tvScroll setHasVerticalScroller:YES];
            _textView = [[FMDVTextView alloc] initWithFrame:tvScroll.bounds];
            [_textView setFont:[NSFont userFixedPitchFontOfSize:13]];
            [_textView setAutomaticQuoteSubstitutionEnabled:NO];
            [_textView setAutomaticDashSubstitutionEnabled:NO];
            [_textView setDelegate:self];
            [tvScroll setDocumentView:_textView];
            _split = [[NSSplitView alloc] initWithFrame:b];
            [_split setVertical:YES];
            [_split setDividerStyle:NSSplitViewDividerStyleThin];
            [_split setDelegate:self];      // persist the divider ratio
            [_split addSubview:tvScroll];  // preview pane is reparented in below
        }
        [_textView setString:StrToNS(LoadDoc(ReadFileUtf8(_file.c_str())))];
        [_previewScroll removeFromSuperview];
        [_split addSubview:_previewScroll];   // move preview into the split's right pane
        [_container addSubview:_split];
        _editing = true;
        [self layoutPanes];
        [self restoreSplitPosition];
        [_window makeFirstResponder:_textView];
        [self reparseFromEditor];
    } else {
        [_previewScroll removeFromSuperview];  // pull preview back out of the split
        [_split removeFromSuperview];
        [_container addSubview:_previewScroll];
        _editing = false;
        [self layoutPanes];
        [_window makeFirstResponder:_preview];
    }
}

// Editor split divider: restore the saved ratio when opening, persist on drag.
- (void)restoreSplitPosition {
    if (!_split) return;
    NSInteger pct = [[NSUserDefaults standardUserDefaults] integerForKey:kPrefSplitPct];
    if (pct <= 0 || pct >= 100) pct = 50;
    double total = _split.bounds.size.width;
    if (total > 1) [_split setPosition:total * pct / 100.0 ofDividerAtIndex:0];
}
- (void)splitViewDidResizeSubviews:(NSNotification*)n {
    (void)n;
    if (!_split || _split.subviews.count < 2) return;
    double total = _split.bounds.size.width;
    if (total < 1) return;
    NSInteger pct = (NSInteger)std::llround(((NSView*)_split.subviews[0]).frame.size.width / total * 100);
    if (pct > 0 && pct < 100)
        [[NSUserDefaults standardUserDefaults] setInteger:pct forKey:kPrefSplitPct];
}

- (void)toggleContents:(id)sender {
    (void)sender;
    [self ensureWindow];
    if (!_tocView) {
        _tocView = [[FMDVTocView alloc] initWithFrame:NSMakeRect(0, 0, 240, _container.bounds.size.height)];
        __unsafe_unretained FMDVAppDelegate* weak = self; // delegate outlives the view (MRC)
        _tocView.onPick = ^(double y) { [weak scrollPreviewToDocY:y]; };
        [_container addSubview:_tocView];
    }
    _tocVisible = !_tocVisible;
    [self refreshToc];
    [self layoutPanes];
}

// Write the editor buffer to disk; returns NO on any write error so callers can
// alert and keep the editor open. Assumes there's something to save (editor open
// with a file). Writes to a temp file and renames into place so a crash or full
// disk mid-write can't truncate the document.
- (BOOL)writeDocToDisk {
    std::string text = _textView.string.UTF8String ?: ""; // NSTextView already uses LF
    std::string tmp = _file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return NO;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (std::fclose(f) == 0) && ok;
    if (ok) ok = (std::rename(tmp.c_str(), _file.c_str()) == 0);
    if (!ok) { std::remove(tmp.c_str()); return NO; }
    _fileMtime = FileModTime(_file); // our own save shouldn't trigger a live reload
    return YES;
}
// A failed save must be visible and non-destructive: the edits stay in the open
// editor rather than being silently lost when a reopen reloads from disk.
- (void)reportSaveFailure {
    NSBeep();
    if (getenv("FMDV_TEST_DRIVE")) return; // headless suite: no modal to block on
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"Couldn’t save the file";
    a.informativeText = @"FMDV couldn’t write the file. Check its permissions and "
                        @"free disk space; your edits are still open in the editor.";
    [a addButtonWithTitle:@"OK"];
    [a runModal];
    [a release];
}
- (void)saveDoc:(id)sender {
    (void)sender;
    // Only the editor has anything to save; a Cmd+S in preview mode used to
    // rewrite the file from its own (CR-stripped) contents, silently
    // normalizing it on disk. Matches the Win32 guard (ID_SAVE: if editing).
    if (!_editing || _file.empty()) return;
    if (![self writeDocToDisk]) [self reportSaveFailure];
}
- (void)saveAndClose:(id)sender {
    (void)sender;
    if (!_editing || _file.empty()) return;
    // Close only on success (Windows Ctrl+Shift+S); a failed save keeps the
    // editor open so the unsaved edits aren't lost to a reload-from-disk.
    if ([self writeDocToDisk]) [self toggleEditor:sender];
    else [self reportSaveFailure];
}
- (void)insertTable:(id)sender { (void)sender; if (_editing && _textView) [_textView insertTableMarkdown]; }

// Updates (Windows Ctrl+U): release picker + in-app install. Releases carry a
// FMDV-macos.zip asset (CI `make dist`); installing downloads it, unzips, and
// swaps the running .app bundle the way the Win32 updater swaps fmdv.exe.
// Modes mirror Windows prefs: notify (default, passive banner), auto (install
// newest on the launch check), pin (hold a tag; no launch check).
- (NSString*)currentVersion {
    const char* o = getenv("FMDV_VERSION_OVERRIDE"); // test hook (Windows parity)
    if (o && o[0]) return [NSString stringWithUTF8String:o];
    NSString* v = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return v.length ? v : @FMDV_VERSION_STR; // raw fmdv-macos binary: no bundle plist
}
- (NSURLRequest*)releasesRequest {
    const char* o = getenv("FMDV_RELEASES_URL"); // test hook: point at a local fixture server
    NSString* u = (o && o[0]) ? [NSString stringWithUTF8String:o]
        : @"https://api.github.com/repos/OrangeBannana/FMDV/releases?per_page=30";
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:u]];
    [req setValue:[@"fmdv-macos/" stringByAppendingString:[self currentVersion]] forHTTPHeaderField:@"User-Agent"];
    req.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    return req;
}

// ---- update mode (NSUserDefaults; Windows: updateMode/pinTag in prefs.txt) ----
- (NSString*)updateMode {
    NSString* m = [[NSUserDefaults standardUserDefaults] stringForKey:kPrefUpdateMode];
    return ([m isEqualToString:@"auto"] || [m isEqualToString:@"pin"]) ? m : @"notify";
}
- (void)setUpdateMode:(NSString*)mode pin:(NSString*)tag {
    NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
    [ud setObject:mode forKey:kPrefUpdateMode];
    if (tag.length) [ud setObject:tag forKey:kPrefPinTag];
    else [ud removeObjectForKey:kPrefPinTag];
}

// Newest release with a macOS asset (API order isn't trusted; Windows parity).
- (const ReleaseInfo*)newestInstallable {
    const ReleaseInfo* best = nullptr;
    for (const auto& r : _updReleases) {
        if (r.macUrl.empty()) continue;
        if (!best || CompareVersions(r.tag, best->tag) > 0) best = &r;
    }
    return best;
}

// The bundle an install may swap: the running .app, when its parent directory
// is writable. A raw fmdv-macos binary (make macos) has no bundle, so the
// picker degrades to opening the releases page.
- (NSString*)installableBundlePath {
    NSString* p = [NSBundle mainBundle].bundlePath;
    if (![p.pathExtension isEqualToString:@"app"]) return nil;
    if (![[NSFileManager defaultManager] isWritableFileAtPath:p.stringByDeletingLastPathComponent]) return nil;
    return p;
}

// ---- fetch (one in flight; decision runs on the main thread when it lands,
// mirroring the Win32 CheckThread → UPD_CHECK_DONE split) ----
- (void)fetchReleases {
    if (_updFetchRunning) return;
    _updFetchRunning = true;
    __unsafe_unretained FMDVAppDelegate* weak = self;
    NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:[self releasesRequest]
        completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
            (void)resp; (void)err;
            NSData* d = [data retain];
            dispatch_async(dispatch_get_main_queue(), ^{ [weak releasesArrived:d]; [d release]; });
        }];
    [task resume];
}
- (void)releasesArrived:(NSData*)data {
    _updFetchRunning = false;
    std::string json;
    if (data.length) json.assign((const char*)data.bytes, data.length);
    std::vector<ReleaseInfo> rel;
    bool ok = !json.empty() && ParseReleasesJson(json, rel);
    _updReleases = rel;
    _updFetched = ok;
    _updFailed = !ok;
    bool autoInstall = _updAutoInstallOnCheck;
    _updAutoInstallOnCheck = false;
    [self refreshUpdatePicker];
    if (!ok) return;

    Str cur = NSStringToStr([self currentVersion]);
    const ReleaseInfo* nw = [self newestInstallable];
    bool newer = nw && CompareVersions(nw->tag, cur) > 0;
    if (autoInstall) {                       // auto mode's launch check
        if (newer) [self startInstall:*nw];
        return;
    }
    if (newer && [[self updateMode] isEqualToString:@"notify"] && !_installRunning) {
        [self showBanner:[NSString stringWithFormat:@"FMDV %@ is available", StrToNS(nw->tag)]
             buttonTitle:@"Update" action:@selector(installNewest:)];
    } else if (!nw && [[self updateMode] isEqualToString:@"notify"]) {
        // no release carries a macOS asset yet: keep the old check-and-link
        // banner pointing at the releases page for any newer tag
        const ReleaseInfo* any = nullptr;
        for (const auto& r : _updReleases) if (!any || CompareVersions(r.tag, any->tag) > 0) any = &r;
        if (any && CompareVersions(any->tag, cur) > 0)
            [self showBanner:[NSString stringWithFormat:@"FMDV %@ is available", StrToNS(any->tag)]
                 buttonTitle:@"View Releases…" action:@selector(openReleasesPage:)];
    }
}

// ---- install: download zip → unzip → swap the bundle ----

// Run a system tool to completion; true on exit 0.
static bool RunTool(NSString* path, NSArray<NSString*>* args) {
    NSTask* t = [[NSTask alloc] init];
    t.launchPath = path;
    t.arguments = args;
    t.standardOutput = [NSPipe pipe];
    t.standardError = [NSPipe pipe];
    bool ok = false;
    @try { [t launch]; [t waitUntilExit]; ok = (t.terminationStatus == 0); }
    @catch (NSException* e) { (void)e; }
    [t release];
    return ok;
}

// Unzip the downloaded FMDV-macos.zip and swap it in place of the running
// bundle. Mirrors the Win32 exe swap: live bundle → <bundle>.old (renaming a
// running image is fine on macOS; open files follow the inode), the download
// moves into the live path, and a failed move rolls back. Runs off the main
// thread; pure file/subprocess work.
static bool SwapInDownloadedBundle(NSString* zipPath, NSString* bundlePath) {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:
                     [NSString stringWithFormat:@"fmdv-update-%@", [NSUUID UUID].UUIDString]];
    if (![fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil]) return false;
    NSString* newApp = [tmp stringByAppendingPathComponent:@"FMDV.app"];
    NSString* old = [bundlePath stringByAppendingString:@".old"];
    bool ok = false;
    do {
        if (!RunTool(@"/usr/bin/ditto", @[@"-x", @"-k", zipPath, tmp])) break;
        // sanity: our app, with a runnable binary — not an error page (Windows: MZ check)
        NSDictionary* plist = [NSDictionary dictionaryWithContentsOfFile:
                               [newApp stringByAppendingPathComponent:@"Contents/Info.plist"]];
        if (![plist[@"CFBundleIdentifier"] isEqual:@"com.orangebannana.fmdv"]) break;
        if (![fm isExecutableFileAtPath:[newApp stringByAppendingPathComponent:@"Contents/MacOS/FMDV"]]) break;
        // any valid signature (Developer ID or ad-hoc) has page hashes, so a
        // truncated or tampered download fails here
        if (!RunTool(@"/usr/bin/codesign", @[@"--verify", @"--deep", newApp])) break;
        // strip a quarantine xattr if the download carried one, or Gatekeeper
        // re-prompts for the app the user already runs (standard updater step)
        RunTool(@"/usr/bin/xattr", @[@"-dr", @"com.apple.quarantine", newApp]);
        [fm removeItemAtPath:old error:nil];
        if (![fm moveItemAtPath:bundlePath toPath:old error:nil]) break;
        if (![fm moveItemAtPath:newApp toPath:bundlePath error:nil]) {
            [fm moveItemAtPath:old toPath:bundlePath error:nil]; // roll back
            break;
        }
        ok = true;
    } while (0);
    [fm removeItemAtPath:tmp error:nil];
    return ok;
}

// Kick off the download+swap worker for one release. Main thread only; one
// install at a time (Windows StartInstall's g_installRunning guard).
- (void)startInstall:(const ReleaseInfo&)r {
    if (_installRunning || r.macUrl.empty()) return;
    NSString* bundle = [self installableBundlePath];
    if (!bundle) { [self openReleasesPage:nil]; return; } // nothing swappable here
    _installRunning = true;
    _installTag = r.tag;
    [self refreshUpdatePicker];
    NSURL* url = [NSURL URLWithString:StrToNS(r.macUrl)];
    __unsafe_unretained FMDVAppDelegate* weak = self;
    NSURLSessionDownloadTask* task = [[NSURLSession sharedSession] downloadTaskWithURL:url
        completionHandler:^(NSURL* loc, NSURLResponse* resp, NSError* err) {
            (void)resp;
            // already off the main thread; do the file work right here
            bool ok = (loc && !err) && SwapInDownloadedBundle(loc.path, bundle);
            dispatch_async(dispatch_get_main_queue(), ^{ [weak installFinished:ok]; });
        }];
    [task resume];
}
- (void)installFinished:(bool)ok {
    _installRunning = false;
    if (ok)
        [self showBanner:[NSString stringWithFormat:@"%@ installed — takes effect on next launch", StrToNS(_installTag)]
             buttonTitle:@"Relaunch" action:@selector(relaunch:)];
    else
        [self showBanner:@"update failed — check github.com/OrangeBannana/FMDV/releases"
             buttonTitle:@"View Releases…" action:@selector(openReleasesPage:)];
    [self refreshUpdatePicker];
}
- (void)installNewest:(id)sender { // banner "Update" button
    (void)sender;
    const ReleaseInfo* nw = [self newestInstallable];
    if (nw) [self startInstall:*nw];
    else [self checkUpdates:sender]; // banner outlived the release list: re-check
}
- (void)relaunch:(id)sender {
    (void)sender;
    NSString* bundle = [NSBundle mainBundle].bundlePath;
    NSArray* args = _file.empty()
        ? @[@"-n", bundle]
        : @[@"-n", bundle, @"--args", [NSString stringWithUTF8String:_file.c_str()]];
    [NSTask launchedTaskWithLaunchPath:@"/usr/bin/open" arguments:args];
    [NSApp terminate:nil];
}
// Windows CleanupOldExe: drop the leftover <bundle>.old from a previous swap.
- (void)cleanupOldBundle {
    NSString* p = [NSBundle mainBundle].bundlePath;
    if ([p.pathExtension isEqualToString:@"app"])
        [[NSFileManager defaultManager] removeItemAtPath:[p stringByAppendingString:@".old"] error:nil];
}

// ---- Cmd+U picker panel ----
- (NSString*)pickerModeLine {
    NSString* mode = [self updateMode];
    if ([mode isEqualToString:@"pin"]) {
        NSString* pin = [[NSUserDefaults standardUserDefaults] stringForKey:kPrefPinTag] ?: @"?";
        return [NSString stringWithFormat:@"pinned to %@   ·   [A] auto-update: off", pin];
    }
    return [NSString stringWithFormat:@"[A] auto-update: %@",
            [mode isEqualToString:@"auto"] ? @"on" : @"off"];
}
- (void)refreshUpdatePicker {
    if (!_updList) return;
    NSString* status = _installRunning
        ? [NSString stringWithFormat:@"installing %@…", StrToNS(_installTag)] : @"";
    [_updList setReleases:_updReleases fetched:_updFetched failed:_updFailed
                  current:NSStringToStr([self currentVersion])
                 modeLine:[self pickerModeLine] status:status];
    if (_updPanel && _updPanel.visible) [self positionUpdatePicker];
}
- (void)positionUpdatePicker { // top-right of the main window (Windows anchor)
    if (!_window || !_updPanel) return;
    double w = 340, h = [_updList desiredHeight] + 24; // + title bar
    NSRect wf = _window.frame;
    [_updPanel setFrame:NSMakeRect(NSMaxX(wf) - w - 16, NSMaxY(wf) - h - 8, w, h) display:YES];
}
- (void)pickerLostKey:(NSNotification*)n { (void)n; [_updPanel orderOut:nil]; } // Windows WM_KILLFOCUS
- (void)closeUpdatePicker { [_updPanel orderOut:nil]; [_window makeKeyWindow]; }
- (void)pickerInstallRow:(long)row {
    if (_installRunning || row < 0 || row >= (long)_updReleases.size()) return;
    const ReleaseInfo r = _updReleases[row]; // copy; install runs async
    if (r.macUrl.empty()) return;
    // pin semantics (Windows PickerInstallSelected): anything but the newest
    // installable pins that tag; installing the newest clears a pin
    const ReleaseInfo* newest = [self newestInstallable];
    if (newest && CompareVersions(r.tag, newest->tag) < 0)
        [self setUpdateMode:@"pin" pin:StrToNS(r.tag)];
    else if ([[self updateMode] isEqualToString:@"pin"])
        [self setUpdateMode:@"notify" pin:nil];
    [self startInstall:r];
    [self refreshUpdatePicker];
}
- (void)toggleAutoUpdate { // 'A' in the picker: auto ↔ notify, clears a pin
    bool on = [[self updateMode] isEqualToString:@"auto"];
    [self setUpdateMode:(on ? @"notify" : @"auto") pin:nil];
    [self refreshUpdatePicker];
}
- (void)checkUpdates:(id)sender { // menu "Check for Updates…" (Cmd+U)
    (void)sender;
    [self ensureWindow];
    if (!_updPanel) {
        _updPanel = [[NSPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, 340, 140)
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskUtilityWindow)
                        backing:NSBackingStoreBuffered defer:NO];
        _updPanel.title = @"Updates";
        _updPanel.releasedWhenClosed = NO;
        _updList = [[FMDVUpdateListView alloc] initWithFrame:[_updPanel.contentView bounds]];
        _updList.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        __unsafe_unretained FMDVAppDelegate* weak = self;
        _updList.onInstall = ^(long row) { [weak pickerInstallRow:row]; };
        _updList.onToggleAuto = ^{ [weak toggleAutoUpdate]; };
        _updList.onClose = ^{ [weak closeUpdatePicker]; };
        [_updPanel.contentView addSubview:_updList];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(pickerLostKey:)
                                                     name:NSWindowDidResignKeyNotification object:_updPanel];
    }
    _updFetched = false;
    _updFailed = false;
    [self refreshUpdatePicker];
    [self positionUpdatePicker];
    [_updPanel makeKeyAndOrderFront:nil];
    [_updPanel makeFirstResponder:_updList];
    [self fetchReleases];
}

// Passive notify (Windows: banner in UPDATE_NOTIFY mode). On launch, if enabled,
// check GitHub silently and show a dismissible top banner only when a newer
// release exists — no alert, no nagging when up to date or offline.
- (BOOL)updateNotifyEnabled {
    NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
    return [ud objectForKey:kPrefUpdateNotify] ? [ud boolForKey:kPrefUpdateNotify] : YES; // default on
}
- (void)toggleUpdateNotify:(id)sender {
    BOOL now = ![self updateNotifyEnabled];
    [[NSUserDefaults standardUserDefaults] setBool:now forKey:kPrefUpdateNotify];
    if ([sender isKindOfClass:[NSMenuItem class]])
        [(NSMenuItem*)sender setState:now ? NSControlStateValueOn : NSControlStateValueOff];
    if (!now && _updateBanner) _updateBanner.hidden = YES;
}
- (void)maybeCheckUpdatesOnLaunch {
    if (![self updateNotifyEnabled]) return;                  // launch checks disabled
    if ([[self updateMode] isEqualToString:@"pin"]) return;   // pinned: no check (Windows parity)
    _updAutoInstallOnCheck = [[self updateMode] isEqualToString:@"auto"];
    [self fetchReleases]; // releasesArrived: banners (notify) or installs (auto)
}
- (void)openReleasesPage:(id)sender {
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://github.com/OrangeBannana/FMDV/releases"]];
}
- (void)dismissUpdateBanner:(id)sender { (void)sender; if (_updateBanner) _updateBanner.hidden = YES; }
- (void)positionUpdateBanner {
    if (!_previewScroll || !_updateBanner) return;
    double w = _updateBanner.frame.size.width, sw = _previewScroll.bounds.size.width;
    _updateBanner.frame = NSMakeRect((sw - w) / 2, _previewScroll.bounds.size.height - 44, w, 34);
}
// One floating strip, reused for every update message: "vX available · Update",
// "vX installed · Relaunch", "update failed · View Releases…". The button's
// title/action swap per message; ✕ always dismisses.
- (void)showBanner:(NSString*)text buttonTitle:(NSString*)title action:(SEL)action {
    [self ensureWindow];
    if (!_previewScroll) return;
    if (!_updateBanner) {
        _updateBanner = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 380, 34)];
        _updateBanner.wantsLayer = YES;
        _updateBanner.layer.backgroundColor = [[NSColor controlAccentColor] CGColor];
        _updateBanner.layer.cornerRadius = 8;
        _updateBanner.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin;
        _updateLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 8, 214, 20)];
        _updateLabel.editable = NO; _updateLabel.bordered = NO; _updateLabel.drawsBackground = NO;
        _updateLabel.textColor = [NSColor whiteColor];
        _updateLabel.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
        [_updateBanner addSubview:_updateLabel];
        _bannerButton = [[NSButton alloc] initWithFrame:NSMakeRect(228, 5, 118, 24)];
        [_bannerButton setBezelStyle:NSBezelStyleRounded];
        [_bannerButton setTarget:self];
        [_updateBanner addSubview:_bannerButton];
        NSButton* x = [[NSButton alloc] initWithFrame:NSMakeRect(352, 6, 22, 22)];
        [x setTitle:@"✕"]; [x setBordered:NO];
        [x setTarget:self]; [x setAction:@selector(dismissUpdateBanner:)];
        [x setAutoresizingMask:NSViewMinXMargin];
        [_updateBanner addSubview:x]; [x release];
    }
    _updateLabel.stringValue = text;
    [_bannerButton setTitle:title];
    [_bannerButton setAction:action];
    // fit the strip to its content: label · button · ✕
    [_updateLabel sizeToFit];
    double lw = _updateLabel.frame.size.width;
    [_bannerButton sizeToFit];
    double bw = std::max(90.0, _bannerButton.frame.size.width + 16);
    _updateLabel.frame = NSMakeRect(12, 8, lw, 20);
    _bannerButton.frame = NSMakeRect(12 + lw + 10, 5, bw, 24);
    double total = 12 + lw + 10 + bw + 6 + 22 + 6;
    NSRect bf = _updateBanner.frame; bf.size.width = total; _updateBanner.frame = bf;
    if (_updateBanner.superview != _previewScroll)
        [_previewScroll addFloatingSubview:_updateBanner forAxis:NSEventGestureAxisVertical];
    _updateBanner.hidden = NO;
    [self positionUpdateBanner];
}

// ---------------- test driver (--test-drive) ----------------
// The macOS analog of the Win32 suite's PostMessage/SendMessage driving
// (cpp/tests/run-tests.ps1): tests/run-tests.sh pipes line commands into stdin
// and reads one ok/err reply per command from stdout. Keystrokes are real
// NSEvents pushed through the same dispatch as user input — window
// performKeyEquivalent → menu → sendEvent → first responder — so shortcuts,
// ghost-text, list continuation, and the find bar are exercised end to end.
// Needs no Accessibility/TCC permission, so it runs on hosted CI.

// "cmd+shift+s", "tab", "x" → characters + keyCode + modifier flags.
static bool ParseKeySpec(NSString* spec, NSString** chars, unsigned short* code,
                         NSEventModifierFlags* flags) {
    static const struct { const char* n; unichar c; unsigned short k; } KEYS[] = {
        {"enter", '\r', 36},  {"tab", '\t', 48},  {"esc", 27, 53},  {"space", ' ', 49},
        {"up", NSUpArrowFunctionKey, 126},   {"down", NSDownArrowFunctionKey, 125},
        {"left", NSLeftArrowFunctionKey, 123}, {"right", NSRightArrowFunctionKey, 124},
        {"home", NSHomeFunctionKey, 115},    {"end", NSEndFunctionKey, 119},
        {"pgup", NSPageUpFunctionKey, 116},  {"pgdn", NSPageDownFunctionKey, 121},
    };
    *flags = 0;
    NSArray<NSString*>* parts = [spec componentsSeparatedByString:@"+"];
    // a bare "+" (zoom key) splits to empty parts; treat it as the literal char
    NSString* name = parts.lastObject.length ? parts.lastObject : @"+";
    for (NSUInteger i = 0; i + 1 < parts.count && parts[i].length; i++) {
        if ([parts[i] isEqualToString:@"cmd"])        *flags |= NSEventModifierFlagCommand;
        else if ([parts[i] isEqualToString:@"shift"]) *flags |= NSEventModifierFlagShift;
        else if ([parts[i] isEqualToString:@"alt"])   *flags |= NSEventModifierFlagOption;
        else if ([parts[i] isEqualToString:@"ctrl"])  *flags |= NSEventModifierFlagControl;
        else return false;
    }
    for (const auto& k : KEYS)
        if ([name isEqualToString:@(k.n)]) {
            *chars = [NSString stringWithCharacters:&k.c length:1];
            *code = k.k;
            return true;
        }
    if (name.length != 1) return false;
    *chars = name;
    *code = 0;
    return true;
}

// Deliver one keystroke the way NSApplication routes a real one. The update
// picker panel is targeted while visible (it would be the key window).
- (void)testSendChars:(NSString*)chars code:(unsigned short)code flags:(NSEventModifierFlags)flags {
    NSWindow* win = (_updPanel && _updPanel.visible) ? (NSWindow*)_updPanel : _window;
    if (!win) return;
    NSEvent* ev = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                   location:NSZeroPoint
                              modifierFlags:flags
                                  timestamp:[NSProcessInfo processInfo].systemUptime
                               windowNumber:win.windowNumber
                                    context:nil
                                 characters:chars
                charactersIgnoringModifiers:chars
                                  isARepeat:NO
                                    keyCode:code];
    if ((flags & NSEventModifierFlagCommand) &&
        ([win performKeyEquivalent:ev] || [[NSApp mainMenu] performKeyEquivalent:ev]))
        return;
    [win sendEvent:ev];
}

- (NSString*)testQuery:(NSString*)what {
    if ([what isEqualToString:@"editor"])   return _editing ? @"1" : @"0";
    if ([what isEqualToString:@"findbar"])  return [_preview findBarVisible] ? @"1" : @"0";
    if ([what isEqualToString:@"findlabel"]) return [_preview findLabelText];
    if ([what isEqualToString:@"toc"])      return (_tocVisible && _tocView && !_tocView.hidden) ? @"1" : @"0";
    if ([what isEqualToString:@"picker"])   return (_updPanel && _updPanel.visible) ? @"1" : @"0";
    if ([what isEqualToString:@"releases"]) return [NSString stringWithFormat:@"%zu", _updReleases.size()];
    if ([what isEqualToString:@"banner"])
        return (_updateBanner && !_updateBanner.hidden) ? _updateLabel.stringValue : @"";
    if ([what isEqualToString:@"title"])    return _window ? _window.title : @"";
    if ([what isEqualToString:@"dark"])     return [_preview dark] ? @"1" : @"0";
    if ([what isEqualToString:@"ghost"])    return _textView ? [_textView ghostText] : @"";
    if ([what isEqualToString:@"editor-text"]) return _textView ? _textView.string : @"";
    if ([what isEqualToString:@"version"])  return [self currentVersion];
    if ([what isEqualToString:@"installing"]) return _installRunning ? @"1" : @"0";
    if ([what isEqualToString:@"headings"]) // parsed-document probe (live reload tests)
        return [NSString stringWithFormat:@"%zu", [_preview headings].size()];
    if ([what isEqualToString:@"laidout"]) return [_preview laidOutInfo]; // "<width> <height>"
    if ([what isEqualToString:@"bannerpos"]) { // "<banner.origin.y> <scrollHeight>", to probe tracking
        if (!_updateBanner || _updateBanner.hidden || !_previewScroll) return @"hidden";
        return [NSString stringWithFormat:@"%d %d", (int)std::llround(_updateBanner.frame.origin.y),
                                                    (int)std::llround(_previewScroll.bounds.size.height)];
    }
    return nil;
}

- (NSString*)testCapture:(NSString*)path {
    if (!_window) return @"err no window";
    NSView* v = _window.contentView;
    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    if (!rep) return @"err capture";
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:path atomically:YES] ? @"ok" : @"err write";
}

// Execute one command line; returns the reply ("ok", "ok <data>", "err <why>").
- (NSString*)testCommand:(NSString*)line {
    NSRange sp = [line rangeOfString:@" "];
    NSString* cmd = sp.location == NSNotFound ? line : [line substringToIndex:sp.location];
    NSString* arg = sp.location == NSNotFound ? @"" : [line substringFromIndex:sp.location + 1];

    if ([cmd isEqualToString:@"key"]) {
        NSString* chars; unsigned short code; NSEventModifierFlags flags;
        if (!ParseKeySpec(arg, &chars, &code, &flags)) return @"err bad key spec";
        [self testSendChars:chars code:code flags:flags];
        return @"ok";
    }
    if ([cmd isEqualToString:@"type"]) { // \n → Enter keystroke, \t → Tab, \\ → backslash
        for (NSUInteger i = 0; i < arg.length; i++) {
            unichar c = [arg characterAtIndex:i];
            unsigned short code = 0;
            if (c == '\\' && i + 1 < arg.length) {
                unichar n = [arg characterAtIndex:++i];
                if (n == 'n') { c = '\r'; code = 36; }
                else if (n == 't') { c = '\t'; code = 48; }
                else c = n;
            }
            [self testSendChars:[NSString stringWithCharacters:&c length:1] code:code flags:0];
        }
        return @"ok";
    }
    if ([cmd isEqualToString:@"query"]) {
        NSString* v = [self testQuery:arg];
        if (!v) return @"err unknown query";
        v = [v stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
        v = [v stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
        return [@"ok " stringByAppendingString:v];
    }
    if ([cmd isEqualToString:@"caret"]) {
        if (!_textView) return @"err no editor";
        [_textView setSelectedRange:NSMakeRange((NSUInteger)arg.integerValue, 0)];
        return @"ok";
    }
    if ([cmd isEqualToString:@"find-step"]) { // Shift+Enter path: synthetic events can't
        [_preview stepFind:(int)arg.integerValue]; // set NSApp.currentEvent's shift flag
        return @"ok";
    }
    if ([cmd isEqualToString:@"resize"]) { // "resize W H" — drive a window resize (WM_SIZE analog)
        NSArray<NSString*>* wh = [arg componentsSeparatedByString:@" "];
        if (wh.count != 2 || !_window) return @"err bad resize";
        [_window setContentSize:NSMakeSize(wh[0].doubleValue, wh[1].doubleValue)];
        [_window layoutIfNeeded];
        return @"ok";
    }
    if ([cmd isEqualToString:@"save-close"]) { // invoke saveAndClose: directly —
        [self saveAndClose:nil];               // synthetic Cmd+Shift+S can't be told
        return @"ok";                          // apart from Cmd+S (both keyEquivalent "s")
    }
    if ([cmd isEqualToString:@"capture"]) return [self testCapture:arg];
    if ([cmd isEqualToString:@"quit"]) {
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        return @"ok";
    }
    return @"err unknown command";
}
@end

// Reads stdin line-by-line on a worker queue; each command runs synchronously
// on the main thread and its reply is flushed before the next line is read, so
// the shell suite never races the UI. EOF (harness died) quits the app.
static void StartTestDriver(FMDVAppDelegate* delegate) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        char buf[8192];
        while (fgets(buf, sizeof buf, stdin)) {
            NSString* line = [[NSString stringWithUTF8String:buf]
                stringByTrimmingCharactersInSet:[NSCharacterSet newlineCharacterSet]];
            if (!line.length) continue;
            __block NSString* resp = nil;
            dispatch_sync(dispatch_get_main_queue(), ^{ resp = [[delegate testCommand:line] retain]; });
            std::fprintf(stdout, "%s\n", resp.UTF8String);
            std::fflush(stdout);
            [resp release];
            if ([line isEqualToString:@"quit"]) return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    });
}

// ---------------- menu ----------------

static void buildMenu(id target) {
    NSMenu* menubar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menubar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [[appMenu addItemWithTitle:@"Check for Updates…" action:@selector(checkUpdates:) keyEquivalent:@"u"] setTarget:target];
    NSMenuItem* notify = [appMenu addItemWithTitle:@"Check for Updates on Launch"
                                            action:@selector(toggleUpdateNotify:) keyEquivalent:@""];
    [notify setTarget:target];
    NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
    BOOL on = [ud objectForKey:kPrefUpdateNotify] ? [ud boolForKey:kPrefUpdateNotify] : YES;
    [notify setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit FMDV" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];

    NSMenuItem* fileItem = [[NSMenuItem alloc] init];
    [menubar addItem:fileItem];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [[fileMenu addItemWithTitle:@"Save" action:@selector(saveDoc:) keyEquivalent:@"s"] setTarget:target];
    NSMenuItem* saveClose = [fileMenu addItemWithTitle:@"Save & Close Editor"
                                                action:@selector(saveAndClose:) keyEquivalent:@"s"];
    [saveClose setKeyEquivalentModifierMask:(NSEventModifierFlagCommand | NSEventModifierFlagShift)];
    [saveClose setTarget:target];
    [fileItem setSubmenu:fileMenu];

    // Edit menu: standard clipboard actions with nil target so they route down
    // the responder chain — the preview handles copy:/selectAll:, the source
    // editor's NSTextView handles the full cut/copy/paste set natively.
    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    [menubar addItem:editItem];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    [editItem setSubmenu:editMenu];

    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [[viewMenu addItemWithTitle:@"Toggle Editor" action:@selector(toggleEditor:) keyEquivalent:@"e"] setTarget:target];
    NSMenuItem* toc = [viewMenu addItemWithTitle:@"Toggle Contents"
                                          action:@selector(toggleContents:) keyEquivalent:@"o"];
    [toc setKeyEquivalentModifierMask:(NSEventModifierFlagCommand | NSEventModifierFlagShift)];
    [toc setTarget:target];
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
