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
    for (size_t li = 1; li < lines.size(); li++) {
        Str line = trim(lines[li]);
        if (line.empty() || isComment(line)) continue;
        Str fw = firstWord(line);
        if (fw == U16("participant") || fw == U16("actor")) {
            Str rest = trim(line.substr(fw.size()));
            // "<id> as <label>"
            Str id = rest, label;
            Str lowRest = lowerStr(rest);
            size_t asPos = lowRest.find(U16(" as "));
            if (asPos != Str::npos) { id = trim(rest.substr(0, asPos)); label = trim(rest.substr(asPos + 4)); }
            if (!id.empty()) actorIndex(id, label);
            continue;
        }
        // message: <src> <arrow> <dst> [: text]
        bool dashed = false; size_t apos = Str::npos; int alen = 0;
        for (size_t i = 0; i < line.size(); i++) {
            bool dh = false; int n = arrowAt(line, i, dh);
            if (n) { apos = i; alen = n; dashed = dh; break; }
        }
        if (apos == Str::npos) continue; // not a message we understand
        Str src = trim(line.substr(0, apos));
        Str after = line.substr(apos + alen);
        Str dst = after, text;
        size_t colon = after.find(U16(':'));
        if (colon != Str::npos) { dst = trim(after.substr(0, colon)); text = trim(after.substr(colon + 1)); }
        else dst = trim(after);
        if (src.empty() || dst.empty()) continue;
        int fi = actorIndex(src, Str());
        int ti = actorIndex(dst, Str());
        d.seq.messages.push_back(SeqMessage{ fi, ti, text, dashed });
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

// Parse a node reference at s[i]: an id plus an optional shape+label:
//   A   A[rect]   A(round)   A([stadium])   A((circle))   A{diamond}
// Returns the id (empty if none) and advances i past the ref; shape/label are
// set when a bracketed form is present.
Str parseNodeRef(const Str& s, size_t& i, NodeShape& shape, Str& label, bool& hasShape) {
    while (i < s.size() && (s[i] == U16(' ') || s[i] == U16('\t'))) i++;
    Str id;
    while (i < s.size() && isIdChar(s[i])) id += s[i++];
    hasShape = false;
    if (id.empty() || i >= s.size()) return id;
    Char open = s[i];
    Char close = 0;
    if (open == U16('[')) { shape = NodeShape::Rect; close = U16(']'); }
    else if (open == U16('(')) { shape = NodeShape::Round; close = U16(')'); }
    else if (open == U16('{')) { shape = NodeShape::Diamond; close = U16('}'); }
    else return id; // bare reference
    i++;
    if (i < s.size() && s[i] == open) i++; // doubled: (( , ([ approximated by the first
    Str lab;
    while (i < s.size() && s[i] != close) lab += s[i++];
    if (i < s.size() && s[i] == close) i++;
    if (i < s.size() && s[i] == close) i++; // doubled close
    label = trim(lab); hasShape = true;
    return id;
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
    return Diagram{}; // gantt/class/state/unknown -> None (fall back to code)
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
    double pad = S(scale, 16);
    double boxPadX = S(scale, 12);
    double boxH = fh + S(scale, 12);

    double maxLabel = 0;
    for (const auto& a : seq.actors) { double w = tm.textWidth(bold, a.label); if (w > maxLabel) maxLabel = w; }
    double colW = (width - 2 * pad) / n;
    double minCol = maxLabel + 2 * boxPadX + S(scale, 24);
    if (colW < minCol) colW = minCol;
    auto colX = [&](int i) { return ox + pad + colW * i + colW / 2; };

    double top = oy + pad;
    double firstMsg = top + boxH + S(scale, 34);
    double msgGap = fh + S(scale, 22);

    // total lifeline length (accounting for self-message loops taking extra room)
    double y = firstMsg;
    for (const auto& m : seq.messages) y += (m.from == m.to) ? (msgGap + S(scale, 16)) : msgGap;
    double lifeBottom = y + S(scale, 6);

    // lifelines (behind boxes/messages)
    for (int i = 0; i < n; i++) { double x = colX(i); lineC(out, x, top + boxH, x, lifeBottom, th.border); }

    // actor boxes
    for (int i = 0; i < n; i++) {
        double cx = colX(i);
        double w = tm.textWidth(bold, seq.actors[i].label);
        double bw = w + 2 * boxPadX, bx = cx - bw / 2;
        fillR(out, { bx, top, bw, boxH }, th.bg2);
        frameR(out, { bx, top, bw, boxH }, th.border);
        textC(out, cx - w / 2, top + (boxH - fh) / 2 + asc, w, fh, seq.actors[i].label, bold, th.text);
    }

    // messages
    double ah = S(scale, 6);
    double my = firstMsg;
    for (const auto& m : seq.messages) {
        if (m.from == m.to) {
            double x = colX(m.from);
            double loopW = S(scale, 34), loopH = S(scale, 16);
            if (!m.text.empty()) {
                double tw = tm.textWidth(body, m.text);
                textC(out, x + loopW + S(scale, 6), my + asc, tw, fh, m.text, body, th.text);
            }
            double yb = my + loopH;
            lineC(out, x, my, x + loopW, my, th.text);
            lineC(out, x + loopW, my, x + loopW, yb, th.text);
            if (m.dashed) dashH(out, x + loopW, x, yb, S(scale, 5), th.text);
            else lineC(out, x + loopW, yb, x, yb, th.text);
            polyC(out, { { x, yb }, { x + ah, yb - ah * 0.7 }, { x + ah, yb + ah * 0.7 } }, th.text);
            my += msgGap + S(scale, 16);
        } else {
            double xf = colX(m.from), xt = colX(m.to);
            double dir = (xt > xf) ? 1 : -1;
            if (!m.text.empty()) {
                double tw = tm.textWidth(body, m.text);
                double midx = (xf + xt) / 2;
                textC(out, midx - tw / 2, my - S(scale, 6) + asc - fh, tw, fh, m.text, body, th.text);
            }
            double ly = my;
            if (m.dashed) dashH(out, xf, xt - dir * ah, ly, S(scale, 5), th.text);
            else lineC(out, xf, ly, xt - dir * ah, ly, th.text);
            polyC(out, { { xt, ly }, { xt - dir * ah, ly - ah * 0.7 }, { xt - dir * ah, ly + ah * 0.7 } }, th.text);
            my += msgGap;
        }
    }

    double bottom = std::max(lifeBottom, my);
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

double layoutFlowchart(const Flowchart& fc, double width, const LayoutTheme& th, TextMeasurer& tm,
                       double scale, double ox, double oy, LayoutResult& out) {
    int n = (int)fc.nodes.size();
    if (n == 0) return 0;
    FontSpec body = bodyFont();
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

    double nodeH = fh + 2 * padY;
    std::vector<double> nodeW(n);
    for (int k = 0; k < n; k++) {
        double w = tm.textWidth(body, fc.nodes[k].label) + 2 * padX;
        if (fc.nodes[k].shape == NodeShape::Diamond) w += S(scale, 24);
        nodeW[k] = w;
    }

    std::vector<std::vector<int>> ranks(maxRank + 1);
    for (int k = 0; k < n; k++) ranks[fc.reverse ? (maxRank - rank[k]) : rank[k]].push_back(k);

    std::vector<RectF> box(n);
    if (!fc.horizontal) {
        double diagW = 0;
        for (auto& rr : ranks) { double w = 0; for (int k : rr) w += nodeW[k]; if (!rr.empty()) w += sibGap * (rr.size() - 1); diagW = std::max(diagW, w); }
        double startX = ox + std::max(pad, (width - diagW) / 2);
        double y = oy + pad;
        for (auto& rr : ranks) {
            double w = 0; for (int k : rr) w += nodeW[k]; if (!rr.empty()) w += sibGap * (rr.size() - 1);
            double x = startX + (diagW - w) / 2;
            for (int k : rr) { box[k] = RectF{ x, y, nodeW[k], nodeH }; x += nodeW[k] + sibGap; }
            y += nodeH + rankGap;
        }
    } else {
        std::vector<double> colW(ranks.size(), 0);
        for (size_t r = 0; r < ranks.size(); r++) for (int k : ranks[r]) colW[r] = std::max(colW[r], nodeW[k]);
        double diagH = 0;
        for (auto& rr : ranks) { double h = rr.size() * nodeH; if (!rr.empty()) h += sibGap * (rr.size() - 1); diagH = std::max(diagH, h); }
        double x = ox + pad;
        for (size_t r = 0; r < ranks.size(); r++) {
            double h = ranks[r].size() * nodeH; if (!ranks[r].empty()) h += sibGap * (ranks[r].size() - 1);
            double y = oy + pad + (diagH - h) / 2;
            for (int k : ranks[r]) { box[k] = RectF{ x + (colW[r] - nodeW[k]) / 2, y, nodeW[k], nodeH }; y += nodeH + sibGap; }
            x += colW[r] + rankGap;
        }
    }

    // edges (under the nodes)
    double ah = S(scale, 9), ahw = S(scale, 5);
    for (const auto& e : fc.edges) {
        if (e.from == e.to) continue; // self-loop: skip (rare in flowcharts)
        const RectF& a = box[e.from]; const RectF& b = box[e.to];
        double acx = a.x + a.w / 2, acy = a.y + a.h / 2, bcx = b.x + b.w / 2, bcy = b.y + b.h / 2;
        PointF p0 = boxEdge(acx, acy, a.w / 2, a.h / 2, bcx, bcy);
        PointF p1 = boxEdge(bcx, bcy, b.w / 2, b.h / 2, acx, acy);
        double dx = p1.x - p0.x, dy = p1.y - p0.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1) len = 1;
        double ux = dx / len, uy = dy / len;
        PointF lineEnd = e.arrow ? PointF{ p1.x - ux * ah, p1.y - uy * ah } : p1;
        if (e.dashed) {
            double dash = S(scale, 5), t = 0, elen = e.arrow ? (len - ah) : len;
            while (t < elen) { double t2 = std::min(t + dash, elen); lineC(out, p0.x + ux * t, p0.y + uy * t, p0.x + ux * t2, p0.y + uy * t2, th.text2); t = t2 + dash; }
        } else {
            lineC(out, p0.x, p0.y, lineEnd.x, lineEnd.y, th.text2);
        }
        if (e.arrow) arrowHead(out, p1, ux, uy, ah, ahw, th.text2);
        if (!e.label.empty()) {
            double lw = tm.textWidth(body, e.label);
            double mx = (p0.x + p1.x) / 2, my = (p0.y + p1.y) / 2;
            fillR(out, { mx - lw / 2 - S(scale, 3), my - fh / 2, lw + 2 * S(scale, 3), fh }, th.bg);
            textC(out, mx - lw / 2, my - fh / 2 + asc, lw, fh, e.label, body, th.text2);
        }
    }

    // nodes
    for (int k = 0; k < n; k++) {
        const RectF& r = box[k];
        if (fc.nodes[k].shape == NodeShape::Diamond) {
            double cx2 = r.x + r.w / 2, cy2 = r.y + r.h / 2;
            std::vector<PointF> dia = { { cx2, r.y }, { r.x + r.w, cy2 }, { cx2, r.y + r.h }, { r.x, cy2 } };
            polyC(out, dia, th.bg2);
            for (size_t p = 0; p < dia.size(); p++) { PointF A = dia[p], B = dia[(p + 1) % dia.size()]; lineC(out, A.x, A.y, B.x, B.y, th.border); }
        } else {
            fillR(out, r, th.bg2);
            frameR(out, r, th.border);
        }
        double lw = tm.textWidth(body, fc.nodes[k].label);
        textC(out, r.x + (r.w - lw) / 2, r.y + (r.h - fh) / 2 + asc, lw, fh, fc.nodes[k].label, body, th.text);
    }

    double bottom = oy;
    for (int k = 0; k < n; k++) bottom = std::max(bottom, box[k].y + box[k].h);
    return (bottom - oy) + pad;
}

} // namespace

double LayoutDiagram(const Diagram& d, double width, const LayoutTheme& th,
                     TextMeasurer& tm, double scale,
                     double originX, double originY, LayoutResult& out) {
    switch (d.kind) {
        case DiagramKind::Pie:       return layoutPie(d.pie, width, th, tm, scale, originX, originY, out);
        case DiagramKind::Sequence:  return layoutSequence(d.seq, width, th, tm, scale, originX, originY, out);
        case DiagramKind::Flowchart: return layoutFlowchart(d.flow, width, th, tm, scale, originX, originY, out);
        default:                     return 0;
    }
}

} // namespace fmdv
