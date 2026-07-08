# macOS Implementation Guide

This guide describes how to get FMDV running on macOS with a GUI while
preserving the repo's current priorities: fast first paint, native rendering,
small artifacts, minimal runtime dependencies, and strong headless tests.

The recommended path is a shared C++ core with native frontends:

```text
core/
  parser, document model, edit helpers, platform-neutral layout data

frontends/win32/
  current Win32/GDI app, adapted to call the shared core

frontends/macos/
  AppKit shell, custom NSView preview, CoreText/CoreGraphics rendering

frontends/cli/
  parse, render, timing, and automation entry points
```

Do not start by writing the macOS UI. Start by measuring the current Windows
implementation, then separate the reusable core without changing behavior, then
measure again. That gives the macOS port a performance target and catches any
regression introduced by the refactor.

## Goals

- Preserve or improve current first-paint behavior on Windows.
- Make the parser and shared behaviors buildable on Windows and macOS.
- Add a macOS GUI that feels native and opens quickly.
- Add a CLI mainly for tests, timing, and programmatic use.
- Collect comparable render/startup data before and after each major phase.
- Avoid browser engines, long-running backend processes, and heavyweight
  cross-platform runtimes unless a later benchmark proves they are worth it.

## Non-Goals

- Do not rewrite the app in SwiftUI, Electron, Tauri, Qt, or another framework
  as the first macOS attempt.
- Do not add a separate backend service. The shared core should be a library,
  not a daemon.
- Do not chase feature expansion during the port. Preserve existing behavior
  first: open file, render preview, scroll, split editor, save, theme, links,
  selection/copy, live reload, update flow.

## Phase 0: Add Timing and Logging First

Timing must be improved before any refactor. The current code has useful pieces:
`FMDV_TIMING=1` records startup marks, and `--benchpaint` measures layout and
culled paint cost in the debug build. Those should become one repeatable
benchmark surface with structured logs.

Add a benchmark/logging layer with these properties:

- One environment variable for the output path: `FMDV_BENCH_LOG`.
- One optional run label: `FMDV_BENCH_LABEL`.
- CSV or JSON Lines output, appended per run.
- Monotonic timestamps and durations.
- Same event names across Windows, CLI, and macOS.
- No network work, update checks, or optional UI creation before the first-paint
  timing mark.
- A default output path under the repo when run from tests, such as
  `bench/results/windows-baseline.csv`.

Recommended event fields:

```text
timestamp,platform,frontend,build,commit,label,file,theme,width,height,
event,duration_ms,blocks,content_height,notes
```

Recommended timing events:

```text
process_start
args_parsed
prefs_loaded
file_read
parsed
window_created
first_layout_start
first_layout_done
window_shown
first_paint
layout_once
paint_viewport_avg
scroll_paint_avg
editor_toggle_first_paint
edit_reparse_layout
```

Add command-line benchmark modes:

```powershell
# Windows examples
$env:FMDV_BENCH_LOG = "bench\results\windows-baseline.csv"
$env:FMDV_BENCH_LABEL = "pre-core-split"
.\cpp\fmdv_dbg.exe ..\test.md --bench-startup --width 900 --height 700
.\cpp\fmdv_dbg.exe ..\test.md --bench-render --width 900 --viewport 700 --scroll-runs 200
.\cpp\fmdv_dbg.exe ..\cpp\tests\stress.md --bench-render --width 1000 --viewport 800 --scroll-runs 200
```

The existing GUI timing path can keep using `QueryPerformanceCounter` on
Windows. The macOS implementation should use `mach_absolute_time`,
`clock_gettime`, or `std::chrono::steady_clock`, but the log schema should stay
the same.

### Phase 0 Acceptance Criteria

- CI can run parser and render benchmarks without a visible desktop.
- Local full tests can still drive the live Windows UI.
- Benchmark files are deterministic enough to compare medians.
- Results include at least 10 runs for each scenario before refactor work.
- The initial Windows baseline is committed as a small markdown summary, not as
  a giant raw data dump.

Recommended summary file:

```text
bench/results/README.md
```

Record:

- Machine and OS version.
- Compiler/toolchain.
- Commit hash.
- Build type.
- Median and p95 for first paint.
- Median and p95 for first layout.
- Median and p95 for viewport paint.
- Notes on whether the app was warm or cold.

