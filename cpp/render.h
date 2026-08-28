#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "markdown.h"
#include "layout.h"
#include "theme.h"

// A clickable link rectangle recorded during layout.
// rc is in DOCUMENT space (y is the document coordinate, NOT scroll-adjusted;
// PaintDocument offsets by scrollY at draw time). Hit-testers map a client point
// into document space first: subtract the preview pane's left edge from x and
// add the current scroll offset to y.
struct LinkHit {
    RECT rc;
    std::wstring href;
};

// A clickable task-list checkbox. rc is in DOCUMENT space (not scroll-adjusted,
// like LinkHit). srcLine is the 0-based source line of the item so the caller can
// toggle its "[ ]"/"[x]" marker; state is 0 unchecked / 1 checked.
struct TaskHit {
    RECT rc;
    int srcLine = -1;
    int state = 0;
};

// A clickable "copy to clipboard" button in a code block's top-right corner.
// rc is in DOCUMENT space (not scroll-adjusted, like LinkHit/TaskHit). iconRc
// is the icon's own tight visual bounds (click feedback highlights it, not the
// extra click padding). text is that block's raw code, verbatim, ready to
// paste into a terminal.
struct CodeCopyHit {
    RECT rc;
    RECT iconRc;
    std::wstring text;
};

// An ordered run of drawn text (one TextOut group), used for selection +
// copy. rc is in DOCUMENT space (not scroll-adjusted). Frags are appended in
// reading order each paint, so indices are stable while layout is unchanged.
struct TextFrag {
    RECT rc;
    std::wstring text;
    HFONT font;
    bool spaceBefore = false; // a space separated this frag from the previous one on the line
    fmdv::FontSpec spec;      // role/bold/italic of this run — rich (HTML) copy source
};

// A point in the selectable text: which fragment and which character within it.
struct SelPoint { int frag = -1; int ch = 0; };
struct Selection { SelPoint a, b; bool active = false; };

// A find-in-doc match: a character range within one TextFrag. Matches don't
// span frag boundaries (a search straddling a formatting change, e.g. bold ->
// plain mid-match, won't be found) — same tradeoff as everything else in this
// renderer that treats a frag as the atomic unit of text (selection, copy).
struct FindMatch { int frag; int chStart; int chEnd; };

// Character/x mapping within a fragment (font selected internally).
// x is in buffer coords; returns a char index into f.text [0..len].
int FragCharAtX(HDC hdc, const TextFrag& f, int x);
// Returns the buffer x coordinate of char index `ch` within f.
int FragXAtChar(HDC hdc, const TextFrag& f, int ch);

// Lay out + (optionally) draw the document into hdc.
// `width` is the full client width in pixels; internal padding is applied.
// `scrollY` is the vertical scroll offset (pixels scrolled down).
// When draw==false, performs measurement only (no GDI output) but still needs
// a valid hdc for text metrics. Returns total content height in pixels.
// Build the cached display list for the document at the given content width.
// Appends link hit-rects to `links` and selectable text runs to `frags` (both
// in document space). Returns total content height. Call when content/width/
// zoom/theme changes — NOT on every scroll.
// `blockTops`, if non-null, is filled with one entry per doc.blocks[i]: the
// document-space y coordinate that block starts at. Used by the TOC sidebar
// to scroll to a heading without re-measuring the whole document.
// `taskHits`, if non-null, is filled with one entry per task-list checkbox for
// click-to-toggle.
// `codeCopyHits`, if non-null, is filled with one entry per code block's
// copy-to-clipboard button.
int LayoutDocument(HDC hdc, int width, const Document& doc, const Theme& th,
                   std::vector<LinkHit>* links, std::vector<TextFrag>* frags,
                   std::vector<int>* blockTops = nullptr,
                   std::vector<TaskHit>* taskHits = nullptr,
                   std::vector<CodeCopyHit>* codeCopyHits = nullptr);

// Paint the cached display list, culled to the viewport [scrollY, scrollY+clientH],
// plus the current selection highlight. Cheap enough to call every frame/scroll.
// `findMatches`/`currentMatch`, if given, draw find-in-doc highlights (all
// matches lightly, the current one emphasized) behind the text, same as
// selection.
// `codeCopyFlash`, if given (the copy-button icon's DOCUMENT-space rect),
// draws the soft click-feedback pill behind that icon — before the text pass,
// which also keeps the afterText decorations (icon frames) above it.
void PaintDocument(HDC hdc, int scrollY, int clientW, int clientH, const Theme& th,
                   const Selection* sel, const std::vector<TextFrag>& frags,
                   const std::vector<FindMatch>* findMatches = nullptr, int currentMatch = -1,
                   const RECT* codeCopyFlash = nullptr);

// Free cached GDI font objects (call at exit).
void FreeFontCache();

// Set the rendering scale (zoom * DPI factor). 1.0 = 100%. Rebuilds the font
// cache so new sizes take effect. Affects fonts, padding, and margins.
void SetRenderScale(double scale);

// Set font antialiasing quality (CLEARTYPE_QUALITY for screen, ANTIALIASED_QUALITY
// for offscreen DIB/PNG to avoid ClearType color fringing). Rebuilds the font cache.
void SetFontQuality(int quality);
