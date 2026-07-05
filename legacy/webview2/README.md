# Legacy WebView2 prototype (Go)

The original FMDV: a Go program that renders markdown with
[goldmark](https://github.com/yuin/goldmark) and displays it in a WebView2
window. It worked, but paid ~250–500 ms of browser-engine startup on every
launch — which is what motivated the from-scratch C++/Win32/GDI rewrite in
[`cpp/`](../../cpp/). Kept for reference.

```powershell
go build -ldflags "-H windowsgui -s -w" -o fmdv-webview2.exe
.\fmdv-webview2.exe path\to\file.md
```

Requires the WebView2 runtime (preinstalled on Windows 10/11).
