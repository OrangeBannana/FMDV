# FMDV — native app (C++ / Win32 GDI)

Custom markdown parser + GDI layout/renderer drawing directly to the window.

- first-paint ~40–55 ms (WebView2 build: 250–500 ms)
- ~400 KB single exe, system libraries only

## Features
- GitHub-style rendering: headings, bold/italic/strikethrough, inline + fenced
  code, blockquotes, bullet/ordered/nested/task lists, tables with column
  alignment, horizontal rules, links, images (alt text).
- **Ctrl+E** — toggle split editor (monospace source left, live preview right);
  drag the divider to resize.
- **Ctrl+D** — toggle dark mode (persists).
- **Ctrl+S** — save. **Ctrl+Shift+S** — save and close editor.
- Scrolling: mouse wheel, scrollbar, PgUp/PgDn/Home/End/arrows/space.
- Preferences (dark mode, split ratio, zoom) saved to `%APPDATA%\fmdv\prefs.txt`.

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
# Capture the live window (incl. editor) via PrintWindow:
.\tests\capture.ps1 -Exe .\fmdv.exe -File ..\test.md -Out shot.png -Command 2001
```

`tests\run-tests.ps1` builds both configurations and runs 47 checks: parser,
rendering, stability, selection + clipboard, save round-trip, autocomplete,
table picker, list continuation.

## Set as default app for .md
1. Right-click any `.md` → **Open with → Choose another app**
2. **Choose an app on your PC** → browse to `fmdv.exe`
3. Check **Always use this app**

Put `fmdv.exe` somewhere stable first (e.g. `%LOCALAPPDATA%\Programs\FMDV\`).

## Source layout
- `fmdv.cpp` — WinMain, window/message loop, input, editor, scrolling, PNG dump
- `markdown.h/.cpp` — parser (text → Document)
- `render.h/.cpp` — font cache + layout/draw engine
- `prefs.h/.cpp` — preference persistence
- `theme.h` — light/dark palettes
- `fmdv.rc` — icon + version resource
