# FMDV Native — Issue & Progress Tracker

Status key: ⬜ todo · 🔄 in progress · ✅ done · 🐛 bug · ⏸ blocked

## Phases
- ✅ **P0** Toolchain — portable MinGW GCC 16 UCRT unzipped locally, g++ + windres verified
- ✅ **P1** Skeleton window — instant open, icon, message loop, timing instrumented
- ✅ **P2** Markdown parser — all block + inline types verified via `--parse-dump`
- ✅ **P3** Layout + GDI render — all block types, wrapping, both themes, verified via `--dump` PNG
- ✅ **P4** Scrolling — wheel, scrollbar, keyboard (PgUp/Dn/Home/End/arrows/space), resize re-layout; verified via viewport dump
- ✅ **P5** Dark mode (Ctrl+D, persists) + prefs at %APPDATA%\fmdv\prefs.txt — verified load applies theme
- ✅ **P6** Editor split (Ctrl+E), live preview, dark theming both panes, Ctrl+S / Ctrl+Shift+S — save round-trip verified
- ✅ **P7** Polish — native exe installed as root fmdv.exe (WebView2 build kept as fmdv-webview2.exe), artifacts cleaned

## Rendering engine improvements (round 2)
Quick wins — all implemented & verified:
- ✅ **Clickable links** — link rects recorded during paint (`LinkHit`), hit-tested on
  click → `ShellExecute`; hand cursor on hover. *(interactive click needs a final manual check)*
- ✅ **Live reload** — 500ms watch timer reloads file on disk change (skips while editing);
  verified the preview auto-refreshed on an external edit.
- ✅ **Dark title bar** — `DWMWA_USE_IMMERSIVE_DARK_MODE`, toggles with Ctrl+D.
- ✅ **Zoom** — Ctrl +/- /0 and Ctrl+wheel; scales fonts + all layout constants via
  `SetRenderScale`/`S()`; persisted as `zoom` in prefs. Verified at 160%.
- ✅ **Persistent back-buffer** — one reusable DC/bitmap resized on WM_SIZE (was
  allocating per paint).
- ✅ **Per-monitor DPI awareness** — `SetProcessDpiAwarenessContext(PER_MONITOR_V2)`,
  `WM_DPICHANGED` re-scales + repositions; initial DPI via `GetDeviceCaps`.
  (folded into the same scale path as zoom)

Round 3 — user-reported polish (all fixed & verified):
- ✅ **Text rendering quality** — switched window to ClearType (grayscale kept only for
  PNG dump); draw consecutive same-font/color words as one TextOut so GDI handles
  spacing/kerning (fixes uneven/loose word spacing). Verified light + dark.
- ✅ **Continuous link underline** — underline + hit-rect now span all words of a link
  including the spaces between them (was per-word with gaps).
- ✅ **Text selection + copy** — drag to select (char-accurate via GetTextExtentExPoint),
  highlight behind text, I-beam cursor, Ctrl+C → clipboard. Click-vs-drag distinguishes
  link-open from selection. Verified: highlight renders, clipboard got correct text
  with spacing preserved across styled runs.

Round 4 — selection completeness (all fixed & verified):
- ✅ **Code-block selection** — unified text drawing through `emitTextFrag`, so code
  blocks now produce selectable fragments (were drawn on a separate path). Verified
  highlight + copy of code lines (indentation preserved).
- ✅ **Cross-block selection** — drag across paragraphs/headings/lists/code; copy emits
  newline-separated text. Verified end-to-end via clipboard.
- ✅ **Ctrl+A** select-all, **double-click** word, **triple-click** line (CS_DBLCLKS +
  triple detection via GetDoubleClickTime). Double-click verified.
- ✅ **Auto-scroll** while dragging past the top/bottom edge (40ms timer, forces a
  synchronous repaint so hit-testing uses fresh fragment positions).

Round 5 — table selection, test suite, markdown autocomplete:
- ✅ **Table cell selection** — table cells now route through `emitTextFrag` (were a
  separate draw path), so they're selectable + copyable; columns space-separated on copy.
