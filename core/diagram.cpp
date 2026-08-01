#include "diagram.h"
#include <cmath>
#include <algorithm>

namespace fmdv {

// ---------------------------------------------------------------- parsing ----

namespace {

Char lower(Char c) { return (c >= U16('A') && c <= U16('Z')) ? (Char)(c - U16('A') + U16('a')) : c; }

Str trim(const Str& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == U16(' ') || s[a] == U16('\t') || s[a] == U16('\r'))) a++;
    while (b > a && (s[b-1] == U16(' ') || s[b-1] == U16('\t') || s[b-1] == U16('\r'))) b--;
    return s.substr(a, b - a);
}

Str lowerStr(const Str& s) { Str o; o.reserve(s.size()); for (Char c : s) o += lower(c); return o; }

std::vector<Str> splitLines(const Str& text) {
    std::vector<Str> out; Str cur;
    for (Char c : text) { if (c == U16('\n')) { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur);
    return out;
}

// First whitespace-delimited token of a line, lowercased.
Str firstWord(const Str& line) {
    Str t = trim(line); Str w;
    for (Char c : t) { if (c == U16(' ') || c == U16('\t')) break; w += lower(c); }
    return w;
}

// Parse a leading decimal number; returns false if none.
bool parseNumber(const Str& s, double& out) {
    Str t = trim(s);
    if (t.empty()) return false;
    size_t i = 0; bool neg = false;
    if (t[i] == U16('-')) { neg = true; i++; } else if (t[i] == U16('+')) i++;
    double val = 0; bool any = false;
    while (i < t.size() && t[i] >= U16('0') && t[i] <= U16('9')) { val = val * 10 + (t[i] - U16('0')); i++; any = true; }
    if (i < t.size() && t[i] == U16('.')) {
        i++; double f = 0.1;
        while (i < t.size() && t[i] >= U16('0') && t[i] <= U16('9')) { val += (t[i] - U16('0')) * f; f *= 0.1; i++; any = true; }
    }
    if (!any) return false;
    out = neg ? -val : val;
    return true;
}

bool isComment(const Str& line) {
    Str t = trim(line);
    return t.size() >= 2 && t[0] == U16('%') && t[1] == U16('%');
}

// ---- pie ----
// pie [showData] [title <text>]
//     title <text>
//     "label" : value
Diagram parsePie(const std::vector<Str>& lines) {
    Diagram d; d.kind = DiagramKind::Pie;
    // first line may carry "showData" and/or "title ..."
    {
        Str first = trim(lines[0]);
        Str rest = trim(first.substr(3)); // drop "pie"
        Str low = lowerStr(rest);
        if (low.compare(0, 8, U16("showdata")) == 0) {
            d.pie.showData = true;
            rest = trim(rest.substr(8));
            low = lowerStr(rest);
        }
        if (low.compare(0, 5, U16("title")) == 0) d.pie.title = trim(rest.substr(5));
    }
    for (size_t li = 1; li < lines.size(); li++) {
        Str line = trim(lines[li]);
        if (line.empty() || isComment(line)) continue;
        if (lowerStr(line).compare(0, 5, U16("title")) == 0 && (line.size() == 5 || line[5] == U16(' '))) {
            d.pie.title = trim(line.substr(5));
            continue;
        }
        // "label" : value
        if (line[0] == U16('"')) {
            size_t close = line.find(U16('"'), 1);
            if (close == Str::npos) continue;
            Str label = line.substr(1, close - 1);
            size_t colon = line.find(U16(':'), close + 1);
            if (colon == Str::npos) continue;
            double v = 0;
            if (!parseNumber(line.substr(colon + 1), v)) continue;
            d.pie.slices.push_back(PieSlice{ label, v });
        }
    }
    if (d.pie.slices.empty()) d.kind = DiagramKind::None; // nothing to draw -> fall back
    return d;
}

// ---- sequence ----
// Detect an arrow token and return its length; sets dashed. Longest match first.
int arrowAt(const Str& s, size_t i, bool& dashed) {
    // ordered so longer/dashed variants win over their prefixes
    struct A { const char* t; bool dash; };
    static const A arr[] = {
        {"-->>", true}, {"->>", false}, {"-->", true}, {"->", false},
        {"--x", true}, {"-x", false}, {"--)", true}, {"-)", false},
    };
    for (const A& a : arr) {
        size_t n = 0; while (a.t[n]) n++;
        if (i + n <= s.size()) {
            bool eq = true;
            for (size_t k = 0; k < n; k++) if (s[i + k] != (Char)a.t[k]) { eq = false; break; }
            if (eq) { dashed = a.dash; return (int)n; }
        }
    }
    return 0;
}

Diagram parseSequence(const std::vector<Str>& lines) {
    Diagram d; d.kind = DiagramKind::Sequence;
    auto actorIndex = [&](const Str& id, const Str& label) -> int {
        for (size_t i = 0; i < d.seq.actors.size(); i++) if (d.seq.actors[i].id == id) {
            if (!label.empty() && d.seq.actors[i].label == d.seq.actors[i].id) d.seq.actors[i].label = label;
            return (int)i;
        }
        d.seq.actors.push_back(SeqActor{ id, label.empty() ? id : label });
        return (int)d.seq.actors.size() - 1;
    };
    auto blockStart = [&](SeqBlock b, const Str& label) {
        SeqEvent e; e.kind = SeqEventKind::BlockStart; e.block = b; e.text = label; d.seq.events.push_back(e);
    };
    for (size_t li = 1; li < lines.size(); li++) {
        Str line = trim(lines[li]);
        if (line.empty() || isComment(line)) continue;
        Str fw = firstWord(line);

        if (fw == U16("autonumber")) { d.seq.autonumber = true; continue; }
        if (fw == U16("participant") || fw == U16("actor")) {
            Str rest = trim(line.substr(fw.size()));
            Str id = rest, label;
            size_t asPos = lowerStr(rest).find(U16(" as "));
            if (asPos != Str::npos) { id = trim(rest.substr(0, asPos)); label = trim(rest.substr(asPos + 4)); }
            if (!id.empty()) actorIndex(id, label);
            continue;
        }
        if (fw == U16("activate") || fw == U16("deactivate")) {
            int a = actorIndex(trim(line.substr(fw.size())), Str());
            SeqEvent e; e.kind = SeqEventKind::Message; e.from = a; e.to = a; e.text = Str();
            e.activate = (fw == U16("activate")) ? 2 : -2; // 2/-2 = standalone (no arrow drawn)
            d.seq.events.push_back(e); continue;
        }
        if (fw == U16("loop")) { blockStart(SeqBlock::Loop, trim(line.substr(4))); continue; }
        if (fw == U16("opt"))  { blockStart(SeqBlock::Opt,  trim(line.substr(3))); continue; }
        if (fw == U16("par"))  { blockStart(SeqBlock::Par,  trim(line.substr(3))); continue; }
        if (fw == U16("alt"))  { blockStart(SeqBlock::Alt,  trim(line.substr(3))); continue; }
        if (fw == U16("else")) { SeqEvent e; e.kind = SeqEventKind::BlockElse; e.text = trim(line.substr(4)); d.seq.events.push_back(e); continue; }
        if (fw == U16("end"))  { SeqEvent e; e.kind = SeqEventKind::BlockEnd; d.seq.events.push_back(e); continue; }
        if (fw == U16("note")) {
            Str rest = trim(line.substr(4)); Str low = lowerStr(rest);
            int pos = 0; size_t skip = 0;
            if (low.compare(0, 9, U16("right of ")) == 0) { pos = 1; skip = 9; }
            else if (low.compare(0, 8, U16("left of ")) == 0) { pos = -1; skip = 8; }
            else if (low.compare(0, 5, U16("over ")) == 0) { pos = 0; skip = 5; }
            else continue;
            Str body2 = trim(rest.substr(skip));
            size_t colon = body2.find(U16(':'));
            Str who = (colon == Str::npos) ? body2 : trim(body2.substr(0, colon));
            Str text = (colon == Str::npos) ? Str() : trim(body2.substr(colon + 1));
            size_t comma = who.find(U16(','));
            int a0 = actorIndex(comma == Str::npos ? who : trim(who.substr(0, comma)), Str());
            int a1 = (comma == Str::npos) ? a0 : actorIndex(trim(who.substr(comma + 1)), Str());
            SeqEvent e; e.kind = SeqEventKind::Note; e.from = std::min(a0, a1); e.to = std::max(a0, a1);
            e.text = text; e.notePos = pos; d.seq.events.push_back(e); continue;
        }

        // message: <src> <arrow>[+/-] <dst> [: text]
        bool dashed = false; size_t apos = Str::npos; int alen = 0;
        for (size_t i = 0; i < line.size(); i++) {
            bool dh = false; int nn = arrowAt(line, i, dh);
            if (nn) { apos = i; alen = nn; dashed = dh; break; }
        }
        if (apos == Str::npos) continue;
        Str src = trim(line.substr(0, apos));
        Str after = line.substr(apos + alen);
        int activate = 0;
        { size_t j = 0; while (j < after.size() && after[j] == U16(' ')) j++;
          if (j < after.size() && after[j] == U16('+')) { activate = 1; after = after.substr(j + 1); }
          else if (j < after.size() && after[j] == U16('-')) { activate = -1; after = after.substr(j + 1); } }
        Str dst = after, text;
        size_t colon = after.find(U16(':'));
        if (colon != Str::npos) { dst = trim(after.substr(0, colon)); text = trim(after.substr(colon + 1)); }
        else dst = trim(after);
        if (src.empty() || dst.empty()) continue;
        SeqEvent e; e.kind = SeqEventKind::Message;
        e.from = actorIndex(src, Str()); e.to = actorIndex(dst, Str());
        e.text = text; e.dashed = dashed; e.activate = activate;
        d.seq.events.push_back(e);
    }
    if (d.seq.actors.empty()) d.kind = DiagramKind::None;
    return d;
}

// ---- flowchart ----
bool isIdChar(Char c) {
    return (c >= U16('a') && c <= U16('z')) || (c >= U16('A') && c <= U16('Z')) ||
           (c >= U16('0') && c <= U16('9')) || c == U16('_');
}
bool isLinkChar(Char c) {
    return c == U16('-') || c == U16('.') || c == U16('=') || c == U16('>') || c == U16('<') ||
           c == U16('o') || c == U16('x');
}

// Fold Mermaid <br> / <br/> / <br /> line breaks to '\n' and trim.
Str normalizeLabel(const Str& s) {
    Str out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == U16('<') && i + 2 < s.size() &&
            (s[i+1] == U16('b') || s[i+1] == U16('B')) && (s[i+2] == U16('r') || s[i+2] == U16('R'))) {
            size_t k = i + 3; while (k < s.size() && s[k] != U16('>')) k++;
            if (k < s.size()) { out += U16('\n'); i = k + 1; continue; }
        }
        out += s[i++];
    }
    return trim(out);
}

