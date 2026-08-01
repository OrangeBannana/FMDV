// Unit tests for the native Mermaid-subset diagrams (core/diagram): parsing of
// pie + sequence syntax, and the layout integration in core/layout — a supported
// ```mermaid block becomes diagram draw commands (incl. FillPolygon wedges /
// arrowheads), while an unsupported one falls back to plain code rendering.
#include "diagram.h"
#include "layout.h"
#include "markdown.h"
#include "test_check.h"
#include <string>

using namespace fmdv;

struct FixedMeasurer : TextMeasurer {
    double textWidth(const FontSpec&, StrView s) override { return (double)s.size() * 10; }
    double lineHeight(const FontSpec&) override { return 20; }
    double ascent(const FontSpec&) override { return 15; }
};

static Diagram parse(const char* body) { return ParseDiagram(FromUtf8(body)); }

static LayoutResult layMd(const char* md, double width = 900) {
    FixedMeasurer tm;
    Document doc = ParseMarkdown(FromUtf8(md));
    return LayoutDocument(doc, width, LightLayoutTheme(), tm, 1.0);
}
static int countKind(const LayoutResult& r, DrawCommand::Kind k) {
    int n = 0; for (const auto& c : r.cmds) if (c.kind == k) n++; return n;
}
static bool hasText(const LayoutResult& r, const char* t) {
    for (const auto& c : r.cmds)
        if (c.kind == DrawCommand::Text && ToUtf8(c.text) == t) return true;
    return false;
}

