#include "markdown.h"
#include <cwctype>

// ---------------- helpers ----------------

static std::wstring rtrim(const std::wstring& s) {
    size_t e = s.size();
    while (e > 0 && (s[e-1] == L' ' || s[e-1] == L'\t' || s[e-1] == L'\r')) e--;
    return s.substr(0, e);
}
static std::wstring ltrim(const std::wstring& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == L' ' || s[b] == L'\t')) b++;
    return s.substr(b);
}
static std::wstring trim(const std::wstring& s) { return ltrim(rtrim(s)); }

static int leadingSpaces(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s) { if (c == L' ') n++; else if (c == L'\t') n += 4; else break; }
    return n;
}

// Split text into lines (text already uses LF endings).
static std::vector<std::wstring> splitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L'\n') { lines.push_back(cur); cur.clear(); }
        else if (c != L'\r') cur += c;
    }
    lines.push_back(cur);
    return lines;
}

// ---------------- inline parsing ----------------

static void appendRun(std::vector<InlineRun>& out, const std::wstring& text,
                      bool b, bool i, bool c, bool s, const std::wstring& href) {
    if (text.empty()) return;
    InlineRun r; r.text = text; r.bold = b; r.italic = i; r.code = c; r.strike = s; r.href = href;
    out.push_back(r);
}

// Find a matching closing delimiter `delim` starting at `from`. Returns npos if none.
static size_t findClose(const std::wstring& s, const std::wstring& delim, size_t from) {
    return s.find(delim, from);
}

// Recursive inline parser. Emits runs with the given active styles.
static void parseInlineInto(const std::wstring& s, std::vector<InlineRun>& out,
                            bool b, bool i, bool c, bool strike, const std::wstring& href,
                            int depth) {
    if (depth > 24) { appendRun(out, s, b, i, c, strike, href); return; }

    std::wstring buf;
    size_t n = s.size();
    for (size_t p = 0; p < n; ) {
        wchar_t ch = s[p];

        // inline code: highest priority, no nested formatting
        if (ch == L'`') {
            size_t close = s.find(L'`', p + 1);
            if (close != std::wstring::npos) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                appendRun(out, s.substr(p + 1, close - p - 1), b, i, true, strike, href);
                p = close + 1; continue;
            }
        }

        // image ![alt](src) -> render alt text (no bitmap loading yet)
        if (ch == L'!' && p + 1 < n && s[p+1] == L'[') {
            size_t rb = s.find(L']', p + 2);
            if (rb != std::wstring::npos && rb + 1 < n && s[rb+1] == L'(') {
                size_t rp = s.find(L')', rb + 2);
                if (rp != std::wstring::npos) {
                    appendRun(out, buf, b, i, c, strike, href); buf.clear();
                    std::wstring alt = s.substr(p + 2, rb - p - 2);
                    appendRun(out, alt.empty() ? L"[image]" : alt, b, i, c, strike, href);
                    p = rp + 1; continue;
                }
            }
        }

        // link [text](href)
        if (ch == L'[') {
            size_t rb = s.find(L']', p + 1);
            if (rb != std::wstring::npos && rb + 1 < n && s[rb+1] == L'(') {
                size_t rp = s.find(L')', rb + 2);
                if (rp != std::wstring::npos) {
                    appendRun(out, buf, b, i, c, strike, href); buf.clear();
                    std::wstring inner = s.substr(p + 1, rb - p - 1);
                    std::wstring url = s.substr(rb + 2, rp - rb - 2);
                    parseInlineInto(inner, out, b, i, c, strike, url, depth + 1);
                    p = rp + 1; continue;
                }
            }
        }

        // bold+italic ***
        if (ch == L'*' && p + 2 < n && s[p+1] == L'*' && s[p+2] == L'*') {
            size_t close = findClose(s, L"***", p + 3);
            if (close != std::wstring::npos) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                parseInlineInto(s.substr(p + 3, close - p - 3), out, true, true, c, strike, href, depth + 1);
                p = close + 3; continue;
            }
        }
        // bold ** or __
        if ((ch == L'*' && p + 1 < n && s[p+1] == L'*') ||
            (ch == L'_' && p + 1 < n && s[p+1] == L'_')) {
            std::wstring d(2, ch);
            size_t close = findClose(s, d, p + 2);
            if (close != std::wstring::npos) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                parseInlineInto(s.substr(p + 2, close - p - 2), out, true, i, c, strike, href, depth + 1);
                p = close + 2; continue;
            }
        }
        // italic * or _
        if (ch == L'*' || ch == L'_') {
            std::wstring d(1, ch);
            size_t close = findClose(s, d, p + 1);
            if (close != std::wstring::npos && close > p + 1) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                parseInlineInto(s.substr(p + 1, close - p - 1), out, b, true, c, strike, href, depth + 1);
                p = close + 1; continue;
            }
        }
        // strikethrough ~~
        if (ch == L'~' && p + 1 < n && s[p+1] == L'~') {
            size_t close = findClose(s, L"~~", p + 2);
            if (close != std::wstring::npos) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                parseInlineInto(s.substr(p + 2, close - p - 2), out, b, i, c, true, href, depth + 1);
                p = close + 2; continue;
            }
        }

        buf += ch;
        p++;
    }
    appendRun(out, buf, b, i, c, strike, href);
}

