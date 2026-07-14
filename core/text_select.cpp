#include "text_select.h"
#include <algorithm>
#include <cmath>

namespace fmdv {

static Str asciiLower(Str s) {
    for (Char& c : s) if (c >= U16('A') && c <= U16('Z')) c = (Char)(c - U16('A') + U16('a'));
    return s;
}

std::vector<FindMatch> FindMatches(const std::vector<SelFrag>& frags, StrView query) {
    std::vector<FindMatch> out;
    Str q = asciiLower(Str(query));
    if (q.empty()) return out;
    for (long i = 0; i < (long)frags.size(); i++) {
        Str hay = asciiLower(frags[i].text);
        size_t pos = 0;
        while ((pos = hay.find(q, pos)) != Str::npos) {
            out.push_back({i, (long)pos, (long)q.size()});
            pos += q.size();
        }
    }
    return out;
}

Str SelectionText(const std::vector<SelFrag>& frags,
                  long a, long aCh, long b, long bCh) {
    if (a > b || (a == b && aCh > bCh)) { std::swap(a, b); std::swap(aCh, bCh); }
    Str out;
    long n = (long)frags.size();
    for (long i = std::max<long>(a, 0); i <= b && i < n; i++) {
        const SelFrag& f = frags[i];
        long len = (long)f.text.size();
        long c0 = (i == a) ? aCh : 0;
        long c1 = (i == b) ? bCh : len;
        c0 = std::max<long>(0, std::min(c0, len));
        c1 = std::max<long>(0, std::min(c1, len));
        if (i > a && i - 1 >= 0) {
            const SelFrag& prev = frags[i - 1];
            if (std::abs(f.baseline - prev.baseline) > 1) out += U16("\n");
            else if (f.x - (prev.x + prev.w) > 2) out += U16(" ");
        }
        if (c1 > c0) out.append(f.text, c0, c1 - c0);
    }
    return out;
}

// If char index `ch` sits inside a double-quoted phrase in `t`, set [s,e) to the
// span between the quotes (exclusive of the marks) and return true; else false.
// Handles straight ("...") and curly (“...”) double quotes. A missing closing
// quote returns false so the caller falls back to single-word selection. The
// scan is bounded to `t`, so quotes on other fragments (lines) never match.
static bool quotedSpan(const Str& t, long ch, long& s, long& e) {
    long n = (long)t.size();
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
        long R = -1; for (long j = ch; j < n; j++)     if (t[j] == kQuote) { R = j; break; }
        if (L >= 0 && R >= 0 && L < R) { s = L + 1; e = R; return true; }
    }
    return false;
}

// Trailing marks trimmed off a double-clicked word (not the quoted-phrase
// path, and not internal hyphens — only punctuation stuck to the end).
static bool isTrailingPunct(Char c) {
    switch (c) {
        case U16('.'): case U16(','): case U16(';'): case U16(':'):
        case U16('!'): case U16('?'):
        case U16(')'): case U16(']'): case U16('}'):
        case U16('\''): case U16('"'):
        case (Char)0x2019: case (Char)0x201D: // curly ' and "
            return true;
        default: return false;
    }
}

WordSpan DoubleClickSpan(const Str& text, long ch) {
    long n = (long)text.size();
    if (ch < 0) ch = 0; else if (ch > n) ch = n;
    long s, e;
    if (quotedSpan(text, ch, s, e)) return {s, e};
    auto isWS = [](Char c) { return c == U16(' ') || c == U16('\t'); };
    s = ch; e = ch;
    while (s > 0 && !isWS(text[s - 1])) s--;
    while (e < n && !isWS(text[e]))     e++;
    if (s == e && ch < n) e = ch + 1;   // click on lone whitespace: take one char
    // Trim trailing punctuation, unless the whole span is punctuation (then
    // leave it intact rather than collapsing to an empty selection).
    long trimmed = e;
    while (trimmed > s && isTrailingPunct(text[trimmed - 1])) trimmed--;
    if (trimmed > s) e = trimmed;
    return {s, e};
}

} // namespace fmdv
