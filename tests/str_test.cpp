// Unit tests for the core string type helpers (core/str): integer formatting
// and the UTF-8 <-> UTF-16 frontend-boundary conversions every non-Windows
// frontend crosses on file load and clipboard copy.
#include "str.h"
#include "test_check.h"
#include <string>

int main() {
    // ---- toStr ----
    check(ToUtf8(toStr(0)) == "0", "toStr: zero");
    check(ToUtf8(toStr(42)) == "42", "toStr: positive");
    check(ToUtf8(toStr(-42)) == "-42", "toStr: negative");
    check(ToUtf8(toStr(1234567890L)) == "1234567890", "toStr: large");

    // ---- FromUtf8: ASCII ----
    {
        Str s = FromUtf8("hello");
        check(s.size() == 5 && s == U16("hello"), "utf8: ascii converts 1:1");
    }
    check(FromUtf8("").empty(), "utf8: empty string");

    // ---- FromUtf8: multi-byte sequences ----
    {
        Str s = FromUtf8("caf\xC3\xA9"); // é U+00E9, 2-byte
        check(s.size() == 4 && s[3] == (Char)0x00E9, "utf8: 2-byte sequence");
    }
    {
        Str s = FromUtf8("\xE2\x82\xAC"); // € U+20AC, 3-byte
        check(s.size() == 1 && s[0] == (Char)0x20AC, "utf8: 3-byte sequence");
    }
    {
        Str s = FromUtf8("\xF0\x9F\x98\x80"); // 😀 U+1F600, 4-byte
        check(s.size() == 2 && s[0] == (Char)0xD83D && s[1] == (Char)0xDE00,
              "utf8: 4-byte becomes a surrogate pair");
    }

    // ---- round trips ----
    {
        const char* fixtures[] = {
            "plain ascii", "caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
            "mix \xE2\x82\xAC and \xF0\x9F\x98\x80 done",
        };
        bool all = true;
        for (const char* f : fixtures) all = all && ToUtf8(FromUtf8(f)) == f;
        check(all, "utf8: ToUtf8(FromUtf8(x)) round-trips");
    }

    // ---- malformed input degrades to U+FFFD, never crashes ----
    {
        Str s = FromUtf8("\x80"); // lone continuation byte
        check(s.size() == 1 && s[0] == (Char)0xFFFD, "utf8: lone continuation -> U+FFFD");
    }
    {
        Str s = FromUtf8("a\xC3"); // truncated 2-byte sequence at end
        check(s.size() == 2 && s[0] == U16('a') && s[1] == (Char)0xFFFD,
              "utf8: truncated sequence at end -> U+FFFD");
    }
    {
        Str s = FromUtf8("\xE2(\xA1"); // 3-byte lead with bad continuation
        check(s.size() == 3 && s[0] == (Char)0xFFFD && s[1] == U16('(')
                  && s[2] == (Char)0xFFFD,
              "utf8: bad continuation resyncs on next byte");
    }
    {
        Str s = FromUtf8("\xFF\xFE"); // invalid lead bytes
        check(s.size() == 2 && s[0] == (Char)0xFFFD && s[1] == (Char)0xFFFD,
              "utf8: invalid lead bytes -> U+FFFD each");
    }

    return summary();
}
