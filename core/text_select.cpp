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

} // namespace fmdv
