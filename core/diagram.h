#pragma once
// Native, dependency-free rendering of a Mermaid-syntax SUBSET (issue #16).
//
// GitHub renders fenced ```mermaid blocks as diagrams, so that is the syntax
// people already write. Full Mermaid ships a JS graph-layout engine (dagre),
// which conflicts with FMDV's premise (no browser engine, ~40 ms start, ~400 KB
// exe). So this handles only the diagram types that need no graph-layout pass:
//
//   phase 1: pie charts and sequence diagrams
//   phase 2: flowcharts (graph/flowchart TD/LR) -- a naive layered layout
//            (longest-path ranks, no edge-crossing minimization)
//
// A ```mermaid block is parsed into this small model; if it is a supported type
// it is laid out into the shared display list (fmdv::DrawCommand, drawn by every
// frontend). Anything unsupported parses to DiagramKind::None and the caller
// falls back to rendering the fence as a plain code block -- degrade, not error.
#include <vector>
#include "str.h"
#include "layout.h"

namespace fmdv {

enum class DiagramKind { None, Pie, Sequence, Flowchart };

// ---- pie ----
struct PieSlice { Str label; double value = 0; };
struct Pie {
    Str title;
    bool showData = false;              // mermaid "pie showData" -> annotate values
    std::vector<PieSlice> slices;
};

// ---- sequence ----
// Modeled as an ordered event stream so messages, notes, and block frames
// (loop/alt/opt/par) interleave in source order.
struct SeqActor { Str id; Str label; };
enum class SeqEventKind { Message, Note, BlockStart, BlockElse, BlockEnd };
enum class SeqBlock { Loop, Alt, Opt, Par };
struct SeqEvent {
    SeqEventKind kind = SeqEventKind::Message;
    int from = 0;            // Message: source actor; Note: first actor of span
    int to = 0;              // Message: target actor; Note: last actor of span
    Str text;               // message / note text, or block label
    bool dashed = false;     // Message: "-->>" style
    int activate = 0;        // Message: +1 activate target, -1 deactivate source
    int notePos = 0;         // Note: -1 left of, 0 over, +1 right of
    SeqBlock block = SeqBlock::Loop; // BlockStart / BlockElse
};
struct Sequence {
    std::vector<SeqActor> actors;
    std::vector<SeqEvent> events;
    bool autonumber = false;
};

// ---- flowchart ----
// Mermaid node shapes: A[rect] A(round) A([stadium]) A[[subroutine]]
// A[(cylinder)] A((circle)) A{diamond} A{{hexagon}}. Labels may contain <br>.
enum class NodeShape { Rect, Round, Stadium, Subroutine, Cylinder, Circle, Diamond, Hexagon };
struct FlowNode {
    Str id;
    Str label;
    NodeShape shape = NodeShape::Rect;
    int rank = 0;   // filled during layout (longest path from a root)
};
struct FlowEdge {
    int from = 0;   // index into Flowchart::nodes
    int to = 0;
    Str label;
    bool arrow = true;    // "-->" (arrowhead) vs "---" (open line)
    bool dashed = false;  // "-.->" style
};
struct Flowchart {
    bool horizontal = false; // LR/RL lay ranks left->right; TD/TB top->bottom
    bool reverse = false;    // BT / RL reverse the rank axis
    std::vector<FlowNode> nodes;
    std::vector<FlowEdge> edges;
};

struct Diagram {
    DiagramKind kind = DiagramKind::None;
    Pie pie;
    Sequence seq;
    Flowchart flow;
};

// Parse the raw text inside a ```mermaid fence (LF-normalized) into a Diagram.
// Returns kind==None for anything not in the supported subset (or empty/degenerate
// input), so the caller can fall back to plain code rendering.
Diagram ParseDiagram(const Str& body);

// Lay the diagram out at content width `width`, appending absolute, document-space
// draw commands (top-left origin at originX/originY) to `out`. `scale` multiplies
// the layout constants (zoom * DPI), matching LayoutDocument. Returns the height
// consumed; kind==None emits nothing and returns 0.
double LayoutDiagram(const Diagram& d, double width, const LayoutTheme& th,
                     TextMeasurer& tm, double scale,
                     double originX, double originY, LayoutResult& out);

} // namespace fmdv
