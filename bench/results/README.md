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

## Pending

- **Windows GUI baseline** — the Windows frontend already emits this schema
  (`fmdv_dbg.exe … --bench-startup/--bench-render`); it just needs to be run on
  a Windows machine to capture `windows-baseline` / `pre-core-split` before the
  Phase 1 core move.
- **CLI `bench-layout`** — after Phase 2 makes layout platform-neutral.
- **macOS AppKit first-paint** — after Phase 4.
