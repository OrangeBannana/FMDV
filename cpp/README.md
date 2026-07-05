# FMDV — Native (C++ / Win32 GDI)

A from-scratch markdown viewer with no browser engine. Custom markdown parser +
hand-written GDI layout/renderer drawing straight to the window.

- **first-paint ~40–55ms** (vs 250–500ms for the old WebView2 build)
- **~417 KB** single exe, no DLLs to ship (uses only system libraries)
- Custom icon embedded

## Features
- GitHub-style rendering: headings, bold/italic/strikethrough, inline + fenced
  code, blockquotes, bullet/ordered/nested lists, task lists, tables (with column
  alignment), horizontal rules, links, images (alt text).
- **Ctrl+E** — toggle split editor (monospace source left, live preview right);
  drag the divider to resize.
- **Ctrl+D** — toggle dark mode (persists).
- **Ctrl+S** — save edits to the file. **Ctrl+Shift+S** — save and close editor.
- Scrolling: mouse wheel, scrollbar, PgUp/PgDn/Home/End/arrows/space.
- Preferences (dark mode, split ratio) saved to `%APPDATA%\fmdv\prefs.txt`.

## Build
Requires a MinGW-w64 toolchain (GCC, UCRT — e.g. [winlibs](https://winlibs.com/) or
MSYS2's `ucrt64`). `build.ps1` finds `g++` on `PATH`, or you can point it at a
specific toolchain with the `FMDV_MINGW` env var or the `-MinGW` parameter (path
to the toolchain's `bin` directory).

```powershell
.\build.ps1          # release: -O2 -mwindows -static -s  -> fmdv.exe
.\build.ps1 -Debug   # console build with symbols -> fmdv_dbg.exe (enables --parse-dump)
```

## Testing (headless, since rendering can't be eyeballed in CI)
```powershell
# Render a document to a PNG to inspect layout:
.\fmdv.exe file.md --dump out.png --width 900 [--dark]
# Render a scrolled viewport:
.\fmdv.exe file.md --dump out.png --width 900 --viewport 600 --scroll 300
# Dump the parsed document model (debug build):
.\fmdv_dbg.exe file.md --parse-dump
# Capture the live window (incl. editor) without screen access:
.\tests\capture.ps1 -Exe .\fmdv.exe -File ..\test.md -Out shot.png -Command 2001
```

## Set as default app for .md
1. Right-click any `.md` → **Open with → Choose another app**
2. **Choose an app on your PC** → browse to this `fmdv.exe`
3. Check **Always use this app**.

Move `fmdv.exe` somewhere stable first (e.g. `%LOCALAPPDATA%\Programs\FMDV\`) so
the association doesn't break if the folder moves.

## Source layout
- `fmdv.cpp` — WinMain, window/message loop, input, editor, scrolling, PNG dump
- `markdown.h/.cpp` — parser (text → Document)
- `render.h/.cpp` — font cache + layout/draw engine
- `prefs.h/.cpp` — preference persistence
- `theme.h` — light/dark palettes
- `fmdv.rc` — icon + version resource
