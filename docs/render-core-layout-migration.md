# Migrating the Windows renderer onto `core/layout`

> **Status: DONE (2026-07-11).** Landed as two commits on this branch, each
> behind the mandatory gate, verified on a real Windows 11 desktop (MSYS2
> UCRT64 g++ 16.1):
>
> - **Step 2a** — `GdiTextMeasurer` routes all layout text metrics through
>   `fmdv::TextMeasurer`; display-list construction unchanged. Gate: `--dump`
>   PNGs byte-identical (test.md light/dark/scrolled + stress.md).
> - **Step 2b** — `core/layout` rewritten as a faithful, integer-exact port of
>   the GDI layout (Windows semantics are the reference: scaled constants via
>   the old S() rounding, floored divisions, bottom-aligned line boxes, run
>   grouping, content-aware table sizing + cell wrapping, blockTops). The Win32
>   `LayoutDocument` now calls it and translates the display list; `PaintDocument`,
>   selection, find, and TOC code are untouched. Gate: PNG diff byte-identical
>   again, full 71-check suite green, `--bench-render` unchanged-or-better,
>   `--bench-startup` first-paint median within budget (28.8 ms post vs
>   35.8 ms pre-core-split main baseline; no regression).
>
> The divergences called out below were resolved as predicted: tables were the
> largest sub-task (ported into `core/layout` before 2b), TOC anchors took
> option (b) (`LayoutResult.blockTops`), zoom kept the bake-scale-into-metrics
> model (`scale` parameter, default 1.0 for macOS/CLI), and font quality stayed
> in the frontend's font cache. Selection hit-testing still measures via
> GDI (`FragCharAtX`/`FragXAtChar` on the translated frags), unchanged.
>
> The section below is kept as the original plan for reference.

**This was a Windows-tested change** — it rewrites
the Win32/GDI layout path (`cpp/render.cpp`) and must be verified with the
headless `--dump` PNG diff on a Windows build at each step. It cannot be done or
verified on macOS, and it is the highest-impact regression risk in the guide's
risk register ("Core split regresses Windows startup"). Do not land any step
without a green Windows PNG diff.

This corresponds to milestone 7 / guide "Phase 2, Step 2a & 2b" for the
**Windows** frontend. `core/layout` already backs the macOS frontend; the goal
is one shared layout engine instead of two parallel ones.

## Where the two engines stand

| Concern | Windows (`cpp/render.cpp`) | Core (`core/layout`) |
| --- | --- | --- |
| Display list | `DrawCmd` / `g_cmds` (`render.cpp:138,145`) | `fmdv::DrawCommand` / `LayoutResult.cmds` |
| Layout entry | `LayoutDocument(HDC,width,Document,Theme,links,frags,blockTops)` (`render.cpp:340`) | `fmdv::LayoutDocument(doc,width,theme,tm)` |
| Paint | `PaintDocument(...)` culls `g_cmds` to viewport | frontend paints `LayoutResult.cmds` (macOS: `PaintLayout`) |
| Font cache | `g_fonts` keyed by `FontKey`, `GetFont` (`render.cpp:26,43`) | none — frontend owns fonts behind `TextMeasurer` |
| Text metrics | `TextW`/`FontHeight` via `GetTextExtentPoint32W` / `GetTextMetricsW` (`render.cpp:84,94`) | `TextMeasurer::{textWidth,lineHeight,ascent}` |
| Wrapping/words | `buildWords` + `layoutWords` (`render.cpp:185,225`) | internal to `LayoutDocument` |
| Tables | `WrapCellText` + `naturalPad`/`colW` content sizing (`render.cpp:308,460,473`) | internal to `LayoutDocument` |
| TOC anchors | `blockTops` — one y per `doc.blocks[i]` | `LayoutResult.headings` — `{level,text,y}` |
| Scale/zoom | baked into font px via `g_scale`/`S()` (`render.cpp:9,10`) | frontend scales (macOS lays out at `width/zoom`, scales the CTM) |
| AA quality | `g_fontQuality` ClearType (screen) vs grayscale (PNG) (`render.cpp:11`) | frontend concern |

## Step 2a — extract measurement (no layout change yet)

