#include "edit_assist.h"
#include <cwctype>
#include <vector>

namespace fmdv {

Suggestion SuggestClose(const Str& line) {
    auto endsWith = [&](const Char* d) {
        size_t n = 0; while (d[n]) n++;
        return line.size() >= n && line.compare(line.size() - n, n, d) == 0;
    };
    auto count = [&](const Str& d) {
        int c = 0; size_t p = 0;
        while ((p = line.find(d, p)) != Str::npos) { c++; p += d.size(); }
        return c;
    };
    auto countCh = [&](Char ch) { int c = 0; for (Char x : line) if (x == ch) c++; return c; };

    // fenced code block: only right after typing the opening ``` (no lang yet).
    // close on its own line, caret on the blank middle line.
    {
        Str t = line; size_t i = 0; while (i < t.size() && (t[i]==U16(' ')||t[i]==U16('\t'))) i++;
        if (t.substr(i) == U16("```")) return { U16("\n\n```"), 1 };
    }
    if (endsWith(U16("**")) && !endsWith(U16("***")) && (count(U16("**")) % 2)) return { U16("**"), 0 };
    if (endsWith(U16("__")) && (count(U16("__")) % 2)) return { U16("__"), 0 };
    if (endsWith(U16("~~")) && (count(U16("~~")) % 2)) return { U16("~~"), 0 };
    if (endsWith(U16("``")) && !endsWith(U16("```")) && (count(U16("``")) % 2)) return { U16("``"), 0 };
    if (endsWith(U16("`")) && !endsWith(U16("``")) && (count(U16("`")) % 2)) return { U16("`"), 0 };
    if (endsWith(U16("*")) && !endsWith(U16("**")) && (countCh(U16('*')) % 2)) return { U16("*"), 0 };
    if (endsWith(U16("(")) && (countCh(U16('(')) > countCh(U16(')')))) return { U16(")"), 0 };
    if (endsWith(U16("["))) {
        // context split: checkbox after a list marker, else a link
        Str rest = line.substr(0, line.size() - 1); // drop trailing '['
        size_t i = 0; while (i < rest.size() && (rest[i]==U16(' ')||rest[i]==U16('\t'))) i++;
        bool listStart = false;
        if (i < rest.size()) {
            size_t j = i;
            if (rest[j]==U16('-')||rest[j]==U16('*')||rest[j]==U16('+')) j++;
            else { size_t d=j; while (d<rest.size() && iswdigit(rest[d])) d++; if (d>j && d<rest.size() && rest[d]==U16('.')) j=d+1; else j=rest.size()+1; }
            if (j <= rest.size() && j < rest.size() && rest[j]==U16(' ')) { listStart = (j+1 == rest.size()); }
        }
        if (listStart) return { U16(" ] "), 3 };          // "- [ ] |" checkbox, caret after
        int ob = countCh(U16('[')), cb = countCh(U16(']'));
        if (ob > cb) return { U16("]()"), 0 };             // "[|]()" link, caret inside brackets
    }
    return {};
}

static bool isHexDigit(Char c) {
    return (c>=U16('0')&&c<=U16('9')) || (c>=U16('a')&&c<=U16('f')) || (c>=U16('A')&&c<=U16('F'));
}
static Char lower(Char c) { return (c>=U16('A')&&c<=U16('Z')) ? (Char)(c - U16('A') + U16('a')) : c; }
static bool isAsciiLetter(Char c) { Char l = lower(c); return l>=U16('a') && l<=U16('z'); }

// The 16 basic CSS/HTML color keywords plus common aliases -> normalized hex.
static Str ColorKeywordHex(const Str& w) {
    struct KV { const char* name; const char* hex; };
    static const KV tbl[] = {
        {"black","000000"},{"silver","c0c0c0"},{"gray","808080"},{"grey","808080"},
        {"white","ffffff"},{"maroon","800000"},{"red","ff0000"},{"purple","800080"},
        {"fuchsia","ff00ff"},{"magenta","ff00ff"},{"green","008000"},{"lime","00ff00"},
        {"olive","808000"},{"yellow","ffff00"},{"navy","000080"},{"blue","0000ff"},
        {"teal","008080"},{"aqua","00ffff"},{"cyan","00ffff"},{"orange","ffa500"},
    };
    for (const KV& kv : tbl) if (w == FromUtf8(kv.name)) return FromUtf8(kv.hex);
    return {};
}

ColorAt ColorLiteralAt(const Str& line, int caret) {
    int n = (int)line.size();
    if (caret < 0) caret = 0;
    if (caret > n) caret = n;

    // hex literal: '#' followed by 3 or 6 hex digits (extra digits, e.g. an
    // #rrggbbaa alpha, are ignored — we take the leading 3 or 6).
    for (int i = 0; i < n; i++) {
        if (line[i] != U16('#')) continue;
        int j = i + 1; while (j < n && isHexDigit(line[j])) j++;
        int digits = j - (i + 1);
        int len = (digits >= 6) ? 7 : (digits >= 3) ? 4 : 0;
        if (!len) continue;
        if (caret >= i && caret <= i + len) {
            ColorAt out; out.found = true; out.start = i; out.len = len;
            if (len == 7) for (int k = i + 1; k <= i + 6; k++) out.rrggbb += lower(line[k]);
            else for (int k = i + 1; k <= i + 3; k++) { Char c = lower(line[k]); out.rrggbb += c; out.rrggbb += c; }
            return out;
        }
    }

    // keyword literal: a maximal ASCII-letter run that names a known color.
    for (int i = 0; i < n; ) {
        if (!isAsciiLetter(line[i])) { i++; continue; }
        int j = i; while (j < n && isAsciiLetter(line[j])) j++;
        if (caret >= i && caret <= j) {
            Str word; for (int k = i; k < j; k++) word += lower(line[k]);
            Str hex = ColorKeywordHex(word);
            if (!hex.empty()) { ColorAt out; out.found = true; out.start = i; out.len = j - i; out.rrggbb = hex; return out; }
        }
        i = j;
    }
    return {};
}

ListEnter DecideListEnter(const Str& line) {
    ListEnter out;
    size_t i = 0; while (i < line.size() && (line[i]==U16(' ')||line[i]==U16('\t'))) i++;
    Str indent = line.substr(0, i);
    Str marker, rest;

    if (i < line.size() && (line[i]==U16('-')||line[i]==U16('*')||line[i]==U16('+'))
        && i+1 < line.size() && line[i+1]==U16(' ')) {
        Char bullet = line[i]; size_t after = i + 2;
        if (line.compare(after, 4, U16("[ ] ")) == 0 || line.compare(after, 4, U16("[x] ")) == 0 ||
            line.compare(after, 4, U16("[X] ")) == 0) {
            marker = Str(1, bullet) + U16(" [ ] "); rest = line.substr(after + 4);
        } else {
            marker = Str(1, bullet) + U16(" "); rest = line.substr(after);
        }
    } else if (i < line.size() && iswdigit(line[i])) {
        size_t d = i; while (d < line.size() && iswdigit(line[d])) d++;
        if (d < line.size() && line[d]==U16('.') && d+1 < line.size() && line[d+1]==U16(' ')) {
            int num = 0; for (size_t k = i; k < d; k++) num = num * 10 + (int)(line[k] - U16('0'));
            marker = toStr(num + 1) + U16(". "); rest = line.substr(d + 2);
        } else return out; // not a list item
    } else return out;     // not a list item

    out.handled = true;
    Str trimmed = rest;
    while (!trimmed.empty() && (trimmed.back()==U16(' ')||trimmed.back()==U16('\t'))) trimmed.pop_back();
    if (trimmed.empty()) {           // empty item -> end the list
        out.endList = true;
        return out;
    }
    out.continuation = indent + marker;
    return out;
}

Str MakeTableMarkdown(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 0) rows = 0;
    Str t;
    t += U16("|");
    for (int c = 0; c < cols; c++) t += U16(" Column ") + toStr(c + 1) + U16(" |");
    t += U16("\n|");
    for (int c = 0; c < cols; c++) t += U16(" --- |");
    t += U16("\n");
    for (int r = 0; r < rows; r++) {
        t += U16("|");
        for (int c = 0; c < cols; c++) t += U16("   |");
        t += U16("\n");
    }
    return t;
}