## Phase 0.5: Decide the Core String Type

Resolve this before extracting anything in Phase 1. It touches the parser's core
type and every signature you are about to move, so deciding late means editing
the same code twice.

The current code is built on `std::wstring`. On Windows `wchar_t` is 16-bit, so
`std::wstring` is effectively UTF-16 and the parser's code-unit arithmetic
(`.compare(pos, n, ...)`, `wcslen`, per-character loops, surrogate handling)
assumes 16-bit units. On macOS and Linux `wchar_t` is 32-bit, so the identical
`std::wstring` silently becomes UTF-32: double the memory, and every hand-off to
CoreText (which takes UTF-16 `unichar` or UTF-8) becomes a re-encode. Code that
assumed 16-bit code units breaks quietly rather than failing to compile.

Pick one core string type and use it everywhere in `core/`:

- `std::u16string` / `char16_t` — least churn from today's UTF-16 assumptions,
  and maps directly to CoreText `unichar`. Recommended.
- UTF-8 `std::string` — smallest and idiomatic on macOS, but requires reworking
  the parser's index arithmetic and converting to UTF-16 at the CoreText edge.

Define neutral aliases in `core/` and forbid `std::wstring` below the frontend
layer:

```cpp
using Str = std::u16string;        // core string type
using StrView = std::u16string_view;
```

Frontends convert at their own boundary through explicit helpers, not ad hoc
casts spread through the codebase:

```cpp
// Win32 boundary only. Keep these out of core/.
static_assert(sizeof(wchar_t) == sizeof(char16_t));
std::wstring ToWin32String(StrView s);
Str FromWin32String(std::wstring_view s);

// CLI boundary.
std::string ToUtf8(StrView s);
Str FromUtf8(std::string_view s);
```

On Windows, `ToWin32String`/`FromWin32String` may use a checked copy or a local
reinterpretation only after the `static_assert`; on macOS, bridge `Str` to
`NSString`/`CFString`; in the CLI, read and write UTF-8. Keep all platform
string APIs and `wchar_t` assumptions in frontend or adapter files. The Phase 2
platform-neutral types below use `Str`, never `std::wstring`.

### Phase 0.5 Acceptance Criteria

> **Status: implemented.** `core/str.h` defines `Str`/`Char` as
> `std::u16string`/`char16_t` off Windows and `std::wstring`/`wchar_t` on Windows
> (already 16-bit UTF-16 there), with a `U16("literal")` macro. Core sources use
> `Str`/`Char`/`U16` and never write `std::wstring`/`wchar_t` directly, so the
> type is uniformly 16-bit UTF-16 everywhere and the Win32 frontend is unchanged.

