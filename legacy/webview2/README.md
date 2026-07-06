# Legacy WebView2 prototype (Go)

The original FMDV: Go + [goldmark](https://github.com/yuin/goldmark) rendering
into a WebView2 window. Superseded by the C++ rewrite in [`cpp/`](../../cpp/) —
WebView2 costs 250–500 ms of engine startup per launch. Kept for reference.

```powershell
go build -ldflags "-H windowsgui -s -w" -o fmdv-webview2.exe
.\fmdv-webview2.exe path\to\file.md
```

Requires the WebView2 runtime (preinstalled on Windows 10/11).