Str ToggleTaskAtLine(const Str& text, int line) {
    if (line < 0) return text;
    // Split into lines on LF (the caller normalizes endings).
    std::vector<Str> lines; Str cur;
    for (Char c : text) { if (c == U16('\n')) { lines.push_back(cur); cur.clear(); } else cur += c; }
    lines.push_back(cur);
    if (line >= (int)lines.size()) return text;

    Str ln = lines[line];
    size_t p = 0;
    while (p < ln.size() && (ln[p] == U16(' ') || ln[p] == U16('\t'))) p++;   // leading indent
    // bullet: "- " / "* " / "+ "  or  "N. "
    if (p + 1 < ln.size() && (ln[p] == U16('-') || ln[p] == U16('*') || ln[p] == U16('+'))
        && ln[p + 1] == U16(' ')) {
        p += 2;
    } else {
        size_t d = p;
        while (d < ln.size() && ln[d] >= U16('0') && ln[d] <= U16('9')) d++;
        if (d > p && d + 1 < ln.size() && ln[d] == U16('.') && ln[d + 1] == U16(' ')) p = d + 2;
        else return text;
    }
    while (p < ln.size() && ln[p] == U16(' ')) p++;                            // spaces before marker
    // marker: "[ ]" / "[x]" / "[X]"
    if (p + 2 >= ln.size() || ln[p] != U16('[') || ln[p + 2] != U16(']')) return text;
    Char inner = ln[p + 1];
    if (inner == U16(' ')) ln[p + 1] = U16('x');
    else if (inner == U16('x') || inner == U16('X')) ln[p + 1] = U16(' ');
    else return text;
    lines[line] = ln;

    Str out;
    for (size_t k = 0; k < lines.size(); k++) { if (k) out += U16('\n'); out += lines[k]; }
    return out;
}

} // namespace fmdv
