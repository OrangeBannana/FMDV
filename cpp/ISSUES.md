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
- Windows parity: the macOS fixes since v1.2.2 landed on Win32 (2026-08-28,
  `6ed437f` on `fix/windows-parity-rich-copy-flash`). Three gaps closed, with
  tests at both levels: (1) **HTML clipboard copy (#36)** — the macOS inline
  HTML-clipboard builder moved into shared `core/html_copy` (single source)
  and both frontends now call it: Windows writes the selection as CF_HTML
  (standard `"HTML Format"` offset header + StartFragment markers) alongside
  the unchanged plain text; the mac `NSPasteboardTypeHTML` now comes from the
  byte-identical fragment builder. New `tests/html_copy_test.cpp` (30
  exact-output checks: styles, headings spanning wrapped frags, link-href
  split/merge, entity escaping in content and hrefs, surrogate-pair-safe
  slicing) was added to every build system plus the CI MinGW test matrix —
  the CI job's hardcoded source/test lists had to be updated for that.
  (2) **afterText z-order** — the 80aaa22 display-list flag was ignored by the
  Win32 painter, so table grid/strikethrough/link-underline drew *under*
  glyphs; `PaintDocument` now runs the mac-equivalent 4 passes
  (backgrounds → highlights → text → afterText decorations), pinned by 24 new
  `tests/layout_test.cpp` checks incl. the 8 px box / 3 px icon radius
  contract and icon bounds inside the hit rect. (3) **Copy-button feedback**
  — the code-block copy button now shows the same soft rounded ~500 ms pill
  (th.sel, icon bounds) as macOS, cleared by a `WM_TIMER` heartbeat;
  `FMDV_TEST_COPY_FLASH_LOG` mirrors `FMDV_TEST_LINK_LOG` for the harness.
  `cpp/tests/run-tests.ps1` gained 5 live-UI parity sections (copy-button
  exact clipboard + no accidental selection, CF_HTML shape + tag content,
  flash lifecycle, find-bar X close via BM_CLICK, rounded-corner pixel check
  on `--dump` PNGs — suite is now 116 checks). All 8 core suites green
  locally (381 checks, gcc 15.2/Linux + `make test`-equivalent); the first
  MinGW/UCRT64 compile and the live-UI run happen in CI on the PR.
- Backfilled log entries for six since-v1.2.2 fixes that shipped without one
  (2026-08-28; commits of 2026-08-26/27): **HTML clipboard copy on preview
  copy** (#36, `e79b078`) — the selection now copies as formatted HTML
  (headings, bold/italic, code, links) alongside plain text so a paste keeps
  its structure; **rich-copy correctness + decoration z-order** (`80aaa22`)
  — links/headings spanning wrapped lines, merged same-href links, entity
  escaping, and decoration commands (strikethrough, link underline, table
  grid) painting *over* the text via the display list's `afterText` flag
  (mac honored it; the Win32 catch-up is the parity item above);
  **code-block copy button** (`388c871`) — a small icon in each fenced
  block's header strip copies the code verbatim on both frontends,
  hit-test surfaced through `core/layout`'s `codeCopyHits`; **rounded
  corners + soft click feedback** (`542cbb2`) — 8 px box / 3 px icon radii
  and a soft ~500 ms rounded highlight on button click instead of a hard
  flash; **find-bar close (X) button** (`c90c8e0`) on both frontends; and
  **inline code leading bias** (`064bf01`) — inline code padding is now
  weighted toward leading rather than trailing, with layout pins. All are
  covered by `tests/run-tests.sh` sections and core layout/text_select pins.
- Windows was missing the "launched with no file" open-dialog that macOS already
  has (2026-07-14 — regression/parity gap, not new in this port): `app.mm`'s
  `applicationDidFinishLaunching` shows an `NSOpenPanel` when launched without a
  file; the Win32 `Run()` just opened a blank window. Added `PickFileToOpen`
  (`GetOpenFileNameW`, `comdlg32` — new link dependency in `build.ps1`), wired in
  right before window creation (after the headless `--dump`/`--parse-dump`/bench
  early-returns, so CLI/test invocations that intentionally pass no file are
  unaffected). Initial folder: the last folder a file was opened from
  (`Prefs.lastOpenDir`, persisted to `prefs.txt` the same way as `dark`/`split`/
  `zoom`), falling back to `%USERPROFILE%\Downloads` the first run or if that
  folder no longer exists. Cancelling falls through to the existing blank-window
  path (mirrors macOS's `else { ensureWindow(); }`). Verified: clean build
  (0 warnings), on-screen capture of the dialog confirms it opens to Downloads
  with the Markdown filter active, and a standalone round-trip check of the new
  `lastOpenDir` (de)serialization (ASCII path, a path near the old 128-byte
  buffer limit, a non-ASCII path, and the empty/first-run case — all against a
  scratch `%APPDATA%`, not the real prefs file) all pass. Not verified live:
  actually driving the picker to select a file end-to-end — this dev sandbox
  can't inject keystrokes into a native modal common dialog (`SendKeys`/
  `AppActivate` don't land focus in it), so "pick a file → main window opens
  with it → `lastdir` updates to that folder" wants one manual pass on a real
  desktop.
- Double-click word model: trailing punctuation now trims (2026-07-14) — sign-off
  decided in favor of trimming (`today.` selects `today`, not `today.`). Changed in
  `core/text_select.cpp`'s `DoubleClickSpan` so both frontends get it; an
  all-punctuation token (e.g. an isolated `...`) is left intact rather than
  emptied. New cases in `tests/text_select_test.cpp`; core suite still `ALL PASS`.
- Windows-verification "honest holes" closed (2026-07-14): the manual checklist's
  five gaps that code review alone couldn't close are now driven live via
  `run-tests.ps1` — editor-pane sync on checkbox click (Ctrl+E), a plain click
  starting no text selection, hit-testing at 150% zoom (glyph x offset scales with
  zoom), and a live `SetWindowPos`-driven window resize (not just `--dump` PNGs) on
  the table-reflow doc. Full suite: 93/93.
- Preview click/selection hit-testing ignored the scroll offset (2026-07-14): `LinkAt`,
  `PointToSel`, and the new `ToggleTaskAt` compared the raw client y against
  document-space rects, so links/selection/checkboxes only hit correctly at
  `scrollY == 0` — scrolled down, a click toggled the wrong checkbox and drags grabbed
  the wrong line. Pre-existing for links/selection; the checkbox code inherited it by
  mirroring `LinkAt`. Fix: convert client→document y (`clientY + g_scrollY`) in all
  three hit-testers (matches macOS, whose `logicalPoint` already gets this from
  `NSScrollView`). Guarded by two new scrolled-click cases in `run-tests.ps1`; the
  stale "scroll-adjusted" comments in `render.h` were corrected. Empirically verified
  (scrolled drag copies the visible line; scrolled click toggles the visible checkbox).
- LF-only text ran together in the EDIT control → convert LF→CRLF when populating editor (EDIT only breaks on CRLF).
- ClearType color fringing in offscreen/double-buffered text → ANTIALIASED_QUALITY fonts.
- Spurious spaces before punctuation after styled spans → track real source spaces per word.
- SetForegroundWindow before paint cost ~250ms → defer foreground until after first paint.
- FOLDERID_RoamingAppData link error in MinGW → use %APPDATA% env var instead.
- WM_PRINTCLIENT bypasses WM_CTLCOLOREDIT (PrintWindow showed light editor in dark mode) — testing artifact only; real on-screen grab confirmed correct dark theming.

## Notes / decisions
- Final-pass review fixes (2026-07-11): a medium-depth review before declaring
  the port/rewrite/refactor done surfaced two issues, both fixed and tested.
  (1) A failed editor save was silent and, via Save & Close, discarded the
  unsaved edits when a reopen reloaded from disk. Save now reports the error and
  Save & Close keeps the editor open on failure — on both platforms (macOS
  `reportSaveFailure`/`writeDocToDisk`; Win32 `ReportSaveError`, and
  `ID_SAVE_CLOSE` now toggles only when `SaveToFile()` succeeds). (2) macOS
  `BenchLayoutRender` could paint into a NULL `CGContextRef` for a degenerate
  size (`--bench-render --width 0`); it now guards like `RenderMarkdownToPng`.
  New macOS checks cover the save-failure path (editor stays open, file intact)
  and the success close; a `save-close` test command invokes `saveAndClose:`
  directly since a synthetic Cmd+Shift+S can't be told apart from Cmd+S.
- macOS live-UI suite landed (2026-07-11): `tests/run-tests.sh` is this
  suite's AppKit analog — the app's `--test-drive` stdin channel executes
  commands as real NSEvents with synchronous replies (no Accessibility
  permission), so unlike the Windows UI suite it runs *gating* on hosted CI.
  88 checks mirroring the sections below, incl. a full updater install E2E
  against a localhost fixture server.
- macOS in-app updater landed (2026-07-11): tagged releases now also carry
  `FMDV-macos.zip` (CI `make dist`), `core/release_info` parses its asset URL
  alongside `fmdv.exe` (`ReleaseInfo.macUrl`), and the AppKit frontend gained
  the full Ctrl+U parity set — picker, auto-update/pin, download + bundle swap.
  Windows code is unaffected beyond the extra (ignored) `macUrl` field.
- `core/layout` migration landed (2026-07-11): the win32 layout now runs the
  shared engine; `render.cpp` translates the core display list into its cached
  GDI command list and paints as before. Two PNG-diff-gated steps on a real
  Windows 11 desktop (MSYS2 UCRT64 g++ 16.1) — Step 2a routed metrics through
  `GdiTextMeasurer` (display list unchanged), Step 2b swapped the layout body
  for `fmdv::LayoutDocument`. `core/layout` was rewritten as an integer-exact
  port of the GDI layout (Windows semantics are the reference: scaled `S()`
  constants, floored divisions, content-aware table sizing + cell wrapping),
  TOC anchors moved to `LayoutResult.blockTops`, zoom stays baked into metrics
  via the `scale` parameter, and selection hit-testing still measures via GDI.
  Gate held: byte-identical `--dump` output, 71/71 suite, bench
  unchanged-or-better, first-paint within budget.
- Portable MinGW unzipped locally on the original dev machine (MSI installers blocked by policy).
- Testing relies on `--dump` PNG output since no screen access.
- Coexists with the working Go/WebView2 build until P7; root `fmdv.exe` stays the
  Go build until the native one reaches parity.
- ✅ **Ordered lists honor source numbers + blockquote spacing (intent change, 2026-08-27).**
  User-reported: quote text crowded its neighbors, and list numbering from the raw
  markdown was lost/reset to 1. Root causes: the parser (`core/markdown.cpp`)
  dropped the `N.` number and the layout kept one global `olCounter` that reset on
  any non-ordered block; the blockquote case had no top margin of its own (6px
  after list items). Fix: `Block::listStart` (author's first number, capped 1e6)
  + per-level `olNext[]/olLive[]` counters in `core/layout.cpp` with GFM
  `<ol start="N">` semantics — a list renders from its first item's number and
  continues sequentially, an interrupted list restarts at its own number, nested
  ordered lists number independently, and a bullet inside an ordered list does NOT
  reset the outer list. Blockquote gains a 16px top margin (GitHub `margin: 1em
  0`: paragraph→quote 32px, list→quote 22px, was 6px). Both `--parse-dump`
  (win32) and CLI `parse` now emit `start=N` for ordered items. The two layout
  fixtures that previously *asserted* the reset-to-1 behavior were updated; new
  fixtures cover non-1 starts, interrupted lists, sequential-from-start, nested +
  interleaved lists, and quote clearance after list items / paragraphs. `test.md`
  gained an ordered-list section so the CI PNG renders exercise both fixes.
  Deliberate behavior change, full plan in
  `docs/fix-plan-quote-spacing-and-ol-numbering.md`; no frontend code touched.

## Performance findings (P1)
- **first-paint ~65ms warm** (was ~230ms before fix; old WebView2 build 250-500ms).
- The fix: `DwmSetWindowAttribute(DWMWA_TRANSITIONS_FORCEDISABLED)` + `SW_SHOWNA`
  cut ShowWindow from ~160ms → ~8ms by killing the window-open animation.
- Remaining ~65ms = USER32/GDI subsystem init (~34ms) + CreateWindow (~25ms) +
  paint (~2ms). This is the cold-process floor; even Notepad pays it. <30ms is not
  realistically reachable without a warm preload daemon.
- Linking gdiplus has no measurable startup cost; GDI+ used only for --dump PNG.
- Release exe: 276 KB (vs 3.3 MB WebView2 build).