- ✅ **Test suite** — `tests/run-tests.ps1`: builds release+debug then runs 32 checks
  (parser, rendering, stability, select-all+copy across all block types, save round-trip,
  autocomplete). Added `ID_SELECT_ALL` (2009) command for scriptable testing. All green.
- ✅ **Markdown autocomplete (ghost text)** — editor subclassed; typing an unbalanced
  opener (`**`,`__`,`~~`,`` ` ``,`[`) shows the closer as gray-italic OVERLAY at the caret
  (never in the buffer). **Tab and only Tab** commits it (one EM_REPLACESEL = one native
  undo unit). Any other key / click / Esc cancels (just hides the overlay; Esc no longer
  closes the window while editing). Parity check avoids suggesting on already-balanced
  delimiters; only fires when caret is at end of line. Fixed parse-dump `%s`→`%ls` bug.

Round 6 — richer autocomplete, table picker, editor polish:
- ✅ **More autocomplete triggers** — `(`→`)`, single `*`→`*`, `` `` ``→`` `` ``, and
  ` ``` `→ multi-line fenced block (blank code line + closing fence, caret in the middle).
- ✅ **Context split for `[`** — after a list marker (`- `/`* `/`+ `/`1. `) it completes a
  checkbox `- [ ] ` (caret after); elsewhere it completes a link `[|]()` (caret inside).
- ✅ **Per-suggestion caret placement** — `Sugg{text, caret}`; closers keep caret before,
  checkbox after, link inside. Multi-line ghost rendered segment-by-segment at the caret
  then the left margin. CRLF-aware commit so caret lands correctly in the edit control.
- ✅ **Table grid-picker (Ctrl+T)** — small popup grid; arrow keys size 1..8 cols/rows,
  Enter inserts a markdown table skeleton at the caret, Esc/lose-focus cancels.
- ✅ **Editor UI** — Cascadia Mono (FF_MODERN fallback), 15px, left/top padding via
  EM_SETRECT.
- Test suite now 42 checks (added 6 autocomplete + 4 table-picker), all green.

Round 7 — list continuation + Tab-character fix:
- ✅ **List continuation on Enter** — bullet (`-`/`*`/`+`) repeats the marker, ordered
  (`N.`) increments, task items repeat `- [ ] `; pressing Enter on an EMPTY item removes
  the marker to end the list. Indentation preserved.
- ✅ **Tab no longer leaks a tab char** — root cause: `WM_KEYDOWN(VK_TAB)` was consumed but
  `TranslateMessage` had already queued a `WM_CHAR(0x09)` that got inserted. Fix: handle
  Tab/Enter at `WM_CHAR` (where insertion actually happens). Now: ghost active + Tab =
  commit, no tab char; no ghost + Tab = a real tab. So cancel the suggestion (Esc/any key)
  then Tab if you want an actual tab.
- Test suite now 47 checks (added list continuation + Tab behavior), all green.

Round 8 — PERF: cached layout + visible-range painting:
- ✅ Split `RenderDocument` into **`LayoutDocument`** (builds a cached display list of
  draw commands in document space; runs once on content/width/zoom/theme change) and
  **`PaintDocument`** (culls the list to the viewport and offsets by scroll; runs every
  frame). Scrolling no longer re-measures/re-lays-out.
- Measured (`fmdv_dbg --benchpaint`): 4000-block doc → layout 193ms ONCE, then
  **1.5ms per scroll-paint** (was a full ~193ms relayout every scroll). Paint cost is
  bounded by the viewport, not the document size.
- first-paint held/improved (~40–55ms normal doc). Selection highlight is computed at
  paint time from cached frags, so it still updates without relayout. Ctrl+D relayouts
  once (colors are baked into the cached commands). All 47 tests still green.

Shipped since round 8 (see cpp/README.md for behavior):
- ✅ **Find in doc (Ctrl+F)** — highlights all matches, Enter/Shift+Enter step with
  wraparound, Esc closes. (find/selection logic now lives in `core/text_select`.)
- ✅ **TOC sidebar (Ctrl+Shift+O)** — headings from the current doc; click to jump.
- ✅ **Content-aware table columns + cell wrapping** — columns size to the widest
  cell and wrap instead of overflowing a narrow pane.
- ✅ **In-app updater (Ctrl+U)** — GitHub Releases list, install any version, modes
  notify/auto-update/pin (`core/release_info` for parse + version compare).

Remaining roadmap (not yet done):
- Smarter fence re-trigger suppression on the closing ```
- Incremental relayout on edit (currently full relayout per keystroke — fine for now)
- Off-thread / debounced parse
- DirectWrite renderer; RichEdit editor
- Selection in the editor pane already works (native EDIT control)

> **macOS port.** The parser, edit helpers, layout, find/selection, and release
> logic now live in a shared `core/` consumed by a native AppKit frontend
> (`frontends/macos/`). Remaining-work tracker:
> [docs/macos-implementation-guide.md](../docs/macos-implementation-guide.md#remaining-work).

## Open bugs
_(none)_

## Resolved
- LF-only text ran together in the EDIT control → convert LF→CRLF when populating editor (EDIT only breaks on CRLF).
- ClearType color fringing in offscreen/double-buffered text → ANTIALIASED_QUALITY fonts.
- Spurious spaces before punctuation after styled spans → track real source spaces per word.
- SetForegroundWindow before paint cost ~250ms → defer foreground until after first paint.
- FOLDERID_RoamingAppData link error in MinGW → use %APPDATA% env var instead.
- WM_PRINTCLIENT bypasses WM_CTLCOLOREDIT (PrintWindow showed light editor in dark mode) — testing artifact only; real on-screen grab confirmed correct dark theming.

## Notes / decisions
- macOS live-UI suite landed (2026-07-11): `tests/run-tests.sh` is this
  suite's AppKit analog — the app's `--test-drive` stdin channel executes
  commands as real NSEvents with synchronous replies (no Accessibility
  permission), so unlike the Windows UI suite it runs *gating* on hosted CI.
  76 checks mirroring the sections below, incl. a full updater install E2E
  against a localhost fixture server.
- macOS in-app updater landed (2026-07-11): tagged releases now also carry
  `FMDV-macos.zip` (CI `make dist`), `core/release_info` parses its asset URL
  alongside `fmdv.exe` (`ReleaseInfo.macUrl`), and the AppKit frontend gained
  the full Ctrl+U parity set — picker, auto-update/pin, download + bundle swap.
  Windows code is unaffected beyond the extra (ignored) `macUrl` field.
- `core/layout` migration landed (2026-07-11): the win32 layout now runs the
  shared engine; `render.cpp` translates the core display list into its cached
  GDI command list and paints as before. Two PNG-diff-gated steps (byte-identical
  `--dump` output, 71/71 suite, bench unchanged-or-better) — see
  [docs/render-core-layout-migration.md](../docs/render-core-layout-migration.md).
- Portable MinGW unzipped locally on the original dev machine (MSI installers blocked by policy).
- Testing relies on `--dump` PNG output since no screen access.
- Coexists with the working Go/WebView2 build until P7; root `fmdv.exe` stays the
  Go build until the native one reaches parity.

## Performance findings (P1)
- **first-paint ~65ms warm** (was ~230ms before fix; old WebView2 build 250-500ms).
- The fix: `DwmSetWindowAttribute(DWMWA_TRANSITIONS_FORCEDISABLED)` + `SW_SHOWNA`
  cut ShowWindow from ~160ms → ~8ms by killing the window-open animation.
- Remaining ~65ms = USER32/GDI subsystem init (~34ms) + CreateWindow (~25ms) +
  paint (~2ms). This is the cold-process floor; even Notepad pays it. <30ms is not
  realistically reachable without a warm preload daemon.
- Linking gdiplus has no measurable startup cost; GDI+ used only for --dump PNG.
- Release exe: 276 KB (vs 3.3 MB WebView2 build).