// Parse a node reference at s[i]: an id plus an optional shape+label. Handles the
// multi-char shape delimiters, longest opener first:
//   A  A[rect]  A(round)  A([stadium])  A[[subroutine]]  A[(cylinder)]
//   A((circle))  A{diamond}  A{{hexagon}}
Str parseNodeRef(const Str& s, size_t& i, NodeShape& shape, Str& label, bool& hasShape) {
    while (i < s.size() && (s[i] == U16(' ') || s[i] == U16('\t'))) i++;
    Str id;
    while (i < s.size() && isIdChar(s[i])) id += s[i++];
    hasShape = false;
    if (id.empty() || i >= s.size()) return id;

    struct Delim { const char* open; const char* close; NodeShape shape; };
    static const Delim delims[] = {
        {"([", "])", NodeShape::Stadium},    {"[[", "]]", NodeShape::Subroutine},
        {"[(", ")]", NodeShape::Cylinder},   {"((", "))", NodeShape::Circle},
        {"{{", "}}", NodeShape::Hexagon},
        {"[",  "]",  NodeShape::Rect},       {"(",  ")",  NodeShape::Round},
        {"{",  "}",  NodeShape::Diamond},
    };
    for (const Delim& d : delims) {
        size_t ol = 0; while (d.open[ol]) ol++;
        bool match = true;
        for (size_t k = 0; k < ol; k++) if (i + k >= s.size() || s[i + k] != (Char)d.open[k]) { match = false; break; }
        if (!match) continue;
        size_t start = i + ol;
        Str closeStr; for (size_t k = 0; d.close[k]; k++) closeStr += (Char)d.close[k];
        size_t found = s.find(closeStr, start);
        Str lab = s.substr(start, (found == Str::npos ? s.size() : found) - start);
        i = (found == Str::npos) ? s.size() : found + closeStr.size();
        shape = d.shape; label = normalizeLabel(lab); hasShape = true;
        return id;
    }
    return id; // bare reference, no shape
}

Diagram parseFlowchart(const std::vector<Str>& lines) {
    Diagram d; d.kind = DiagramKind::Flowchart;
    // header: "graph TD" / "flowchart LR" ...
    {
        Str first = trim(lines[0]);
        // drop the keyword
        size_t k = 0; while (k < first.size() && isIdChar(first[k])) k++;
        Str dir = lowerStr(trim(first.substr(k)));
        if (dir.compare(0, 2, U16("lr")) == 0) { d.flow.horizontal = true; }
        else if (dir.compare(0, 2, U16("rl")) == 0) { d.flow.horizontal = true; d.flow.reverse = true; }
        else if (dir.compare(0, 2, U16("bt")) == 0) { d.flow.reverse = true; }
        // td / tb / (default) -> vertical, not reversed
    }
    auto nodeIndex = [&](const Str& id, bool hasShape, NodeShape shape, const Str& label) -> int {
        for (size_t n = 0; n < d.flow.nodes.size(); n++) if (d.flow.nodes[n].id == id) {
            if (hasShape) { d.flow.nodes[n].shape = shape; if (!label.empty()) d.flow.nodes[n].label = label; }
            return (int)n;
        }
        FlowNode fn; fn.id = id; fn.shape = hasShape ? shape : NodeShape::Rect;
        fn.label = (hasShape && !label.empty()) ? label : id;
        d.flow.nodes.push_back(fn);
        return (int)d.flow.nodes.size() - 1;
    };
    for (size_t li = 1; li < lines.size(); li++) {
        Str line = trim(lines[li]);
        if (line.empty() || isComment(line)) continue;
        Str fw = firstWord(line);
        // structural keywords we don't lay out are skipped (degrade gracefully)
        if (fw == U16("subgraph") || fw == U16("end") || fw == U16("direction") ||
            fw == U16("style") || fw == U16("classdef") || fw == U16("class") ||
            fw == U16("click") || fw == U16("linkstyle")) continue;

        size_t i = 0;
        NodeShape sh = NodeShape::Rect; Str lab; bool has = false;
        Str src = parseNodeRef(line, i, sh, lab, has);
        if (src.empty()) continue;
        int from = nodeIndex(src, has, sh, lab);
        // chain: src (op [label]) dst (op [label]) dst ...
        bool any = false;
        while (true) {
            while (i < line.size() && (line[i] == U16(' ') || line[i] == U16('\t'))) i++;
            if (i >= line.size() || !isLinkChar(line[i])) break;
            size_t opStart = i;
            while (i < line.size() && isLinkChar(line[i])) i++;
            Str op = line.substr(opStart, i - opStart);
            bool dashed = op.find(U16('.')) != Str::npos;
            bool arrow = op.find(U16('>')) != Str::npos || op.find(U16('x')) != Str::npos || op.find(U16('o')) != Str::npos;
            // optional |label|
            Str elabel;
            while (i < line.size() && (line[i] == U16(' ') || line[i] == U16('\t'))) i++;
            if (i < line.size() && line[i] == U16('|')) {
                i++; size_t bar = line.find(U16('|'), i);
                if (bar != Str::npos) { elabel = trim(line.substr(i, bar - i)); i = bar + 1; }
            }
            NodeShape sh2 = NodeShape::Rect; Str lab2; bool has2 = false;
            Str dst = parseNodeRef(line, i, sh2, lab2, has2);
            if (dst.empty()) break;
            int to = nodeIndex(dst, has2, sh2, lab2);
            d.flow.edges.push_back(FlowEdge{ from, to, elabel, arrow, dashed });
            from = to; any = true;
        }
        (void)any;
    }
    if (d.flow.nodes.empty()) d.kind = DiagramKind::None;
    return d;
}

