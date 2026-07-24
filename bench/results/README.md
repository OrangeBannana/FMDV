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
rows are directly comparable. Since 2026-07-11 both frontends also run the
**same shared layout engine** (`core/layout`); only text measurement (GDI vs
CoreText) and painting differ, so compare medians on similar hardware and
expect small text-metric differences, not structural ones.

The macOS rows are captured above. The **win32 headless layout/render rows are
now produced in CI**: the `build` (windows-latest) job runs `fmdv_dbg.exe
--bench-render`, prints the rows, and uploads them as the `win32-bench` artifact
(the macOS job likewise uploads `macos-bench`). Pull those from a CI run to fill
the `win32` layout/render column here. The one metric CI cannot produce is **GUI
first-paint/startup**, which needs a real Windows desktop with a window server
(`--bench-startup`).

## Windows — desktop baseline (`local-desktop`)

Captured on a real Windows desktop (the metric CI cannot produce), including the
Phase 1 regression check of the core split against pre-split `main`.

- Machine: AMD Ryzen 7 5700U, Windows 11 Home (26200)
- Toolchain: MSYS2 UCRT64 g++ 16.1.0, per `cpp/build.ps1`
- Commit: `a96e8ac` (branch) vs `8aaaa9a` (main); warm runs

**GUI first paint — regression check vs `main`** (release build, `test.md`,
default window, same `FMDV_TIMING` instrument on both, 12 runs each):

| Build | First-paint median | p95 |
| --- | --- | --- |
| `main` (pre-core-split, `8aaaa9a`) | 35.8 ms | 37.4 ms |
| this branch (post-core-split) | 36.1 ms | 44.6 ms |

Delta: **+0.2 ms (+0.6 %) median — within the Phase 1 budget** (<5 % or <5 ms).

**`--bench-startup` schema rows** (release, `test.md`, 900×700, 12 runs):
`first_paint` median 53.6 ms / p95 73.9 ms; `window_shown` median 47.2 ms.
(Higher than the `FMDV_TIMING` numbers above because the elapsed clock starts at
`process_start` and the run forces an explicit window size.)

**`--bench-render`** (debug build, matching the CI `win32-bench` artifact;
width 900, viewport 700, 200 scroll runs):

| File | layout_once | paint_viewport_avg | scroll_paint_avg |
| --- | --- | --- | --- |
| `test.md` | 3.17 ms | 1.04 ms | 1.09 ms |
| `README.md` | 16.2 ms (90 ms cold¹) | 2.51 ms | 4.51 ms |
| `cpp/tests/stress.md` | 3.89 ms | 2.01 ms | 1.67 ms |

¹ README embeds screenshots; the first layout pays a one-time PNG decode.
`main` shows the same behavior (`--benchpaint`: ~20 ms warm), so this is
pre-existing, not a core-split regression.

**Post-layout-migration re-check** (same machine, after the win32 frontend
adopted `core/layout` — the two frontends now share one layout engine):
`--bench-startup` `first_paint` median 28.8 ms / p95 32.6 ms (12 runs, warm) —
no regression vs the 53.6 ms pre-migration row above (laptop turbo variance
makes the improvement hard to attribute; the budget check is what matters).
`--bench-render` (debug): `test.md` layout 1.07 ms / viewport 0.46 ms /
scroll 0.50 ms; `stress.md` 1.37 / 0.80 / 0.88 — unchanged or better, with
byte-identical `--dump` PNGs.

## Pending
- Nothing — the Windows adoption of `core/layout` landed 2026-07-11
  (PNG-diff-gated; see [cpp/ISSUES.md](../../cpp/ISSUES.md)), so both frontends
  now measure the same shared layout engine.