Introduce a `GdiTextMeasurer : fmdv::TextMeasurer` in the Win32 frontend that
wraps the **exact** calls the layout uses today, so pixels cannot move:

- `textWidth` → `GetTextExtentPoint32W` (as in `TextW`, `render.cpp:94`).
- `lineHeight` / `ascent` → `GetTextMetricsW` (`tmHeight` / `tmAscent`, as in
  `FontHeight`, `render.cpp:84`).
- Map `fmdv::FontSpec` → `HFONT` through the existing `g_fonts` cache
  (`GetFont`, `render.cpp:43`); honor `g_scale` and `g_fontQuality` by rebuilding
  the cache on `SetRenderScale`/`SetFontQuality` exactly as now.

Route the current `render.cpp` layout's measurement through this measurer (still
building `g_cmds` the old way). **Gate:** `--dump` PNG byte-identical to the
pre-change build for `test.md` (light + dark + a scrolled viewport) and
`cpp/tests/stress.md`; `--bench-render` medians unchanged within noise.

## Step 2b — adopt `fmdv::LayoutDocument`

Replace the body of the Win32 `LayoutDocument` (`buildWords`/`layoutWords`/table
code that fills `g_cmds`) with a call to `fmdv::LayoutDocument(doc, width, th,
gdiMeasurer)`, then paint `LayoutResult.cmds` with a small `GdiPainter` that
translates each `fmdv::DrawCommand`:

- `FillRect`/`FrameRect`/`Line` → the existing GDI ops (`emitRect`/`emitFrame`/
  `emitLine` equivalents).
- `Text` → `SelectObject(FontSpec→HFONT)` + `TextOutW`; apply `underline`/
  `strike`; `rect.y` is the baseline.
- `Color` (RGBA) → `COLORREF` (`RGB(r,g,b)`); map `LayoutTheme` from the existing
  `Theme`.

Rebuild the selection `TextFrag`s and `LinkHit`s from the core result the way the
macOS frontend does (`app.mm` builds `Frag`s from `Text` commands;
`LayoutResult.links` gives hrefs). **Gate:** same PNG diff + selection tests.

## Divergences to resolve (each can regress Windows — treat as sub-tasks)

1. **TOC anchors.** Windows uses per-block `blockTops`; core exposes only
   `headings{level,text,y}`. Either (a) switch the Windows TOC to `headings`
   (matches macOS), or (b) add `blockTops` to `LayoutResult`. (a) is less code
   and unifies behavior; verify the TOC still scrolls to the right place.
2. **Tables.** Windows has content-aware column sizing + cell wrapping
   (`WrapCellText`, `naturalPad`, `colW`, `render.cpp:308+`). Confirm
   `core/layout`'s table layout matches; if it doesn't, port the column-sizing
   logic into `core/layout` **before** 2b, or Windows tables will visibly
   regress. This is the largest sub-task.
3. **Selection hit-testing.** `FragCharAtX`/`FragXAtChar` (`render.h:38-40`) take
   an `HDC`; re-point them at `GdiTextMeasurer` so measurement matches layout.
   Keep offsets as UTF-16 code units (guide requirement; add the emoji/combining/
   CJK fixtures).
4. **Scale/zoom.** Decide one model: keep baking scale into `GdiTextMeasurer`
   font px (smallest Windows delta), or move to the macOS model (lay out at
   logical width, scale at paint). The former is safer for a no-regression port.
5. **Font quality.** Preserve ClearType-on-screen / grayscale-for-PNG
   (`g_fontQuality`) inside the measurer's font creation.

## Acceptance (guide Phase 2)

- Windows screenshots + headless `--dump` remain visually equivalent.
- Step 2a and Step 2b **each** pass the PNG diff before the next begins.
- Selection hit-testing still works; tests assert UTF-16 code-unit offsets for
  the Unicode fixtures.
- `--bench-render` (`layout_once`, `paint_viewport_avg`, `scroll_paint_avg`)
  stays comparable to the pre-change baseline.
- The full Windows test suite (`cpp/tests/run-tests.ps1`) stays green.

## Why not now

No Windows toolchain (MinGW/GDI) or desktop is available in the current
environment, so none of the gates above can be exercised. Landing any part blind
would risk a silent Windows regression with no way to catch it — exactly what the
staged PNG-diff process exists to prevent.