// ---- state diagram (stateDiagram / stateDiagram-v2) ----
// Maps onto the flowchart model: states are rounded nodes, transitions are
// edges, and [*] is a start (as source) or end (as target) pseudo-state.
Diagram parseState(const std::vector<Str>& lines) {
    Diagram d; d.kind = DiagramKind::Flowchart; d.flow.horizontal = false;
    auto stateIndex = [&](const Str& token, bool isTarget) -> int {
        Str id = token;
        NodeShape shape = NodeShape::Round;
        if (token == U16("[*]")) { id = isTarget ? U16("[*]__end") : U16("[*]__start"); shape = isTarget ? NodeShape::DotRing : NodeShape::Dot; }
        for (size_t k = 0; k < d.flow.nodes.size(); k++) if (d.flow.nodes[k].id == id) return (int)k;
        FlowNode fn; fn.id = id; fn.shape = shape; fn.label = (token == U16("[*]")) ? Str() : id;
        d.flow.nodes.push_back(fn);
        return (int)d.flow.nodes.size() - 1;
    };
    for (size_t li = 1; li < lines.size(); li++) {
        Str line = trim(lines[li]);
        if (line.empty() || isComment(line)) continue;
        Str fw = firstWord(line);
        if (fw == U16("direction")) {
            Str dir = lowerStr(trim(line.substr(9)));
            if (dir.compare(0, 2, U16("lr")) == 0) d.flow.horizontal = true;
            else if (dir.compare(0, 2, U16("rl")) == 0) { d.flow.horizontal = true; d.flow.reverse = true; }
            else if (dir.compare(0, 2, U16("bt")) == 0) d.flow.reverse = true;
            continue;
        }
        if (fw == U16("state")) {
            Str rest = trim(line.substr(5));
            if (!rest.empty() && rest[0] == U16('"')) { // state "desc" as id
                size_t close = rest.find(U16('"'), 1);
                if (close == Str::npos) continue;
                Str desc = rest.substr(1, close - 1);
                size_t asPos = lowerStr(rest).find(U16(" as "), close);
                if (asPos != Str::npos) { Str id = trim(rest.substr(asPos + 4)); int k = stateIndex(id, false); d.flow.nodes[k].label = desc; }
            } else { // state id  (ignore composite "{" bodies)
                Str id; for (Char c : rest) { if (c == U16(' ') || c == U16('\t') || c == U16('{')) break; id += c; }
                if (!id.empty()) stateIndex(id, false);
            }
            continue;
        }
        if (fw == U16("note") || fw == U16("end")) continue;

        size_t ar = line.find(U16("-->"));
        if (ar == Str::npos) continue;
        Str src = trim(line.substr(0, ar));
        Str after = line.substr(ar + 3);
        Str dst = after, label;
        size_t colon = after.find(U16(':'));
        if (colon != Str::npos) { dst = trim(after.substr(0, colon)); label = trim(after.substr(colon + 1)); }
        else dst = trim(after);
        // take the first whitespace-delimited token as the state name
        auto firstToken = [](const Str& s) { Str t; for (Char c : s) { if (c == U16(' ') || c == U16('\t')) break; t += c; } return t; };
        src = firstToken(src); dst = firstToken(dst);
        if (src.empty() || dst.empty()) continue;
        int from = stateIndex(src, false), to = stateIndex(dst, true);
        d.flow.edges.push_back(FlowEdge{ from, to, label, true, false });
    }
    if (d.flow.nodes.empty()) d.kind = DiagramKind::None;
    return d;
}

// ---- class diagram (classDiagram) ----
// Classes become multi-compartment boxes (name + members); relationships become
// edges with UML end markers (triangle/diamond/arrow), mapped onto the shared
// flowchart layered layout.
Diagram parseClass(const std::vector<Str>& lines) {
    Diagram d; d.kind = DiagramKind::Flowchart; d.flow.horizontal = false;
    auto classIndex = [&](const Str& id) -> int {
        for (size_t k = 0; k < d.flow.nodes.size(); k++) if (d.flow.nodes[k].id == id) return (int)k;
        FlowNode fn; fn.id = id; fn.shape = NodeShape::Class; fn.label = id;
        d.flow.nodes.push_back(fn);
        return (int)d.flow.nodes.size() - 1;
    };
    auto stripQuoted = [](const Str& s) { Str o; bool q = false; for (Char c : s) { if (c == U16('"')) { q = !q; continue; } if (!q) o += c; } return o; };
    auto firstId = [](const Str& s) { Str t; size_t i = 0; while (i < s.size() && !isIdChar(s[i])) i++; while (i < s.size() && isIdChar(s[i])) t += s[i++]; return t; };
    auto lastId = [](const Str& s) { Str t; size_t i = s.size(); while (i > 0 && !isIdChar(s[i-1])) i--; size_t e = i; while (i > 0 && isIdChar(s[i-1])) i--; return s.substr(i, e - i); };

    int curClass = -1;
    for (size_t li = 1; li < lines.size(); li++) {
        Str line = trim(lines[li]);
        if (line.empty() || isComment(line)) continue;
        if (curClass >= 0) { // inside a class { } body
            if (line[0] == U16('}')) { curClass = -1; continue; }
            d.flow.nodes[curClass].members.push_back(line);
            continue;
        }
        Str fw = firstWord(line);
        if (fw == U16("direction")) { Str dir = lowerStr(trim(line.substr(9))); if (dir.compare(0,2,U16("lr"))==0) d.flow.horizontal = true; continue; }
        if (fw == U16("class")) {
            Str rest = trim(line.substr(5));
            Str id; for (Char c : rest) { if (!isIdChar(c)) break; id += c; }
            if (id.empty()) continue;
            int k = classIndex(id);
            if (rest.find(U16('{')) != Str::npos && rest.find(U16('}')) == Str::npos) curClass = k; // open body
            continue;
        }
        if (fw == U16("note") || fw == U16("namespace") || fw == U16("end")) continue;

        // relationship: find the connector run ("--" or "..") with optional markers
        size_t op0 = Str::npos;
        for (size_t i = 0; i + 1 < line.size(); i++)
            if ((line[i] == U16('-') && line[i+1] == U16('-')) || (line[i] == U16('.') && line[i+1] == U16('.'))) { op0 = i; break; }
        if (op0 != Str::npos) {
            size_t l = op0; while (l > 0 && (line[l-1]==U16('<')||line[l-1]==U16('|')||line[l-1]==U16('*')||line[l-1]==U16('o'))) l--;
            size_t r = op0 + 2; while (r < line.size() && (line[r]==U16('|')||line[r]==U16('>')||line[r]==U16('*')||line[r]==U16('o'))) r++;
            Str op = line.substr(l, r - l);
            bool dashed = op.find(U16('.')) != Str::npos;
            int tail = 0, head = 0;
            if (op.size() >= 2 && op[0]==U16('<') && op[1]==U16('|')) tail = 2;
            else if (op[0]==U16('<')) tail = 1; else if (op[0]==U16('*')) tail = 3; else if (op[0]==U16('o')) tail = 4;
            size_t z = op.size();
            if (z >= 2 && op[z-1]==U16('>') && op[z-2]==U16('|')) head = 2;
            else if (op[z-1]==U16('>')) head = 1; else if (op[z-1]==U16('*')) head = 3; else if (op[z-1]==U16('o')) head = 4;
            Str A = line.substr(0, l), after = line.substr(r), label;
            size_t colon = after.find(U16(':'));
            if (colon != Str::npos) { label = trim(after.substr(colon + 1)); after = after.substr(0, colon); }
            Str aid = lastId(stripQuoted(A)), bid = firstId(stripQuoted(after));
            if (aid.empty() || bid.empty()) continue;
            int from = classIndex(aid), to = classIndex(bid);
            d.flow.edges.push_back(FlowEdge{ from, to, label, false, dashed, head, tail });
            continue;
        }
        // member declaration outside a body:  ClassName : +member
        size_t colon = line.find(U16(':'));
        if (colon != Str::npos) {
            Str who = firstId(line);
            if (!who.empty()) { int k = classIndex(who); Str mem = trim(line.substr(colon + 1)); if (!mem.empty()) d.flow.nodes[k].members.push_back(mem); }
        }
    }
    if (d.flow.nodes.empty()) d.kind = DiagramKind::None;
    return d;
}

// ---- user journey ----
// journey / title / section <name> / <task>: <score>: <actor,actor>
Diagram parseJourney(const std::vector<Str>& lines) {
    Diagram d; d.kind = DiagramKind::Journey;
    int curSection = -1;
    for (size_t li = 1; li < lines.size(); li++) {
        Str line = trim(lines[li]);
        if (line.empty() || isComment(line)) continue;
        Str fw = firstWord(line);
        if (fw == U16("title")) { d.journey.title = trim(line.substr(5)); continue; }
        if (fw == U16("section")) { d.journey.sections.push_back(trim(line.substr(7))); curSection = (int)d.journey.sections.size() - 1; continue; }
        // task:  name : score : actorA, actorB
        size_t c1 = line.find(U16(':'));
        if (c1 == Str::npos) continue;
        JourneyTask t; t.section = curSection;
        t.name = trim(line.substr(0, c1));
        Str rest = line.substr(c1 + 1);
        size_t c2 = rest.find(U16(':'));
        Str scoreStr = (c2 == Str::npos) ? rest : rest.substr(0, c2);
        double sc = 3; parseNumber(scoreStr, sc); t.score = (int)(sc + 0.5);
        if (t.score < 1) t.score = 1;
        if (t.score > 5) t.score = 5;
        if (c2 != Str::npos) {
            Str who = rest.substr(c2 + 1);
            Str cur;
            for (Char ch : who) { if (ch == U16(',')) { Str a = trim(cur); if (!a.empty()) t.actors.push_back(a); cur.clear(); } else cur += ch; }
            Str a = trim(cur); if (!a.empty()) t.actors.push_back(a);
        }
        if (!t.name.empty()) d.journey.tasks.push_back(t);
    }
    if (d.journey.tasks.empty()) d.kind = DiagramKind::None;
    return d;
}

} // namespace

