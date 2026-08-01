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
        check(d.seq.messages.size() == 2, "seq: two messages");
        check(d.seq.messages[0].from == 0 && d.seq.messages[0].to == 1, "seq: msg endpoints");
        check(!d.seq.messages[0].dashed && d.seq.messages[1].dashed, "seq: dashed detection (-->>)");
    }
    {   // actors auto-created in first-seen order from messages
        Diagram d = parse("sequenceDiagram\n  A->>B: x\n  B->>C: y\n");
        check(d.seq.actors.size() == 3, "seq: auto-created actors");
        check(ToUtf8(d.seq.actors[0].id) == "A" && ToUtf8(d.seq.actors[2].id) == "C", "seq: actor order");
    }
    {   // self-message
        Diagram d = parse("sequenceDiagram\n  A->>A: think\n");
        check(d.seq.messages.size() == 1 && d.seq.messages[0].from == d.seq.messages[0].to, "seq: self message");
    }

    // ---- unsupported / non-mermaid ----
    check(parse("graph TD\n  A-->B\n").kind == DiagramKind::None, "unsupported: flowchart -> None");
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
    {   // unsupported mermaid falls back to a plain code block (raw text as Mono)
        LayoutResult r = layMd("```mermaid\ngraph TD\nA-->B\n```\n");
        check(countKind(r, DrawCommand::FillPolygon) == 0, "fallback: no diagram polygons");
        check(hasText(r, "graph TD") && hasText(r, "A-->B"), "fallback: raw code lines rendered");
    }
    {   // a normal (non-mermaid) code block is unaffected
        LayoutResult r = layMd("```python\nprint(1)\n```\n");
        check(countKind(r, DrawCommand::FillPolygon) == 0 && hasText(r, "print(1)"), "code: python block unchanged");
    }

    return summary();
}
