#pragma once
// Shared rich-clipboard (HTML) fragment builder — the one implementation of
// the copy-to-clipboard HTML markup, used by both frontends so macOS and
// Windows paste into recipients byte-for-byte the same way (issue #36,
// "Copy and Paste").
//
// The core stays platform-neutral: frontends pass their selectable text
// records (a fragment = a styled text run with its document-space box and
// baseline) plus the layout's link hits, and get back the HTML fragment
// body as UTF-8. Each frontend wraps it in the envelope its clipboard type
// expects:
//   - macOS:   NSPasteboardTypeHTML  =  <html><body>fragment</body></html>
//   - Windows: "HTML Format" (CF_HTML) = Version/offset header +
//             <html><body><!--StartFragment-->fragment<!--EndFragment-->
//             </body></html>\r\n
//
// The plain-text copy path is untouched and remains the single source of
// textual content; this builder only re-expresses that same selection in
// HTML, and its <br>/space separators mirror the plain-text line/gap rules
// so the two copies stay in sync.

#include <cstddef>
#include <string>
#include <vector>

#include "layout.h"  // FontSpec, FontRole, RectF, LinkHit
#include "str.h"

namespace fmdv {

// A selectable text run, in the same shape as the macOS Frag record
// (box and baseline in document space, top-left origin, y-down).
struct CopyFrag {
    RectF box;
    FontSpec font;
    Str text;
    double baseline = 0;
};

// Build the HTML fragment of the selection over `frags` from (start,
// startCh) to (end, endCh), inclusive — the same fragment/character
// endpoints the plain-text copy uses. Selection endpoints must already be
// normalized (start <= end; characters within bounds) exactly as the
// frontends' normSel does.
//
// Markup rules (identical on both platforms):
//   - heading runs get <hN>; a heading that wraps onto several fragments
//     shares one open/close pair (tracked across fragments, inner link closed
//     before the heading is opened on the next fragment);
//   - code runs (FontRole::Mono) get <code style=\"white-space:pre\"> so
//     literal spacing/indentation survives the paste;
//   - bold/italic runs get <b>/<i>;
//   - a fragment whose box overlaps a link hit is wrapped in
//     <a href=\"...\">; consecutive fragments sharing the href reuse one
//     <a>; adjacent links with *different* hrefs close/reopen in between —
//     the runs must not merge across a link boundary (80aaa22);
//   - & < > \" are escaped in text and attribute values;
//   - slices that would split a UTF-16 surrogate pair are adjusted so no
//     lone surrogate reaches the UTF-8 encoding (which would make NSString
//     / MultiByteToWideChar reject the buffer);
//   - fragments on different baselines are separated by <br>, fragments with
//     a horizontal gap > 2px by a space — matching SelectionText.
std::string ClipboardHtmlFragment(const std::vector<CopyFrag>& frags,
                                  std::size_t start, std::size_t startCh,
                                  std::size_t end, std::size_t endCh,
                                  const std::vector<LinkHit>& links);

}  // namespace fmdv
