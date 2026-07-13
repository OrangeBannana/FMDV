// Unit tests for the shared editor helpers (core/edit_assist): autocomplete
// ghost text, list continuation on Enter, and table markdown generation. These
// back the Win32 editor pane and the future macOS one, so both sides of every
// decision (suggest / don't suggest) are pinned down here.
#include "edit_assist.h"
#include "test_check.h"
#include <string>

using namespace fmdv;

static std::string u8(const Str& s) { return ToUtf8(s); }

// SuggestClose as a printable "caret|text" signature ('\n' shown as '\N').
static std::string sug(const char* line) {
    Suggestion s = SuggestClose(FromUtf8(line));
    std::string t = u8(s.text), esc;
    for (char c : t) { if (c == '\n') esc += "\\n"; else esc += c; }
    return std::to_string(s.caret) + "|" + esc;
}

int main() {
    // ---- SuggestClose: matched pairs ----
    check(sug("**") == "0|**", "suggest: ** closes bold");
    check(sug("some **") == "0|**", "suggest: ** mid-line");
    check(sug("**bold**") == "0|", "suggest: balanced ** offers nothing");
    check(sug("__") == "0|__", "suggest: __ closes bold");
    check(sug("~~") == "0|~~", "suggest: ~~ closes strike");
    check(sug("*") == "0|*", "suggest: * closes italic");
    check(sug("***") == "0|", "suggest: *** (ambiguous) offers nothing");
    check(sug("`") == "0|`", "suggest: backtick closes code");
    check(sug("``") == "0|``", "suggest: double backtick closes");
    check(sug("`x`") == "0|", "suggest: closed code offers nothing");
    check(sug("(") == "0|)", "suggest: paren closes");
    check(sug("()") == "0|", "suggest: balanced parens offer nothing");
    check(sug("(a) and (") == "0|)", "suggest: counts parens across the line");

    // ---- SuggestClose: code fence ----
    check(sug("```") == "1|\\n\\n```", "suggest: fence adds closing fence, caret on middle line");
    check(sug("  ```") == "1|\\n\\n```", "suggest: indented fence still closes");
    check(sug("```py") == "0|", "suggest: fence with language offers nothing");

    // ---- SuggestClose: '[' context split (checkbox vs link) ----
    check(sug("- [") == "3| ] ", "suggest: checkbox after bullet marker");
    check(sug("* [") == "3| ] ", "suggest: checkbox after * bullet");
    check(sug("  - [") == "3| ] ", "suggest: checkbox after indented bullet");
    check(sug("1. [") == "3| ] ", "suggest: checkbox after ordered marker");
    check(sug("[") == "0|]()", "suggest: bare [ becomes a link");
    check(sug("see [") == "0|]()", "suggest: [ mid-text becomes a link");
    check(sug("- [ ] a [") == "0|]()", "suggest: [ after checkbox content is a link");

    // ---- SuggestClose: no suggestion ----
    check(sug("") == "0|", "suggest: empty line offers nothing");
    check(sug("plain text") == "0|", "suggest: plain text offers nothing");

    // ---- DecideListEnter ----
    {
        ListEnter e = DecideListEnter(FromUtf8("- item"));
        check(e.handled && !e.endList && u8(e.continuation) == "- ",
              "enter: bullet continues");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("  * item"));
        check(e.handled && u8(e.continuation) == "  * ",
              "enter: indent and bullet char preserved");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("+ item"));
        check(e.handled && u8(e.continuation) == "+ ", "enter: + bullet continues");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("3. item"));
        check(e.handled && u8(e.continuation) == "4. ", "enter: ordered increments");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("9. item"));
        check(e.handled && u8(e.continuation) == "10. ", "enter: 9 rolls to 10");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("- [x] done"));
        check(e.handled && u8(e.continuation) == "- [ ] ",
              "enter: checked task continues unchecked");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("- [ ] todo"));
        check(e.handled && u8(e.continuation) == "- [ ] ", "enter: task continues");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("- "));
        check(e.handled && e.endList, "enter: empty bullet ends the list");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("2. "));
        check(e.handled && e.endList, "enter: empty ordered item ends the list");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("- [ ] "));
        check(e.handled && e.endList, "enter: empty task item ends the list");
    }
    {
        ListEnter e = DecideListEnter(FromUtf8("- x   "));
        check(e.handled && !e.endList,
              "enter: trailing spaces alone don't end the list");
    }
    check(!DecideListEnter(FromUtf8("plain")).handled, "enter: plain text unhandled");
    check(!DecideListEnter(FromUtf8("-nospace")).handled, "enter: -nospace unhandled");
    check(!DecideListEnter(FromUtf8("1.nospace")).handled, "enter: 1.nospace unhandled");
    check(!DecideListEnter(FromUtf8("")).handled, "enter: empty line unhandled");
    check(!DecideListEnter(FromUtf8("-")).handled, "enter: lone dash unhandled");

    // ---- MakeTableMarkdown ----
    check(u8(MakeTableMarkdown(2, 1)) ==
              "| Column 1 | Column 2 |\n| --- | --- |\n|   |   |\n",
          "table: 2x1 exact markdown");
    check(u8(MakeTableMarkdown(1, 0)) == "| Column 1 |\n| --- |\n",
          "table: zero rows emits header+separator only");
    check(u8(MakeTableMarkdown(0, -3)) == "| Column 1 |\n| --- |\n",
          "table: cols/rows clamp to 1x0");
    {
        std::string t = u8(MakeTableMarkdown(3, 2));
        int lines = 0; for (char c : t) if (c == '\n') lines++;
        check(lines == 4 && t.find("Column 3") != std::string::npos,
              "table: 3x2 has 4 lines and 3 columns");
    }

    // ---- ToggleTaskAtLine ----
    check(u8(ToggleTaskAtLine(FromUtf8("- [ ] todo"), 0)) == "- [x] todo", "task: unchecked -> checked");
    check(u8(ToggleTaskAtLine(FromUtf8("- [x] done"), 0)) == "- [ ] done", "task: checked -> unchecked");
    check(u8(ToggleTaskAtLine(FromUtf8("- [X] done"), 0)) == "- [ ] done", "task: uppercase X unchecks");
    // only the target line changes; other lines and content are preserved
    check(u8(ToggleTaskAtLine(FromUtf8("# H\n- [ ] a\n- [ ] b"), 2)) == "# H\n- [ ] a\n- [x] b",
          "task: toggles only the target line");
    // indentation, other bullets, and ordered items
    check(u8(ToggleTaskAtLine(FromUtf8("    - [ ] nested"), 0)) == "    - [x] nested", "task: indented item");
    check(u8(ToggleTaskAtLine(FromUtf8("* [ ] star"), 0)) == "* [x] star", "task: star bullet");
    check(u8(ToggleTaskAtLine(FromUtf8("1. [ ] ordered"), 0)) == "1. [x] ordered", "task: ordered item");
    // non-task / out-of-range lines are returned unchanged
    check(u8(ToggleTaskAtLine(FromUtf8("- plain item"), 0)) == "- plain item", "task: plain bullet unchanged");
    check(u8(ToggleTaskAtLine(FromUtf8("just text [ ] here"), 0)) == "just text [ ] here",
          "task: bracket not after a bullet unchanged");
    check(u8(ToggleTaskAtLine(FromUtf8("- [ ] a"), 5)) == "- [ ] a", "task: out-of-range line unchanged");
    check(u8(ToggleTaskAtLine(FromUtf8("- [ ] a"), -1)) == "- [ ] a", "task: negative line unchanged");

    return summary();
}
