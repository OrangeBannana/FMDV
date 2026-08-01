#pragma once
// Native, dependency-free rendering of a Mermaid-syntax SUBSET (issue #16).
//
// GitHub renders fenced ```mermaid blocks as diagrams, so that is the syntax
// people already write. Full Mermaid ships a JS graph-layout engine (dagre),
// which conflicts with FMDV's premise (no browser engine, ~40 ms start, ~400 KB
// exe). So this handles only the diagram types that need no graph-layout pass:
//
//   phase 1 (here): pie charts and sequence diagrams
//   phase 2 (later): flowcharts (graph TD/LR) -- a separate issue
//
// A ```mermaid block is parsed into this small model; if it is a supported type
// it is laid out into the shared display list (fmdv::DrawCommand, drawn by every
// frontend). Anything unsupported parses to DiagramKind::None and the caller
// falls back to rendering the fence as a plain code block -- degrade, not error.
#include <vector>
#include "str.h"
#include "layout.h"

namespace fmdv {

enum class DiagramKind { None, Pie, Sequence };

// ---- pie ----
struct PieSlice { Str label; double value = 0; };
struct Pie {
    Str title;
    bool showData = false;              // mermaid "pie showData" -> annotate values
    std::vector<PieSlice> slices;
};

// ---- sequence ----
struct SeqActor { Str id; Str label; };
struct SeqMessage {
    int from = 0;                       // index into Sequence::actors
    int to = 0;
    Str text;
    bool dashed = false;                // "-->>" / "-->" style
};
struct Sequence {
    std::vector<SeqActor> actors;
    std::vector<SeqMessage> messages;
};

struct Diagram {
    DiagramKind kind = DiagramKind::None;
    Pie pie;
    Sequence seq;
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
