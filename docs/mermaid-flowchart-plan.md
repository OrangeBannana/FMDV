# Native Mermaid Flowchart Rendering — Implementation Plan (Path A)

**Status:** proposal / reference doc for implementation. Nothing wired yet.

**Goal.** Render ` ```mermaid ` fenced blocks as real diagrams, laid out and
painted by the shared C++ `core/` — **no browser, no JS engine, no new runtime
dependency.** Scope is deliberately narrow: **`flowchart` / `graph` only**, to
start. Everything else falls back to today's behavior (show the raw fenced text).

This is "Path A" from the design discussion: parse Mermaid ourselves, run our own
layered-graph layout, and emit into the existing `LayoutResult` display list so
diagrams inherit zoom, DPI, dark mode, selection, and viewport culling for free.

---

## 1. Why this fits the codebase

- A ` ```mermaid ` block already parses to a `Block{ type=CodeBlock, lang="mermaid",
  codeText=... }` — see [`core/markdown.h`](../core/markdown.h). **No parser change
  needed to detect it.**
- The display-list vocabulary in [`core/layout.h`](../core/layout.h) — `FillRect`,
  `FrameRect`, `Line`, `Text` — is exactly what a flowchart needs (boxes, borders,
  connectors, labels). Frontends (GDI, CoreText) already paint these.
- Layout is measurement-abstracted via `TextMeasurer`, so node sizing stays
  per-platform while the graph algorithm stays shared and unit-testable.

### Hard constraint: primitives available
The display list has **no polygon fill and no arc/rounded-rect**. Consequences:

| Want | How we do it with existing primitives |
|---|---|
| Rectangle node | `FillRect` (bg) + `FrameRect` (border) |
| Rounded / stadium node `(...)` | Approximate as rectangle for spike; optional stroked corners later |
| Diamond `{...}` / hexagon `{{...}}` | Outline via `Line` strokes (no fill), or rectangle fallback |
| Arrowhead | Two short `Line` strokes forming a "V" (same trick as the task-list checkmark, [`layout.cpp:315`](../core/layout.cpp)) |
| Edge label | `FillRect` (bg patch for legibility) + `Text` |

If we later decide filled diamonds/rounded rects are worth it, that's a *separate*
proposal to add a `FillPoly` (or `RoundRect`) `DrawCommand::Kind` — out of scope here.

---

## 2. Module layout

Two new files in `core/`, mirroring the `markdown` (parse) / `layout` (geometry)
split the repo already uses:

```
core/mermaid.h      // public types + ParseMermaid + LayoutMermaid
core/mermaid.cpp    // grammar parse + Sugiyama layered layout (pure, measurer-driven)
tests/mermaid_test.cpp   // unit tests (parse + layout), same harness as the others
```

`core/layout.cpp` gains a small branch in `case BlockType::CodeBlock:` that calls
into `mermaid` and emits the returned geometry as draw commands. **No frontend
changes** for the spike — the existing painters already handle every primitive.

### Public API (`core/mermaid.h`)

```cpp
namespace fmdv {

enum class MmDir { TB, LR, BT, RL };          // flowchart direction
enum class MmShape { Rect, Round, Diamond, Hexagon };

struct MmNode {
    Str id;
    std::vector<Str> labelLines;              // label split on <br/> and "\n"
    MmShape shape = MmShape::Rect;
    RectF box;                                // filled in by LayoutMermaid (doc space)
};

struct MmEdge {
    int from = -1, to = -1;                   // indices into MmGraph::nodes
    Str label;
    bool arrowTo = true;                       // "-->" head at target
    std::vector<double> px, py;               // polyline waypoints (LayoutMermaid)
    RectF labelBox;                            // where the label sits (LayoutMermaid)
};

// Parse only — no geometry, no measurement. Deterministic & unit-testable alone.
struct MmGraph {
    bool ok = false;                          // false => caller falls back to raw text
    MmDir dir = MmDir::TB;
    std::vector<MmNode> nodes;
    std::vector<MmEdge> edges;
};
MmGraph ParseMermaid(const Str& code);

// Assign box sizes (via tm) and positions (Sugiyama). Sets node.box, edge.px/py.
// Returns total content size; caller advances layout Y by height.
struct MmLayout { double width = 0, height = 0; };
MmLayout LayoutMermaid(MmGraph& g, TextMeasurer& tm, const FontSpec& font,
                       double scale);

} // namespace fmdv
```

Keeping `ParseMermaid` pure (no `TextMeasurer`) means grammar tests need no font
stub, and layout tests can use the same fake measurer `tests/layout_test.cpp`
already defines.

---

## 3. Diagram-type dispatch & supported grammar

### 3.1 Two-tier detection (the router)

