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
// same fields in %APPDATA%\fmdv\prefs.txt (dark / zoom / split). Update mode+pin
// are Windows-only for now (the macOS updater is check-and-link, not install).
static NSString* const kPrefDark         = @"FMDVDark";
static NSString* const kPrefZoomPct      = @"FMDVZoomPct";
static NSString* const kPrefSplitPct     = @"FMDVSplitPct";
static NSString* const kPrefUpdateNotify = @"FMDVUpdateNotify"; // check on launch (default on)

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
- (instancetype)initWithDoc:(const Document&)doc dark:(bool)dark;
- (void)setDoc:(const Document&)doc;
- (bool)dark;
- (void)toggleDark;
- (void)zoomBy:(double)factor;
- (void)zoomReset;
- (void)scrollToDocY:(double)docY;
- (std::vector<fmdv::HeadingRef>)headings;
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

- (NSPoint)logicalPoint:(NSEvent*)ev {
    NSPoint p = [self convertPoint:ev.locationInWindow fromView:nil];
    return NSMakePoint(p.x / _zoom, p.y / _zoom);
}
- (void)mouseDown:(NSEvent*)ev {
    _dragging = false;
    long fi, ch;
    if (![self hitPoint:[self logicalPoint:ev] frag:&fi ch:&ch]) { [self clearSelection]; return; }
    if (ev.clickCount >= 3) {                 // triple click: select the line
        double base = _frags[fi].baseline; long lo = fi, hi = fi;
        while (lo > 0 && std::abs(_frags[lo - 1].baseline - base) < 1) lo--;
        while (hi + 1 < (long)_frags.size() && std::abs(_frags[hi + 1].baseline - base) < 1) hi++;
        _selA = lo; _selACh = 0; _selB = hi; _selBCh = (long)_frags[hi].text.size(); _hasSel = true;
    } else if (ev.clickCount == 2) {          // double click: select the word (frag)
        _selA = fi; _selACh = 0; _selB = fi; _selBCh = (long)_frags[fi].text.size(); _hasSel = true;
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
        // select-all / copy belong to the editor when it's focused
        BOOL previewFocused = (self.window.firstResponder == self);
        if (previewFocused && [c isEqualToString:@"a"]) { [self selectAll:nil]; return YES; }
        if (previewFocused && [c isEqualToString:@"c"]) { [self copySelection]; return YES; }
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

- (void)mouseUp:(NSEvent*)ev {
    if (_dragging) { _dragging = false; return; } // a drag made a selection; keep it
    // A plain click: follow a link if one was hit, and drop any selection.
    NSPoint p = [self logicalPoint:ev];
    for (const auto& lk : _layout.links) {
        const auto& r = lk.rect;
        if (p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h) {
            [self clearSelection];
            NSURL* url = [NSURL URLWithString:StrToNS(lk.href)];
            if (url) [[NSWorkspace sharedWorkspace] openURL:url];
            return;
        }
    }
    if (ev.clickCount == 1) [self clearSelection];
}
@end

// ---------------- source editor (list continuation via core/edit_assist) ----

@interface FMDVTextView : NSTextView
@end

@implementation FMDVTextView {
    NSString* _ghost;       // autocomplete overlay (not in the text buffer); nil = none
    NSInteger _ghostCaret;  // caret offset within _ghost after Tab-commit
}
- (void)dealloc { [_ghost release]; [super dealloc]; }

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
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_preview];
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
    (void)app; [self openPath:filename]; return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    (void)n;
    [NSApp activateIgnoringOtherApps:YES];
    // Passive update check runs after first paint (Windows: async, post-paint).
    __unsafe_unretained FMDVAppDelegate* weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [weak maybeCheckUpdatesOnLaunch]; });
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

- (void)saveDoc:(id)sender {
    (void)sender;
    if (_file.empty()) return;
    std::string text = _editing ? std::string(_textView.string.UTF8String ?: "") : ToUtf8(LoadDoc(ReadFileUtf8(_file.c_str())));
    // NSTextView already uses LF; write as UTF-8.
    FILE* f = std::fopen(_file.c_str(), "wb");
    if (f) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
    _fileMtime = FileModTime(_file); // our own save shouldn't trigger a live reload
}
- (void)saveAndClose:(id)sender {
    [self saveDoc:sender];
    if (_editing) [self toggleEditor:sender]; // save & close editor (Windows Ctrl+Shift+S)
}
- (void)insertTable:(id)sender { (void)sender; if (_editing && _textView) [_textView insertTableMarkdown]; }

