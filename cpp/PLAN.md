# FMDV — Native C++ / Win32 GDI Rewrite

> **Historical design document** written before the rewrite started, kept for
> context. A few details drifted during implementation: layout lives in
> `render.cpp` (there is no separate `layout.cpp`), and prefs are a plain-text
> `prefs.txt` rather than JSON. See `README.md` for the current state and
> `ISSUES.md` for the development log.

## Goal
A `.md` viewer that opens **near-instantly** (target: window visible in <30ms cold,
vs ~250–500ms for the WebView2/Go build). Achieved by eliminating the Chromium
engine entirely: a custom markdown parser + a hand-written GDI layout/render engine
drawing directly to the window.

## Why this can be near-instant
The WebView2 build spends ~150–300ms booting a full browser engine. A plain Win32
window + GDI has effectively zero engine startup — the cost is just process load
(~2–5ms) + parse + layout + first paint (all sub-millisecond for typical docs).

---

## Architecture

```
cpp/
  fmdv.cpp        WinMain, window class, message loop, input handling, mode switching
  markdown.h/.cpp Parser: UTF-8 text  ->  Document (vector of Blocks w/ inline spans)
  layout.h/.cpp   Layout: Document + client width  ->  vector of positioned DrawItems
  render.h/.cpp   Render: DrawItems -> GDI DC (screen, double-buffered) OR memory DIB
  theme.h         Light/dark color palettes, font size table
  prefs.h/.cpp    Load/save %APPDATA%\fmdv\prefs.json (dark flag, split ratio)
  fmdv.rc         Icon resource (reuse fmdv.ico)
  build.ps1       Sets PATH to portable MinGW, runs windres + g++, strips, outputs exe
  tests/          Sample .md files exercising every feature
```

### Data model
- **Block**: Heading(level), Paragraph, CodeBlock(lang), BlockQuote, ListItem(ordered,
  depth, checked-state), Table(headers, rows), HorizontalRule.
- **Inline span** (within a block's text): runs carrying flags {bold, italic, code,
  strike, link(href)}.

### Render pipeline
1. Parse once on load (and on every edit keystroke in editor mode).
2. Layout against current client width — produces absolute (x,y) draw items with
   the right font handle + color. Word-wrap via `GetTextExtentPoint32W`.
3. Paint: blit from a cached back-buffer DIB (double-buffering kills flicker);
   re-layout only on resize or content change, not on scroll.

### Fonts (created once, cached, deleted on exit)
Body, H1–H6 (scaled), monospace (code/pre), and bold/italic/bolditalic variants of
body + mono. Use `CreateFontW` with ClearType quality.

---

## Build toolchain
Portable **MinGW-w64 (winlibs, GCC 16, UCRT)** unzipped locally (no MSI — group
policy blocked installers on the original dev machine). `g++` + `windres`.
Static-link libstdc++ and use `-mwindows` for a GUI subsystem exe (no console window).

Build flags: `-O2 -municode -mwindows -static -s -Wall -Wextra`.
Link: `-lgdi32 -lgdiplus -lcomctl32 -luser32 -lshell32`.

---

## Testing strategy (no screen access — must self-verify)

1. **`--dump <out.png>`** — headless mode: parse + layout + render to an in-memory
   DIB at a fixed width, save as PNG via GDI+, exit (no window). The agent reads the
   PNG to visually confirm rendering. *This is the primary correctness check.*
2. **`--parse-dump`** — print the parsed Document model as indented text to stdout;
   verifies parser logic independent of rendering.
3. **Startup timing** — when `FMDV_TIMING=1`, log QueryPerformanceCounter delta from
   entry to first-paint into stderr. Confirms the speed goal numerically.
4. **No-crash smoke** — launch with each test file, sleep, assert process alive, kill.
5. **Clean build gate** — `-Wall -Wextra` must produce zero warnings before a phase
   is marked done.

Test corpus in `cpp/tests/`: headings, emphasis, nested lists, task lists, code
fences, inline code, blockquotes, tables, hr, links, long paragraphs (wrap),
CRLF file, empty file, huge file (perf).

---

## Phases

- **P0 Toolchain** — unzip MinGW, verify `g++ --version`, compile a 1-line Win32 exe.
- **P1 Skeleton** — instant window, icon, correct title, dark-aware background,
  message loop, clean exit. Verify timing <30ms.
- **P2 Parser** — blocks + inlines. Verify via `--parse-dump`.
- **P3 Layout + Render** — fonts, wrapping, all block types, GDI paint +
  double-buffer. Verify via `--dump` PNGs.
- **P4 Scroll + Resize** — wheel, scrollbar, keyboard (PgUp/Dn/Home/End), re-layout
  on resize.
- **P5 Theme + Prefs** — Ctrl+D dark toggle, persist to prefs.json, restore on open.
- **P6 Editor** — Ctrl+E split, multiline EDIT control left + live preview right,
  draggable splitter, Ctrl+S save / Ctrl+Shift+S save&close.
- **P7 Polish** — perf pass, replace root `fmdv.exe`, update file-association notes.

See ISSUES.md for live tracking.
