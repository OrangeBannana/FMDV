# Windows test handoff — `fix/windows-parity-rich-copy-flash`

**Goal:** Verify the branch tip builds and passes on real Windows **before** merging.
**Rule:** test only — do NOT push anything, do NOT merge, do NOT modify sources.

Branch base at time of writing: **`ca3a42e`**; the tip must be **`c9c612e`** (this handoff doc) — verify both appear in `git log`. If the checked-out tip differs, stop
and report — the tested byte sequence must match.

---

## Part 1 — Human instructions

### 1. Prerequisites
- Windows 10/11, Git, PowerShell 5+.
- **MinGW-w64 UCRT64** toolchain (`g++` + `windres`) — matching CI. Make it
  available one of three ways:
  - on `PATH`, or
  - `$env:FMDV_MINGW = "C:\path\to\mingw64\bin"`, or
  - pass `-MinGW <path>` (see `cpp/build.ps1`).
- ~2 GB disk for build intermediates.

### 2. Get the code
```powershell
git clone https://github.com/OrangeBannana/FMDV.git   # or cd into an existing clone
cd FMDV
git fetch origin fix/windows-parity-rich-copy-flash
git checkout fix/windows-parity-rich-copy-flash
git log -1 --oneline      # must print: c9c612e (this handoff doc); its parent is ca3a42e
```

### 3. Build + full automated suite (one command)
```powershell
powershell -ExecutionPolicy Bypass -File cpp\tests\run-tests.ps1
```
Builds **release** (`cpp\fmdv.exe`) + **debug** (`cpp\fmdv_dbg.exe`) from clean,
then runs the **116-check** live-UI suite (selection, autocomplete, copy-button
flash, CF_HTML rich clipboard, find bar, geometry).
**Pass = 116/116, zero FAIL lines.**

### 4. Headless checks (as CI does)
```powershell
cd cpp
.\fmdv.exe  test.md --parse-dump                       # must print the layout dump (6+ non-empty info lines)
.\fmdv.exe  test.md --dump light.png  --width 900      # PNG exists, ≥ 1500 bytes
.\fmdv.exe  test.md --dump dark.png   --width 900 --dark
.\fmdv.exe  test.md --dump scroll.png --width 900 --viewport 500 --scroll 250
.\fmdv_dbg.exe test.md --bench-render --width 900 --viewport 500 --scroll-runs 5   # completes, prints bench numbers
```

### 5. Hands-on parity checks (the actual bug fixes — visual)
1. **G2 copy-flash:** click a code block → a copy button appears → click it → the
   button should **flash/highlight briefly and revert** (no stuck highlight, no
   1-frame blink).
2. **G3 rich copy:** select a passage containing inline code/links → Ctrl+C →
   paste into **Word** (or WordPad/Notepad++): text should arrive as **rich HTML**
   (code spans formatted, links clickable), not plain text. Paste plain-text path:
   the copy *button* should still give **plain text**.
3. **G1 afterText z-order:** any inline elements with trailing text render with
   correct layering (text not clipped/overpainted).

### 6. Report back
- `run-tests.ps1`: total PASS/FAIL + any failing check names verbatim.
- §4: one line each (ok / error text), file sizes for the PNGs.
- §5: pass/fail per item with a one-line description of any anomaly.
- Compiler warning count from the build step (should be zero on our sources).

---

## Part 2 — Agent prompt (paste to a coding agent on the Windows machine)

```text
You are on a Windows 10/11 machine with Git and a MinGW-w64 UCRT64 toolchain
(g++ and windres reachable via PATH or $env:FMDV_MINGW — if neither works,
report that as the single blocker and stop; do not install anything).

Repo: https://github.com/OrangeBannana/FMDV.git
Branch under test: fix/windows-parity-rich-copy-flash (tip must be c9c612e or newer, with ca3a42e in its history)

TASK — verify only. Do NOT modify sources, do NOT push, do NOT create PRs.

1. git clone the repo (or use existing), git fetch, git checkout the branch,
   confirm `git log -1 --oneline` starts with c9c612e (this handoff doc). If not, stop and report.

2. Run the full suite (it builds release+debug first):
   powershell -ExecutionPolicy Bypass -File cpp\tests\run-tests.ps1
   Capture the full transcript. Pass criterion: 116/116, no FAIL lines.

3. From cpp/, run the headless checks and verify each:
   .\fmdv.exe test.md --parse-dump            → prints a non-empty layout dump
   .\fmdv.exe test.md --dump light.png  --width 900          → PNG ≥ 1500 bytes
   .\fmdv.exe test.md --dump dark.png   --width 900 --dark   → PNG ≥ 1500 bytes
   .\fmdv.exe test.md --dump scroll.png --width 900 --viewport 500 --scroll 250
   .\fmdv_dbg.exe test.md --bench-render --width 900 --viewport 500 --scroll-runs 5 → exits 0
   Report file sizes where applicable.

4. Visual parity checks (drive the real window; you may use run-tests.ps1's
   PostMessage helpers as reference for how to script this):
   a) G2: code copy-button click → button flashes and reverts (not stuck).
   b) G3: select text with inline code + a link, copy, and confirm the
      clipboard holds BOTH a text/html (CF_HTML) AND text/plain entry —
      e.g. via Add-Type -AssemblyName System.Windows.Forms;
      [System.Windows.Forms.Clipboard]::GetDataObject() then
      GetDataObject().GetFormats(). Rich path must not be plain-text only.
   c) G1: inline-code text renders with trailing text correctly layered
      (no clipping/overpaint in the --dump PNG is an acceptable proxy).

5. Final report format (plain text, copy-pasteable):
   - build: ok/failed + warning count
   - suite: N/116 + verbatim failing lines if any
   - headless: 5 lines, one per command
   - parity: G1/G2/G3 pass/fail + one line each
   - anything anomalous, verbatim

If ANY step fails: capture exact error output, identify the failing
check/section, and stop — report the failure rather than fixing it.
```

---

## Why this shape
The agent prompt is deliberately **verify-only, no-fix**. The first *real
MinGW* compile of the G3 clipboard code happens at step 2's build step — the
three build fixes in `5b5fe93` were found by cross-compiling on Linux, so
real-toolchain behavior is the main open question. A build failure there is the
most likely early signal; report it back for a same-turn fix + push, then re-run
from step 2 on the Windows box.

## Context: what this branch contains (tip `c9c612e` over base `f285155`)
| Commit | What |
|---|---|
| `6ed437f` | Parity: G1 afterText z-order, G2 copy-button flash, G3 rich HTML clipboard — Win32 port + tests + CI wiring |
| `59081ec` | Docs: refresh test-suite counts (html_copy added) |
| `8af697a` | Docs: record since-v1.2.2 fixes + Windows parity in log/feature lists/manuals |
| `5b5fe93` | Fix 3 Win32 G3 build errors found by cross-compilation |
| `2d03570` | `.tools/` local toolchain dir → .gitignore |
| `ca3a42e` | .gitignore: IDE/editor scratch, logs, wine prefix |
| `c9c612e` | This handoff doc (instructions + agent prompt) |

Already verified locally (Linux): full Win32 + all-core cross-compile green
(-Wall -Wextra), 8/8 core suites cross-compiled to PE, 381/381 core checks on
host g++, run-tests.ps1 116/116 syntax-verified, docs audit.
Still Windows-only: the actual build + runtime above. Still macOS-only (CI):
swiftc build of `frontends/macos/app.mm` + its 88-check suite.
