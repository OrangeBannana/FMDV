#include "edit_assist.h"
#include <cwctype>

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

} // namespace fmdv
