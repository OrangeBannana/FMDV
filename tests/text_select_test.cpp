// Unit tests for the shared find + selection logic (core/text_select), which
// backs the macOS preview's Cmd+F and text selection/copy. Built and run by
// `make check`. No UI — pure logic.
#include "text_select.h"
#include <cstdio>
#include <string>

using namespace fmdv;

static int failures = 0;
static std::string u8(const Str& s) { return ToUtf8(s); }

static void check(bool ok, const char* name) {
    std::printf("%s  %s\n", ok ? "OK  " : "FAIL", name);
    if (!ok) failures++;
}

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

    std::printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