Diagram ParseDiagram(const Str& body) {
    std::vector<Str> lines = splitLines(body);
    // first non-empty, non-comment line selects the diagram type
    size_t first = 0;
    while (first < lines.size()) {
        Str t = trim(lines[first]);
        if (!t.empty() && !isComment(t)) break;
        first++;
    }
    if (first >= lines.size()) return Diagram{};
    std::vector<Str> body2(lines.begin() + first, lines.end());
    Str kw = firstWord(body2[0]);
    if (kw == U16("pie")) return parsePie(body2);
    if (kw == U16("sequencediagram")) return parseSequence(body2);
    if (kw == U16("graph") || kw == U16("flowchart")) return parseFlowchart(body2);
    if (kw.compare(0, 12, U16("statediagram")) == 0) return parseState(body2);
    if (kw == U16("classdiagram")) return parseClass(body2);
    if (kw == U16("journey")) return parseJourney(body2);
    return Diagram{}; // gantt/er/gitgraph/unknown -> None (fall back to code)
}

// ----------------------------------------------------------------- layout ----

namespace {

double S(double scale, double v) { return std::floor(v * scale + 0.5); }

FontSpec bodyFont() { return FontSpec{ FontRole::Body, false, false, RoleSizePx(FontRole::Body) }; }
FontSpec boldFont() { return FontSpec{ FontRole::Body, true,  false, RoleSizePx(FontRole::Body) }; }

void fillR(LayoutResult& o, RectF r, Color c) {
    DrawCommand d; d.kind = DrawCommand::FillRect; d.rect = r; d.color = c; o.cmds.push_back(std::move(d));
}
void frameR(LayoutResult& o, RectF r, Color c) {
    DrawCommand d; d.kind = DrawCommand::FrameRect; d.rect = r; d.color = c; o.cmds.push_back(std::move(d));
}
void lineC(LayoutResult& o, double x1, double y1, double x2, double y2, Color c) {
    DrawCommand d; d.kind = DrawCommand::Line; d.rect = { x1, y1, x2, y2 }; d.color = c; o.cmds.push_back(std::move(d));
}
void polyC(LayoutResult& o, std::vector<PointF> pts, Color c) {
    DrawCommand d; d.kind = DrawCommand::FillPolygon; d.points = std::move(pts); d.color = c; o.cmds.push_back(std::move(d));
}
void textC(LayoutResult& o, double x, double baseline, double w, double h, const Str& s, const FontSpec& f, Color c) {
    DrawCommand d; d.kind = DrawCommand::Text; d.rect = { x, baseline, w, h };
    d.text = s; d.font = f; d.color = c; d.selectable = false; // diagram labels aren't part of text selection
    o.cmds.push_back(std::move(d));
}
// dashed horizontal line as short segments
void dashH(LayoutResult& o, double x1, double x2, double y, double dash, Color c) {
    double dir = (x2 >= x1) ? 1 : -1, x = x1;
    while ((dir > 0 && x < x2) || (dir < 0 && x > x2)) {
        double xe = x + dir * dash;
        if ((dir > 0 && xe > x2) || (dir < 0 && xe < x2)) xe = x2;
        lineC(o, x, y, xe, y, c);
        x = xe + dir * dash; // gap
    }
}
// split a label on '\n', measure its widest line, and draw it centered
std::vector<Str> labelLines(const Str& s) {
    std::vector<Str> v; Str cur;
    for (Char c : s) { if (c == U16('\n')) { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur); return v;
}
double widestLine(TextMeasurer& tm, const FontSpec& f, const std::vector<Str>& lines) {
    double w = 0; for (const auto& ln : lines) w = std::max(w, tm.textWidth(f, ln)); return w;
}
void drawLabelBlock(LayoutResult& o, const std::vector<Str>& lines, double cx, double blockTop,
                    double fh, double asc, const FontSpec& f, Color c, TextMeasurer& tm) {
    double y = blockTop;
    for (const auto& ln : lines) { double w = tm.textWidth(f, ln); textC(o, cx - w / 2, y + asc, w, fh, ln, f, c); y += fh; }
}

// A qualitative palette (GitHub-ish), cycled across pie slices.
const Color PIE_PALETTE[] = {
    {0x54,0x70,0xc6}, {0x91,0xcc,0x75}, {0xfa,0xc8,0x58}, {0xee,0x66,0x66},
    {0x73,0xc0,0xde}, {0x3b,0xa2,0x72}, {0xfc,0x84,0x52}, {0x9a,0x60,0xb4},
    {0xea,0x7c,0xcc}, {0x84,0x8a,0x94},
};
Color pieColor(size_t i) { return PIE_PALETTE[i % (sizeof(PIE_PALETTE) / sizeof(PIE_PALETTE[0]))]; }

double layoutPie(const Pie& pie, double width, const LayoutTheme& th, TextMeasurer& tm,
                 double scale, double ox, double oy, LayoutResult& out) {
    FontSpec body = bodyFont(), bold = boldFont();
    double fh = tm.lineHeight(body), asc = tm.ascent(body);
    double pad = S(scale, 16);
    double y = oy + pad;

    if (!pie.title.empty()) {
        double w = tm.textWidth(bold, pie.title);
        textC(out, ox + (width - w) / 2, y + asc, w, fh, pie.title, bold, th.text);
        y += fh + S(scale, 12);
    }

    double total = 0; for (const auto& s : pie.slices) total += (s.value > 0 ? s.value : 0);
    if (total <= 0) total = 1;

    // circle on the left, legend column on the right
    double diam = width - S(scale, 200);
    double dmax = S(scale, 240), dmin = S(scale, 120);
    if (diam > dmax) diam = dmax;
    if (diam < dmin) diam = dmin;
    double R = diam / 2;
    double cx = ox + pad + R, cy = y + R;

    double a0 = -90.0; // start at 12 o'clock
    for (size_t i = 0; i < pie.slices.size(); i++) {
        double frac = (pie.slices[i].value > 0 ? pie.slices[i].value : 0) / total;
        double sweep = frac * 360.0;
        double a1 = a0 + sweep;
        std::vector<PointF> pts;
        pts.push_back(PointF{ cx, cy });
        int steps = (int)std::ceil(sweep / 4.0); if (steps < 1) steps = 1;
        for (int k = 0; k <= steps; k++) {
            double a = (a0 + (a1 - a0) * (double)k / steps) * 3.14159265358979 / 180.0;
            pts.push_back(PointF{ cx + R * std::cos(a), cy + R * std::sin(a) });
        }
        polyC(out, std::move(pts), pieColor(i));
        a0 = a1;
    }

    // legend
    double lx = cx + R + S(scale, 28);
    double ly = y;
    double sw = S(scale, 13);
    for (size_t i = 0; i < pie.slices.size(); i++) {
        fillR(out, { lx, ly + (fh - sw) / 2, sw, sw }, pieColor(i));
        Str lbl = pie.slices[i].label;
        if (pie.showData) {
            double pct = (pie.slices[i].value > 0 ? pie.slices[i].value : 0) / total * 100.0;
            lbl += U16(" (") + toStr((long)(pct + 0.5)) + U16("%)");
        }
        double tw = tm.textWidth(body, lbl);
        textC(out, lx + sw + S(scale, 8), ly + asc, tw, fh, lbl, body, th.text);
        ly += fh + S(scale, 7);
    }

    double bottom = std::max(cy + R, ly);
    return (bottom - oy) + pad;
}

double layoutSequence(const Sequence& seq, double width, const LayoutTheme& th, TextMeasurer& tm,
                      double scale, double ox, double oy, LayoutResult& out) {
    int n = (int)seq.actors.size();
    if (n == 0) return 0;
    FontSpec body = bodyFont(), bold = boldFont();
    double fh = tm.lineHeight(body), asc = tm.ascent(body);
    double pad = S(scale, 16), boxPadX = S(scale, 12), boxH = fh + S(scale, 12);
    double ah = S(scale, 6), notePad = S(scale, 6), barW = S(scale, 8);

    double maxLabel = 0;
    for (const auto& a : seq.actors) maxLabel = std::max(maxLabel, tm.textWidth(bold, a.label));
    double colW = (width - 2 * pad) / n;
    double minCol = maxLabel + 2 * boxPadX + S(scale, 30);
    if (colW < minCol) colW = minCol;
    auto colX = [&](int i) { return ox + pad + colW * i + colW / 2; };

    double top = oy + pad;
    double firstMsg = top + boxH + S(scale, 34);
    double msgGap = fh + S(scale, 22);
    auto textLines = [](const Str& s) { int l = 1; for (Char c : s) if (c == U16('\n')) l++; return l; };

    // Single walk: messages/notes/dividers go to the foreground list `fg`; block
    // frames and activation bars are collected and emitted behind everything.
    LayoutResult fg;
    struct Bar { int actor; double y0, y1; int depth; };
    struct Frame { SeqBlock block; Str label; double y0, y1, x0, x1; };
    std::vector<Bar> bars; std::vector<Frame> frames;
    std::vector<std::vector<double>> actStack(n);
    std::vector<size_t> openBlocks; // indices into frames
    int number = 0;

    auto frameBounds = [&](int depth, double& fx0, double& fx1) {
        double m = std::max(S(scale, 12), S(scale, 34) - depth * S(scale, 8));
        fx0 = colX(0) - m; fx1 = colX(n - 1) + m;
    };

    double my = firstMsg;
    for (const auto& e : seq.events) {
        switch (e.kind) {
        case SeqEventKind::Message: {
            // activation toggles (from +/- shorthand or activate/deactivate)
            if (e.activate == 1 || e.activate == 2) actStack[e.to].push_back(my);
            if (e.activate == -1 || e.activate == -2) {
                int a = (e.activate == -1) ? e.from : e.to;
                if (!actStack[a].empty()) { double y0 = actStack[a].back(); actStack[a].pop_back(); bars.push_back({ a, y0, my, (int)actStack[a].size() }); }
            }
            if (e.activate == 2 || e.activate == -2) break; // standalone (no arrow), no advance
            Str label = e.text;
            if (seq.autonumber) { number++; label = toStr(number) + U16(" ") + label; }
            if (e.from == e.to) {
                double x = colX(e.from), loopW = S(scale, 34), loopH = S(scale, 16);
                if (!label.empty()) { double tw = tm.textWidth(body, label); textC(fg, x + loopW + S(scale, 6), my + asc, tw, fh, label, body, th.text); }
                double yb = my + loopH;
                lineC(fg, x, my, x + loopW, my, th.text);
                lineC(fg, x + loopW, my, x + loopW, yb, th.text);
                if (e.dashed) dashH(fg, x + loopW, x, yb, S(scale, 5), th.text);
                else lineC(fg, x + loopW, yb, x, yb, th.text);
                polyC(fg, { { x, yb }, { x + ah, yb - ah * 0.7 }, { x + ah, yb + ah * 0.7 } }, th.text);
                my += msgGap + S(scale, 16);
            } else {
                double xf = colX(e.from), xt = colX(e.to), dir = (xt > xf) ? 1 : -1;
                if (!label.empty()) { double tw = tm.textWidth(body, label); textC(fg, (xf + xt) / 2 - tw / 2, my - S(scale, 6) + asc - fh, tw, fh, label, body, th.text); }
                if (e.dashed) dashH(fg, xf, xt - dir * ah, my, S(scale, 5), th.text);
                else lineC(fg, xf, my, xt - dir * ah, my, th.text);
                polyC(fg, { { xt, my }, { xt - dir * ah, my - ah * 0.7 }, { xt - dir * ah, my + ah * 0.7 } }, th.text);
                my += msgGap;
            }
            break;
        }
        case SeqEventKind::Note: {
            std::vector<Str> lines = labelLines(e.text);
            double tw = widestLine(tm, body, lines), nh = lines.size() * fh + 2 * notePad;
            double nx0, nx1;
            if (e.notePos > 0)      { nx0 = colX(e.to) + S(scale, 10); nx1 = nx0 + tw + 2 * notePad; }
            else if (e.notePos < 0) { nx1 = colX(e.from) - S(scale, 10); nx0 = nx1 - tw - 2 * notePad; }
            else { // over
                double cxc = (colX(e.from) + colX(e.to)) / 2, halfw = std::max(tw / 2 + notePad, (colX(e.to) - colX(e.from)) / 2 + colW * 0.3);
                nx0 = cxc - halfw; nx1 = cxc + halfw;
            }
            fillR(fg, { nx0, my, nx1 - nx0, nh }, th.bg3);
            frameR(fg, { nx0, my, nx1 - nx0, nh }, th.border);
            drawLabelBlock(fg, lines, (nx0 + nx1) / 2, my + notePad, fh, asc, body, th.text, tm);
            my += nh + S(scale, 12);
            break;
        }
        case SeqEventKind::BlockStart: {
            double fx0, fx1; frameBounds((int)openBlocks.size(), fx0, fx1);
            frames.push_back({ e.block, e.text, my, my, fx0, fx1 });
            openBlocks.push_back(frames.size() - 1);
            my += 2 * fh + S(scale, 8); // label row + clearance for the first message's text
            break;
        }
        case SeqEventKind::BlockElse: {
            if (!openBlocks.empty()) {
                const Frame& f = frames[openBlocks.back()];
                dashH(fg, f.x0, f.x1, my, S(scale, 5), th.border);
                Str lbl = U16("[") + e.text + U16("]");
                textC(fg, f.x0 + S(scale, 8), my + asc + S(scale, 2), tm.textWidth(body, lbl), fh, lbl, body, th.text2);
            }
            my += 2 * fh + S(scale, 6);
            break;
        }
        case SeqEventKind::BlockEnd: {
            if (!openBlocks.empty()) { frames[openBlocks.back()].y1 = my; openBlocks.pop_back(); }
            my += S(scale, 12);
            break;
        }
        }
    }
    double bottom = my + S(scale, 6);
    // close anything left open at the bottom
    for (int a = 0; a < n; a++) for (double y0 : actStack[a]) bars.push_back({ a, y0, bottom - S(scale, 6), 0 });
    while (!openBlocks.empty()) { frames[openBlocks.back()].y1 = bottom - S(scale, 6); openBlocks.pop_back(); }

    // ---- assemble: lifelines, frames, bars, actor boxes, then foreground ----
    for (int i = 0; i < n; i++) { double x = colX(i); lineC(out, x, top + boxH, x, bottom, th.border); }
    auto blockWord = [](SeqBlock b) {
        switch (b) { case SeqBlock::Loop: return U16("loop"); case SeqBlock::Alt: return U16("alt");
                     case SeqBlock::Opt: return U16("opt"); default: return U16("par"); }
    };
    for (const auto& f : frames) {
        frameR(out, { f.x0, f.y0, f.x1 - f.x0, f.y1 - f.y0 }, th.border);
        Str w = blockWord(f.block);
        double tabW = tm.textWidth(bold, w) + S(scale, 10);
        fillR(out, { f.x0, f.y0, tabW, fh + S(scale, 4) }, th.bg2);
        frameR(out, { f.x0, f.y0, tabW, fh + S(scale, 4) }, th.border);
        textC(out, f.x0 + S(scale, 5), f.y0 + asc + S(scale, 2), tm.textWidth(bold, w), fh, w, bold, th.text2);
        if (!f.label.empty()) {
            Str lbl = U16("[") + f.label + U16("]");
            textC(out, f.x0 + tabW + S(scale, 6), f.y0 + asc + S(scale, 2), tm.textWidth(body, lbl), fh, lbl, body, th.text2);
        }
    }
    for (const auto& b : bars) {
        double x = colX(b.actor) + b.depth * (barW - S(scale, 2));
        fillR(out, { x - barW / 2, b.y0, barW, std::max(b.y1 - b.y0, S(scale, 6)) }, th.bg2);
        frameR(out, { x - barW / 2, b.y0, barW, std::max(b.y1 - b.y0, S(scale, 6)) }, th.border);
    }
    for (int i = 0; i < n; i++) {
        double cx = colX(i), w = tm.textWidth(bold, seq.actors[i].label), bw = w + 2 * boxPadX;
        fillR(out, { cx - bw / 2, top, bw, boxH }, th.bg2);
        frameR(out, { cx - bw / 2, top, bw, boxH }, th.border);
        textC(out, cx - w / 2, top + (boxH - fh) / 2 + asc, w, fh, seq.actors[i].label, bold, th.text);
    }
    out.cmds.insert(out.cmds.end(), fg.cmds.begin(), fg.cmds.end());
    (void)textLines;
    return (bottom - oy) + pad;
}

// exit point on an axis-aligned box (center cx,cy, half-sizes hw,hh) along the
// ray toward (tx,ty) -- where an edge should meet the node border.
PointF boxEdge(double cx, double cy, double hw, double hh, double tx, double ty) {
    double dx = tx - cx, dy = ty - cy;
    if (dx == 0 && dy == 0) return PointF{ cx, cy };
    double sx = (dx != 0) ? hw / std::fabs(dx) : 1e18;
    double sy = (dy != 0) ? hh / std::fabs(dy) : 1e18;
    double t = std::min(sx, sy);
    return PointF{ cx + dx * t, cy + dy * t };
}
// filled triangular arrowhead: tip at P, pointing along unit vector (ux,uy).
void arrowHead(LayoutResult& o, PointF P, double ux, double uy, double len, double half, Color c) {
    double bx = P.x - ux * len, by = P.y - uy * len;
    double px = -uy, py = ux; // perpendicular
    polyC(o, { P, { bx + px * half, by + py * half }, { bx - px * half, by - py * half } }, c);
}
double markerLen(int m, double scale) { return m == 0 ? 0.0 : m == 1 ? S(scale, 9) : m == 2 ? S(scale, 13) : S(scale, 16); }
void outlinePoly(LayoutResult& o, const std::vector<PointF>& p, Color c) {
    for (size_t k = 0; k < p.size(); k++) { const PointF& A = p[k]; const PointF& B = p[(k + 1) % p.size()]; lineC(o, A.x, A.y, B.x, B.y, c); }
}
// UML relationship end markers. tip at P; (ux,uy) points INTO the node.
void drawMarker(LayoutResult& o, int m, PointF P, double ux, double uy, double scale, const LayoutTheme& th) {
    double px = -uy, py = ux;
    if (m == 1) { arrowHead(o, P, ux, uy, S(scale, 9), S(scale, 5), th.text2); return; }
    if (m == 2) { // hollow triangle (inheritance / realization)
        double len = S(scale, 13), hw = S(scale, 8), bx = P.x - ux * len, by = P.y - uy * len;
        std::vector<PointF> tri = { P, { bx + px * hw, by + py * hw }, { bx - px * hw, by - py * hw } };
        polyC(o, tri, th.bg); outlinePoly(o, tri, th.text2);
        return;
    }
    if (m == 3 || m == 4) { // diamond: 3 filled (composition), 4 open (aggregation)
        double len = S(scale, 16), hw = S(scale, 6);
        PointF mid = { P.x - ux * (len / 2), P.y - uy * (len / 2) }, tail = { P.x - ux * len, P.y - uy * len };
        std::vector<PointF> di = { P, { mid.x + px * hw, mid.y + py * hw }, tail, { mid.x - px * hw, mid.y - py * hw } };
        if (m == 3) polyC(o, di, th.text2);
        else { polyC(o, di, th.bg); outlinePoly(o, di, th.text2); }
    }
}

// rounded-rect outline polygon (round / stadium / circle share this).
std::vector<PointF> roundRectPoly(double x, double y, double w, double h, double r) {
    r = std::min(r, std::min(w / 2, h / 2));
    std::vector<PointF> p; int seg = 5;
    auto arc = [&](double cx, double cy, double a0, double a1) {
        for (int k = 0; k <= seg; k++) { double a = (a0 + (a1 - a0) * k / seg) * 3.14159265358979 / 180.0; p.push_back({ cx + r * std::cos(a), cy + r * std::sin(a) }); }
    };
    arc(x + r, y + r, 180, 270); arc(x + w - r, y + r, 270, 360);
    arc(x + w - r, y + h - r, 0, 90); arc(x + r, y + h - r, 90, 180);
    return p;
}
void strokePoly(LayoutResult& o, const std::vector<PointF>& p, Color c) {
    for (size_t k = 0; k < p.size(); k++) { const PointF& A = p[k]; const PointF& B = p[(k + 1) % p.size()]; lineC(o, A.x, A.y, B.x, B.y, c); }
}
// Draw a node's shape (fill bg2 + border) at rect r.
void drawNodeShape(LayoutResult& o, NodeShape shape, const RectF& r, double scale, const LayoutTheme& th) {
    double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    switch (shape) {
        case NodeShape::Diamond: {
            std::vector<PointF> dia = { { cx, r.y }, { r.x + r.w, cy }, { cx, r.y + r.h }, { r.x, cy } };
            polyC(o, dia, th.bg2); strokePoly(o, dia, th.border); break;
        }
        case NodeShape::Hexagon: {
            double hx = std::min(r.h / 2, r.w / 4);
            std::vector<PointF> hex = { { r.x + hx, r.y }, { r.x + r.w - hx, r.y }, { r.x + r.w, cy },
                                        { r.x + r.w - hx, r.y + r.h }, { r.x + hx, r.y + r.h }, { r.x, cy } };
            polyC(o, hex, th.bg2); strokePoly(o, hex, th.border); break;
        }
        case NodeShape::Round: {
            auto p = roundRectPoly(r.x, r.y, r.w, r.h, S(scale, 8)); polyC(o, p, th.bg2); strokePoly(o, p, th.border); break;
        }
        case NodeShape::Stadium: case NodeShape::Circle: {
            auto p = roundRectPoly(r.x, r.y, r.w, r.h, r.h / 2); polyC(o, p, th.bg2); strokePoly(o, p, th.border); break;
        }
        case NodeShape::Dot: { // state start: filled dot
            auto p = roundRectPoly(r.x, r.y, r.w, r.h, r.h / 2); polyC(o, p, th.text); break;
        }
        case NodeShape::DotRing: { // state end: ring around a filled dot
            auto outer = roundRectPoly(r.x, r.y, r.w, r.h, r.h / 2); polyC(o, outer, th.bg); strokePoly(o, outer, th.text);
            double in = r.w * 0.3; auto inner = roundRectPoly(r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in, (r.h - 2 * in) / 2);
            polyC(o, inner, th.text); break;
        }
        case NodeShape::Subroutine: {
            fillR(o, r, th.bg2); frameR(o, r, th.border);
            double inset = S(scale, 6);
            lineC(o, r.x + inset, r.y, r.x + inset, r.y + r.h, th.border);
            lineC(o, r.x + r.w - inset, r.y, r.x + r.w - inset, r.y + r.h, th.border);
            break;
        }
        case NodeShape::Cylinder: {
            double cap = std::min(S(scale, 8), r.h / 4);
            fillR(o, r, th.bg2); frameR(o, r, th.border);
            // suggest a lid: a shallow arc line near the top
            std::vector<PointF> lid;
            for (int k = 0; k <= 8; k++) { double t = k / 8.0; lid.push_back({ r.x + t * r.w, r.y + cap + (0.5 - std::fabs(t - 0.5)) * cap }); }
            for (size_t k = 1; k < lid.size(); k++) lineC(o, lid[k-1].x, lid[k-1].y, lid[k].x, lid[k].y, th.border);
            break;
        }
        default: /* Rect */ fillR(o, r, th.bg2); frameR(o, r, th.border); break;
    }
}

double layoutFlowchart(const Flowchart& fc, double width, const LayoutTheme& th, TextMeasurer& tm,
                       double scale, double ox, double oy, LayoutResult& out) {
    int n = (int)fc.nodes.size();
    if (n == 0) return 0;
    FontSpec body = bodyFont(), bold = boldFont();
    double fh = tm.lineHeight(body), asc = tm.ascent(body);
    double pad = S(scale, 16), padX = S(scale, 14), padY = S(scale, 9);
    double rankGap = S(scale, 46), sibGap = S(scale, 26);

    // ranks: BFS depth from roots (nodes with no incoming edge). Shortest-path
    // depth stays compact and is inherently cycle-safe (a visited node keeps its
    // rank), so a back edge — e.g. a retry loop — doesn't inflate the layout the
    // way longest-path would. Edge-crossing minimization is intentionally out of
    // scope (issue #16, naive layered layout).
    std::vector<int> rank(n, -1), indeg(n, 0);
    for (const auto& e : fc.edges) if (e.from != e.to) indeg[e.to]++;
    std::vector<int> q;
    for (int k = 0; k < n; k++) if (indeg[k] == 0) { rank[k] = 0; q.push_back(k); }
    size_t qi = 0;
    auto bfs = [&]() {
        while (qi < q.size()) {
            int u = q[qi++];
            for (const auto& e : fc.edges)
                if (e.from == u && e.to != u && rank[e.to] < 0) { rank[e.to] = rank[u] + 1; q.push_back(e.to); }
        }
    };
    bfs();
    // components with no root (pure cycles) or disconnected nodes: seed and continue
    for (int k = 0; k < n; k++) if (rank[k] < 0) { rank[k] = 0; q.push_back(k); bfs(); }
    int maxRank = 0; for (int r : rank) maxRank = std::max(maxRank, r);

    // per-node sizes (multi-line labels + shape padding)
    std::vector<double> nodeW(n), nodeH(n);
    for (int k = 0; k < n; k++) {
        std::vector<Str> lines = labelLines(fc.nodes[k].label);
        double tw = widestLine(tm, body, lines);
        double w = tw + 2 * padX, h = lines.size() * fh + 2 * padY;
        switch (fc.nodes[k].shape) {
            case NodeShape::Diamond:    w += S(scale, 30); h += S(scale, 14); break;
            case NodeShape::Hexagon:    w += S(scale, 26); break;
            case NodeShape::Stadium:    w += h; break; // room for the semicircle ends
            case NodeShape::Subroutine: w += S(scale, 18); break;
            case NodeShape::Cylinder:   h += S(scale, 10); break;
            case NodeShape::Circle: { double d = std::max(tw, (double)lines.size() * fh) + 2 * padX + S(scale, 8); w = h = d; break; }
            case NodeShape::Dot: case NodeShape::DotRing: { double d = S(scale, 15); w = h = d; break; }
            case NodeShape::Class: {
                double nameW = tm.textWidth(bold, fc.nodes[k].label), mw = 0;
                for (const auto& m : fc.nodes[k].members) mw = std::max(mw, tm.textWidth(body, m));
                w = std::max(nameW, mw) + 2 * padX;
                h = fh + 2 * padY;
                if (!fc.nodes[k].members.empty()) h += fc.nodes[k].members.size() * fh + 2 * padY;
                break;
            }
            default: break;
        }
        nodeW[k] = w; nodeH[k] = h;
    }

    // display rank (BT/RL reverse the axis)
    std::vector<int> dr(n);
    for (int k = 0; k < n; k++) dr[k] = fc.reverse ? (maxRank - rank[k]) : rank[k];
    int R = maxRank;

    // ---- layered graph with dummy nodes for edges that span >1 rank ----
    // A long edge becomes a chain through one dummy per intermediate rank, so
    // it routes through the gaps between ranks (its label lands there too, not
    // on a node) and ordering can treat every segment as rank-adjacent.
    struct LN { int rank; int real; double x = 0, y = 0; }; // real < 0 => dummy
    std::vector<LN> ln;
    std::vector<int> realL(n);
    for (int k = 0; k < n; k++) { realL[k] = (int)ln.size(); ln.push_back({ dr[k], k }); }
    std::vector<std::vector<int>> chain(fc.edges.size());
    std::vector<char> skip(fc.edges.size(), 0);
    for (size_t ei = 0; ei < fc.edges.size(); ei++) {
        const FlowEdge& e = fc.edges[ei];
        if (e.from == e.to) { skip[ei] = 1; continue; }
        int r0 = dr[e.from], r1 = dr[e.to];
        std::vector<int>& ch = chain[ei];
        ch.push_back(realL[e.from]);
        int span = (r0 > r1) ? (r0 - r1) : (r1 - r0);
        if (span > 1) { int step = (r1 > r0) ? 1 : -1; for (int r = r0 + step; r != r1; r += step) { ch.push_back((int)ln.size()); ln.push_back({ r, -1 }); } }
        ch.push_back(realL[e.to]);
    }

    std::vector<std::vector<int>> layer(R + 1);
    for (int i = 0; i < (int)ln.size(); i++) layer[ln[i].rank].push_back(i);
    std::vector<std::vector<int>> adj(ln.size());
    for (auto& ch : chain) for (size_t j = 0; j + 1 < ch.size(); j++) { adj[ch[j]].push_back(ch[j+1]); adj[ch[j+1]].push_back(ch[j]); }

    // ---- crossing reduction: median-heuristic ordering sweeps ----
    std::vector<int> posIn(ln.size(), 0);
    for (auto& lay : layer) for (int p = 0; p < (int)lay.size(); p++) posIn[lay[p]] = p;
    auto median = [&](int node, int fromRank) -> double {
        std::vector<int> ps;
        for (int nb : adj[node]) if (ln[nb].rank == fromRank) ps.push_back(posIn[nb]);
        if (ps.empty()) return -1.0;
        std::sort(ps.begin(), ps.end());
        size_t m = ps.size();
        return (m % 2) ? (double)ps[m/2] : (ps[m/2 - 1] + ps[m/2]) / 2.0;
    };
    auto reorder = [&](int r, int fromRank) {
        std::vector<int>& lay = layer[r];
        std::vector<std::pair<double,int>> keyed;
        for (int nd : lay) { double md = median(nd, fromRank); keyed.push_back({ md < 0 ? (double)posIn[nd] : md, nd }); }
        std::stable_sort(keyed.begin(), keyed.end(), [](const std::pair<double,int>& a, const std::pair<double,int>& b){ return a.first < b.first; });
        for (int p = 0; p < (int)lay.size(); p++) { lay[p] = keyed[p].second; posIn[lay[p]] = p; }
    };
    for (int sweep = 0; sweep < 4; sweep++) {
        if (sweep % 2 == 0) for (int r = 1; r <= R; r++) reorder(r, r - 1);
        else                for (int r = R - 1; r >= 0; r--) reorder(r, r + 1);
    }

    // ---- coordinate assignment ----
    double dummyLane = S(scale, 12);
    auto lnW = [&](int i) { return ln[i].real >= 0 ? nodeW[ln[i].real] : dummyLane; };
    auto lnH = [&](int i) { return ln[i].real >= 0 ? nodeH[ln[i].real] : dummyLane; };
    std::vector<RectF> box(n);

    if (!fc.horizontal) {
        std::vector<double> rowH(R + 1, fh);
        for (int i = 0; i < (int)ln.size(); i++) if (ln[i].real >= 0) rowH[ln[i].rank] = std::max(rowH[ln[i].rank], nodeH[ln[i].real]);
        std::vector<double> layW(R + 1, 0); double diagW = 0;
        for (int r = 0; r <= R; r++) { double w = 0; for (int nd : layer[r]) w += lnW(nd); if (!layer[r].empty()) w += sibGap * (layer[r].size() - 1); layW[r] = w; diagW = std::max(diagW, w); }
        double centerX = ox + std::max(pad + diagW / 2, width / 2);
        double y = oy + pad;
        for (int r = 0; r <= R; r++) {
            double x = centerX - layW[r] / 2;
            for (int nd : layer[r]) { double w = lnW(nd); ln[nd].x = x + w / 2; ln[nd].y = y + rowH[r] / 2; x += w + sibGap; }
            y += rowH[r] + rankGap;
        }
    } else {
        std::vector<double> colW(R + 1, fh);
        for (int i = 0; i < (int)ln.size(); i++) if (ln[i].real >= 0) colW[ln[i].rank] = std::max(colW[ln[i].rank], nodeW[ln[i].real]);
        std::vector<double> layH(R + 1, 0); double diagH = 0;
        for (int r = 0; r <= R; r++) { double h = 0; for (int nd : layer[r]) h += lnH(nd); if (!layer[r].empty()) h += sibGap * (layer[r].size() - 1); layH[r] = h; diagH = std::max(diagH, h); }
        double x = ox + pad;
        for (int r = 0; r <= R; r++) {
            double y = oy + pad + (diagH - layH[r]) / 2;
            for (int nd : layer[r]) { double h = lnH(nd); ln[nd].x = x + colW[r] / 2; ln[nd].y = y + h / 2; y += h + sibGap; }
            x += colW[r] + rankGap;
        }
    }
    for (int i = 0; i < (int)ln.size(); i++) if (ln[i].real >= 0) { int k = ln[i].real; box[k] = RectF{ ln[i].x - nodeW[k]/2, ln[i].y - nodeH[k]/2, nodeW[k], nodeH[k] }; }

    // ---- edges: polyline through the chain waypoints, with end markers ----
    auto unit = [](double dx, double dy) { double l = std::sqrt(dx * dx + dy * dy); if (l < 1) l = 1; return PointF{ dx / l, dy / l }; };
    for (size_t ei = 0; ei < fc.edges.size(); ei++) {
        if (skip[ei]) continue;
        const FlowEdge& e = fc.edges[ei];
        const std::vector<int>& ch = chain[ei];
        std::vector<PointF> wp; for (int idx : ch) wp.push_back({ ln[idx].x, ln[idx].y });
        const RectF& a = box[e.from]; const RectF& b = box[e.to];
        wp.front() = boxEdge(a.x + a.w/2, a.y + a.h/2, a.w/2, a.h/2, wp[1].x, wp[1].y);
        wp.back()  = boxEdge(b.x + b.w/2, b.y + b.h/2, b.w/2, b.h/2, wp[wp.size()-2].x, wp[wp.size()-2].y);
        PointF headTip = wp.back(), tailTip = wp.front();
        PointF uh = unit(headTip.x - wp[wp.size()-2].x, headTip.y - wp[wp.size()-2].y);
        PointF ut = unit(tailTip.x - wp[1].x, tailTip.y - wp[1].y);
        int head = (e.headMarker >= 0) ? e.headMarker : (e.arrow ? 1 : 0);
        int tail = (e.headMarker >= 0) ? e.tailMarker : 0;
        if (head) { double hL = markerLen(head, scale); wp.back()  = { headTip.x - uh.x * hL, headTip.y - uh.y * hL }; }
        if (tail) { double tL = markerLen(tail, scale); wp.front() = { tailTip.x - ut.x * tL, tailTip.y - ut.y * tL }; }
        for (size_t j = 0; j + 1 < wp.size(); j++) {
            if (e.dashed) {
                double sx = wp[j].x, sy = wp[j].y, dxx = wp[j+1].x - sx, dyy = wp[j+1].y - sy, L = std::sqrt(dxx*dxx + dyy*dyy);
                if (L < 1) L = 1;
                double vx = dxx/L, vy = dyy/L, dash = S(scale, 5), t = 0;
                while (t < L) { double t2 = std::min(t + dash, L); lineC(out, sx + vx*t, sy + vy*t, sx + vx*t2, sy + vy*t2, th.text2); t = t2 + dash; }
            } else lineC(out, wp[j].x, wp[j].y, wp[j+1].x, wp[j+1].y, th.text2);
        }
        if (head) drawMarker(out, head, headTip, uh.x, uh.y, scale, th);
        if (tail) drawMarker(out, tail, tailTip, ut.x, ut.y, scale, th);
        if (!e.label.empty()) {
            size_t mid = wp.size() / 2;
            PointF A = wp[mid - 1], B = wp[mid];
            double mx = (A.x + B.x) / 2, my = (A.y + B.y) / 2, lw = tm.textWidth(body, e.label);
            fillR(out, { mx - lw/2 - S(scale, 3), my - fh/2, lw + 2*S(scale, 3), fh }, th.bg);
            textC(out, mx - lw/2, my - fh/2 + asc, lw, fh, e.label, body, th.text2);
        }
    }

    // ---- nodes ----
    for (int k = 0; k < n; k++) {
        const RectF& r = box[k];
        if (fc.nodes[k].shape == NodeShape::Class) {
            fillR(out, r, th.bg2); frameR(out, r, th.border);
            double nameW = tm.textWidth(bold, fc.nodes[k].label);
            textC(out, r.x + (r.w - nameW) / 2, r.y + padY + asc, nameW, fh, fc.nodes[k].label, bold, th.text);
            if (!fc.nodes[k].members.empty()) {
                double divY = r.y + fh + 2 * padY;
                lineC(out, r.x, divY, r.x + r.w, divY, th.border);
                double my2 = divY + padY;
                for (const auto& m : fc.nodes[k].members) { double mw = tm.textWidth(body, m); textC(out, r.x + padX, my2 + asc, mw, fh, m, body, th.text); my2 += fh; }
            }
            continue;
        }
        drawNodeShape(out, fc.nodes[k].shape, r, scale, th);
        std::vector<Str> lines = labelLines(fc.nodes[k].label);
        double blockTop = r.y + (r.h - lines.size() * fh) / 2;
        drawLabelBlock(out, lines, r.x + r.w / 2, blockTop, fh, asc, body, th.text, tm);
    }

    double bottom = oy;
    for (int k = 0; k < n; k++) bottom = std::max(bottom, box[k].y + box[k].h);
    return (bottom - oy) + pad;
}

double layoutJourney(const Journey& j, double width, const LayoutTheme& th, TextMeasurer& tm,
                     double scale, double ox, double oy, LayoutResult& out) {
    FontSpec body = bodyFont(), bold = boldFont();
    double fh = tm.lineHeight(body), asc = tm.ascent(body);
    double pad = S(scale, 16), padX = S(scale, 12), gap = S(scale, 18), dotD = S(scale, 16);
    // score 1..5 -> red..green (index 0 unused)
    const Color sc[6] = { {0,0,0}, {0xe0,0x6b,0x6b}, {0xef,0xa8,0x5c}, {0xf2,0xcf,0x5a}, {0x9c,0xcc,0x65}, {0x5a,0xb5,0x6a} };

    double y = oy + pad;
    if (!j.title.empty()) { double w = tm.textWidth(bold, j.title); textC(out, ox + (width - w) / 2, y + asc, w, fh, j.title, bold, th.text); y += fh + gap; }
    double sectionY = y, taskY = y + fh + S(scale, 8);

    int n = (int)j.tasks.size();
    auto actorsStr = [](const JourneyTask& t) { Str s; for (size_t a = 0; a < t.actors.size(); a++) { if (a) s += U16(", "); s += t.actors[a]; } return s; };
    std::vector<double> tw(n); double taskH = fh + S(scale, 8) + dotD;
    bool anyActors = false;
    for (int i = 0; i < n; i++) { if (!j.tasks[i].actors.empty()) anyActors = true; }
    if (anyActors) taskH += S(scale, 4) + fh;
    for (int i = 0; i < n; i++) {
        double w = std::max(tm.textWidth(bold, j.tasks[i].name), tm.textWidth(body, actorsStr(j.tasks[i])));
        tw[i] = std::max(w, dotD) + 2 * padX;
    }

    std::vector<double> bx(n);
    std::vector<std::pair<double,double>> span(j.sections.size(), { 1e18, -1e18 });
    double x = ox + pad; int prevSec = -999;
    for (int i = 0; i < n; i++) {
        if (prevSec != -999 && j.tasks[i].section != prevSec) x += gap;
        bx[i] = x;
        int s = j.tasks[i].section;
        if (s >= 0 && s < (int)span.size()) { span[s].first = std::min(span[s].first, x); span[s].second = std::max(span[s].second, x + tw[i]); }
        x += tw[i] + S(scale, 10);
        prevSec = j.tasks[i].section;
    }

    // section bands (behind) + titles
    for (size_t s = 0; s < j.sections.size(); s++) {
        if (span[s].second < span[s].first) continue;
        double bx0 = span[s].first - S(scale, 6), bx1 = span[s].second + S(scale, 6);
        fillR(out, { bx0, sectionY, bx1 - bx0, (taskY + taskH + S(scale, 6)) - sectionY }, th.bg3);
        double lw = tm.textWidth(bold, j.sections[s]);
        textC(out, (bx0 + bx1) / 2 - lw / 2, sectionY + asc, lw, fh, j.sections[s], bold, th.text2);
    }

    // task cards
    for (int i = 0; i < n; i++) {
        const JourneyTask& t = j.tasks[i];
        RectF r{ bx[i], taskY, tw[i], taskH };
        auto rp = roundRectPoly(r.x, r.y, r.w, r.h, S(scale, 6)); polyC(out, rp, th.bg2); strokePoly(out, rp, th.border);
        double nameW = tm.textWidth(bold, t.name);
        textC(out, r.x + (r.w - nameW) / 2, r.y + S(scale, 5) + asc, nameW, fh, t.name, bold, th.text);
        // score dot with the number inside
        double dcx = r.x + r.w / 2, dcy = r.y + fh + S(scale, 8) + dotD / 2;
        auto dot = roundRectPoly(dcx - dotD / 2, dcy - dotD / 2, dotD, dotD, dotD / 2);
        polyC(out, dot, sc[t.score]);
        Str num = toStr(t.score); double nw = tm.textWidth(body, num);
        textC(out, dcx - nw / 2, dcy - fh / 2 + asc, nw, fh, num, body, th.text);
        Str who = actorsStr(t);
        if (!who.empty()) { double ww = tm.textWidth(body, who); textC(out, r.x + (r.w - ww) / 2, r.y + taskH - fh + asc - S(scale, 2), ww, fh, who, body, th.text2); }
    }
    return (taskY + taskH - oy) + pad;
}

} // namespace

double LayoutDiagram(const Diagram& d, double width, const LayoutTheme& th,
                     TextMeasurer& tm, double scale,
                     double originX, double originY, LayoutResult& out) {
    switch (d.kind) {
        case DiagramKind::Pie:       return layoutPie(d.pie, width, th, tm, scale, originX, originY, out);
        case DiagramKind::Sequence:  return layoutSequence(d.seq, width, th, tm, scale, originX, originY, out);
        case DiagramKind::Flowchart: return layoutFlowchart(d.flow, width, th, tm, scale, originX, originY, out);
        case DiagramKind::Journey:   return layoutJourney(d.journey, width, th, tm, scale, originX, originY, out);
        default:                     return 0;
    }
}

} // namespace fmdv