int main() {
    // ---- pie parsing ----
    {
        Diagram d = parse("pie showData\n    title Pets\n    \"Dogs\" : 60\n    \"Cats\" : 40\n");
        check(d.kind == DiagramKind::Pie, "pie: recognized");
        check(d.pie.showData, "pie: showData flag");
        check(ToUtf8(d.pie.title) == "Pets", "pie: title parsed");
        check(d.pie.slices.size() == 2, "pie: two slices");
        check(ToUtf8(d.pie.slices[0].label) == "Dogs" && d.pie.slices[0].value == 60, "pie: slice 0");
        check(d.pie.slices[1].value == 40, "pie: slice 1 value");
    }
    {   // title on the pie line, decimals, no showData
        Diagram d = parse("pie title Split\n\"A\" : 42.5\n\"B\" : 7\n");
        check(d.kind == DiagramKind::Pie && ToUtf8(d.pie.title) == "Split", "pie: inline title");
        check(!d.pie.showData, "pie: showData defaults off");
        check(d.pie.slices.size() == 2 && d.pie.slices[0].value == 42.5, "pie: decimal value");
    }
    check(parse("pie\n").kind == DiagramKind::None, "pie: no slices -> None (fallback)");

    // ---- sequence parsing ----
    {
        Diagram d = parse("sequenceDiagram\n  participant Alice\n  participant Bob as Bobby\n"
                          "  Alice->>Bob: Hi\n  Bob-->>Alice: Yo\n");
        check(d.kind == DiagramKind::Sequence, "seq: recognized");
        check(d.seq.actors.size() == 2, "seq: two actors");
        check(ToUtf8(d.seq.actors[1].label) == "Bobby", "seq: 'as' label");
        check(d.seq.events.size() == 2, "seq: two message events");
        check(d.seq.events[0].from == 0 && d.seq.events[0].to == 1, "seq: msg endpoints");
        check(!d.seq.events[0].dashed && d.seq.events[1].dashed, "seq: dashed detection (-->>)");
    }
    {   // actors auto-created in first-seen order from messages
        Diagram d = parse("sequenceDiagram\n  A->>B: x\n  B->>C: y\n");
        check(d.seq.actors.size() == 3, "seq: auto-created actors");
        check(ToUtf8(d.seq.actors[0].id) == "A" && ToUtf8(d.seq.actors[2].id) == "C", "seq: actor order");
    }
    {   // self-message
        Diagram d = parse("sequenceDiagram\n  A->>A: think\n");
        check(d.seq.events.size() == 1 && d.seq.events[0].from == d.seq.events[0].to, "seq: self message");
    }
    {   // autonumber, notes, activation +/- , loop block
        Diagram d = parse("sequenceDiagram\n  autonumber\n  A->>+B: req\n  B-->>-A: res\n"
                          "  Note over A,B: shared\n  loop retry\n  A->>B: ping\n  end\n");
        check(d.seq.autonumber, "seq: autonumber flag");
        int notes = 0, loops = 0, ends = 0, act = 0, deact = 0;
        for (const auto& e : d.seq.events) {
            if (e.kind == SeqEventKind::Note) notes++;
            if (e.kind == SeqEventKind::BlockStart && e.block == SeqBlock::Loop) loops++;
            if (e.kind == SeqEventKind::BlockEnd) ends++;
            if (e.kind == SeqEventKind::Message && e.activate == 1) act++;
            if (e.kind == SeqEventKind::Message && e.activate == -1) deact++;
        }
        check(notes == 1 && loops == 1 && ends == 1, "seq: note + loop/end events");
        check(act == 1 && deact == 1, "seq: +/- activation parsed");
        check(ToUtf8(d.seq.events[2].text) == "shared", "seq: note text + span");
    }
    {   // alt / else / opt structure
        Diagram d = parse("sequenceDiagram\n  alt ok\n  A->>B: x\n  else fail\n  A->>B: y\n  end\n"
                          "  opt maybe\n  A->>B: z\n  end\n");
        int starts = 0, elses = 0, ends = 0;
        for (const auto& e : d.seq.events) {
            if (e.kind == SeqEventKind::BlockStart) starts++;
            if (e.kind == SeqEventKind::BlockElse) elses++;
            if (e.kind == SeqEventKind::BlockEnd) ends++;
        }
        check(starts == 2 && elses == 1 && ends == 2, "seq: alt/else/opt structure");
    }

    // ---- flowchart parsing ----
    {
        Diagram d = parse("graph TD\n  A[Start] --> B{OK?}\n  B -->|yes| C(Done)\n  B -->|no| A\n");
        check(d.kind == DiagramKind::Flowchart, "flow: graph TD recognized");
        check(!d.flow.horizontal && !d.flow.reverse, "flow: TD orientation");
        check(d.flow.nodes.size() == 3, "flow: three nodes");
        check(ToUtf8(d.flow.nodes[0].label) == "Start", "flow: node label from [..]");
        check(d.flow.nodes[1].shape == NodeShape::Diamond, "flow: {..} is a diamond");
        check(d.flow.nodes[2].shape == NodeShape::Round, "flow: (..) is round");
        check(d.flow.edges.size() == 3, "flow: three edges");
        check(ToUtf8(d.flow.edges[1].label) == "yes", "flow: edge label |..|");
    }
    {   // flowchart LR + chained edges + dashed + open link
        Diagram d = parse("flowchart LR\n  A --> B --> C\n  A -.-> C\n  B --- C\n");
        check(d.flow.horizontal, "flow: LR is horizontal");
        check(d.flow.nodes.size() == 3 && d.flow.edges.size() == 4, "flow: chained edge expands to two");
        check(d.flow.edges[0].from == 0 && d.flow.edges[0].to == 1, "flow: chain A->B");
        check(d.flow.edges[1].from == 1 && d.flow.edges[1].to == 2, "flow: chain B->C");
        check(d.flow.edges[2].dashed, "flow: -.-> is dashed");
        check(!d.flow.edges[3].arrow, "flow: --- is an open link");
    }
    check(parse("graph RL\nA-->B\n").flow.reverse, "flow: RL reverses rank axis");
    {   // node shapes
        Diagram d = parse("graph TD\n  A([stad]) --> B[[sub]]\n  C[(db)] --> D((circ))\n  E{{hex}} --> F(round)\n");
        check(d.flow.nodes[0].shape == NodeShape::Stadium, "flow: ([..]) stadium");
        check(d.flow.nodes[1].shape == NodeShape::Subroutine, "flow: [[..]] subroutine");
        check(d.flow.nodes[2].shape == NodeShape::Cylinder, "flow: [(..)] cylinder");
        check(d.flow.nodes[3].shape == NodeShape::Circle, "flow: ((..)) circle");
        check(d.flow.nodes[4].shape == NodeShape::Hexagon, "flow: {{..}} hexagon");
        check(d.flow.nodes[5].shape == NodeShape::Round, "flow: (..) round");
    }
    {   // multi-line labels via <br>
        Diagram d = parse("graph TD\n  A[Line1<br>Line2] --> B[x<br/>y]\n");
        check(ToUtf8(d.flow.nodes[0].label) == "Line1\nLine2", "flow: <br> becomes newline");
        check(ToUtf8(d.flow.nodes[1].label) == "x\ny", "flow: <br/> variant");
    }

    // ---- state diagram (maps onto the flowchart model) ----
    {
        Diagram d = parse("stateDiagram-v2\n  [*] --> Idle\n  Idle --> Running : start\n  Running --> [*]\n");
        check(d.kind == DiagramKind::Flowchart, "state: maps to flowchart model");
        check(d.flow.nodes.size() == 4, "state: start + 2 states + end");
        check(d.flow.nodes[0].shape == NodeShape::Dot, "state: [*] as source is a start dot");
        bool hasEnd = false; for (const auto& nd : d.flow.nodes) if (nd.shape == NodeShape::DotRing) hasEnd = true;
        check(hasEnd, "state: [*] as target is an end ring");
        check(d.flow.edges.size() == 3, "state: three transitions");
        check(ToUtf8(d.flow.edges[1].label) == "start", "state: transition label");
        check(d.flow.nodes[1].shape == NodeShape::Round, "state: named state is a rounded box");
    }
    {   // "state \"desc\" as id" aliasing + direction
        Diagram d = parse("stateDiagram-v2\n  direction LR\n  state \"Waiting room\" as w\n  [*] --> w\n");
        check(d.flow.horizontal, "state: direction LR");
        int wi = -1; for (size_t k = 0; k < d.flow.nodes.size(); k++) if (ToUtf8(d.flow.nodes[k].id) == "w") wi = (int)k;
        check(wi >= 0 && ToUtf8(d.flow.nodes[wi].label) == "Waiting room", "state: quoted description alias");
    }

    // ---- unsupported / non-mermaid ----
    check(parse("gantt\n  title X\n").kind == DiagramKind::None, "unsupported: gantt -> None");
    check(parse("just some text\n").kind == DiagramKind::None, "unsupported: prose -> None");
    check(parse("").kind == DiagramKind::None, "unsupported: empty -> None");

    // ---- layout integration (via core/layout on a ```mermaid block) ----
    {
        LayoutResult r = layMd("```mermaid\npie\n\"A\" : 1\n\"B\" : 1\n```\n");
        check(countKind(r, DrawCommand::FillPolygon) == 2, "layout: pie emits one wedge per slice");
        check(hasText(r, "A") && hasText(r, "B"), "layout: pie legend labels drawn");
    }
    {
        LayoutResult r = layMd("```mermaid\nsequenceDiagram\nAlice->>Bob: Hi\n```\n");
        check(countKind(r, DrawCommand::FillPolygon) >= 1, "layout: sequence draws an arrowhead");
        check(hasText(r, "Alice") && hasText(r, "Bob"), "layout: sequence actor boxes labelled");
        check(hasText(r, "Hi"), "layout: sequence message text drawn");
    }
    {
        LayoutResult r = layMd("```mermaid\ngraph TD\nA[Start]-->B{Q}\nB-->C\n```\n");
        check(countKind(r, DrawCommand::FillPolygon) >= 2, "layout: flowchart draws diamond + arrowhead polygons");
        check(hasText(r, "Start") && hasText(r, "Q") && hasText(r, "C"), "layout: flowchart node labels drawn");
    }
    {   // truly unsupported mermaid falls back to a plain code block (raw text as Mono)
        LayoutResult r = layMd("```mermaid\ngantt\ntitle Roadmap\n```\n");
        check(countKind(r, DrawCommand::FillPolygon) == 0, "fallback: no diagram polygons");
        check(hasText(r, "gantt") && hasText(r, "title Roadmap"), "fallback: raw code lines rendered");
    }
    {   // a normal (non-mermaid) code block is unaffected
        LayoutResult r = layMd("```python\nprint(1)\n```\n");
        check(countKind(r, DrawCommand::FillPolygon) == 0 && hasText(r, "print(1)"), "code: python block unchanged");
    }

    return summary();
}