Detection is exact, never a best-guess:

- **Tier 1 — is it Mermaid?** The fence info string: `b.lang == "mermaid"`
  (lower-cased; accept the `mmd` alias). Already captured in `Block.lang`.
- **Tier 2 — which diagram type?** Mermaid self-declares its type as the **first
  keyword of the body** (`flowchart`, `sequenceDiagram`, `classDiagram`, …). That
  keyword is the **dispatch key**.

To find that keyword, `ParseMermaid` must first **skip anything that can legally
precede it**, or a valid diagram will be misrouted to passthrough:
1. **YAML frontmatter** — a leading `---` … `---` block (title/config).
2. **Init directives** — `%%{init: { ... }}%%`.
3. **Comments** — `%% …` lines, and blank lines.

Then read the first real token, lower-case it, normalize (`graph` → treat as
`flowchart`; strip the direction suffix like `TD`/`LR`; `stateDiagram-v2` →
`stateDiagram`), and match against the **implemented-types whitelist**.

### 3.2 Progressive coverage — the whole strategy

**Implemented now → render natively:** `flowchart` / `graph`.

**Every other type → pass straight through to the regular code-block render**
(the raw fenced text you get today). This is not a failure mode; it is the
designed default.

The key property: **the passthrough set shrinks monotonically as diagram types are
added.** Each new type is an independent, additive milestone with **zero regression
risk** — until a type is implemented, an unknown keyword always yields exactly
today's behavior, so shipping flowchart support can never break rendering of a
sequence or class diagram (it just stays as text). As `sequenceDiagram`,
`classDiagram`, etc. get built (each its own parse+layout behind the same dispatch),
they drop out of passthrough one by one:

```
v1:  flowchart ─► native      | everything else ─► passthrough (raw text)
+M3: flowchart, sequence ─► native | rest ─► passthrough
…    (each added type removes itself from passthrough; the default set only gets smaller)
```

So the dispatcher is a whitelist that grows over time; the fallback is the safety
net that guarantees correctness at every step.

### 3.3 Supported `flowchart` grammar (v1)

Support the shapes/edges an LLM actually emits for a `flowchart`. **Parse and
ignore** unknown lines rather than failing the whole diagram where reasonable;
**bail to raw text** (whole block) only on structural confusion.

Supported:
- Header: `flowchart TD|TB|LR|RL|BT` and the `graph` synonym.
- Node decls with shapes: `A["rect"]`, `B("round")`, `C{"diamond"}`, `D{{"hex"}}`.
  Bare `A` (no brackets) = rectangle whose label is the id.
- Edges: `A --> B`, `A --- B` (no head), with labels `A -- text --> B` and
  `A -->|text| B`.
- Multi-target on one line: `A --> B & C` (optional; can defer).
- Labels: `<br/>` and literal `\n` split into `labelLines`. `&quot;`→`"` etc.
- Comments: `%% ...` lines. Blank lines. Leading frontmatter / `init` directive
  (skipped per §3.1).

Explicitly **out of scope within a flowchart → ignore the directive, keep
rendering** (do not bail the whole diagram): subgraphs, `click`, `style`/`classDef`,
`linkStyle`, markdown-in-node, icons. (Subgraphs and styling are natural v2 items —
see §8.)

Other **diagram types** (`sequenceDiagram`, `classDiagram`, `stateDiagram`,
`erDiagram`, `gantt`, `pie`, `gitGraph`, `mindmap`, `journey`, `C4Context`, …) are
handled by §3.2 dispatch — they pass through to raw text until implemented.

---

## 4. Layout algorithm (Sugiyama / layered)

Compute everything in a canonical **top-to-bottom** frame, then transform axes for
`LR`/`BT`/`RL` at the end. Phases:

1. **Node sizing.** For each node, measure `labelLines` with `tm` at `font`.
   `box.w = maxLineWidth + 2*padX` (min width clamp); `box.h = nLines*lineH + 2*padY`.
2. **Cycle removal.** Flowcharts may contain cycles. DFS; reverse back-edges so the
   working graph is a DAG (restore direction only for arrowhead placement).
3. **Layer assignment.** Longest-path layering from sources (`layer[v] = max over
   preds (layer+1)`). Adequate and simple for v1; network-simplex is a later
   refinement for tighter diagrams.
4. **Normalize.** Insert dummy nodes on any edge spanning >1 layer, so every edge
   connects adjacent layers. Dummy chains become edge bend points.
5. **Crossing reduction.** Order nodes within each layer. Median/barycenter
   heuristic, alternating down/up sweeps (e.g. 8 passes), keep the ordering with
   fewest crossings.
