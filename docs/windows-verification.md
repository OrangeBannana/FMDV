# Windows verification — manual QA for the shared-core additions

The features added in the later commits on this branch are implemented in the
shared `core/` (unit-tested on both toolchains) plus a Win32 wiring in
`cpp/fmdv.cpp` / `cpp/render.cpp` that mirrors the existing link-click path.
The Win32 target can't be built on macOS, so this checklist verifies it on a
real Windows machine. CI's Windows job compiles it and runs the core unit tests;
the items below are the hands-on parts.

## What specifically needs checking here

| Area | Where the risk is |
|---|---|
| Clickable task checkboxes | New Win32 wiring: `ToggleTaskAt` in `fmdv.cpp` (hit-test in `WM_LBUTTONUP`), `WriteTextToFile` refactor, `render.cpp` `TaskHit` translation |
| Table reflow overflow fix | Shared `core/layout.cpp` (min-content column floor + token break); Win32 renders it via `render.cpp` |
| Empty list-item spacing | Shared `core/layout.cpp` |
| Double-click word / quoted-phrase selection | Windows switched from `IsWordChar` to the shared `DoubleClickSpan` — a behavior change to verify |

## 0. Build + automated tests

```powershell
cd cpp
.\build.ps1                                  # -> cpp\fmdv.exe   (must succeed clean)
```

Core unit tests under MinGW (these cover `ToggleTaskAtLine`, `DoubleClickSpan`,
and the table/layout changes — same suites CI runs):

```powershell
# from the repo root, with g++ on PATH (or FMDV_MINGW set)
$core = "core/str.cpp","core/markdown.cpp","core/edit_assist.cpp","core/release_info.cpp","core/layout.cpp","core/text_select.cpp"
foreach ($t in "str","markdown","edit_assist","release_info","layout","text_select","bench_log") {
  g++ -std=c++17 -O2 -Wall -Wextra -Icore "tests/${t}_test.cpp" @core -o "build/${t}-test.exe"
  & "build/${t}-test.exe"     # expect: ALL PASS (0 failures)
}
```

Optionally run the live Win32 UI suite (selection/clipboard/autocomplete):

```powershell
powershell -ExecutionPolicy Bypass -File cpp\tests\run-tests.ps1
```

- [ ] `build.ps1` compiles `fmdv.exe` with no errors.
- [ ] All 7 core unit-test exes print `ALL PASS`.

## 1. Clickable task checkboxes (new Win32 wiring)

Create `tasks.md`:

```markdown
# Tasks

- [ ] first
- [x] second
- [ ] third
```

```powershell
.\fmdv.exe tasks.md
```

- [ ] Clicking an **unchecked** box fills it in; clicking a **checked** box clears it.
- [ ] After clicking, the on-disk file updates (`type tasks.md`) — `[ ]`↔`[x]` on the
      clicked line only; every other line is byte-for-byte unchanged.
- [ ] Toggling works for the first, middle, and last item (source-line mapping).
- [ ] Open the editor (**Ctrl+E**) and click a checkbox in the preview → the raw
      markdown in the editor pane updates to match, and the file still saves correctly.
- [ ] Clicking the checkbox does **not** start a text selection or follow a link.

## 2. Empty list-item spacing (shared layout)

Add two empty checkboxes above a paragraph:

```markdown
- [x]
- [x]

Following paragraph.
```

- [ ] The two checkboxes render on **separate lines** and do **not** overlap each
      other or the "Following paragraph." line (this was the overlap bug).

## 3. Table reflow — no column overflow (shared layout)

Open a doc with a wide table where the first column holds long unbreakable tokens
(e.g. `FIRMWARE_SPEC.md`'s "Hardware interfaces" table: `SENSE_FWD`, `RELAY_HB`,
`USB_SERIAL`).

- [ ] At a normal width, columns look correct.
- [ ] **Drag the window narrower** and watch the table reflow: the `Signal`/`Dir`
      column tokens stay **inside their own column** — they do **not** spill into
      the next column. The prose columns (`Hardware`, `Notes`) wrap instead.
- [ ] Make the window very narrow: long tokens break across lines rather than
      overflowing; nothing bleeds into an adjacent column.

## 4. Double-click selection behavior change (shared `DoubleClickSpan`)

Windows now uses the same word model as macOS (whitespace-delimited, plus
quoted-phrase). Verify with a line like:
`He said "hello there world" and used COPYME-UNIQUE-TOKEN-42 today.`

- [ ] **Double-click** a plain word (`today`) → selects just that word.
- [ ] **Double-click** `COPYME-UNIQUE-TOKEN-42` → selects the **whole** hyphenated
      token (new: the old `IsWordChar` model split on the hyphens).
- [ ] **Double-click** inside `"hello there world"` → selects the three words
      **between** the quotes (quote marks excluded).
- [ ] **Triple-click** a line → selects the whole line.
- [ ] Drag-select, **Ctrl+A**, **Ctrl+C** still work and copy the expected text.

## Report

Note any step that fails with: the doc used, what you saw vs. expected, and a
screenshot if it's visual. Everything green here means the shared-core additions
behave identically on Windows and macOS.
