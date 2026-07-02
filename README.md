# FMDV — Fast MD Viewer

A lightning-fast native Windows markdown viewer/editor. Double-click a `.md` file
and the window is up in ~40–55 ms — no browser engine, no Electron. A custom
markdown parser + a hand-written Win32/GDI layout & rendering engine.

- **~40–55 ms** cold first-paint (vs 250–500 ms for a WebView2 build)
- **~417 KB** single self-contained exe, no DLLs to ship
- Cached layout + viewport culling → scrolling stays ~1.5 ms even on huge docs

## Run
The prebuilt native binary is [`fmdv.exe`](fmdv.exe) at the repo root:

```
fmdv.exe path\to\file.md
```

To make it your default `.md` handler: right-click a `.md` file → *Open with* →
*Choose another app* → browse to `fmdv.exe` → *Always*. (Move the exe somewhere
stable first, e.g. `%LOCALAPPDATA%\Programs\FMDV\`, so the association survives.)

## Features
- GitHub-style rendering: headings, bold/italic/strikethrough, inline + fenced code,
  blockquotes, bullet/ordered/nested lists, task lists, tables (with alignment),
  rules, links, images (alt text). Light + dark themes.
- **Ctrl+E** split editor with live preview · **Ctrl+D** dark mode · **Ctrl+S** save
  (`Ctrl+Shift+S` save & close) · **Ctrl+±/0** and Ctrl+scroll zoom.
- Text selection + copy in the preview (double-click word, triple-click line, Ctrl+A).
- Clickable links, live reload on external file change, per-monitor DPI.
- Editor niceties: markdown autocomplete (ghost text, Tab to accept), list
  continuation on Enter, and a **Ctrl+T** table grid-picker.

## Source & build
The app lives in [`cpp/`](cpp/). See [cpp/README.md](cpp/README.md) for the build
(portable MinGW-w64, `cpp\build.ps1`), the headless test/inspection flags, and
[cpp/ISSUES.md](cpp/ISSUES.md) for the full development log and roadmap.

```powershell
cd cpp
.\build.ps1                       # -> cpp\fmdv.exe
powershell -File tests\run-tests.ps1   # 47-check suite
```

> **Toolchain note:** the build script points at a portable MinGW at
> `C:\Users\<user>\mingw\mingw64` (MSI installers were blocked by policy on the
> original machine, so it was unzipped manually). On a new device, install/unzip
> MinGW-w64 (GCC, UCRT) and update the `$mingw` path at the top of `build.ps1`.

## History
Started as a Go + WebView2 prototype (`main.go`, still in the repo), then rewritten
from scratch in C++/Win32/GDI for the speed. The git history reflects that pivot.
