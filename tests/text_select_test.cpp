// Unit tests for the shared find + selection logic (core/text_select), which
// backs the macOS preview's Cmd+F and text selection/copy. Built and run by
// `make check`. No UI — pure logic.
#include "text_select.h"
#include "test_check.h"
#include <string>

using namespace fmdv;

static std::string u8(const Str& s) { return ToUtf8(s); }

// Build a fragment line: words laid out left-to-right with a space gap, one baseline.
static void addLine(std::vector<SelFrag>& v, std::initializer_list<const char*> words,
                    double baseline) {
    double x = 40;
    for (const char* w : words) {
        Str t = FromUtf8(w);
        double width = (double)t.size() * 8; // fake advance
        v.push_back({t, x, width, baseline});
        x += width + 5; // > 2px gap => a space between words
    }
}

int main() {
    std::vector<SelFrag> frags;
    addLine(frags, {"The", "quick", "brown", "Fox"}, 100);   // 0..3
    addLine(frags, {"the", "lazy", "dog"}, 130);             // 4..6

    // ---- FindMatches ----
    auto m = FindMatches(frags, FromUtf8("the"));
    // case-insensitive: "The"(0), "the"(4); "brown"/"Fox" have no "the"
    check(m.size() == 2, "find: 'the' matches twice (case-insensitive)");
    check(m.size() == 2 && m[0].frag == 0 && m[0].start == 0 && m[0].len == 3, "find: first match position");
    check(m.size() == 2 && m[1].frag == 4, "find: second match in second line");

    check(FindMatches(frags, FromUtf8("")).empty(), "find: empty query -> no matches");
    check(FindMatches(frags, FromUtf8("zzz")).empty(), "find: no match -> empty");
    // frag-atomic: "quickbrown" spans two fragments, so it must NOT match
    check(FindMatches(frags, FromUtf8("quickbrown")).empty(), "find: matches don't span fragments");
    // two hits within one fragment
    std::vector<SelFrag> rep = { {FromUtf8("abcabc"), 0, 48, 10} };
    check(FindMatches(rep, FromUtf8("abc")).size() == 2, "find: two matches in one fragment");

    // ---- SelectionText ----
    // whole first line
    Str s1 = SelectionText(frags, 0, 0, 3, (long)frags[3].text.size());
    check(u8(s1) == "The quick brown Fox", "select: whole line joins words with spaces");
    // partial within one fragment
    Str s2 = SelectionText(frags, 1, 0, 1, 3); // "qui" from "quick"
    check(u8(s2) == "qui", "select: partial single fragment");
    // across a line break -> newline
    Str s3 = SelectionText(frags, 3, 0, 4, (long)frags[4].text.size()); // "Fox" + "\n" + "the"
    check(u8(s3) == "Fox\nthe", "select: line change inserts newline");
    // reversed endpoints normalize
    Str s4 = SelectionText(frags, 1, 3, 1, 0);
    check(u8(s4) == "qui", "select: reversed endpoints normalize");
    // adjacent fragments with no gap concatenate (no space)
    std::vector<SelFrag> tight = { {FromUtf8("foo"), 0, 24, 10}, {FromUtf8("bar"), 24, 24, 10} };
    check(u8(SelectionText(tight, 0, 0, 1, 3)) == "foobar", "select: touching fragments concatenate");

    // ---- Unicode fixtures: offsets are UTF-16 code units (guide Phase 2) ----
    // Emoji U+1F600 is a surrogate pair = 2 code units, so "a😀b" is 4 units
    // and 'b' sits at code-unit offset 3 (not scalar offset 2).
    std::vector<SelFrag> emoji = { {FromUtf8("a\xF0\x9F\x98\x80""b"), 0, 32, 10} };
    check(emoji[0].text.size() == 4, "unicode: surrogate pair is 2 code units");
    auto me = FindMatches(emoji, FromUtf8("b"));
    check(me.size() == 1 && me[0].start == 3, "find: offset after emoji counts code units");
    check(u8(SelectionText(emoji, 0, 1, 0, 3)) == "\xF0\x9F\x98\x80",
          "select: code-unit range [1,3) is the whole emoji");
    // Combining mark: "cafe" + U+0301 is 5 code units; a selection keeps both
    // the base letter and its combining accent.
    std::vector<SelFrag> comb = { {FromUtf8("cafe\xCC\x81"), 0, 40, 10} };
    check(comb[0].text.size() == 5, "unicode: combining mark is its own code unit");
    check(u8(SelectionText(comb, 0, 3, 0, 5)) == "e\xCC\x81",
          "select: base letter + combining mark survive slicing");
    // CJK (no spaces): offsets are straight code-unit positions.
    std::vector<SelFrag> cjk = { {FromUtf8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
                                           "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"), 0, 96, 10} };
    auto mc = FindMatches(cjk, FromUtf8("\xE6\x9C\xAC\xE8\xAA\x9E"));
    check(mc.size() == 1 && mc[0].frag == 0 && mc[0].start == 1 && mc[0].len == 2,
          "find: CJK match at code-unit offset 1, len 2");

    // ---- edge cases ----
    check(FindMatches({}, FromUtf8("x")).empty(), "find: empty fragment list");
    check(FindMatches(frags, FromUtf8("supercalifragilistic")).empty(),
          "find: query longer than any fragment");
    // non-overlapping scan: "aaa" contains "aa" once, not twice
    std::vector<SelFrag> tri = { {FromUtf8("aaa"), 0, 24, 10} };
    check(FindMatches(tri, FromUtf8("aa")).size() == 1, "find: matches don't overlap");
    // case folding is ASCII-only: 'É' vs 'é' don't match
    std::vector<SelFrag> acc = { {FromUtf8("\xC3\x89"), 0, 8, 10} }; // É
    check(FindMatches(acc, FromUtf8("\xC3\xA9")).empty(), "find: folding is ASCII-only");

    check(SelectionText({}, 0, 0, 0, 5).empty(), "select: empty fragment list");
    check(u8(SelectionText(frags, 0, 0, 0, 999)) == "The",
          "select: char offsets clamp to fragment length");
    check(u8(SelectionText(frags, -3, 0, 0, 3)) == "The",
          "select: negative start fragment clamps");
    check(u8(SelectionText(frags, 6, 0, 999, 3)) == "dog",
          "select: end fragment clamps to fragment count");
    check(SelectionText(frags, 1, 2, 1, 2).empty(), "select: zero-width selection is empty");

    return summary();
}
