#include "markdown.h"
#include <cwctype>

// ---------------- helpers ----------------

static Str rtrim(const Str& s) {
    size_t e = s.size();
    while (e > 0 && (s[e-1] == U16(' ') || s[e-1] == U16('\t') || s[e-1] == U16('\r'))) e--;
    return s.substr(0, e);
}
static Str ltrim(const Str& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == U16(' ') || s[b] == U16('\t'))) b++;
    return s.substr(b);
}
static Str trim(const Str& s) { return ltrim(rtrim(s)); }

static int leadingSpaces(const Str& s) {
    int n = 0;
    for (Char c : s) { if (c == U16(' ')) n++; else if (c == U16('\t')) n += 4; else break; }
    return n;
}

// Split text into lines (text already uses LF endings).
static std::vector<Str> splitLines(const Str& text) {
    std::vector<Str> lines;
    Str cur;
    for (Char c : text) {
        if (c == U16('\n')) { lines.push_back(cur); cur.clear(); }
        else if (c != U16('\r')) cur += c;
    }
    lines.push_back(cur);
    return lines;
}

// ---------------- inline parsing ----------------

static void appendRun(std::vector<InlineRun>& out, const Str& text,
                      bool b, bool i, bool c, bool s, const Str& href) {
    if (text.empty()) return;
    InlineRun r; r.text = text; r.bold = b; r.italic = i; r.code = c; r.strike = s; r.href = href;
    out.push_back(r);
}

// Find a matching closing delimiter `delim` starting at `from`. Returns npos if none.
static size_t findClose(const Str& s, const Str& delim, size_t from) {
    return s.find(delim, from);
}

// Recursive inline parser. Emits runs with the given active styles.
static void parseInlineInto(const Str& s, std::vector<InlineRun>& out,
                            bool b, bool i, bool c, bool strike, const Str& href,
                            int depth) {
    if (depth > 24) { appendRun(out, s, b, i, c, strike, href); return; }

    Str buf;
    size_t n = s.size();
    for (size_t p = 0; p < n; ) {
        Char ch = s[p];

        // inline code: highest priority, no nested formatting
        if (ch == U16('`')) {
            size_t close = s.find(U16('`'), p + 1);
            if (close != Str::npos) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                appendRun(out, s.substr(p + 1, close - p - 1), b, i, true, strike, href);
                p = close + 1; continue;
            }
        }

        // image ![alt](src) -> render alt text (no bitmap loading yet)
        if (ch == U16('!') && p + 1 < n && s[p+1] == U16('[')) {
            size_t rb = s.find(U16(']'), p + 2);
            if (rb != Str::npos && rb + 1 < n && s[rb+1] == U16('(')) {
                size_t rp = s.find(U16(')'), rb + 2);
                if (rp != Str::npos) {
                    appendRun(out, buf, b, i, c, strike, href); buf.clear();
                    Str alt = s.substr(p + 2, rb - p - 2);
                    appendRun(out, alt.empty() ? U16("[image]") : alt, b, i, c, strike, href);
                    p = rp + 1; continue;
                }
            }
        }

        // link [text](href)
        if (ch == U16('[')) {
            size_t rb = s.find(U16(']'), p + 1);
            if (rb != Str::npos && rb + 1 < n && s[rb+1] == U16('(')) {
                size_t rp = s.find(U16(')'), rb + 2);
                if (rp != Str::npos) {
                    appendRun(out, buf, b, i, c, strike, href); buf.clear();
                    Str inner = s.substr(p + 1, rb - p - 1);
                    Str url = s.substr(rb + 2, rp - rb - 2);
                    parseInlineInto(inner, out, b, i, c, strike, url, depth + 1);
                    p = rp + 1; continue;
                }
            }
        }

        // bold+italic ***
        if (ch == U16('*') && p + 2 < n && s[p+1] == U16('*') && s[p+2] == U16('*')) {
            size_t close = findClose(s, U16("***"), p + 3);
            if (close != Str::npos) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                parseInlineInto(s.substr(p + 3, close - p - 3), out, true, true, c, strike, href, depth + 1);
                p = close + 3; continue;
            }
        }
        // bold ** or __
        if ((ch == U16('*') && p + 1 < n && s[p+1] == U16('*')) ||
            (ch == U16('_') && p + 1 < n && s[p+1] == U16('_'))) {
            Str d(2, ch);
            size_t close = findClose(s, d, p + 2);
            if (close != Str::npos) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                parseInlineInto(s.substr(p + 2, close - p - 2), out, true, i, c, strike, href, depth + 1);
                p = close + 2; continue;
            }
        }
        // italic * or _
        if (ch == U16('*') || ch == U16('_')) {
            Str d(1, ch);
            size_t close = findClose(s, d, p + 1);
            if (close != Str::npos && close > p + 1) {
                appendRun(out, buf, b, i, c, strike, href); buf.clear();
                parseInlineInto(s.substr(p + 1, close - p - 1), out, b, true, c, strike, href, depth + 1);
                p = close + 1; continue;
            }
        }
        // strikethrough ~~
        if (ch == U16('~') && p + 1 < n && s[p+1] == U16('~')) {
            size_t close = findClose(s, U16("~~"), p + 2);
            if (close != Str::npos) {
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

std::vector<InlineRun> ParseInline(const Str& text) {
    std::vector<InlineRun> out;
    parseInlineInto(text, out, false, false, false, false, U16(""), 0);
    return out;
}

// ---------------- table detection ----------------

static std::vector<Str> splitCells(const Str& line) {
    // split on unescaped '|', trim, drop leading/trailing empties from border pipes
    std::vector<Str> cells;
    Str cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == U16('|') && (i == 0 || line[i-1] != U16('\\'))) {
            cells.push_back(trim(cur)); cur.clear();
        } else cur += line[i];
    }
    cells.push_back(trim(cur));
    if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
    if (!cells.empty() && cells.back().empty()) cells.pop_back();
    return cells;
}