std::vector<InlineRun> ParseInline(const std::wstring& text) {
    std::vector<InlineRun> out;
    parseInlineInto(text, out, false, false, false, false, L"", 0);
    return out;
}

// ---------------- table detection ----------------

static std::vector<std::wstring> splitCells(const std::wstring& line) {
    // split on unescaped '|', trim, drop leading/trailing empties from border pipes
    std::vector<std::wstring> cells;
    std::wstring cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == L'|' && (i == 0 || line[i-1] != L'\\')) {
            cells.push_back(trim(cur)); cur.clear();
        } else cur += line[i];
    }
    cells.push_back(trim(cur));
    if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
    if (!cells.empty() && cells.back().empty()) cells.pop_back();
    return cells;
}

std::vector<std::wstring> SplitTableCells(const std::wstring& line) { return splitCells(line); }

static bool isTableSeparator(const std::wstring& line, std::vector<int>* aligns) {
    std::wstring t = trim(line);
    if (t.find(L'|') == std::wstring::npos && t.find(L'-') == std::wstring::npos) return false;
    auto cells = splitCells(t);
    if (cells.empty()) return false;
    if (aligns) aligns->clear();
    for (auto& c : cells) {
        std::wstring cc = trim(c);
        if (cc.empty()) return false;
        bool left = cc.front() == L':';
        bool right = cc.back() == L':';
        // middle must be all dashes
        size_t a = left ? 1 : 0, z = cc.size() - (right ? 1 : 0);
        if (z <= a) return false;
        for (size_t k = a; k < z; k++) if (cc[k] != L'-') return false;
        if (aligns) {
            if (left && right) aligns->push_back(AlignCenter);
            else if (right) aligns->push_back(AlignRight);
            else aligns->push_back(AlignLeft);
        }
    }
    return true;
}

// ---------------- block parsing ----------------

