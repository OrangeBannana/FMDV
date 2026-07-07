#include "edit_assist.h"
#include <cwctype>

namespace fmdv {

Suggestion SuggestClose(const std::wstring& line) {
    auto endsWith = [&](const wchar_t* d) {
        size_t n = 0; while (d[n]) n++;
        return line.size() >= n && line.compare(line.size() - n, n, d) == 0;
    };
    auto count = [&](const std::wstring& d) {
        int c = 0; size_t p = 0;
        while ((p = line.find(d, p)) != std::wstring::npos) { c++; p += d.size(); }
        return c;
    };
    auto countCh = [&](wchar_t ch) { int c = 0; for (wchar_t x : line) if (x == ch) c++; return c; };

    // fenced code block: only right after typing the opening ``` (no lang yet).
    // close on its own line, caret on the blank middle line.
    {
        std::wstring t = line; size_t i = 0; while (i < t.size() && (t[i]==L' '||t[i]==L'\t')) i++;
        if (t.substr(i) == L"```") return { L"\n\n```", 1 };
    }
    if (endsWith(L"**") && !endsWith(L"***") && (count(L"**") % 2)) return { L"**", 0 };
    if (endsWith(L"__") && (count(L"__") % 2)) return { L"__", 0 };
    if (endsWith(L"~~") && (count(L"~~") % 2)) return { L"~~", 0 };
    if (endsWith(L"``") && !endsWith(L"```") && (count(L"``") % 2)) return { L"``", 0 };
    if (endsWith(L"`") && !endsWith(L"``") && (count(L"`") % 2)) return { L"`", 0 };
    if (endsWith(L"*") && !endsWith(L"**") && (countCh(L'*') % 2)) return { L"*", 0 };
    if (endsWith(L"(") && (countCh(L'(') > countCh(L')'))) return { L")", 0 };
    if (endsWith(L"[")) {
        // context split: checkbox after a list marker, else a link
        std::wstring rest = line.substr(0, line.size() - 1); // drop trailing '['
        size_t i = 0; while (i < rest.size() && (rest[i]==L' '||rest[i]==L'\t')) i++;
        bool listStart = false;
        if (i < rest.size()) {
            size_t j = i;
            if (rest[j]==L'-'||rest[j]==L'*'||rest[j]==L'+') j++;
            else { size_t d=j; while (d<rest.size() && iswdigit(rest[d])) d++; if (d>j && d<rest.size() && rest[d]==L'.') j=d+1; else j=rest.size()+1; }
            if (j <= rest.size() && j < rest.size() && rest[j]==L' ') { listStart = (j+1 == rest.size()); }
        }
        if (listStart) return { L" ] ", 3 };          // "- [ ] |" checkbox, caret after
        int ob = countCh(L'['), cb = countCh(L']');
        if (ob > cb) return { L"]()", 0 };             // "[|]()" link, caret inside brackets
    }
    return {};
}

ListEnter DecideListEnter(const std::wstring& line) {
    ListEnter out;
    size_t i = 0; while (i < line.size() && (line[i]==L' '||line[i]==L'\t')) i++;
    std::wstring indent = line.substr(0, i);
    std::wstring marker, rest;

    if (i < line.size() && (line[i]==L'-'||line[i]==L'*'||line[i]==L'+')
        && i+1 < line.size() && line[i+1]==L' ') {
        wchar_t bullet = line[i]; size_t after = i + 2;
        if (line.compare(after, 4, L"[ ] ") == 0 || line.compare(after, 4, L"[x] ") == 0 ||
            line.compare(after, 4, L"[X] ") == 0) {
            marker = std::wstring(1, bullet) + L" [ ] "; rest = line.substr(after + 4);
        } else {
            marker = std::wstring(1, bullet) + L" "; rest = line.substr(after);
        }
    } else if (i < line.size() && iswdigit(line[i])) {
        size_t d = i; while (d < line.size() && iswdigit(line[d])) d++;
        if (d < line.size() && line[d]==L'.' && d+1 < line.size() && line[d+1]==L' ') {
            int num = 0; for (size_t k = i; k < d; k++) num = num * 10 + (int)(line[k] - L'0');
            marker = std::to_wstring(num + 1) + L". "; rest = line.substr(d + 2);
        } else return out; // not a list item
    } else return out;     // not a list item

    out.handled = true;
    std::wstring trimmed = rest;
    while (!trimmed.empty() && (trimmed.back()==L' '||trimmed.back()==L'\t')) trimmed.pop_back();
    if (trimmed.empty()) {           // empty item -> end the list
        out.endList = true;
        return out;
    }
    out.continuation = indent + marker;
    return out;
}

std::wstring MakeTableMarkdown(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 0) rows = 0;
    std::wstring t;
    t += L"|";
    for (int c = 0; c < cols; c++) t += L" Column " + std::to_wstring(c + 1) + L" |";
    t += L"\n|";
    for (int c = 0; c < cols; c++) t += L" --- |";
    t += L"\n";
    for (int r = 0; r < rows; r++) {
        t += L"|";
        for (int c = 0; c < cols; c++) t += L"   |";
        t += L"\n";
    }
    return t;
}

} // namespace fmdv