- The core string type is chosen and documented before Phase 1 extraction.
- `core/` contains no `wchar_t` or `std::wstring` (except the aliases in
  `str.h`'s Windows branch).
- All `std::wstring`/`wchar_t` use is confined to Win32 frontend or adapter
  files, with helpers named clearly enough to grep.
- `--parse-dump` and any benchmark logging that prints core text converts
  through the same boundary helpers as the app.
- Parser output is byte-for-byte equivalent for existing fixtures after the type
  migration (compare via `--parse-dump`).

## Build System and CI

The repo builds only through `build.ps1` (MinGW) and CI runs only on
`windows-latest`. A shared core plus a CLI and two frontends needs portable build
tooling early, but the CI targets should appear in the same order as the code
they exercise.

- Introduce CMake alongside `build.ps1` when the core is extracted (Phase 1).
  Initial targets: `core` (static lib), `fmdv-win32`, and a tiny
  `core-smoke`/`core-bench` executable if needed to run parser and benchmark
  checks without the future CLI. Keep `build.ps1` as a thin wrapper so the
  existing Windows workflow and flags are unchanged.
- Add the `fmdv-cli` CMake target in Phase 3, then expand the macOS CI job to
  build and run it.
- Add a `macos-latest` CI job as soon as `core` is buildable. Before Phase 3 it
  should build `core` plus the minimal smoke/benchmark executable; after Phase 3
  it should build `core` and `fmdv-cli` and run parser/benchmark checks
  headlessly.
- Keep the Windows job as the release gate; the existing headless `--dump` PNG
  and `--parse-dump` checks stay the correctness baseline.

Keep test lanes explicit:

- **Headless gating:** parser dumps, PNG dumps, core benchmarks, CLI checks, and
  mocked updater/release parsing. These may run in CI on every PR.
- **Windows UI suite:** live window, clipboard, selection, autocomplete, table
  picker, update picker UI. Keep this local or non-blocking in CI unless it
  becomes reliable on hosted runners.
- **Networked update checks:** never gate PR CI on live GitHub release fetches.
  Gate only deterministic release JSON parsing and version comparison; keep
  live fetches as manual diagnostics.

## Phase 1: Separate the Core Without Changing Behavior

After baseline timing is recorded, move reusable code out of the Win32 app. Keep
the Windows app behavior identical at this stage.

Proposed structure:

```text
core/
  markdown.h
  markdown.cpp
  document.h
  edit_assist.h
  edit_assist.cpp
  release_info.h
  release_info.cpp
  text_io.h
  text_io.cpp

frontends/win32/
  fmdv.cpp
  render_gdi.h
  render_gdi.cpp
  prefs_win32.cpp
  updater_win32.cpp
  fmdv.rc
  fmdv.ico

frontends/cli/
  fmdv_cli.cpp

bench/
  results/
  fixtures/
```

Move in small steps:

1. Move the markdown parser and document model into `core/`.
2. Extract UTF-8 read/write and LF normalization into `core/text_io.*`, with
   platform-specific filesystem wrappers where needed.
3. Extract pure editor helpers:
   - autocomplete suggestion calculation
   - list continuation decision
   - table markdown generation
4. Extract release JSON parsing and version comparison.
5. Leave GDI rendering in Windows until the parser/editor/helper extraction is
   green.
6. Re-run the full Windows test suite and the benchmark suite.

### Phase 1 Acceptance Criteria

- Windows release and debug builds still pass.
- Full Windows test suite still passes.
- The first-paint median does not regress materially. A practical initial budget
  is less than 5 percent regression, or less than 5 ms absolute regression,
  whichever is larger.
- Parser output is byte-for-byte equivalent for existing test fixtures.
- Benchmark summary is updated with `post-core-split` data.

## Phase 2: Make Layout Platform-Neutral

The current renderer is fast, but the layout API is tied to GDI types. To share
layout behavior with macOS, split layout from platform drawing and measurement.

Introduce platform-neutral types:

```cpp
struct Color { uint8_t r, g, b, a; };
struct RectF { double x, y, w, h; };
enum class FontRole { Body, Mono, H1, H2, H3, H4, H5, H6 };

struct FontSpec {
    FontRole role;
    bool bold;
    bool italic;
    double px;
};

struct TextRun {
    RectF rect;
    Str text;                 // core string type from Phase 0.5, not std::wstring
    FontSpec font;
    Color color;
    bool spaceBefore;
};

struct DrawCommand {
    enum Kind { Rect, Frame, Line, Text } kind;
    RectF rect;
    Color color;
    FontSpec font;
    Str text;
};
```

Introduce two interfaces:

```cpp
class TextMeasurer {
public:
    virtual double width(const FontSpec& font, StrView text) = 0;
    virtual double height(const FontSpec& font) = 0;
    virtual int codeUnitAtX(const FontSpec& font, StrView text, double x) = 0;
    virtual double xAtCodeUnit(const FontSpec& font, StrView text, int index) = 0;
};

class Painter {
public:
    virtual void fillRect(RectF rect, Color color) = 0;
    virtual void frameRect(RectF rect, Color color) = 0;
    virtual void line(double x1, double y1, double x2, double y2, Color color) = 0;
    virtual void text(double x, double y, const FontSpec& font,
                      Color color, StrView text) = 0;
};
```

Windows gets a `GdiTextMeasurer` and `GdiPainter`. macOS gets a
`CoreTextMeasurer` and `CoreGraphicsPainter`. The hit-testing methods return
UTF-16 code-unit offsets, matching the chosen core string type and the current
Win32 behavior. That keeps selection/copy behavior portable and avoids mixing
code units, Unicode scalar values, and grapheme clusters in one API. If a later
phase wants user-perceived-character selection, introduce it deliberately as a
separate behavior change.

Do this in two steps so Windows never changes behavior and any regression is
caught immediately:

- **Step 2a — extract measurement.** Route all text metrics through
  `TextMeasurer`. On Windows, `GdiTextMeasurer` wraps the exact same
  `GetTextExtentPoint32W` calls the layout uses today, so pixels must not move.
  Prove it with the existing headless `--dump` PNG diff before continuing.
- **Step 2b — extract painting.** Route all drawing through `Painter`
  (`GdiPainter` on Windows). Re-run the PNG diff and `--bench-render`.

The layout code currently measures text inline while it lays out (single pass,
`HDC`-driven in `render.cpp`); threading a `TextMeasurer` through that pass and
reworking the `HFONT`-keyed font cache is the largest chunk of this phase.
Budget for it.

Cross-platform layout fixtures need tolerance, not an exact match: parser output
can be byte-identical while CoreText and GDI choose different wrap points, line
counts, and `content_height` (which feeds scrolling). Compare structure and
allow small metric deltas; do not assert pixel-identical macOS output.

Add text-measurement and selection fixtures before the macOS renderer is judged
complete:

- ASCII words and punctuation.
- Emoji represented by surrogate pairs.
- Combining marks, such as `e` plus U+0301.
- CJK text without spaces.
- Mixed LTR text with inline code and links.

### Phase 2 Acceptance Criteria

- Windows screenshots and headless dumps remain visually equivalent.
- Step 2a and Step 2b each pass the `--dump` PNG diff before the next begins.
- Selection hit testing still works, and tests assert UTF-16 code-unit offsets
  for the Unicode fixtures above.
- `--bench-render` output stays comparable to Phase 0 and Phase 1.
- The layout library compiles on macOS before any AppKit UI is written.

## Phase 3: Add the CLI

The CLI is not the main product, but it is important for tests and automation.
It should be the first non-Windows consumer of `core/`.

Suggested commands:

```text
fmdv-cli parse file.md
fmdv-cli suggest --line "**"
fmdv-cli table --cols 3 --rows 4
fmdv-cli bench-parse file.md --runs 100
fmdv-cli bench-layout file.md --width 900 --runs 50
```

Later, after the macOS renderer exists:

```text
fmdv-cli render file.md --out out.png --width 900 --theme dark
```

### Phase 3 Acceptance Criteria

- CLI builds on macOS and Windows.
- Parser tests can run through the CLI.
- Benchmark logs from the CLI use the same schema as the GUI apps.

## Phase 4: Build the macOS AppKit GUI

Start with a minimal Objective-C++ AppKit app, not SwiftUI. The goal is fastest
first useful paint with direct access to the C++ core.

Recommended files:

```text
frontends/macos/
  main.mm
  FMDVAppDelegate.mm
  FMDVWindowController.mm
  FMDVPreviewView.mm
  FMDVEditorController.mm
  CoreTextMeasurer.mm
  CoreGraphicsPainter.mm
  PrefsMac.mm
  UpdaterMac.mm
```

Initial window path:

1. Parse command-line file argument.
2. Read file and parse markdown.
3. Create `NSApplication`.
4. Create `NSWindow`.
5. Create one custom `NSView` for preview.
6. Layout once for the view width.
7. Show the window and force the first draw.
8. Record `first_paint`.
9. Only then create optional menus, schedule update checks, or initialize editor
   structures.

Preview implementation:

- `FMDVPreviewView : NSView`
- Draw cached display commands in `drawRect:`.
- Use CoreGraphics for fills, rules, frames, and selection highlights.
- Use CoreText for text measurement and drawing.
- Use `NSScrollView` or a lightweight custom scroll path. Start with
  `NSScrollView` unless measurement proves it is too slow.
- Use layer backing only if it improves measured first paint or scroll paint.

Editor implementation:

- Use `NSTextView` for the source editor.
- Create it lazily when the user toggles split editor.
- Reuse core edit helpers for autocomplete, table insertion, and list
  continuation.
- The ghost-text autocomplete overlay is drawn outside the text buffer on
  Windows; `NSTextView` has no equivalent primitive, so re-implement the overlay
  against its layout manager rather than expecting a direct port. Also account
  for LF (macOS) vs CRLF (Win32 EDIT control) and the different undo model.
- Use native macOS shortcuts:
  - Command-E or Command-Option-E for edit toggle, depending on menu conflicts
  - Command-S save
  - Command-Shift-S save and close editor
  - Command-plus/minus/0 zoom
  - Command-U update picker if it does not conflict with expected text behavior

macOS platform APIs:

- Preferences: `NSUserDefaults` or `~/Library/Application Support/FMDV/prefs.txt`.
- Links: `NSWorkspace openURL:`.
- File watching: `dispatch_source` or FSEvents.
- Clipboard: `NSPasteboard`.
- Update checks: `URLSession` after first paint.
- Packaging: `.app` bundle first; signing/notarization later.

### Phase 4 Acceptance Criteria

- App opens a markdown file from Finder or command line.
- First-paint timing is recorded with the shared schema.
- Preview renders the same fixture corpus as Windows. Parser structure should
  match exactly; layout and screenshots should use the Phase 2 metric tolerances
  rather than pixel-identical assertions.
- Scroll paint benchmark is recorded.
- Dark mode and zoom work.
- Link hit testing and selection/copy work before editor polish begins.

## Phase 5: Compare Windows and macOS

After the macOS preview is working, run the same fixtures on both platforms.
Keep hardware notes explicit because Windows and macOS will often run on
different machines.

Minimum comparison matrix:

```text
platform,frontend,file,width,height,theme,runs,first_paint_median_ms,
first_paint_p95_ms,layout_median_ms,viewport_paint_median_ms,
scroll_paint_median_ms
```

Scenarios:

- Empty file.
- `test.md`.
- Current README.
- `cpp/tests/stress.md`.
- Large generated markdown file with thousands of blocks.
- Split editor first open.
- Live edit reparse/layout after one keystroke.

Interpretation rules:

- Compare first useful paint, not just process start.
- Report cold and warm separately.
- Treat update checks and optional editor initialization as post-paint work.
- Compare medians first, p95 second.
- Do not optimize macOS with a different feature set than Windows.

## Risk Register

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Core split regresses Windows startup | High | Measure before and after every phase; keep Windows shell behavior unchanged during Phase 1. |
| Core string type (`wchar_t` is 16-bit on Windows, 32-bit elsewhere) | High | Decide the core string type in Phase 0.5; forbid `std::wstring` in `core/`; verify parser output is unchanged after migration. |
| CI target added before the code it builds exists | Medium | Add CMake and macOS CI incrementally: `core` plus a smoke/benchmark executable first, then add `fmdv-cli` to CI in Phase 3. |
| Unicode selection semantics drift between GDI and CoreText | Medium | Define hit testing as UTF-16 code-unit offsets; add emoji, combining-mark, CJK, and mixed inline-formatting fixtures before macOS renderer acceptance. |
| macOS editor integration diverges from Win32 | High | Reuse the core edit-decision helpers, but treat `NSTextView` integration (LF vs CRLF, undo model, ghost-text overlay) as a separate rewrite, not a port. |
| CoreText wrapping differs from GDI | Medium | Use platform-neutral layout tests plus visual fixture dumps; allow small text metric differences but preserve structure. |
| macOS text rendering starts slower than expected | Medium | Cache fonts, lazy-create editor, avoid SwiftUI/WebView, benchmark layer-backed vs non-layer-backed views. |
| Cross-platform layout abstraction becomes too generic | Medium | Abstract only measurement and painting; keep FMDV's document/display-list model simple. |
| Update mechanism differs by platform | Low | Share release parsing/version compare; keep install/swap platform-specific. |
| CLI grows into a second product | Low | Keep CLI focused on tests, automation, and benchmarks. |

## Recommended Milestones

> **Status (updated 2026-07-08).** Milestones 2–6 and 8–12 are complete and green
> in CI (Windows build + a `macos-latest` job that builds the CLI and the AppKit
> app, runs the unit tests, and renders fixtures). Milestones 1, 7, and 13 have
> Windows-side remainders (benchmark capture and the GDI→`core/layout` migration
> — see Remaining Work §4/§5). The macOS app opens files, renders (light/dark),
> scrolls, zooms, selects/copies, follows links, finds (Cmd+F), shows a TOC
> sidebar (Cmd+Shift+O), and has a lazy split editor with ghost-text
> autocomplete, list continuation, and table insert.
>
> **The port is not yet at full Windows parity.** Live reload, preferences
> persistence (dark/zoom/split), and the passive update-notify banner have since
> been implemented; the remaining reduced piece is the updater's **in-app
> install** (auto-update / pin), which is blocked by packaging. Several live
> interactions have only been compile/no-crash checked, and packaging
> (signing/notarization, a macOS release artifact) is not done. The complete,
> honest list is in **[Remaining Work](#remaining-work)** below.

1. `bench/logging`: add unified benchmark logging and capture Windows baseline.
2. `core/string-type`: choose the core string type (Phase 0.5) and migrate the
   parser/model off `std::wstring`.
3. `build/cmake`: add CMake targets for `core`, `fmdv-win32`, and any minimal
   smoke/benchmark executable needed by early macOS CI; keep `build.ps1`
   compatible.
4. `core/parser`: move parser/document model and keep Windows green.
5. `core/edit-helpers`: extract autocomplete, list continuation, and table
   generation.
6. `core/releases`: extract version comparison and release JSON parsing.
7. `core/layout`: introduce platform-neutral draw commands and GDI adapters
   (Step 2a measurement, then Step 2b painting).
8. `cli`: add parse and benchmark commands, then expand `macos-latest` CI to
   build and run `fmdv-cli`.
9. `macos/skeleton`: AppKit window, file open, parse, first paint.
10. `macos/render`: CoreText/CoreGraphics renderer and fixture comparisons.
11. `macos/interactions`: scrolling, selection/copy, links, dark mode, zoom.
12. `macos/editor`: lazy split editor, save, autocomplete, table picker.
13. `compare`: publish Windows vs macOS timing summary.

## Remaining Work

Tracker for everything left before the macOS port reaches Windows parity and is
shippable. Status key: ⬜ todo · 🔄 in progress · ✅ done · ⛔ blocked. Grouped by
kind, most impactful first. (Excludes running the app on a Windows machine, which
is an environment limitation, not a work item.)

### At a glance (updated 2026-07-08)

| Area | Status |
| --- | --- |
| Core render / interaction / editor features (render, scroll, zoom, dark, select+copy, links, find, TOC, editor, autocomplete, list continuation, table) | ✅ done |
| Live reload on external change | ✅ done |
| Preferences persistence (dark / zoom / split) | ✅ done |
| Updater — notify banner + launch-check preference | ✅ done |
| Updater — auto-update / pin / in-app install | ⛔ blocked by packaging (§3) |
| Hands-on Mac QA of live interactions (§2) | ⬜ needs a Mac desktop |
| Packaging — code signing / notarization + release artifact (§3) | ⬜ todo |
| Windows layout-engine unification (§4) | ⬜ planned (Windows-tested) |
| Windows benchmark — layout/render (§5) | 🔄 captured via CI artifact |
| Windows benchmark — GUI first-paint (§5) | ⬜ needs a Windows desktop |

### 1. Feature parity gaps — Windows has these; macOS does not yet

- ✅ **Live reload on external file change.** An `NSTimer` polls the file's mtime
  every 500 ms (mirroring the Win32 `WM_TIMER`) and reparses into the preview
  when it changes on disk; skips while the split editor is open so an external
  change never clobbers in-progress edits; a save-from-editor updates the stored
  mtime so it doesn't self-trigger. *(Reparse path smoke-tested on macOS; the
  visual refresh wants a hands-on pass — see §2.)*
- ✅ **Preferences persistence.** Dark mode, zoom, and the editor split ratio are
  saved to `NSUserDefaults` (`FMDVDark`/`FMDVZoomPct`/`FMDVSplitPct`) and
  restored before first paint — mirroring the Win32 `prefs.txt` dark/zoom/split
  fields. An explicit `--dark` flag still forces dark for that launch. Update
  mode/pin are not persisted (the macOS updater is check-and-link, not install).
  *(Compiles + launches clean; restore-on-relaunch wants a hands-on pass.)*
- 🔄 **Updater (partial).** Done: the passive "update available" banner on launch
  (Windows `UPDATE_NOTIFY` parity) — a silent GitHub check shows a dismissible
  top banner only when a newer release exists — plus a persisted "Check for
  Updates on Launch" preference (`FMDVUpdateNotify`) and the manual Cmd+U check.
  Still missing vs Windows: **auto-update**, **pin/downgrade**, and **in-app
  install** (exe swap). Those are blocked by code signing + the absence of a
  macOS release artifact (see §3), so they can't be closed on this branch.

### 2. Live macOS interactions — compile + no-crash only (logic is unit-tested)

The find/selection *logic* has unit tests (`tests/text_select_test.cpp`) and
rendering/TOC/ghost-text are verified via PNG/live-capture, but these
event-driven paths need a hands-on pass on a Mac desktop (no screen capture in
CI):

- ⬜ Selection drag (mouse-down → drag → copy), double/triple-click, auto-scroll.
- ⬜ Find bar: typing, Enter/Shift+Enter stepping + wraparound, Esc to close.
- ⬜ Cmd+U live network fetch + result alert.
- ⬜ Full editor session: type → reparse → list continuation → save → table insert.
- ⬜ TOC sidebar + split-editor pane resizing.
- ⬜ `.md` double-click file association (Info.plist `CFBundleDocumentTypes`).
- ⬜ Live reload: confirm the preview visibly refreshes on an external edit (the
  reparse path is smoke-tested; the visual update isn't screenshot-verified).
- ⬜ Preferences: toggle dark/zoom, drag the editor split, relaunch, and confirm
  each is restored.
- ⬜ Update banner: confirm it appears when a newer release exists, "View
  Releases…" opens the page, ✕ dismisses, and the launch-check preference
  toggles it off.

### 3. Packaging / distribution

- ⬜ **Code signing + notarization.** The `.app` is unsigned → Gatekeeper warns
  on first launch.
- ⬜ **macOS release artifact.** The CI `release` job is Windows-only (attaches
  `fmdv.exe`). Add a DMG or zipped `.app` and attach it to tagged releases. This
  also unblocks the updater's in-app install (§1).

### 4. Architecture / optional

- ⬜ **Unify the layout engines.** Milestone 7 (`core/layout`) currently backs
  only the macOS frontend; Windows still lays out in its GDI `render.cpp`.
  Migrating `render.cpp` onto `core/layout` would give one shared layout path
  (and is the last structural item in "Definition of Done"). Optional — Windows
  works as-is. A concrete, code-grounded step-by-step plan (Step 2a/2b, the
  specific divergences to resolve, and the PNG-diff gates) is written up in
  [render → core/layout migration](render-core-layout-migration.md). It is a
  **Windows-tested** change and hasn't been started — there is no Windows
  toolchain here to compile it or run the mandatory PNG diff.

### 5. Benchmarks

- 🔄 **Windows baseline.** The win32 headless **layout/render** rows now come from
  CI — the `build` job prints them and uploads a `win32-bench` artifact (see
  `bench/results/README.md`). Remaining: **GUI first-paint/startup**
  (`--bench-startup`), which needs a real Windows desktop (CI has no window
  server). The macOS and CLI columns are already recorded.

### 6. Known minor limitations (not necessarily blockers)

- TOC heading Y-offsets can go stale after a window resize until the next
  relayout.
- Find/selection are **frag-atomic**: a match or selection can't span a
  formatting-change boundary mid-line (e.g. bold→plain). This is intentional and
  matches the Windows behavior.

### Not a gap

- **Per-monitor / HiDPI:** handled natively by macOS (Retina backing scale) — no
  code needed. On Windows this is explicit (`PER_MONITOR_V2`).

## Definition of Done

The macOS port should be considered viable when:

- The shared `core/` builds on Windows and macOS from one CMake configuration
  with no `std::wstring` below the frontend layer.
- Windows timing after refactor is within the agreed regression budget.
- macOS first-paint timing is measured and competitive with Windows on similar
  hardware, or any difference is explained by OS/window-server behavior.
- The macOS app can open, render, scroll, edit, save, and copy from common
  markdown files.
- The CLI can run parser and layout benchmarks on both platforms.
- The repo has a repeatable benchmark summary showing:
  - Windows baseline before core split.
  - Windows after core split.
  - macOS AppKit/CoreText preview.
  - Windows versus macOS comparison notes.