Document ParseMarkdown(const std::wstring& text) {
    Document doc;
    auto lines = splitLines(text);
    size_t i = 0, N = lines.size();

    auto flushParagraph = [&](std::wstring& acc) {
        if (trim(acc).empty()) { acc.clear(); return; }
        Block b; b.type = BlockType::Paragraph;
        b.runs = ParseInline(trim(acc));
        doc.blocks.push_back(std::move(b));
        acc.clear();
    };

    std::wstring para;

    while (i < N) {
        const std::wstring& raw = lines[i];
        std::wstring line = rtrim(raw);
        std::wstring t = trim(line);

        // blank line
        if (t.empty()) { flushParagraph(para); i++; continue; }

        // fenced code block
        if (t.rfind(L"```", 0) == 0 || t.rfind(L"~~~", 0) == 0) {
            flushParagraph(para);
            std::wstring fence = t.substr(0, 3);
            Block b; b.type = BlockType::CodeBlock;
            b.lang = trim(t.substr(3));
            i++;
            std::wstring code;
            while (i < N) {
                std::wstring cl = rtrim(lines[i]);
                if (trim(cl).rfind(fence, 0) == 0 && trim(trim(cl).substr(3)).empty()) { i++; break; }
                if (!code.empty()) code += L"\n";
                code += lines[i];
                i++;
            }
            b.codeText = code;
            doc.blocks.push_back(std::move(b));
            continue;
        }

        // heading
        if (t[0] == L'#') {
            int lvl = 0;
            while (lvl < (int)t.size() && t[lvl] == L'#') lvl++;
            if (lvl >= 1 && lvl <= 6 && lvl < (int)t.size() && t[lvl] == L' ') {
                flushParagraph(para);
                Block b; b.type = BlockType::Heading; b.level = lvl;
                b.runs = ParseInline(trim(t.substr(lvl + 1)));
                doc.blocks.push_back(std::move(b));
                i++; continue;
            }
        }

        // horizontal rule (--- *** ___), 3+ of same char, nothing else
        if (t.size() >= 3) {
            wchar_t c0 = t[0];
            if (c0 == L'-' || c0 == L'*' || c0 == L'_') {
                bool allSame = true;
                for (wchar_t c : t) { if (c != c0 && c != L' ') { allSame = false; break; } }
                int count = 0; for (wchar_t c : t) if (c == c0) count++;
                if (allSame && count >= 3) {
                    flushParagraph(para);
                    Block b; b.type = BlockType::HRule;
                    doc.blocks.push_back(std::move(b));
                    i++; continue;
                }
            }
        }

        // blockquote
        if (t[0] == L'>') {
            flushParagraph(para);
            std::wstring quote;
            while (i < N) {
                std::wstring ql = trim(rtrim(lines[i]));
                if (ql.empty() || ql[0] != L'>') break;
                std::wstring inner = ql.substr(1);
                if (!inner.empty() && inner[0] == L' ') inner = inner.substr(1);
                if (!quote.empty()) quote += L" ";
                quote += inner;
                i++;
            }
            Block b; b.type = BlockType::BlockQuote;
            b.runs = ParseInline(trim(quote));
            doc.blocks.push_back(std::move(b));
            continue;
        }

        // table: current line has '|' and next line is a separator
        if (line.find(L'|') != std::wstring::npos && i + 1 < N) {
            std::vector<int> aligns;
            if (isTableSeparator(lines[i+1], &aligns)) {
                flushParagraph(para);
                int startLine = (int)i;
                Block b; b.type = BlockType::Table; b.aligns = aligns;
                auto hdr = splitCells(t);
                for (auto& h : hdr) { TableCell c; c.runs = ParseInline(h); b.headers.push_back(std::move(c)); }
                i += 2;
                while (i < N) {
                    std::wstring rl = rtrim(lines[i]);
                    if (trim(rl).empty() || rl.find(L'|') == std::wstring::npos) break;
                    auto cells = splitCells(trim(rl));
                    TableRow row;
                    for (auto& cc : cells) { TableCell c; c.runs = ParseInline(cc); row.cells.push_back(std::move(c)); }
                    b.rows.push_back(std::move(row));
                    i++;
                }
                b.srcStartLine = startLine;
                b.srcEndLine = (int)i - 1;
                doc.blocks.push_back(std::move(b));
                continue;
            }
        }

        // list item (unordered -,*,+  or ordered N. )
        {
            int indent = leadingSpaces(line);
            std::wstring s = ltrim(line);
            bool isUl = (s.size() >= 2 && (s[0] == L'-' || s[0] == L'*' || s[0] == L'+') && s[1] == L' ');
            bool isOl = false;
            size_t olStart = 0;
            if (!isUl) {
                size_t d = 0; while (d < s.size() && iswdigit(s[d])) d++;
                if (d > 0 && d + 1 < s.size() && s[d] == L'.' && s[d+1] == L' ') { isOl = true; olStart = d + 2; }
            }
            if (isUl || isOl) {
                flushParagraph(para);
                Block b; b.type = BlockType::ListItem;
                b.ordered = isOl;
                b.level = indent / 2;
                std::wstring content = isUl ? s.substr(2) : s.substr(olStart);
                // task list
                std::wstring ct = ltrim(content);
                if (ct.rfind(L"[ ]", 0) == 0) { b.taskState = 0; content = ct.substr(3); }
                else if (ct.rfind(L"[x]", 0) == 0 || ct.rfind(L"[X]", 0) == 0) { b.taskState = 1; content = ct.substr(3); }
                b.runs = ParseInline(trim(content));
                doc.blocks.push_back(std::move(b));
                i++; continue;
            }
        }

        // default: paragraph line (accumulate)
        if (!para.empty()) para += L" ";
        para += t;
        i++;
    }
    flushParagraph(para);
    return doc;
}