std::vector<Str> SplitTableCells(const Str& line) { return splitCells(line); }

static bool isTableSeparator(const Str& line, std::vector<int>* aligns) {
    Str t = trim(line);
    if (t.find(U16('|')) == Str::npos && t.find(U16('-')) == Str::npos) return false;
    auto cells = splitCells(t);
    if (cells.empty()) return false;
    if (aligns) aligns->clear();
    for (auto& c : cells) {
        Str cc = trim(c);
        if (cc.empty()) return false;
        bool left = cc.front() == U16(':');
        bool right = cc.back() == U16(':');
        // middle must be all dashes
        size_t a = left ? 1 : 0, z = cc.size() - (right ? 1 : 0);
        if (z <= a) return false;
        for (size_t k = a; k < z; k++) if (cc[k] != U16('-')) return false;
        if (aligns) {
            if (left && right) aligns->push_back(AlignCenter);
            else if (right) aligns->push_back(AlignRight);
            else aligns->push_back(AlignLeft);
        }
    }
    return true;
}

// ---------------- block parsing ----------------

Document ParseMarkdown(const Str& text) {
    Document doc;
    auto lines = splitLines(text);
    size_t i = 0, N = lines.size();

    auto flushParagraph = [&](Str& acc) {
        if (trim(acc).empty()) { acc.clear(); return; }
        Block b; b.type = BlockType::Paragraph;
        b.runs = ParseInline(trim(acc));
        doc.blocks.push_back(std::move(b));
        acc.clear();
    };

    Str para;

    while (i < N) {
        const Str& raw = lines[i];
        Str line = rtrim(raw);
        Str t = trim(line);

        // blank line
        if (t.empty()) { flushParagraph(para); i++; continue; }

        // fenced code block
        if (t.rfind(U16("```"), 0) == 0 || t.rfind(U16("~~~"), 0) == 0) {
            flushParagraph(para);
            Str fence = t.substr(0, 3);
            Block b; b.type = BlockType::CodeBlock;
            b.lang = trim(t.substr(3));
            i++;
            Str code;
            while (i < N) {
                Str cl = rtrim(lines[i]);
                if (trim(cl).rfind(fence, 0) == 0 && trim(trim(cl).substr(3)).empty()) { i++; break; }
                if (!code.empty()) code += U16("\n");
                code += lines[i];
                i++;
            }
            b.codeText = code;
            doc.blocks.push_back(std::move(b));
            continue;
        }

        // heading
        if (t[0] == U16('#')) {
            int lvl = 0;
            while (lvl < (int)t.size() && t[lvl] == U16('#')) lvl++;
            if (lvl >= 1 && lvl <= 6 && lvl < (int)t.size() && t[lvl] == U16(' ')) {
                flushParagraph(para);
                Block b; b.type = BlockType::Heading; b.level = lvl;
                b.runs = ParseInline(trim(t.substr(lvl + 1)));
                doc.blocks.push_back(std::move(b));
                i++; continue;
            }
        }

        // horizontal rule (--- *** ___), 3+ of same char, nothing else
        if (t.size() >= 3) {
            Char c0 = t[0];
            if (c0 == U16('-') || c0 == U16('*') || c0 == U16('_')) {
                bool allSame = true;
                for (Char c : t) { if (c != c0 && c != U16(' ')) { allSame = false; break; } }
                int count = 0; for (Char c : t) if (c == c0) count++;
                if (allSame && count >= 3) {
                    flushParagraph(para);
                    Block b; b.type = BlockType::HRule;
                    doc.blocks.push_back(std::move(b));
                    i++; continue;
                }
            }
        }

        // blockquote
        if (t[0] == U16('>')) {
            flushParagraph(para);
            Str quote;
            while (i < N) {
                Str ql = trim(rtrim(lines[i]));
                if (ql.empty() || ql[0] != U16('>')) break;
                Str inner = ql.substr(1);
                if (!inner.empty() && inner[0] == U16(' ')) inner = inner.substr(1);
                if (!quote.empty()) quote += U16(" ");
                quote += inner;
                i++;
            }
            Block b; b.type = BlockType::BlockQuote;
            b.runs = ParseInline(trim(quote));
            doc.blocks.push_back(std::move(b));
            continue;
        }

        // table: current line has '|' and next line is a separator
        if (line.find(U16('|')) != Str::npos && i + 1 < N) {
            std::vector<int> aligns;
            if (isTableSeparator(lines[i+1], &aligns)) {
                flushParagraph(para);
                int startLine = (int)i;
                Block b; b.type = BlockType::Table; b.aligns = aligns;
                auto hdr = splitCells(t);
                for (auto& h : hdr) { TableCell c; c.runs = ParseInline(h); b.headers.push_back(std::move(c)); }
                i += 2;
                while (i < N) {
                    Str rl = rtrim(lines[i]);
                    if (trim(rl).empty() || rl.find(U16('|')) == Str::npos) break;
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
            Str s = ltrim(line);
            bool isUl = (s.size() >= 2 && (s[0] == U16('-') || s[0] == U16('*') || s[0] == U16('+')) && s[1] == U16(' '));
            bool isOl = false;
            size_t olStart = 0;
            if (!isUl) {
                size_t d = 0; while (d < s.size() && iswdigit(s[d])) d++;
                if (d > 0 && d + 1 < s.size() && s[d] == U16('.') && s[d+1] == U16(' ')) { isOl = true; olStart = d + 2; }
            }
            if (isUl || isOl) {
                flushParagraph(para);
                Block b; b.type = BlockType::ListItem;
                b.ordered = isOl;
                b.level = indent / 2;
                b.srcStartLine = (int)i; // lets the frontend rewrite this line (task toggle)
                Str content = isUl ? s.substr(2) : s.substr(olStart);
                // task list
                Str ct = ltrim(content);
                if (ct.rfind(U16("[ ]"), 0) == 0) { b.taskState = 0; content = ct.substr(3); }
                else if (ct.rfind(U16("[x]"), 0) == 0 || ct.rfind(U16("[X]"), 0) == 0) { b.taskState = 1; content = ct.substr(3); }
                b.runs = ParseInline(trim(content));
                doc.blocks.push_back(std::move(b));
                i++; continue;
            }
        }

        // default: paragraph line (accumulate)
        if (!para.empty()) para += U16(" ");
        para += t;
        i++;
    }
    flushParagraph(para);
    return doc;
}