// Updates (Windows Ctrl+U). macOS can't swap a running, unsigned binary and the
// releases carry no macOS asset, so this checks GitHub (shared core parse +
// version compare) and links to the releases page rather than installing.
- (NSString*)currentVersion {
    NSString* v = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return v.length ? v : @"1.1.0";
}
- (NSURLRequest*)releasesRequest {
    NSURL* url = [NSURL URLWithString:@"https://api.github.com/repos/OrangeBannana/FMDV/releases?per_page=30"];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
    [req setValue:[@"fmdv-macos/" stringByAppendingString:[self currentVersion]] forHTTPHeaderField:@"User-Agent"];
    return req;
}
// Newest release tag if it's newer than the running version, else nil.
static NSString* NewerTag(const std::string& json, const Str& cur) {
    std::vector<ReleaseInfo> rel;
    ParseReleasesJson(json, rel);
    const ReleaseInfo* newest = nullptr;
    for (const auto& r : rel) if (!newest || CompareVersions(r.tag, newest->tag) > 0) newest = &r;
    return (newest && CompareVersions(newest->tag, cur) > 0) ? StrToNS(newest->tag) : nil;
}
- (void)checkUpdates:(id)sender {
    (void)sender;
    __unsafe_unretained FMDVAppDelegate* weak = self;
    NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:[self releasesRequest]
        completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
            (void)resp; (void)err;
            NSData* d = [data retain];
            dispatch_async(dispatch_get_main_queue(), ^{ [weak showUpdateResult:d]; [d release]; });
        }];
    [task resume];
}
- (void)showUpdateResult:(NSData*)data {
    std::string json;
    if (data.length) json.assign((const char*)data.bytes, data.length);
    std::vector<ReleaseInfo> rel;
    ParseReleasesJson(json, rel);
    Str cur = NSStringToStr([self currentVersion]);
    const ReleaseInfo* newest = nullptr;
    for (const auto& r : rel) if (!newest || CompareVersions(r.tag, newest->tag) > 0) newest = &r;

    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"FMDV %@", [self currentVersion]];
    if (rel.empty()) {
        a.informativeText = @"Couldn't reach GitHub releases.";
    } else if (newest && CompareVersions(newest->tag, cur) > 0) {
        a.informativeText = [NSString stringWithFormat:
            @"%@ is available. In-app update isn't supported on macOS yet — open the releases page to download.",
            StrToNS(newest->tag)];
    } else {
        a.informativeText = @"You have the latest version.";
    }
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"View Releases…"];
    NSModalResponse r = [a runModal];
    if (r == NSAlertSecondButtonReturn)
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://github.com/OrangeBannana/FMDV/releases"]];
    [a release];
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
    if (![self updateNotifyEnabled]) return;
    __unsafe_unretained FMDVAppDelegate* weak = self;
    Str cur = NSStringToStr([self currentVersion]);
    NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:[self releasesRequest]
        completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
            (void)resp; (void)err;
            if (!data.length) return;            // offline: stay silent
            NSData* d = [data retain];
            dispatch_async(dispatch_get_main_queue(), ^{
                std::string json((const char*)d.bytes, d.length);
                [d release];
                NSString* tag = NewerTag(json, cur);
                if (tag) [weak showUpdateBanner:tag];
            });
        }];
    [task resume];
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
- (void)showUpdateBanner:(NSString*)tag {
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
        NSButton* view = [[NSButton alloc] initWithFrame:NSMakeRect(228, 5, 118, 24)];
        [view setTitle:@"View Releases…"]; [view setBezelStyle:NSBezelStyleRounded];
        [view setTarget:self]; [view setAction:@selector(openReleasesPage:)];
        [_updateBanner addSubview:view]; [view release];
        NSButton* x = [[NSButton alloc] initWithFrame:NSMakeRect(352, 6, 22, 22)];
        [x setTitle:@"✕"]; [x setBordered:NO];
        [x setTarget:self]; [x setAction:@selector(dismissUpdateBanner:)];
        [_updateBanner addSubview:x]; [x release];
    }
    _updateLabel.stringValue = [NSString stringWithFormat:@"FMDV %@ is available", tag];
    if (_updateBanner.superview != _previewScroll)
        [_previewScroll addFloatingSubview:_updateBanner forAxis:NSEventGestureAxisVertical];
    _updateBanner.hidden = NO;
    [self positionUpdateBanner];
}
@end

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
