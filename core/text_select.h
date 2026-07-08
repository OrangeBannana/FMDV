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

} // namespace fmdv
