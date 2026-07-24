#pragma once
// Platform-neutral find + selection operations over laid-out text fragments
// (one per Text draw command). Pure logic — no UI — so it can be unit-tested and
// shared by any frontend. The frontend supplies the fragment geometry.
#include <vector>
#include "str.h"

namespace fmdv {

// A selectable text run: the fragment's text plus enough geometry (document
// space) to reconstruct spacing and line breaks when copying a selection.
struct SelFrag {
    Str text;
    double x = 0, w = 0;  // horizontal extent, for the inter-word space heuristic
    double baseline = 0;  // to detect a line change between adjacent fragments
};

struct FindMatch { long frag; long start; long len; };

// Case-insensitive (ASCII-fold) substring matches within each fragment. Matches
// never span fragments (the frag-atomic behavior the Windows app also has).
std::vector<FindMatch> FindMatches(const std::vector<SelFrag>& frags, StrView query);

// Reconstruct selected text from frags[a..b], char `aCh` in the first through
// `bCh` in the last. Endpoints may be given in any order. A newline is inserted
// between fragments on different lines and a space between fragments separated
// by a horizontal gap; otherwise fragments are concatenated directly.
Str SelectionText(const std::vector<SelFrag>& frags,
                  long aFrag, long aCh, long bFrag, long bCh);

// A half-open [start,end) character range within one fragment's text.
struct WordSpan { long start = 0; long end = 0; };

// Selection for a double-click over one fragment's `text` at insertion index
// `ch`. If `ch` lies inside a double-quoted phrase ("..." straight or “...”
// curly) that has a closing quote within this same text, returns the span
// between the quotes (quote marks excluded); otherwise the whitespace-delimited
// word under `ch` (a click on lone whitespace yields the single character),
// with trailing punctuation (. , ; : ! ? ) ] } ' " and their curly-quote
// variants) trimmed off the end — unless the whole token is punctuation, in
// which case it's returned intact rather than emptied. Quote matching is
// scoped to `text` (one line/fragment); a missing closing quote falls back to
// the word. `ch` is clamped to [0, text.size()].
WordSpan DoubleClickSpan(const Str& text, long ch);

} // namespace fmdv
