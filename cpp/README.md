# FMDV — native app (C++ / Win32 GDI)

Custom markdown parser + GDI layout/renderer drawing directly to the window.

- first-paint ~40–55 ms (WebView2 build: 250–500 ms)
- ~400 KB single exe, system libraries only

## Features
- GitHub-style rendering: headings, bold/italic/strikethrough, inline + fenced
  code, blockquotes, bullet/ordered/nested/task lists, tables with column
  alignment, horizontal rules, links, images (alt text). Table columns size to
  content (proportional to the widest cell) and wrap instead of overflowing
  when the pane is too narrow to fit everything.
- **Ctrl+E** — toggle split editor (monospace source left, live preview right);
  drag the divider to resize.
- **Ctrl+T** — insert a table via a grid picker (grows past 8x8 on demand, up
  to 20x20). Caret inside an existing table reopens the picker in resize mode
  instead — grows/shrinks rows and columns, preserving existing cell content.
- **Ctrl+D** — toggle dark mode (persists).
- **Ctrl+S** — save. **Ctrl+Shift+S** — save and close editor.
- **Ctrl+U** — updates: list GitHub releases, install any version in-app
  (running exe is swapped; takes effect next launch). Modes: notify (default),
  auto-update, or pin a specific version. Check runs async after first paint.
  Installing a release older than the one that introduced this feature
  requires a confirming second Enter (that version has no way back in-app).
- **Ctrl+Shift+O** — table of contents sidebar (headings from the current
  doc); click an entry to scroll there. Coexists with the split editor (a
  third pane to the left of it). No independent scrolling in v1 — entries
  past the window height are clipped rather than scrollable.
- **Ctrl+F** — find in doc: a small search box highlights every match (current
  one emphasized), Enter/Shift+Enter step forward/backward with wraparound,
  Esc closes. Matches don't span a formatting-change boundary within a line
  (e.g. bold -> plain mid-match won't be found) — same frag-is-atomic
  tradeoff selection/copy already make.
- Scrolling: mouse wheel, scrollbar, PgUp/PgDn/Home/End/arrows/space.
- Preferences (dark mode, split ratio, zoom, update mode/pin) saved to
  `%APPDATA%\fmdv\prefs.txt`.

## Build
Requires MinGW-w64 (GCC, UCRT — [winlibs](https://winlibs.com/) or MSYS2
`ucrt64`). `build.ps1` uses `g++` from `PATH`, the `FMDV_MINGW` env var, or the
`-MinGW` parameter (path to the toolchain's `bin` directory).

```powershell
.\build.ps1          # release: -O2 -mwindows -static -s  -> fmdv.exe
.\build.ps1 -Debug   # console build with symbols -> fmdv_dbg.exe (enables --parse-dump)
```

## Headless testing
```powershell
# Render a document to a PNG at a fixed width:
.\fmdv.exe file.md --dump out.png --width 900 [--dark]
# Render a scrolled viewport:
.\fmdv.exe file.md --dump out.png --width 900 --viewport 600 --scroll 300
# Dump the parsed document model (debug build):
.\fmdv_dbg.exe file.md --parse-dump
# Structured benchmark log (debug build):
$env:FMDV_BENCH_LOG = "..\bench\results\windows-baseline.csv"
$env:FMDV_BENCH_LABEL = "pre-core-split"
.\fmdv_dbg.exe ..\test.md --bench-startup --width 900 --height 700
.\fmdv_dbg.exe ..\test.md --bench-render --width 900 --viewport 700 --scroll-runs 200
# Capture the live window (incl. editor) via PrintWindow:
.\tests\capture.ps1 -Exe .\fmdv.exe -File ..\test.md -Out shot.png -Command 2001
# Updater (debug build): offline unit checks / live API / real download+swap
.\fmdv_dbg.exe --test-updater
.\fmdv_dbg.exe --check-updates
.\fmdv_dbg.exe --install-tag v1.0.0
# FMDV_VERSION_OVERRIDE=<ver> makes the app report that version (test hook)
```

`tests\run-tests.ps1` builds both configurations and runs 73 checks: parser,
rendering, stability, TOC sidebar, find in doc, selection + clipboard,
save round-trip, autocomplete, table picker (insert + resize), list
continuation, updater. `tests\run-tests-hidden.ps1` runs the identical suite
with every launched window relocated off-screen first, so it doesn't pop
windows over whatever else you're doing.

## Set as default app for .md
1. Right-click any `.md` → **Open with → Choose another app**
2. **Choose an app on your PC** → browse to `fmdv.exe`
3. Check **Always use this app**

Put `fmdv.exe` somewhere stable first (e.g. `%LOCALAPPDATA%\Programs\FMDV\`).

## Source layout
This directory is the Win32 frontend; the platform-neutral parts live in
[`../core/`](../core/) and are shared with the CLI (and the future macOS app).
See [../docs/macos-implementation-guide.md](../docs/macos-implementation-guide.md).

- `fmdv.cpp` — WinMain, window/message loop, input, editor, scrolling, PNG dump
- `render.h/.cpp` — font cache + layout/draw engine
- `prefs.h/.cpp` — preference persistence
- `theme.h` — light/dark palettes
- `updater.h/.cpp` — GitHub API fetch + in-place exe swap
- `fmdv.rc` — icon + version resource

Shared core (`../core/`):
- `markdown.h/.cpp` — parser (text → Document)
- `edit_assist.h/.cpp` — autocomplete, list continuation, table generation
- `release_info.h/.cpp` — release JSON parsing + version comparison
- `bench_log.h` — structured benchmark logging schema
