# Benchmark Results

Comparable timing summaries across FMDV frontends. Raw per-run CSV logs
(`FMDV_BENCH_LOG`) are not committed — this file records the medians. See
[the macOS implementation guide](../../docs/macos-implementation-guide.md),
Phase 0 and Phase 5, for the schema and methodology.

Reproduce the CLI numbers with:

```sh
make cli
FMDV_BENCH_LOG=build/bench.csv FMDV_BENCH_LABEL=local \
  ./build/fmdv-cli bench-parse test.md --runs 200
```

## macOS — CLI parse baseline (`cli-bootstrap`)

First off-Windows measurement of the shared parser. Parse-only (layout is not
yet platform-neutral — that is Phase 2), so these isolate `ParseMarkdown`.

- Machine: Apple M3 Pro, macOS 26.5.2 (arm64)
- Toolchain: Apple clang 21.0.0, `-std=c++17 -O2`
- Frontend: `fmdv-cli` (release), commit `18bfef3`
- Runs: 200 per file, warm process

| File | Blocks | Parse median | Parse p95 | Parse min |
| --- | --- | --- | --- | --- |
| `test.md` | 17 | 0.022 ms | 0.031 ms | 0.022 ms |
| `README.md` | 34 | 0.047 ms | 0.063 ms | 0.044 ms |
| `cpp/tests/stress.md` | 15 | 0.017 ms | 0.024 ms | 0.017 ms |

Parsing is sub-0.1 ms for typical documents on this hardware, confirming the
core parser is not a first-paint bottleneck on macOS. The interesting cost
(layout + first paint) is measured once the layout is platform-neutral.

The CLI writes the same 15-column schema as the Windows frontend's bench mode
(`cpp/fmdv.cpp`, `FMDV_BENCH_LOG`), so these rows are directly comparable to a
Windows run.

## macOS — CoreText render baseline (`macos-baseline`)

Layout + render medians for the native macOS frontend (`fmdv-macos
--bench-render`), now that layout is platform-neutral (Phase 2) and the CoreText/
CoreGraphics renderer exists (Phase 4). Reproduce with:

```sh
make macos
FMDV_BENCH_LOG=build/bench.csv ./build/fmdv-macos --bench-render test.md --width 900 --runs 200
```

- Machine: Apple M3 Pro, macOS 26.5.2 (arm64); Apple clang, `-O2`; width 900, warm.

| File | Blocks | Parse median | Layout median | Render median |
| --- | --- | --- | --- | --- |
| `test.md` | 17 | 0.20 ms | 0.25 ms | 0.34 ms |
| `README.md` | 37 | 0.08 ms | 0.89 ms | 1.39 ms |
| `cpp/tests/stress.md` | 15 | 0.04 ms | 0.29 ms | 0.52 ms |

Parse + layout + a full off-screen render stays comfortably sub-2 ms per document
on this hardware, so first paint is dominated by process/window startup, not the
content pipeline — consistent with FMDV's fast-first-paint goal.

## Windows vs macOS comparison

The macOS (`macos`) and Windows (`win32`) frontends emit the **same 15-column
schema**, so their `layout_once` / `paint_viewport_avg` / `scroll_paint_avg`
rows are directly comparable. Compare medians on similar hardware, noting that
the two frontends use different layout engines (GDI vs the shared `core/layout`),
so small metric differences are expected — compare structure and order of
magnitude, not exact pixels.

The macOS rows are captured above. The **win32 headless layout/render rows are
now produced in CI**: the `build` (windows-latest) job runs `fmdv_dbg.exe
--bench-render`, prints the rows, and uploads them as the `win32-bench` artifact
(the macOS job likewise uploads `macos-bench`). Pull those from a CI run to fill
the `win32` layout/render column here. The one metric CI cannot produce is **GUI
first-paint/startup**, which needs a real Windows desktop with a window server
(`--bench-startup`).

## Pending

- **Windows GUI first-paint** — `fmdv_dbg.exe … --bench-startup` needs a real
  Windows desktop (CI has no window server). The layout/render rows are already
  covered by the CI `win32-bench` artifact described above.
- **Windows adoption of `core/layout`** — the shared engine currently backs the
  macOS frontend; migrating `render.cpp` onto it is a separate, Windows-tested
  step. Plan:
  [render → core/layout migration](../../docs/render-core-layout-migration.md).