6. **Coordinate assignment.**
   - `y` per layer = running sum of `(max node height in layer) + rankSep`.
   - `x` within a layer: place in order with `nodeSep` gaps; then a centering pass
     (align each node toward the median of its neighbors' x, resolving overlaps).
     Brandes–Köpf alignment is the later upgrade for prettier straight edges.
7. **Edge routing.** Polyline through source-border → dummy centers → target-border.
   Clip endpoints to node rectangles. Arrowhead V at the target boundary.
8. **Edge-label placement.** Midpoint of the edge's central segment; reserve a
   `labelBox` (measured) so §2's bg patch can keep it readable over lines.
9. **Axis transform.** `LR`: swap (x,y). `BT`/`RL`: flip the respective axis.
   Recompute total `width/height`.
10. **Fit-to-width.** If natural width > available content width, uniformly scale
    all coordinates so it fits (FMDV scrolls vertically only). Record final
    `MmLayout{width,height}`.

Determinism: stable node/edge order from parse; no hashing/iteration-order
dependence — required for golden tests.

---

## 5. Integration point (`core/layout.cpp`)

Inside `LayoutDocument`'s block loop, in the existing `case BlockType::CodeBlock:`
([`layout.cpp:332`](../core/layout.cpp)), branch **before** the current
plain-code-block rendering:

```cpp
case BlockType::CodeBlock: {
    if (b.lang == U16("mermaid")) {
        MmGraph g = ParseMermaid(b.codeText);
        if (g.ok) {
            FontSpec f = roleFont(FontRole::Body, false, false);
            MmLayout ml = LayoutMermaid(g, tm, f, cx.scale);
            double x0 = cx.left, y0 = y;      // offset diagram into doc space
            emitMermaid(cx, g, x0, y0, f);    // fill/frame/drawLine/textCmd helpers
            y = y0 + ml.height + Sc(cx, 16);
            break;                            // handled — skip raw-text path
        }
        // g.ok == false: fall through to render the fence as a normal code block
    }
    /* ...existing plain code-block layout unchanged... */
}
```

`emitMermaid` is a static helper in `layout.cpp` (it needs the file-local
`fill/frame/drawLine/textCmd/Sc`): nodes → `fill`+`frame`(+stroked shape); labels →
`textCmd` (mark `selectable=true` so diagram text copies); edges → `drawLine`
polylines + arrowhead strokes; edge labels → bg `fill` + `textCmd`. Node/label text
should be added to `res.links` only if we later support `click` (not in v1).

**Failure is graceful:** `g.ok == false` (unsupported diagram type, parse error)
falls straight through to the code block you get today. Never throw, never blank.

---

## 6. Milestones

Each milestone is independently shippable and testable. Stop after any of them and
the app is still correct (worst case: raw text fallback).

### M0 — Spike (proof it lays out & paints natively)
- `ParseMermaid`: header + `A["x"]` rects + `A --> B` edges only. No labels, no
  shapes, no cycles.
- `LayoutMermaid`: sizing + longest-path layering + naive in-order x placement +
  straight edges (no dummy routing, no crossing reduction).
- `emitMermaid`: filled/framed rect nodes, straight line edges, V arrowheads.
- Hook into `layout.cpp`. **Acceptance:** the Master Wiring diagram from
  `TEST_BENCH_ASSEMBLY.md` (re-expressed as `flowchart TD`) renders as native boxes
  + arrows in the real window, scales with Ctrl+±, and recolors in dark mode.
- CLI: `fmdv parse` / an inspection flag dumps node boxes + edge polylines for a
  fixture (see §7).

### M1 — Real layout quality
- Dummy-node normalization + median/barycenter crossing reduction + centered x
  coordinate pass. Edge routing through bend points.
- Edge **labels** (`-- text -->`, `-->|text|`) with legibility bg patch.
- All four directions (`TB/LR/BT/RL`) via the axis transform.
- Fit-to-width scaling.
- **Acceptance:** Master Wiring diagram looks comparable to the Graphviz reference
  render (few/zero crossings, POWER sits beside the ESP32, no overlaps).

### M2 — Shapes & polish
- Diamond/hexagon outlines (stroked) + rounded-rect approximation for `(...)`.
- `A --> B & C` fan-out. `&quot;`/entity unescaping. `%%` comments robustly.
- Dark-mode node fills tuned against `LayoutTheme` (`bg2`/`border`/`text`).
- **Acceptance:** the block-style diagrams elsewhere in the corpus render cleanly;
  decision nodes read as diamonds.

### M3 (optional) — Sequence diagrams
- Separate, *simpler* path: `sequenceDiagram` needs no graph layout (deterministic
  lifelines + ordered messages). Own parse + layout, same emit primitives.
- **Acceptance:** a basic `participant`/`->>`/`-->>` sequence renders.

---

## 7. Test plan

Mirror the existing per-module unit-test setup: `tests/mermaid_test.cpp`, built by
the Makefile's `build/%-test` rule and run under `make test` / `ctest`. Add
`mermaid` to `TEST_NAMES` and `core/mermaid.cpp` to `TEST_CORE`
([`Makefile:132-138`](../Makefile)), and to `CMakeLists.txt` for the CMake path.

**Parse tests (no measurer):**
- Header variants → correct `MmDir`; `graph` normalizes to flowchart; unknown
  diagram type (`sequenceDiagram`, garbage) → `ok=false` (routes to passthrough).
- **Type detection skips preamble:** a flowchart preceded by `---`/`config`
  frontmatter, a `%%{init:…}%%` directive, and `%%` comments still detects
  `flowchart` (regression guard for the §3.1 footgun); the same preamble in front
  of an unsupported type still yields `ok=false`.
- Each node shape parses to the right `MmShape`; bare id → rect with id label.
- Edge forms (`-->`, `---`, `-- x -->`, `-->|x|`) → correct `from/to/label/arrowTo`.
- `<br/>` and `\n` split `labelLines`; entity unescape.
- Malformed input → `ok=false` (never crash), so the caller falls back.

**Layout tests (fake `TextMeasurer`, fixed metrics — reuse the one in
`tests/layout_test.cpp`):**
- Node count/ids preserved; every `box` has the sized dimensions.
- **No two node boxes overlap** (assert pairwise); layers strictly increasing in y
  (canonical TB).
- Deterministic: same input → identical coordinates across runs.
- Cycle input terminates and produces a valid layering (no infinite loop).
- `LR` transform swaps axes as expected.
- Edge polylines start/end on the correct node borders; arrowTo matches parse.

**Integration / display-list tests (extend `layout_test.cpp`):**
- A `CodeBlock{lang="mermaid"}` with a valid graph emits `FillRect`/`Line`/`Text`
  commands and advances `y` by the diagram height.
- An **unsupported** mermaid block (`sequenceDiagram ...`) emits the *same* commands
  as a plain code block (fallback path verified).

**CLI inspection (manual + `make check`):** add a small `fmdv mermaid <file>` (CLI
frontend) that prints parsed nodes/edges and laid-out boxes, so fixtures can be
eyeballed and diffed without a GUI. Optionally a golden-text fixture under
`tests/fixtures/` diffed in `run-tests.sh`.

**Visual regression (macOS CI already renders fixtures):** add a `.md` fixture with
one `flowchart` and render it to PNG in CI the way fixtures are rendered today; a
human-reviewed reference image guards against layout regressions.

---

## 8. Scope, fallback, and non-goals

- **Fallback is a feature.** Any unsupported/broken diagram → the existing raw
  code-block render. Users never get a blank or an error; worst case is what they
  see today.
- **v1 non-goals:** subgraphs, `style`/`classDef`/`linkStyle`, `click` links,
  icons/images in nodes, non-flowchart diagram types (except the optional M3
  sequence path), markdown formatting inside labels.
- **Natural v2:** subgraph boxes (a nested layout + surrounding frame), `classDef`
  colors mapped onto `fill/frame`, `click` → append to `res.links` so diagram nodes
  become clickable like other links.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Layout quality worse than dagre/Graphviz | Ship M0 as boxes+straight-edges (still better than ASCII); invest crossing-reduction + centered coords in M1; Brandes–Köpf later only if needed. |
| Layout is the bulk of the work, easy to under-budget | Milestones isolate it; M0 uses the naive version so there's a working diagram early. |
| Users paste unsupported Mermaid features | Hard scope + raw-text fallback; message nothing, just render the fence. |
| Wide diagrams vs vertical-only scroll | Fit-to-width uniform scale (§4.10). |
| No polygon-fill primitive | Stroked shapes + rectangle fallbacks (§2); defer a `FillPoly` primitive to a separate proposal. |
| Binary-size / startup creep | Pure `core/` code, no deps; measure `.exe` size + cold-paint before/after and keep within the README's stated numbers. |

---

## 10. Definition of done (v1 = M0–M2)

- `flowchart TD/LR/BT/RL` with rect/round/diamond/hex nodes and labeled/plain edges
  renders natively on Windows **and** macOS, from a real ` ```mermaid ` block.
- Scales with zoom/DPI, recolors in dark mode, text is selectable/copyable.
- Unsupported diagrams and parse errors fall back to raw text.
- `tests/mermaid_test.cpp` passes under `make test` and CMake/`ctest`; CI renders a
  flowchart fixture on both platforms.
- Measured `.exe` size and cold first-paint stay within the README's promises.
