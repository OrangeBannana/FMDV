// FMDV - Fast MD Viewer (native Win32 / GDI)
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <string>
#include <vector>
#include "theme.h"
#include "markdown.h"
#include "edit_assist.h"
#include "render.h"
#include "prefs.h"
#include "updater.h"
#include "version.h"

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_ROUND 2
#endif

static double g_freq = 0.0;
static LARGE_INTEGER g_start;

static double NowMs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - g_start.QuadPart) * 1000.0 / g_freq;
}

static bool g_timing = false;
struct TimeMark { const char* label; double ms; };
static TimeMark g_marks[32];
static int g_markCount = 0;
static void Timing(const char* label) {
    if (!g_timing) return;
    if (g_markCount < 32) { g_marks[g_markCount].label = label; g_marks[g_markCount].ms = NowMs(); g_markCount++; }
}
static void FlushTiming() {
    if (!g_timing) return;
    wchar_t path[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n) return;
    wcscat_s(path, MAX_PATH, L"fmdv_timing.log");
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    for (int i = 0; i < g_markCount; i++)
        fprintf(f, "[timing] %-16s %.3f ms\n", g_marks[i].label, g_marks[i].ms);
    fclose(f);
}

// --- Globals for window state ---
static std::wstring g_filePath;
static Theme g_theme;
static bool g_dark = false;
static Document g_doc;
static Prefs g_prefs;

// Read a UTF-8 file into a wstring, strip BOM, normalize CRLF -> LF.
static bool ReadFileUtf8(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    std::vector<char> buf((size_t)sz.QuadPart);
    DWORD rd = 0;
    if (!buf.empty()) ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr);
    CloseHandle(h);
    size_t off = 0;
    if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        off = 3; // UTF-8 BOM
    int need = MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)(buf.size() - off), nullptr, 0);
    std::wstring w(need, L'\0');
    if (need) MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)(buf.size() - off), &w[0], need);
    // normalize CRLF -> LF
    std::wstring norm; norm.reserve(w.size());
    for (wchar_t c : w) { if (c != L'\r') norm += c; }
    out.swap(norm);
    return true;
}

#ifdef FMDV_CONSOLE
static const char* BlockTypeName(BlockType t) {
    switch (t) {
        case BlockType::Heading: return "Heading";
        case BlockType::Paragraph: return "Paragraph";
        case BlockType::CodeBlock: return "CodeBlock";
        case BlockType::BlockQuote: return "BlockQuote";
        case BlockType::ListItem: return "ListItem";
        case BlockType::Table: return "Table";
        case BlockType::HRule: return "HRule";
    }
    return "?";
}
static void DumpRuns(const std::vector<InlineRun>& runs) {
    for (auto& r : runs) {
        wprintf(L"    run [");
        if (r.bold) wprintf(L"b");
        if (r.italic) wprintf(L"i");
        if (r.code) wprintf(L"c");
        if (r.strike) wprintf(L"s");
        if (!r.href.empty()) wprintf(L"link=%ls", r.href.c_str());
        wprintf(L"] \"%ls\"\n", r.text.c_str());
    }
}
static void ParseDump(const Document& doc) {
    wprintf(L"=== Document: %zu blocks ===\n", doc.blocks.size());
    for (size_t bi = 0; bi < doc.blocks.size(); bi++) {
        const Block& b = doc.blocks[bi];
        wprintf(L"[%zu] %hs", bi, BlockTypeName(b.type));
        if (b.type == BlockType::Heading) wprintf(L" level=%d", b.level);
        if (b.type == BlockType::ListItem) wprintf(L" %ls depth=%d task=%d",
            b.ordered ? L"ordered" : L"bullet", b.level, b.taskState);
        if (b.type == BlockType::CodeBlock) wprintf(L" lang=%ls", b.lang.c_str());
        wprintf(L"\n");
        if (b.type == BlockType::CodeBlock) {
            wprintf(L"    <code>\n%ls\n    </code>\n", b.codeText.c_str());
        } else if (b.type == BlockType::Table) {
            wprintf(L"    headers: %zu cols, %zu rows, aligns:", b.headers.size(), b.rows.size());
            for (int a : b.aligns) wprintf(L" %d", a);
            wprintf(L"\n");
            for (auto& h : b.headers) { wprintf(L"    H:"); DumpRuns(h.runs); }
            for (size_t ri = 0; ri < b.rows.size(); ri++) {
                wprintf(L"    row %zu:\n", ri);
                for (auto& c : b.rows[ri].cells) DumpRuns(c.runs);
            }
        } else {
            DumpRuns(b.runs);
        }
    }
}
#endif

// ---- GDI+ PNG dump (headless visual test) ----
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(info[i].MimeType, format) == 0) { *pClsid = info[i].Clsid; return (int)i; }
    }
    return -1;
}

static bool DumpToPng(const std::wstring& outPath, int width, int viewportH = 0, int scroll = 0) {
    SetFontQuality(ANTIALIASED_QUALITY); // grayscale for offscreen DIB (avoids ClearType fringing in PNG)
    Theme th = g_dark ? DarkTheme() : LightTheme();

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    int contentH = LayoutDocument(mem, width, g_doc, th, nullptr, nullptr);
    int height = (viewportH > 0) ? viewportH : contentH;
    if (height < 100) height = 100;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, dib);

    RECT full{ 0, 0, width, height };
    HBRUSH bg = CreateSolidBrush(th.bg);
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    std::vector<TextFrag> noFrags;
    PaintDocument(mem, scroll, width, height, th, nullptr, noFrags);

    bool ok = false;
    {
        Gdiplus::Bitmap bmp(dib, nullptr);
        CLSID png;
        if (GetEncoderClsid(L"image/png", &png) >= 0)
            ok = (bmp.Save(outPath.c_str(), &png, nullptr) == Gdiplus::Ok);
    }

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

static int g_scrollY = 0;
static int g_contentH = 0;   // total laid-out document height
static int g_clientH = 0;    // current client area height
static int g_clientW = 0;

// --- structured benchmark logging ---
static bool g_benchEnabled = false;
static bool g_benchStartup = false;
static std::wstring g_benchLogPath;
static std::string g_benchLabel;
static std::string g_benchCommit;
static int g_benchWindowW = 1100;
static int g_benchWindowH = 800;
static bool g_firstLayoutLogged = false;

static std::string WideToUtf8(const std::wstring& w) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(need, '\0');
    if (need) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], need, nullptr, nullptr);
    return s;
}

static std::string EnvUtf8(const wchar_t* name) {
    wchar_t* value = _wgetenv(name);
    return (value && value[0]) ? WideToUtf8(value) : std::string();
}

static std::string Csv(const std::string& s) {
    bool quote = false;
    for (char c : s) if (c == ',' || c == '"' || c == '\n' || c == '\r') { quote = true; break; }
    if (!quote) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static void EnsureParentDirs(const std::wstring& path) {
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] != L'\\' && path[i] != L'/') continue;
        if (i == 0 || (i == 2 && path[1] == L':')) continue;
        std::wstring dir = path.substr(0, i);
        if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    }
}

static bool FileIsEmptyOrMissing(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return true;
    return fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0;
}

static std::string IsoUtcNow() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
             (unsigned)st.wMilliseconds);
    return buf;
}

static void InitBenchLog(bool forceDefaultPath) {
    wchar_t* path = _wgetenv(L"FMDV_BENCH_LOG");
    if (path && path[0]) g_benchLogPath = path;
    else if (forceDefaultPath) g_benchLogPath = L"bench\\results\\windows-baseline.csv";
    if (g_benchLogPath.empty()) return;

    g_benchEnabled = true;
    g_benchLabel = EnvUtf8(L"FMDV_BENCH_LABEL");
    g_benchCommit = EnvUtf8(L"FMDV_BENCH_COMMIT");
    if (g_benchCommit.empty()) g_benchCommit = EnvUtf8(L"GITHUB_SHA");
}

static void BenchLog(const char* event, double durationMs, int width = 0, int height = 0,
                     int contentH = -1, const char* notes = "") {
    if (!g_benchEnabled) return;

    EnsureParentDirs(g_benchLogPath);
    bool needHeader = FileIsEmptyOrMissing(g_benchLogPath);
    FILE* f = _wfopen(g_benchLogPath.c_str(), L"ab");
    if (!f) return;
    if (needHeader) {
        fprintf(f, "timestamp,platform,frontend,build,commit,label,file,theme,width,height,event,duration_ms,blocks,content_height,notes\n");
    }

    const char* build =
#ifdef FMDV_CONSOLE
        "debug";
#else
        "release";
#endif
    int outW = width > 0 ? width : (g_clientW > 0 ? g_clientW : g_benchWindowW);
    int outH = height > 0 ? height : (g_clientH > 0 ? g_clientH : g_benchWindowH);
    int outContentH = contentH >= 0 ? contentH : g_contentH;

    fprintf(f, "%s,windows,win32,%s,%s,%s,%s,%s,%d,%d,%s,%.3f,%zu,%d,%s\n",
            Csv(IsoUtcNow()).c_str(),
            Csv(build).c_str(),
            Csv(g_benchCommit).c_str(),
            Csv(g_benchLabel).c_str(),
            Csv(WideToUtf8(g_filePath)).c_str(),
            g_dark ? "dark" : "light",
            outW,
            outH,
            Csv(event).c_str(),
            durationMs,
            g_doc.blocks.size(),
            outContentH,
            Csv(notes).c_str());
    fclose(f);
}

#ifdef FMDV_CONSOLE
static int RunBenchRender(int width, int viewportH, int scrollRuns) {
    if (width <= 0) width = 1000;
    if (viewportH <= 0) viewportH = 800;
    if (scrollRuns <= 0) scrollRuns = 1;

    SetFontQuality(ANTIALIASED_QUALITY);
    Theme th = g_dark ? DarkTheme() : LightTheme();
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    if (!screen || !mem) {
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        return 2;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -viewportH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return 2;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, dib);

    std::vector<TextFrag> frags;
    double t0 = NowMs();
    int contentH = LayoutDocument(mem, width, g_doc, th, nullptr, &frags);
    double layoutMs = NowMs() - t0;
    BenchLog("layout_once", layoutMs, width, viewportH, contentH, "duration");

    auto paintOnce = [&](int scroll) {
        RECT full{0, 0, width, viewportH};
        HBRUSH bg = CreateSolidBrush(th.bg);
        FillRect(mem, &full, bg);
        DeleteObject(bg);
        PaintDocument(mem, scroll, width, viewportH, th, nullptr, frags);
    };

    double t1 = NowMs();
    for (int i = 0; i < scrollRuns; i++) paintOnce(0);
    double viewportMs = (NowMs() - t1) / scrollRuns;
    BenchLog("paint_viewport_avg", viewportMs, width, viewportH, contentH, "duration");

    double t2 = NowMs();
    for (int i = 0; i < scrollRuns; i++) {
        int scroll = (contentH > viewportH) ? (i * (contentH - viewportH) / scrollRuns) : 0;
        paintOnce(scroll);
    }
    double scrollMs = (NowMs() - t2) / scrollRuns;
    BenchLog("scroll_paint_avg", scrollMs, width, viewportH, contentH, "duration");

    wprintf(L"blocks=%zu contentH=%d layout_once=%.2f ms paint_viewport_avg=%.3f ms scroll_paint_avg(%d)=%.3f ms\n",
            g_doc.blocks.size(), contentH, layoutMs, viewportMs, scrollRuns, scrollMs);

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    FreeFontCache();
    return 0;
}
#endif

// --- editor state (P6) ---
static bool g_editing = false;
static HWND g_hEdit = nullptr;
static HFONT g_editFont = nullptr;
static HBRUSH g_editBrush = nullptr;
static int g_splitX = 0;          // pixel x of the divider's left edge
static bool g_dragging = false;
static std::wstring g_rawText;    // current markdown source
static const int DIVIDER_W = 5;
static const int EDIT_ID = 1001;
static const int MIN_PANE = 160;

// --- table of contents sidebar ---
struct TocHeading { int level; std::wstring text; int docY; };
static bool g_showToc = false;
static std::vector<TocHeading> g_toc;
static std::vector<int> g_blockTops;      // doc-space y per g_doc.blocks[i], from LayoutDocument
static const int TOC_ROW_H = 26, TOC_PAD_TOP = 12, TOC_PAD_X = 12, TOC_INDENT = 12;
static int g_tocHoverIdx = -1;
static bool g_tocTrackingLeave = false;    // TrackMouseEvent armed for WM_MOUSELEAVE

// --- find in doc (Ctrl+F) ---
static HWND g_findHwnd = nullptr;
static HWND g_findEdit = nullptr;
static HBRUSH g_findBrush = nullptr;
static std::vector<FindMatch> g_findMatches;
static int g_findCurrent = -1;
static const int FIND_ID = 1002;

// command IDs (driven by the accelerator table so they work even while typing)
enum { ID_EDIT_TOGGLE = 2001, ID_DARK = 2002, ID_SAVE = 2003, ID_SAVE_CLOSE = 2004,
       ID_ZOOM_IN = 2005, ID_ZOOM_OUT = 2006, ID_ZOOM_RESET = 2007, ID_COPY = 2008,
       ID_SELECT_ALL = 2009, ID_INSERT_TABLE = 2010, ID_UPDATES = 2011, ID_TOC = 2012,
       ID_FIND = 2013 };

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// forward declarations (used by helpers defined above their definitions)
static int PreviewLeft();
static void UpdateLayout(HWND hwnd, bool redrawScrollbar = true);
static void RebuildFindMatches();

// zoom / DPI
static int g_zoomPct = 100;
static UINT g_dpi = 96;
static void ApplyScale() { SetRenderScale((g_zoomPct / 100.0) * (g_dpi / 96.0)); }

// clickable links recorded during the last on-screen paint (buffer coords)
static std::vector<LinkHit> g_links;

// text selection state
static std::vector<TextFrag> g_frags;   // drawn text runs from the last paint
static Selection g_sel;                 // normalized selection (a <= b)
static SelPoint g_selAnchor;            // where the drag started
static bool g_selecting = false;        // left button down + dragging to select
static POINT g_downPt = {};             // mouse-down point (to tell click from drag)
static POINT g_dragPt = {};             // latest drag point (for auto-scroll)
static bool g_autoScroll = false;       // auto-scroll timer running
static DWORD g_lastDblTime = 0;         // for triple-click detection
static const UINT_PTR AUTOSCROLL_TIMER = 2;

// persistent double-buffer
static HDC g_backDC = nullptr;
static HBITMAP g_backBmp = nullptr;
static int g_backW = 0, g_backH = 0;

// file-watch (live reload)
static const UINT_PTR WATCH_TIMER = 1;
static FILETIME g_fileTime = {};

static FILETIME FileMtime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        return fa.ftLastWriteTime;
    return FILETIME{};
}

// Dark/light title bar to match the theme.
static void ApplyTitleBar(HWND hwnd) {
    BOOL dark = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// Ensure the persistent back-buffer matches (w,h); recreate only when size changes.
static void EnsureBackBuffer(HDC ref, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!g_backDC) g_backDC = CreateCompatibleDC(ref);
    if (g_backW != w || g_backH != h) {
        HBITMAP nb = CreateCompatibleBitmap(ref, w, h);
        SelectObject(g_backDC, nb);              // deselects the previous bitmap
        if (g_backBmp) DeleteObject(g_backBmp);  // now safe to delete
        g_backBmp = nb;
        g_backW = w; g_backH = h;
    }
}

static void FreeBackBuffer() {
    if (g_backBmp) { DeleteObject(g_backBmp); g_backBmp = nullptr; }
    if (g_backDC)  { DeleteDC(g_backDC); g_backDC = nullptr; }
    g_backW = g_backH = 0;
}

// Return the href under a client point, or empty. Link rects are in preview-buffer
// coords (x from 0), so subtract the preview pane's left edge.
static std::wstring LinkAt(int clientX, int clientY) {
    int bx = clientX - PreviewLeft();
    for (const auto& l : g_links)
        if (bx >= l.rc.left && bx < l.rc.right && clientY >= l.rc.top && clientY < l.rc.bottom)
            return l.href;
    return L"";
}

static bool SelLess(const SelPoint& a, const SelPoint& b) {
    return a.frag < b.frag || (a.frag == b.frag && a.ch < b.ch);
}

// Map a client point to a selection point (nearest text fragment + char).
// g_frags rects are in document space, so convert clientY through the scroll
// offset before hit-testing against them (see emitRun in render.cpp).
static SelPoint PointToSel(HWND hwnd, int clientX, int clientY) {
    SelPoint sp;
    if (g_frags.empty()) return sp;
    int bx = clientX - PreviewLeft();
    int docY = clientY + g_scrollY;
    HDC dc = GetDC(hwnd);
    int best = -1; long bestDist = 0x7fffffff; bool rowHit = false;
    for (int i = 0; i < (int)g_frags.size(); i++) {
        const RECT& r = g_frags[i].rc;
        bool inRow = docY >= r.top && docY < r.bottom;
        if (inRow && bx >= r.left && bx <= r.right) { best = i; rowHit = true; break; }
        long dist;
        if (inRow) dist = (bx < r.left) ? (r.left - bx) : (bx - r.right);
        else       dist = 100000 + labs((long)((r.top + r.bottom) / 2 - docY));
        if (inRow && !rowHit) { rowHit = true; bestDist = dist; best = i; }
        else if (inRow && dist < bestDist) { bestDist = dist; best = i; }
        else if (!rowHit && dist < bestDist) { bestDist = dist; best = i; }
    }
    if (best < 0) { ReleaseDC(hwnd, dc); return sp; }
    sp.frag = best;
    sp.ch = FragCharAtX(dc, g_frags[best], bx);
    ReleaseDC(hwnd, dc);
    return sp;
}

static bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

// Select the word under a client point (double-click).
static void SelectWordAt(HWND hwnd, int x, int y) {
    SelPoint sp = PointToSel(hwnd, x, y);
    if (sp.frag < 0 || sp.frag >= (int)g_frags.size()) return;
    const std::wstring& t = g_frags[sp.frag].text;
    int a = sp.ch, b = sp.ch;
    if (a > (int)t.size()) a = b = (int)t.size();
    while (a > 0 && IsWordChar(t[a-1])) a--;
    while (b < (int)t.size() && IsWordChar(t[b])) b++;
    if (a == b) { // not on a word char — select the single char
        if (b < (int)t.size()) b++;
    }
    g_sel.a = SelPoint{ sp.frag, a };
    g_sel.b = SelPoint{ sp.frag, b };
    g_sel.active = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Select the whole visual line under a client point (triple-click).
static void SelectLineAt(HWND hwnd, int x, int y) {
    SelPoint sp = PointToSel(hwnd, x, y);
    if (sp.frag < 0 || sp.frag >= (int)g_frags.size()) return;
    int top = g_frags[sp.frag].rc.top;
    int first = sp.frag, last = sp.frag;
    while (first > 0 && g_frags[first-1].rc.top == top) first--;
    while (last + 1 < (int)g_frags.size() && g_frags[last+1].rc.top == top) last++;
    g_sel.a = SelPoint{ first, 0 };
    g_sel.b = SelPoint{ last, (int)g_frags[last].text.size() };
    g_sel.active = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void SelectAll(HWND hwnd) {
    if (g_frags.empty()) return;
    g_sel.a = SelPoint{ 0, 0 };
    g_sel.b = SelPoint{ (int)g_frags.size() - 1, (int)g_frags.back().text.size() };
    g_sel.active = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Build the selected text and put it on the clipboard.
static void CopySelection(HWND hwnd) {
    if (!g_sel.active) return;
    std::wstring out;
    int top = INT_MIN;
    for (int i = g_sel.a.frag; i <= g_sel.b.frag && i < (int)g_frags.size(); i++) {
        const TextFrag& f = g_frags[i];
        int c0 = (i == g_sel.a.frag) ? g_sel.a.ch : 0;
        int c1 = (i == g_sel.b.frag) ? g_sel.b.ch : (int)f.text.size();
        if (c1 < c0) continue;
        if (i != g_sel.a.frag) {
            if (f.rc.top != top) out += L"\n";
            else if (f.spaceBefore) out += L" ";
        }
        out += f.text.substr(c0, c1 - c0);
        top = f.rc.top;
    }
    if (out.empty()) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (out.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        memcpy(p, out.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static void ApplyZoom(HWND hwnd, int pct) {
    if (pct < 50) pct = 50;
    if (pct > 300) pct = 300;
    if (pct == g_zoomPct) return;
    g_zoomPct = pct;
    ApplyScale();
    g_prefs.zoomPct = pct;
    SavePrefs(g_prefs);
    UpdateLayout(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static int TocW() { return g_showToc ? MulDiv(220, g_dpi, 96) : 0; }
static int PreviewLeft() { return g_editing ? g_splitX + DIVIDER_W : TocW(); }
static int PreviewWidth() { int w = g_clientW - PreviewLeft(); return w > 0 ? w : 0; }

static void ClampSplit() {
    if (g_splitX < TocW() + MIN_PANE) g_splitX = TocW() + MIN_PANE;
    if (g_splitX > g_clientW - MIN_PANE) g_splitX = g_clientW - MIN_PANE;
    if (g_splitX < 0) g_splitX = 0;
}

static int MaxScroll() {
    int m = g_contentH - g_clientH;
    return m > 0 ? m : 0;
}

// Reposition the edit control to fill the left pane.
static void PositionEdit() {
    if (!g_hEdit) return;
    if (g_editing) {
        int left = TocW();
        MoveWindow(g_hEdit, left, 0, g_splitX - left, g_clientH, TRUE);
        // inset the text with a little breathing room (left/top padding);
        // EM_SETRECT is control-relative, not parent-relative — the control's
        // own origin already moved to `left` via MoveWindow above
        RECT fr{ 14, 12, (g_splitX - left) - 8, g_clientH };
        SendMessageW(g_hEdit, EM_SETRECT, 0, (LPARAM)&fr);
        ShowWindow(g_hEdit, SW_SHOW);
    } else {
        ShowWindow(g_hEdit, SW_HIDE);
    }
}

// Recompute content height for the current preview width and update the scrollbar.
// redrawScrollbar=false skips the forced synchronous scrollbar repaint — used
// while dragging the editor divider, which calls this on every WM_MOUSEMOVE
// tick; forcing a redraw that often made the native scrollbar visibly flicker.
static void UpdateLayout(HWND hwnd, bool redrawScrollbar) {
    RECT rc; GetClientRect(hwnd, &rc);
    g_clientW = rc.right; g_clientH = rc.bottom;
    g_sel.active = false; // re-layout invalidates fragment indices
    if (g_editing) { ClampSplit(); PositionEdit(); }
    HDC dc = GetDC(hwnd);
    bool logFirstLayout = g_benchEnabled && !g_firstLayoutLogged;
    double layoutStart = NowMs();
    if (logFirstLayout) BenchLog("first_layout_start", layoutStart, PreviewWidth(), g_clientH, -1, "elapsed");
    g_contentH = LayoutDocument(dc, PreviewWidth(), g_doc, g_theme, &g_links, &g_frags, &g_blockTops);
    if (logFirstLayout) {
        g_firstLayoutLogged = true;
        BenchLog("first_layout_done", NowMs() - layoutStart, PreviewWidth(), g_clientH, g_contentH, "duration");
    }
    ReleaseDC(hwnd, dc);

    g_toc.clear();
    for (size_t i = 0; i < g_doc.blocks.size(); i++) {
        const Block& b = g_doc.blocks[i];
        if (b.type != BlockType::Heading) continue;
        std::wstring text; for (auto& r : b.runs) text += r.text;
        int docY = (i < g_blockTops.size()) ? g_blockTops[i] : 0;
        g_toc.push_back(TocHeading{ b.level, text, docY });
    }

    if (g_findHwnd) RebuildFindMatches(); // frag indices are stale after relayout

    if (g_scrollY > MaxScroll()) g_scrollY = MaxScroll();

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (g_contentH > 0) ? g_contentH - 1 : 0;
    si.nPage = g_clientH;
    si.nPos = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, redrawScrollbar);
}

static void ScrollTo(HWND hwnd, int y) {
    if (y < 0) y = 0;
    if (y > MaxScroll()) y = MaxScroll();
    if (y == g_scrollY) return;
    g_scrollY = y;
    SetScrollPos(hwnd, SB_VERT, g_scrollY, TRUE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Pull text from the edit control, normalize CRLF->LF, reparse, relayout, repaint preview.
static void ReparseFromEdit(HWND hwnd) {
    if (!g_hEdit) return;
    int len = GetWindowTextLengthW(g_hEdit);
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(g_hEdit, &buf[0], len + 1);
    buf.resize(len);
    std::wstring norm; norm.reserve(buf.size());
    for (wchar_t c : buf) if (c != L'\r') norm += c;
    g_rawText = norm;
    g_doc = ParseMarkdown(norm);
    UpdateLayout(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Write current edit text (or g_rawText) back to the file as UTF-8 (LF endings).
static bool SaveToFile() {
    if (g_filePath.empty()) return false;
    std::wstring text = g_rawText;
    if (g_hEdit) {
        int len = GetWindowTextLengthW(g_hEdit);
        std::wstring buf(len + 1, L'\0');
        GetWindowTextW(g_hEdit, &buf[0], len + 1);
        buf.resize(len);
        std::wstring norm; norm.reserve(buf.size());
        for (wchar_t c : buf) if (c != L'\r') norm += c;
        text = norm;
        g_rawText = norm;
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::vector<char> utf8(need);
    if (need) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), need, nullptr, nullptr);
    // Write to a temp file and swap it into place: writing the target with
    // CREATE_ALWAYS truncates first, so a crash or full disk mid-write would
    // destroy the document. Same pattern the updater uses for the exe swap.
    std::wstring tmp = g_filePath + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL wok = utf8.empty() ? TRUE : WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
    wok = wok && wr == (DWORD)utf8.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!wok) { DeleteFileW(tmp.c_str()); return false; }
    // ReplaceFileW keeps the target's attributes/ACLs but requires it to
    // exist; fall back to a plain move if the file vanished since load.
    if (!ReplaceFileW(g_filePath.c_str(), tmp.c_str(), nullptr, 0, nullptr, nullptr) &&
        !MoveFileExW(tmp.c_str(), g_filePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    g_fileTime = FileMtime(g_filePath); // don't let our own write trigger a live-reload
    return true;
}

// A failed save must be visible and non-destructive: surface the error and (for
// Save & Close) keep the editor open so the unsaved edits aren't lost.
static void ReportSaveError(HWND hwnd) {
    MessageBoxW(hwnd,
        L"FMDV couldn't write the file. Check its permissions and free disk "
        L"space; your edits are still open in the editor.",
        L"Couldn't save the file", MB_OK | MB_ICONWARNING);
}

// ---------------- markdown autocomplete (ghost text) ----------------
//
// The suggestion is drawn as a gray-italic OVERLAY at the caret — it is never
// part of the text buffer. Tab inserts it for real (one EM_REPLACESEL = one
// native undo unit). Any other key/click just hides the overlay (cancel). This
// keeps undo/cancel semantics trivial.

static std::wstring g_ghost;       // current suggestion text (empty = none)
static int g_ghostCaret = 0;       // caret offset into the suggestion after commit
static HFONT g_ghostFont = nullptr;
static int g_editMarginX = 14;     // must match PositionEdit's EM_SETRECT left inset

// The suggestion logic lives in core/edit_assist.h (fmdv::SuggestClose) so the
// CLI and macOS frontends share it. This file keeps only the Win32 glue below.

// Recompute the ghost suggestion from the editor's current caret + line.
static void UpdateGhost() {
    if (!g_hEdit) return;
    std::wstring prev = g_ghost;
    g_ghost.clear(); g_ghostCaret = 0;

    DWORD s = 0, e = 0;
    SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (s == e) { // no active selection
        int len = GetWindowTextLengthW(g_hEdit);
        std::wstring text(len + 1, L'\0');
        GetWindowTextW(g_hEdit, &text[0], len + 1);
        text.resize(len);
        int caret = (int)e;
        if (caret <= (int)text.size()) {
            bool atLineEnd = (caret == (int)text.size() || text[caret] == L'\r' || text[caret] == L'\n');
            if (atLineEnd) {
                int ls = caret;
                while (ls > 0 && text[ls-1] != L'\n') ls--;
                fmdv::Suggestion sg = fmdv::SuggestClose(text.substr(ls, caret - ls));
                g_ghost = sg.text; g_ghostCaret = sg.caret;
            }
        }
    }
    if (g_ghost != prev) InvalidateRect(g_hEdit, nullptr, FALSE);
}

// Insert the ghost for real at the caret; place caret per the suggestion.
static void CommitGhost() {
    if (g_ghost.empty() || !g_hEdit) return;
    std::wstring ins = g_ghost; int coff = g_ghostCaret;
    g_ghost.clear();
    DWORD s = 0, e = 0;
    SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    // edit control uses CRLF; our suggestion uses \n — expand for insertion + offset
    std::wstring crlf; int crlfCaret = 0;
    for (int i = 0; i < (int)ins.size(); i++) {
        if (i == coff) crlfCaret = (int)crlf.size();
        if (ins[i] == L'\n') crlf += L"\r\n"; else crlf += ins[i];
    }
    if (coff >= (int)ins.size()) crlfCaret = (int)crlf.size();
    SendMessageW(g_hEdit, EM_SETSEL, e, e);
    SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)crlf.c_str()); // TRUE => undoable
    DWORD cpos = e + crlfCaret;
    SendMessageW(g_hEdit, EM_SETSEL, cpos, cpos);
    InvalidateRect(g_hEdit, nullptr, FALSE);
}

static void ClearGhost() {
    if (g_ghost.empty()) return;
    g_ghost.clear();
    if (g_hEdit) InvalidateRect(g_hEdit, nullptr, FALSE);
}

// On Enter inside a list item, continue the list (next bullet / next number /
// task box). On an EMPTY item, remove the marker to end the list. Returns true
// if it handled the Enter (caller should swallow the default newline).
static bool HandleListEnter() {
    if (!g_hEdit) return false;
    DWORD s = 0, e = 0;
    SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (s != e) return false;
    int len = GetWindowTextLengthW(g_hEdit);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(g_hEdit, &text[0], len + 1); text.resize(len);
    int caret = (int)e;
    if (caret > (int)text.size()) return false;
    int ls = caret; while (ls > 0 && text[ls-1] != L'\n') ls--;

    // Decision (which marker to continue, or to end the list) is shared core;
    // the frontend applies it to the edit control (CRLF line endings).
    fmdv::ListEnter le = fmdv::DecideListEnter(text.substr(ls, caret - ls));
    if (!le.handled) return false;
    if (le.endList) { // empty item -> clear the marker
        SendMessageW(g_hEdit, EM_SETSEL, ls, caret);
        SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)L"");
        return true;
    }
    std::wstring ins = L"\r\n" + le.continuation;
    SendMessageW(g_hEdit, EM_SETSEL, caret, caret);
    SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)ins.c_str());
    return true;
}

// Subclass proc for the edit control: draws the ghost, autocomplete Tab-commit,
// and list continuation. Tab/Enter are handled at WM_CHAR (where the character
// would actually be inserted) so nothing stray leaks in.
static LRESULT CALLBACK EditSubProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { ClearGhost(); return 0; } // cancel suggestion (don't close window)
        // any key except Tab/modifiers cancels the ghost; Tab is left alone so its
        // WM_CHAR can commit (or, with no ghost, insert a real tab)
        if (!g_ghost.empty() && wp != VK_TAB && wp != VK_SHIFT && wp != VK_CONTROL && wp != VK_MENU)
            ClearGhost();
        break;
    case WM_CHAR:
        if (wp == L'\t') {                       // Tab
            if (!g_ghost.empty()) { CommitGhost(); return 0; } // accept suggestion, no tab char
            break;                                             // no suggestion -> insert a real tab
        }
        if (wp == L'\r') {                        // Enter
            if (HandleListEnter()) return 0;      // list continuation, swallow default newline
            break;                                // otherwise a normal newline
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_KILLFOCUS:
        ClearGhost();
        break;
    case WM_PAINT: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp); // let the edit draw its text
        if (!g_ghost.empty()) {
            POINT pt; GetCaretPos(&pt);
            HDC dc = GetDC(hwnd);
            HFONT old = (HFONT)SelectObject(dc, g_ghostFont ? g_ghostFont : g_editFont);
            SetTextColor(dc, g_dark ? RGB(0x6e,0x76,0x81) : RGB(0xab,0xb1,0xbb));
            SetBkMode(dc, TRANSPARENT);
            TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
            // multi-line ghost: first segment at the caret, the rest at the left margin
            int x = pt.x, y = pt.y; size_t p = 0;
            while (p <= g_ghost.size()) {
                size_t nl = g_ghost.find(L'\n', p);
                std::wstring seg = g_ghost.substr(p, (nl == std::wstring::npos ? g_ghost.size() : nl) - p);
                if (!seg.empty()) TextOutW(dc, x, y, seg.c_str(), (int)seg.size());
                if (nl == std::wstring::npos) break;
                x = g_editMarginX; y += tm.tmHeight; p = nl + 1;
            }
            SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
        }
        return r;
    }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------- table grid-picker (Ctrl+T) ----------------
//
// A small popup grid: arrow keys size the table (1..TP_HARD_MAX cols/rows),
// Enter inserts a markdown table skeleton at the caret, Esc cancels. The grid
// starts showing TP_MAX (8x8) cells; pushing past that boundary grows the
// visible grid (and the popup window) to fit, up to TP_HARD_MAX, and shrinks
// back down (never below TP_MAX) as the selection shrinks again.

static HWND g_tpHwnd = nullptr;
static int g_tpCols = 2, g_tpRows = 3;
static int g_tpVisCols = 8, g_tpVisRows = 8; // currently visible/allocated grid size
static const int TP_MAX = 8, TP_HARD_MAX = 20;
static const int TP_CELL = 20, TP_GAP = 3, TP_PAD = 10, TP_LABEL = 22;

// edit mode: caret was inside an existing table when Ctrl+T opened the
// picker, so Enter resizes that table in place instead of inserting a new one
static bool g_tpEditMode = false;
static int g_tpEditStartLine = -1, g_tpEditEndLine = -1;
static std::vector<int> g_tpEditAligns;

static SIZE TpWindowSize(int visCols, int visRows) {
    int gridW = visCols * (TP_CELL + TP_GAP) - TP_GAP;
    int gridH = visRows * (TP_CELL + TP_GAP);
    return SIZE{ TP_PAD * 2 + gridW, TP_PAD * 2 + gridH + TP_LABEL };
}

static void InsertTableMarkdown(int cols, int rows) {
    if (!g_hEdit) return;
    std::wstring t;
    // if the caret isn't at the start of a line, drop to a fresh line first
    DWORD s = 0, e = 0; SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int len = GetWindowTextLengthW(g_hEdit);
    std::wstring all(len + 1, L'\0'); GetWindowTextW(g_hEdit, &all[0], len + 1); all.resize(len);
    if (e > 0 && e <= (DWORD)all.size() && all[e-1] != L'\n') t += L"\r\n";
    // Core builds the table with LF; the edit control wants CRLF.
    for (wchar_t c : fmdv::MakeTableMarkdown(cols, rows)) {
        if (c == L'\n') t += L"\r\n"; else t += c;
    }
    SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)t.c_str());
}

// ---- table resize (edit an existing table's row/column count in place) ----

// Table block containing the given 0-based source line, or nullptr.
static const Block* FindTableAtLine(int line) {
    if (line < 0) return nullptr;
    for (const auto& b : g_doc.blocks)
        if (b.type == BlockType::Table && line >= b.srcStartLine && line <= b.srcEndLine)
            return &b;
    return nullptr;
}

static const wchar_t* AlignSeparator(int align) {
    switch (align) {
        case AlignCenter: return L":---:";
        case AlignRight:  return L"---:";
        default:          return L"---";
    }
}

// Rewrite the table occupying source lines [startLine, endLine] to newCols x
// newRows, preserving existing cell text (and per-column alignment) by
// index; new cells/columns get blank/default content. Operates on the raw
// edit-control text so markdown inside existing cells (bold, links, ...)
// survives untouched — the parsed Block model already stripped that into
// InlineRun flags, so it can't be used to reconstruct source text.
static void ResizeTableMarkdown(int startLine, int endLine, const std::vector<int>& oldAligns,
                                 int newCols, int newRows) {
    if (!g_hEdit || startLine < 0 || endLine < startLine) return;

    int totalLines = (int)SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0);
    int startChar = (int)SendMessageW(g_hEdit, EM_LINEINDEX, startLine, 0);
    bool hasTrailingLine = (endLine + 1 < totalLines);
    int endChar = hasTrailingLine ? (int)SendMessageW(g_hEdit, EM_LINEINDEX, endLine + 1, 0)
                                  : GetWindowTextLengthW(g_hEdit);
    if (startChar < 0 || endChar < startChar) return;

    int len = GetWindowTextLengthW(g_hEdit);
    std::wstring all(len + 1, L'\0'); GetWindowTextW(g_hEdit, &all[0], len + 1); all.resize(len);
    std::wstring block = all.substr(startChar, endChar - startChar);

    std::vector<std::wstring> lines; std::wstring cur;
    for (wchar_t c : block) { if (c == L'\n') { lines.push_back(cur); cur.clear(); } else if (c != L'\r') cur += c; }
    lines.push_back(cur);
    if (lines.size() < 2) return; // not actually a header+separator pair

    std::vector<std::wstring> oldHeader = SplitTableCells(lines[0]);
    std::vector<std::vector<std::wstring>> oldRows;
    for (size_t li = 2; li < lines.size(); li++) {
        if (lines[li].find(L'|') == std::wstring::npos) continue;
        oldRows.push_back(SplitTableCells(lines[li]));
    }

    std::wstring t;
    t += L"|";
    for (int c = 0; c < newCols; c++) {
        std::wstring cell = (c < (int)oldHeader.size()) ? oldHeader[c] : (L"Column " + std::to_wstring(c + 1));
        t += L" " + cell + L" |";
    }
    t += L"\r\n|";
    for (int c = 0; c < newCols; c++) {
        int a = (c < (int)oldAligns.size()) ? oldAligns[c] : AlignLeft;
        t += L" "; t += AlignSeparator(a); t += L" |";
    }
    for (int r = 0; r < newRows; r++) {
        t += L"\r\n|";
        const std::vector<std::wstring>* src = (r < (int)oldRows.size()) ? &oldRows[r] : nullptr;
        for (int c = 0; c < newCols; c++) {
            if (src && c < (int)src->size()) t += L" " + (*src)[c] + L" |";
            else t += L"   |";
        }
    }
    if (hasTrailingLine) t += L"\r\n";

    SendMessageW(g_hEdit, EM_SETSEL, startChar, endChar);
    SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)t.c_str());
}

static void CloseTablePicker() {
    if (g_tpHwnd) { HWND h = g_tpHwnd; g_tpHwnd = nullptr; DestroyWindow(h); }
    if (g_hEdit) SetFocus(g_hEdit);
}

static LRESULT CALLBACK TablePickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: {
        switch (wp) {
            case VK_RIGHT: if (g_tpCols < TP_HARD_MAX) g_tpCols++; break;
            case VK_LEFT:  if (g_tpCols > 1) g_tpCols--; break;
            case VK_DOWN:  if (g_tpRows < TP_HARD_MAX) g_tpRows++; break;
            case VK_UP:    if (g_tpRows > 1) g_tpRows--; break;
            case VK_RETURN:
                if (g_tpEditMode) ResizeTableMarkdown(g_tpEditStartLine, g_tpEditEndLine, g_tpEditAligns, g_tpCols, g_tpRows);
                else InsertTableMarkdown(g_tpCols, g_tpRows);
                CloseTablePicker();
                return 0;
            case VK_ESCAPE: CloseTablePicker(); return 0;
            default: return 0;
        }
        // resize the visible grid to fit the current selection (at least
        // TP_MAX) — tracks the selection both growing and shrinking
        {
            int newVisCols = g_tpCols > TP_MAX ? g_tpCols : TP_MAX;
            int newVisRows = g_tpRows > TP_MAX ? g_tpRows : TP_MAX;
            if (newVisCols != g_tpVisCols || newVisRows != g_tpVisRows) {
                g_tpVisCols = newVisCols; g_tpVisRows = newVisRows;
                SIZE sz = TpWindowSize(g_tpVisCols, g_tpVisRows);
                // top-left stays put (near the caret) — only the right/bottom edge moves
                SetWindowPos(hwnd, nullptr, 0, 0, sz.cx, sz.cy, SWP_NOMOVE | SWP_NOZORDER);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // WM_PAINT fills the whole client area; skip the default erase (was flickering white on resize)
    case WM_KILLFOCUS:
        CloseTablePicker();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        Theme th = g_dark ? DarkTheme() : LightTheme();
        HBRUSH bg = CreateSolidBrush(th.bg); FillRect(hdc, &rc, bg); DeleteObject(bg);
        for (int r = 0; r < g_tpVisRows; r++) for (int c = 0; c < g_tpVisCols; c++) {
            int x = TP_PAD + c * (TP_CELL + TP_GAP);
            int y = TP_PAD + r * (TP_CELL + TP_GAP);
            RECT cell{ x, y, x + TP_CELL, y + TP_CELL };
            bool on = (c < g_tpCols && r < g_tpRows);
            HBRUSH b = CreateSolidBrush(on ? th.link : th.bg2);
            FillRect(hdc, &cell, b); DeleteObject(b);
            HBRUSH bd = CreateSolidBrush(th.border); FrameRect(hdc, &cell, bd); DeleteObject(bd);
        }
        wchar_t lbl[40];
        _snwprintf_s(lbl, 40, _TRUNCATE, g_tpEditMode ? L"resize to %d x %d" : L"%d x %d table", g_tpCols, g_tpRows);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, th.text);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT); HFONT of = (HFONT)SelectObject(hdc, f);
        int gridH = g_tpVisRows * (TP_CELL + TP_GAP);
        TextOutW(hdc, TP_PAD, TP_PAD + gridH, lbl, (int)wcslen(lbl));
        SelectObject(hdc, of);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        CloseTablePicker();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowTablePicker(HWND main) {
    if (!g_editing || !g_hEdit || g_tpHwnd) return;
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TablePickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FMDV_TablePicker";
        RegisterClassExW(&wc);
        reg = true;
    }
    int caretLine = (int)SendMessageW(g_hEdit, EM_LINEFROMCHAR, (WPARAM)-1, 0);
    const Block* tbl = FindTableAtLine(caretLine);
    if (tbl) {
        g_tpEditMode = true;
        g_tpEditStartLine = tbl->srcStartLine;
        g_tpEditEndLine = tbl->srcEndLine;
        g_tpEditAligns = tbl->aligns;
        g_tpCols = (int)tbl->aligns.size();
        g_tpRows = (int)tbl->rows.size();
        if (g_tpCols < 1) g_tpCols = 1;
        if (g_tpCols > TP_HARD_MAX) g_tpCols = TP_HARD_MAX;
        if (g_tpRows < 1) g_tpRows = 1;
        if (g_tpRows > TP_HARD_MAX) g_tpRows = TP_HARD_MAX;
    } else {
        g_tpEditMode = false;
        g_tpEditStartLine = g_tpEditEndLine = -1;
        g_tpEditAligns.clear();
        g_tpCols = 2; g_tpRows = 3;
    }
    g_tpVisCols = g_tpCols > TP_MAX ? g_tpCols : TP_MAX;
    g_tpVisRows = g_tpRows > TP_MAX ? g_tpRows : TP_MAX;
    SIZE sz = TpWindowSize(g_tpVisCols, g_tpVisRows);
    POINT pt; GetCaretPos(&pt); ClientToScreen(g_hEdit, &pt);
    g_tpHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"FMDV_TablePicker", L"",
        WS_POPUP | WS_BORDER, pt.x, pt.y + 18, sz.cx, sz.cy, main, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_tpHwnd) { ShowWindow(g_tpHwnd, SW_SHOW); SetFocus(g_tpHwnd); }
}

// ---------------- in-app updates (Ctrl+U) ----------------
//
// A one-shot timer 2.5s after first paint starts a worker thread that fetches
// the release list (WinHTTP; never on the startup path). Modes (prefs):
//   notify - banner when a newer release exists, install from the picker
//   auto   - download + swap in the background, takes effect next launch
//   pin    - stay on a chosen tag; no startup check
// The picker lists all releases; Enter installs the selection. Picking any
// version other than the newest pins it; picking the newest clears the pin.

static const UINT_PTR UPDATE_TIMER = 3;
static const UINT WM_APP_UPDATE = WM_APP + 1;
enum { UPD_CHECK_DONE = 1, UPD_INSTALL_OK = 2, UPD_INSTALL_FAIL = 3 };

static HWND g_mainHwnd = nullptr;
static CRITICAL_SECTION g_updLock;          // guards g_relPending
static std::vector<ReleaseInfo> g_relPending; // worker output
static std::vector<ReleaseInfo> g_releases;   // UI copy (main thread only)
static bool g_relFetched = false, g_relFailed = false;
static bool g_fetchRunning = false, g_installRunning = false;
static bool g_autoInstallOnCheck = false; // pending check should auto-install (main thread only)
static std::wstring g_banner;               // status bar text; empty = hidden
static std::wstring g_installTag;           // tag being installed (main sets, banner reads)
static std::wstring g_installUrl;           // url for InstallThread (main sets before spawn)

static int BannerH() { return g_banner.empty() ? 0 : MulDiv(28, g_dpi, 96); }

static void SetBanner(HWND hwnd, const std::wstring& text) {
    g_banner = text;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// newest release that has an fmdv.exe asset (API order isn't trusted)
static const ReleaseInfo* NewestInstallable() {
    const ReleaseInfo* best = nullptr;
    for (const auto& r : g_releases) {
        if (r.exeUrl.empty()) continue;
        if (!best || CompareVersions(r.tag, best->tag) > 0) best = &r;
    }
    return best;
}

// Fetch-only: the auto-install decision runs on the main thread when
// UPD_CHECK_DONE lands, so g_installTag/g_installUrl stay main-thread-owned
// and a user-initiated install from the picker can't run concurrently with an
// auto-install (both funnel through StartInstall's g_installRunning guard).
static DWORD WINAPI CheckThread(LPVOID) {
    std::vector<ReleaseInfo> rel;
    bool ok = FetchReleases(rel);
    EnterCriticalSection(&g_updLock);
    g_relPending = rel;
    LeaveCriticalSection(&g_updLock);
    PostMessageW(g_mainHwnd, WM_APP_UPDATE, UPD_CHECK_DONE, ok ? 0 : 1);
    return 0;
}

static DWORD WINAPI InstallThread(LPVOID) {
    UpdateResult r = DownloadAndInstall(g_installUrl);
    PostMessageW(g_mainHwnd, WM_APP_UPDATE, r == UpdateResult::Ok ? UPD_INSTALL_OK : UPD_INSTALL_FAIL, (LPARAM)r);
    return 0;
}

// Spawn the install worker for (tag, url). Main thread only; refuses while
// another install is in flight. Returns true if the worker was started.
static bool StartInstall(const std::wstring& tag, const std::wstring& url) {
    if (g_installRunning || url.empty()) return false;
    g_installTag = tag;
    g_installUrl = url;
    g_installRunning = true;
    HANDLE h = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    if (!h) { g_installRunning = false; return false; }
    CloseHandle(h);
    return true;
}

static void StartUpdateCheck(bool autoInstall) {
    if (g_fetchRunning) return;
    g_fetchRunning = true;
    g_autoInstallOnCheck = autoInstall;
    HANDLE h = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else { g_fetchRunning = false; g_autoInstallOnCheck = false; }
}

// ---- update picker popup ----

static HWND g_upHwnd = nullptr;
static int g_upSel = 0;
static int g_upConfirmIdx = -1; // index awaiting a second Enter to confirm a no-updater downgrade

static void CloseUpdatePicker() {
    if (g_upHwnd) { HWND h = g_upHwnd; g_upHwnd = nullptr; DestroyWindow(h); }
}

// Releases older than this predate Ctrl+U: installing one strands the user
// with no in-app way back up, so the picker requires a confirming second Enter.
static bool PredatesUpdater(const std::wstring& tag) {
    return CompareVersions(tag, FMDV_UPDATER_MIN_WSTR) < 0;
}

static int UpRows() { return (int)g_releases.size() < 8 ? (int)g_releases.size() : 8; }

static std::string Narrow(const std::wstring& w) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(need, '\0');
    if (need) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], need, nullptr, nullptr);
    return s;
}

// Returns true if the install proceeded (or was armed as a pending confirm).
static bool PickerInstallSelected() {
    if (g_installRunning) return false;
    if (g_upSel < 0 || g_upSel >= (int)g_releases.size()) return false;
    const ReleaseInfo& r = g_releases[g_upSel];
    if (r.exeUrl.empty()) return false;

    if (PredatesUpdater(r.tag) && g_upConfirmIdx != g_upSel) {
        g_upConfirmIdx = g_upSel; // arm; a second Enter on the same row confirms
        return true;              // repaint to show the confirmation prompt
    }
    g_upConfirmIdx = -1;

    // pin semantics: anything but the newest installable pins that tag
    const ReleaseInfo* newest = NewestInstallable();
    if (newest && CompareVersions(r.tag, newest->tag) < 0) {
        g_prefs.updateMode = UPDATE_PIN;
        g_prefs.pinTag = Narrow(r.tag);
    } else if (g_prefs.updateMode == UPDATE_PIN) {
        g_prefs.updateMode = UPDATE_NOTIFY;
        g_prefs.pinTag.clear();
    }
    SavePrefs(g_prefs);

    StartInstall(r.tag, r.exeUrl);
    if (g_upHwnd) InvalidateRect(g_upHwnd, nullptr, FALSE);
    return true;
}

static LRESULT CALLBACK UpdatePickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        switch (wp) {
            case VK_DOWN: if (g_upSel + 1 < UpRows()) { g_upSel++; g_upConfirmIdx = -1; } break;
            case VK_UP:   if (g_upSel > 0) { g_upSel--; g_upConfirmIdx = -1; } break;
            case VK_RETURN: PickerInstallSelected(); break;
            case 'A': // toggle auto-update (clears a pin)
                g_prefs.updateMode = (g_prefs.updateMode == UPDATE_AUTO) ? UPDATE_NOTIFY : UPDATE_AUTO;
                g_prefs.pinTag.clear();
                SavePrefs(g_prefs);
                break;
            case VK_ESCAPE:
                if (g_upConfirmIdx >= 0) { g_upConfirmIdx = -1; break; } // cancel the pending confirm first
                CloseUpdatePicker();
                return 0;
            default: return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        CloseUpdatePicker();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        Theme th = g_dark ? DarkTheme() : LightTheme();
        HBRUSH bg = CreateSolidBrush(th.bg); FillRect(hdc, &rc, bg); DeleteObject(bg);

        HFONT f = CreateFontW(-MulDiv(13, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT of = (HFONT)SelectObject(hdc, f);
        SetBkMode(hdc, TRANSPARENT);

        int pad = MulDiv(12, g_dpi, 96), row = MulDiv(24, g_dpi, 96), y = pad;
        wchar_t buf[160];

        SetTextColor(hdc, th.text);
        _snwprintf_s(buf, 160, _TRUNCATE, L"FMDV %ls — releases", CurrentVersion().c_str());
        TextOutW(hdc, pad, y, buf, (int)wcslen(buf)); y += row;

        SetTextColor(hdc, th.text2);
        if (g_prefs.updateMode == UPDATE_PIN)
            _snwprintf_s(buf, 160, _TRUNCATE, L"pinned to %hs   ·   [A] auto-update: off",
                         g_prefs.pinTag.c_str());
        else
            _snwprintf_s(buf, 160, _TRUNCATE, L"[A] auto-update: %ls",
                         g_prefs.updateMode == UPDATE_AUTO ? L"on" : L"off");
        TextOutW(hdc, pad, y, buf, (int)wcslen(buf)); y += row;

        if (!g_relFetched) {
            SetTextColor(hdc, th.text2);
            const wchar_t* s = g_relFailed ? L"couldn't reach GitHub" : L"checking…";
            TextOutW(hdc, pad, y, s, (int)wcslen(s));
        } else if (g_releases.empty()) {
            SetTextColor(hdc, th.text2);
            TextOutW(hdc, pad, y, L"no releases found", 17);
        } else {
            const ReleaseInfo* newest = NewestInstallable();
            for (int i = 0; i < UpRows(); i++, y += row) {
                const ReleaseInfo& r = g_releases[i];
                if (i == g_upSel) {
                    RECT sel{ pad / 2, y - 2, rc.right - pad / 2, y + row - 4 };
                    HBRUSH sb = CreateSolidBrush(th.sel);
                    FillRect(hdc, &sel, sb); DeleteObject(sb);
                }
                bool cur = CompareVersions(r.tag, CurrentVersion()) == 0;
                _snwprintf_s(buf, 160, _TRUNCATE, L"%ls%ls%ls%ls", r.tag.c_str(),
                             (newest && &r == newest) ? L"  · latest" : L"",
                             cur ? L"  · current" : L"",
                             r.exeUrl.empty() ? L"  · no exe" : L"");
                SetTextColor(hdc, r.exeUrl.empty() ? th.text2 : th.text);
                TextOutW(hdc, pad, y, buf, (int)wcslen(buf));
            }
        }

        // status footer
        y = rc.bottom - row;
        bool confirming = (g_upConfirmIdx == g_upSel && g_upConfirmIdx >= 0 &&
                           g_upConfirmIdx < (int)g_releases.size());
        SetTextColor(hdc, confirming ? th.link : th.text2);
        const wchar_t* foot = g_installRunning ? L"installing…"
                            : confirming ? L"no updater in that release — Enter again to confirm"
                            : L"↑↓ select · Enter install · Esc close";
        TextOutW(hdc, pad, y, foot, (int)wcslen(foot));

        SelectObject(hdc, of); DeleteObject(f);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        CloseUpdatePicker();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowUpdatePicker(HWND main) {
    if (g_upHwnd) { CloseUpdatePicker(); return; } // Ctrl+U toggles
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = UpdatePickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FMDV_UpdatePicker";
        RegisterClassExW(&wc);
        reg = true;
    }
    if (!g_relFetched && !g_fetchRunning) StartUpdateCheck(false);
    g_upSel = 0;
    g_upConfirmIdx = -1;
    for (int i = 0; i < (int)g_releases.size() && i < 8; i++)
        if (CompareVersions(g_releases[i].tag, CurrentVersion()) == 0) { g_upSel = i; break; }

    int w = MulDiv(400, g_dpi, 96);
    int h = MulDiv(12 * 2 + 24 * 3, g_dpi, 96) + MulDiv(24, g_dpi, 96) * (UpRows() > 0 ? UpRows() : 1);
    RECT mr; GetWindowRect(main, &mr);
    g_upHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"FMDV_UpdatePicker", L"",
        WS_POPUP | WS_BORDER, mr.right - w - MulDiv(48, g_dpi, 96), mr.top + MulDiv(64, g_dpi, 96),
        w, h, main, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_upHwnd) { ShowWindow(g_upHwnd, SW_SHOW); SetFocus(g_upHwnd); }
}

static void EnsureEditControl(HWND hwnd) {
    if (g_hEdit) return;
    g_hEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_NOHIDESEL,
        0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)EDIT_ID, GetModuleHandleW(nullptr), nullptr);
    if (!g_editFont) {
        LOGFONTW lf = {};
        lf.lfHeight = -15;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN; // monospace fallback if face missing
        wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Cascadia Mono"); // clean modern mono (Win11)
        g_editFont = CreateFontIndirectW(&lf);
        lf.lfItalic = TRUE;                       // gray-italic font for ghost text
        g_ghostFont = CreateFontIndirectW(&lf);
    }
    SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    SetWindowSubclass(g_hEdit, EditSubProc, 1, 0); // ghost drawing + Tab/cancel
    // EDIT control needs CRLF line breaks; g_rawText uses LF
    std::wstring crlf; crlf.reserve(g_rawText.size() + 64);
    for (wchar_t c : g_rawText) { if (c == L'\n') crlf += L"\r\n"; else crlf += c; }
    SetWindowTextW(g_hEdit, crlf.c_str());
}

static void ToggleEditor(HWND hwnd) {
    g_editing = !g_editing;
    if (g_editing) {
        EnsureEditControl(hwnd);
        if (g_splitX <= 0) g_splitX = g_clientW * g_prefs.splitPct / 100;
        ClampSplit();
        PositionEdit();
        UpdateLayout(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        SetFocus(g_hEdit);
    } else {
        PositionEdit();
        UpdateLayout(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        SetFocus(hwnd);
    }
}

// ---------------- find in doc (Ctrl+F) ----------------
//
// A small popup (like the table/update pickers) hosting a native single-line
// EDIT control. Typing rebuilds matches against g_frags (the same flattened
// text model selection/copy already use); Enter/Shift+Enter step through
// them, scrolling the preview to keep the current match in view. Highlights
// are drawn by PaintDocument itself (phase 1b, behind text) so they stay
// correct through scrolling without any extra bookkeeping here.

static void CloseFindBar() {
    if (g_findHwnd) { HWND h = g_findHwnd; g_findHwnd = nullptr; g_findEdit = nullptr; DestroyWindow(h); }
    if (g_findBrush) { DeleteObject(g_findBrush); g_findBrush = nullptr; }
    if (!g_findMatches.empty() || g_findCurrent != -1) {
        g_findMatches.clear();
        g_findCurrent = -1;
        if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
    if (g_mainHwnd) SetFocus(g_editing ? g_hEdit : g_mainHwnd);
}

static std::wstring LowerCopy(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = towlower(c);
    return r;
}

// Rebuild g_findMatches from the find box's current text against g_frags.
// Matches don't span frag boundaries — see the FindMatch comment in render.h.
static void RebuildFindMatches() {
    g_findMatches.clear();
    g_findCurrent = -1;
    if (!g_findEdit) return;
    int len = GetWindowTextLengthW(g_findEdit);
    if (len > 0) {
        std::wstring q(len + 1, L'\0'); GetWindowTextW(g_findEdit, &q[0], len + 1); q.resize(len);
        std::wstring qLower = LowerCopy(q);
        if (!qLower.empty()) {
            for (int fi = 0; fi < (int)g_frags.size(); fi++) {
                std::wstring low = LowerCopy(g_frags[fi].text);
                size_t pos = 0;
                while ((pos = low.find(qLower, pos)) != std::wstring::npos) {
                    g_findMatches.push_back(FindMatch{ fi, (int)pos, (int)(pos + qLower.size()) });
                    pos += qLower.size();
                }
            }
        }
    }
    if (!g_findMatches.empty()) g_findCurrent = 0;
}

// Scroll the preview so the given match's frag is comfortably in view.
static void ScrollToMatch(int idx) {
    if (idx < 0 || idx >= (int)g_findMatches.size() || !g_mainHwnd) return;
    int fragIdx = g_findMatches[idx].frag;
    if (fragIdx < 0 || fragIdx >= (int)g_frags.size()) return;
    int target = g_frags[fragIdx].rc.top - g_clientH / 3;
    ScrollTo(g_mainHwnd, target);
}

static void FindStep(int dir) {
    if (g_findMatches.empty() || !g_mainHwnd) return;
    int n = (int)g_findMatches.size();
    g_findCurrent = ((g_findCurrent + dir) % n + n) % n;
    ScrollToMatch(g_findCurrent);
    InvalidateRect(g_mainHwnd, nullptr, FALSE);
    if (g_findHwnd) InvalidateRect(g_findHwnd, nullptr, FALSE); // match-count label
}

static LRESULT CALLBACK FindEditSubProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { FindStep(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0; }
        if (wp == VK_ESCAPE) { CloseFindBar(); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'A') { // EDIT has no built-in Ctrl+A
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        // Up/Down/PgUp/PgDn scroll the document — a single-line EDIT doesn't
        // use them for anything, unlike Home/End (caret move) or Space (a
        // valid search character), which stay native. Otherwise there'd be
        // no way to scroll without clicking out of the box first.
        if (wp == VK_UP)    { ScrollTo(g_mainHwnd, g_scrollY - 40); return 0; }
        if (wp == VK_DOWN)  { ScrollTo(g_mainHwnd, g_scrollY + 40); return 0; }
        if (wp == VK_PRIOR) { ScrollTo(g_mainHwnd, g_scrollY - (g_clientH - 40)); return 0; }
        if (wp == VK_NEXT)  { ScrollTo(g_mainHwnd, g_scrollY + (g_clientH - 40)); return 0; }
        break;
    case WM_MOUSEWHEEL: // forward so the doc scrolls even while the box has focus
        if (g_mainHwnd) PostMessageW(g_mainHwnd, WM_MOUSEWHEEL, wp, lp);
        return 0;
    case WM_KILLFOCUS:
        // Close on focus loss to anything except the find bar itself or the
        // main window — clicking/selecting in the doc shouldn't dismiss find
        // (matches browser Ctrl+F convention), only clicking fully away does.
        if ((HWND)wp != g_findHwnd && (HWND)wp != g_mainHwnd) CloseFindBar();
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK FindBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // WM_PAINT fills the themed background; skip the default white erase
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == FIND_ID) {
            RebuildFindMatches();
            ScrollToMatch(g_findCurrent);
            if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        Theme th = g_dark ? DarkTheme() : LightTheme();
        SetTextColor(dc, th.text);
        SetBkColor(dc, th.bg);
        if (!g_findBrush) g_findBrush = CreateSolidBrush(th.bg);
        return (LRESULT)g_findBrush;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        Theme th = g_dark ? DarkTheme() : LightTheme();
        HBRUSH bg = CreateSolidBrush(th.bg); FillRect(hdc, &rc, bg); DeleteObject(bg);

        wchar_t buf[64];
        if (g_findMatches.empty()) {
            int len = g_findEdit ? GetWindowTextLengthW(g_findEdit) : 0;
            wcscpy_s(buf, 64, len > 0 ? L"no matches" : L"");
        } else {
            _snwprintf_s(buf, 64, _TRUNCATE, L"%d of %zu", g_findCurrent + 1, g_findMatches.size());
        }
        HFONT f = CreateFontW(-MulDiv(13, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT of = (HFONT)SelectObject(hdc, f);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, th.text2);
        RECT statusRc{ rc.right - MulDiv(96, g_dpi, 96), 0, rc.right - MulDiv(8, g_dpi, 96), rc.bottom };
        DrawTextW(hdc, buf, (int)wcslen(buf), &statusRc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        SelectObject(hdc, of); DeleteObject(f);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        CloseFindBar();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowFindBar(HWND main) {
    if (g_findHwnd) { // already open: select-all + refocus, like browser Ctrl+F
        SendMessageW(g_findEdit, EM_SETSEL, 0, -1);
        SetFocus(g_findEdit);
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FindBarProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FMDV_FindBar";
        RegisterClassExW(&wc);
        reg = true;
    }
    int w = MulDiv(300, g_dpi, 96), h = MulDiv(34, g_dpi, 96);
    RECT mr; GetWindowRect(main, &mr);
    g_findHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"FMDV_FindBar", L"",
        WS_POPUP | WS_BORDER, mr.right - w - MulDiv(24, g_dpi, 96), mr.top + MulDiv(40, g_dpi, 96),
        w, h, main, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_findHwnd) return;
    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_findHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    int pad = MulDiv(6, g_dpi, 96);
    int editW = w - MulDiv(110, g_dpi, 96);
    g_findEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad, pad, editW, h - 2 * pad, g_findHwnd, (HMENU)(INT_PTR)FIND_ID,
        GetModuleHandleW(nullptr), nullptr);
    HFONT ef = CreateFontW(-MulDiv(14, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(g_findEdit, WM_SETFONT, (WPARAM)ef, TRUE);
    SetWindowSubclass(g_findEdit, FindEditSubProc, 1, 0);

    ShowWindow(g_findHwnd, SW_SHOW);
    SetFocus(g_findEdit);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // we paint everything in WM_PAINT (no flicker)
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        int previewLeft = PreviewLeft();
        int pw = PreviewWidth();
        int h = g_clientH;

        // render preview into the persistent back-buffer (origin 0,0), then blit
        EnsureBackBuffer(hdc, pw, h);
        HDC mem = g_backDC;

        RECT prc{ 0, 0, pw, h };
        HBRUSH bg = CreateSolidBrush(g_theme.bg);
        FillRect(mem, &prc, bg);
        DeleteObject(bg);

        PaintDocument(mem, g_scrollY, pw, h, g_theme, &g_sel, g_frags,
                      g_findHwnd ? &g_findMatches : nullptr, g_findCurrent);
        BitBlt(hdc, previewLeft, 0, pw, h, mem, 0, 0, SRCCOPY);

        // divider bar
        if (g_editing) {
            RECT dv{ g_splitX, 0, g_splitX + DIVIDER_W, h };
            HBRUSH db = CreateSolidBrush(g_theme.border);
            FillRect(hdc, &dv, db);
            DeleteObject(db);
        }

        // table of contents sidebar (headings, click to jump — no independent
        // scroll in v1: entries past the window height are simply clipped)
        if (g_showToc) {
            int tw = TocW();
            RECT trc{ 0, 0, tw, h };
            HBRUSH tbg = CreateSolidBrush(g_theme.bg2);
            FillRect(hdc, &trc, tbg); DeleteObject(tbg);
            RECT tborder{ tw - 1, 0, tw, h };
            HBRUSH tbd = CreateSolidBrush(g_theme.border);
            FillRect(hdc, &tborder, tbd); DeleteObject(tbd);

            int clipId = SaveDC(hdc);
            IntersectClipRect(hdc, 0, 0, tw, h);
            HFONT tf = CreateFontW(-MulDiv(13, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT tfHover = CreateFontW(-MulDiv(13, g_dpi, 96), 0, 0, 0, FW_NORMAL, TRUE, 0, 0,
                                        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT tof = (HFONT)SelectObject(hdc, tf);
            SetBkMode(hdc, TRANSPARENT);
            int rowH = MulDiv(TOC_ROW_H, g_dpi, 96);
            int padTop = MulDiv(TOC_PAD_TOP, g_dpi, 96);
            int padX = MulDiv(TOC_PAD_X, g_dpi, 96);
            int indent = MulDiv(TOC_INDENT, g_dpi, 96);
            int ty = padTop;
            for (int ti = 0; ti < (int)g_toc.size(); ti++) {
                const TocHeading& t = g_toc[ti];
                bool hover = (ti == g_tocHoverIdx);
                SelectObject(hdc, hover ? tfHover : tf);
                SetTextColor(hdc, hover ? g_theme.link : (t.level <= 2 ? g_theme.text : g_theme.text2));
                int tx = padX + (t.level - 1) * indent;
                RECT cell{ tx, ty, tw - padX, ty + rowH };
                DrawTextW(hdc, t.text.c_str(), (int)t.text.size(), &cell,
                          DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
                ty += rowH;
            }
            SelectObject(hdc, tof); DeleteObject(tf); DeleteObject(tfHover);
            RestoreDC(hdc, clipId);
        }

        // update banner: overlay strip across the top (no relayout)
        if (!g_banner.empty()) {
            int bh = BannerH();
            RECT br{ 0, 0, g_clientW, bh };
            HBRUSH bb = CreateSolidBrush(g_theme.bg2);
            FillRect(hdc, &br, bb); DeleteObject(bb);
            RECT bl{ 0, bh - 1, g_clientW, bh };
            HBRUSH lb = CreateSolidBrush(g_theme.border);
            FillRect(hdc, &bl, lb); DeleteObject(lb);
            HFONT bf = CreateFontW(-MulDiv(13, g_dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT obf = (HFONT)SelectObject(hdc, bf);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, g_theme.link);
            TextOutW(hdc, MulDiv(12, g_dpi, 96), MulDiv(6, g_dpi, 96),
                     g_banner.c_str(), (int)g_banner.size());
            SelectObject(hdc, obf); DeleteObject(bf);
        }

        EndPaint(hwnd, &ps);
        static bool firstPaintDone = false;
        if (!firstPaintDone) {
            firstPaintDone = true;
            Timing("first-paint");
            BenchLog("first_paint", NowMs(), pw, h, g_contentH, "elapsed");
            FlushTiming();
        }
        return 0;
    }
    case WM_SIZE:
        UpdateLayout(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_CONTROL) < 0) { // Ctrl+wheel = zoom
            ApplyZoom(hwnd, g_zoomPct + (delta > 0 ? 10 : -10));
            return 0;
        }
        // Proportional, not delta/WHEEL_DELTA lines: precision touchpads send
        // small fractional deltas for smooth scrolling (not just clean +-120
        // clicks), and truncating integer division rounds most of those to
        // zero lines — scrolling would barely respond at all on a trackpad.
        int px = MulDiv(delta, 60, WHEEL_DELTA);
        ScrollTo(hwnd, g_scrollY - px);
        return 0;
    }

    case WM_VSCROLL: {
        int y = g_scrollY;
        int line = 40, page = g_clientH - 40;
        switch (LOWORD(wp)) {
            case SB_LINEUP:   y -= line; break;
            case SB_LINEDOWN: y += line; break;
            case SB_PAGEUP:   y -= page; break;
            case SB_PAGEDOWN: y += page; break;
            case SB_TOP:      y = 0; break;
            case SB_BOTTOM:   y = MaxScroll(); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &si);
                y = si.nTrackPos;
                break;
            }
        }
        ScrollTo(hwnd, y);
        return 0;
    }

    case WM_COMMAND: {
        // control notifications (edit control) carry a non-null lParam
        if (lp != 0 && HIWORD(wp) == EN_CHANGE && LOWORD(wp) == EDIT_ID) {
            ReparseFromEdit(hwnd);
            UpdateGhost();
            return 0;
        }
        switch (LOWORD(wp)) {
            case ID_EDIT_TOGGLE: ToggleEditor(hwnd); return 0;
            case ID_DARK:
                g_dark = !g_dark;
                g_theme = g_dark ? DarkTheme() : LightTheme();
                g_prefs.dark = g_dark;
                SavePrefs(g_prefs);
                if (g_editBrush) { DeleteObject(g_editBrush); g_editBrush = nullptr; }
                if (g_findBrush) { DeleteObject(g_findBrush); g_findBrush = nullptr; }
                ApplyTitleBar(hwnd);
                UpdateLayout(hwnd); // colors are baked into the cached display list
                InvalidateRect(hwnd, nullptr, TRUE);
                if (g_hEdit) InvalidateRect(g_hEdit, nullptr, TRUE);
                if (g_findHwnd) InvalidateRect(g_findHwnd, nullptr, TRUE);
                return 0;
            case ID_SAVE:       if (g_editing && !SaveToFile()) ReportSaveError(hwnd); return 0;
            case ID_SAVE_CLOSE: if (g_editing) { if (SaveToFile()) ToggleEditor(hwnd); else ReportSaveError(hwnd); } return 0;
            case ID_ZOOM_IN:    ApplyZoom(hwnd, g_zoomPct + 10); return 0;
            case ID_ZOOM_OUT:   ApplyZoom(hwnd, g_zoomPct - 10); return 0;
            case ID_ZOOM_RESET: ApplyZoom(hwnd, 100); return 0;
            case ID_COPY:       CopySelection(hwnd); return 0;
            case ID_SELECT_ALL: SelectAll(hwnd); return 0;
            case ID_INSERT_TABLE: ShowTablePicker(hwnd); return 0;
            case ID_UPDATES:      ShowUpdatePicker(hwnd); return 0;
            case ID_TOC:
                g_showToc = !g_showToc;
                UpdateLayout(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            case ID_FIND:         ShowFindBar(hwnd); return 0;
        }
        return 0;
    }

    case WM_APP_UPDATE: {
        if (wp == UPD_CHECK_DONE) {
            g_fetchRunning = false;
            EnterCriticalSection(&g_updLock);
            g_releases.swap(g_relPending);
            LeaveCriticalSection(&g_updLock);
            g_relFetched = (lp == 0);
            g_relFailed = (lp != 0);
            if (g_relFetched && g_prefs.updateMode == UPDATE_NOTIFY) {
                const ReleaseInfo* nw = NewestInstallable();
                if (nw && CompareVersions(nw->tag, CurrentVersion()) > 0)
                    SetBanner(hwnd, nw->tag + L" available — Ctrl+U to update");
            }
            if (g_relFetched && g_autoInstallOnCheck) {
                // auto mode: install the newest exe-bearing release if newer
                const ReleaseInfo* nw = NewestInstallable();
                if (nw && CompareVersions(nw->tag, CurrentVersion()) > 0)
                    StartInstall(nw->tag, nw->exeUrl);
            }
            g_autoInstallOnCheck = false;
            if (g_upHwnd) { // resize to fit the arrived list, keep top-right anchor
                RECT wr; GetWindowRect(g_upHwnd, &wr);
                int w = wr.right - wr.left;
                int h = MulDiv(12 * 2 + 24 * 3, g_dpi, 96) + MulDiv(24, g_dpi, 96) * (UpRows() > 0 ? UpRows() : 1);
                SetWindowPos(g_upHwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                InvalidateRect(g_upHwnd, nullptr, TRUE);
            }
        } else if (wp == UPD_INSTALL_OK) {
            g_installRunning = false;
            SetBanner(hwnd, g_installTag + L" installed — takes effect on next launch");
            if (g_upHwnd) InvalidateRect(g_upHwnd, nullptr, TRUE);
        } else if (wp == UPD_INSTALL_FAIL) {
            g_installRunning = false;
            std::wstring why = UpdateResultMessage((UpdateResult)lp);
            SetBanner(hwnd, L"update failed: " + why + L" — github.com/OrangeBannana/FMDV/releases");
            if (g_upHwnd) InvalidateRect(g_upHwnd, nullptr, TRUE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'C') { CopySelection(hwnd); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'A') { SelectAll(hwnd); return 0; }
        switch (wp) {
            // Escape only dismisses overlays (find bar, pickers — each closes
            // itself via its own handler before a keystroke ever reaches
            // here). On the bare main window it's a no-op: quitting the app
            // needs Alt+F4 or the window's own close button, same as any
            // other app — a lone Escape used to close the whole window,
            // which meant a quick double-tap while dismissing an overlay
            // (first press closes the overlay, second reaches this handler)
            // silently quit the app.
            case VK_ESCAPE:
                if (g_findHwnd) CloseFindBar(); // belt-and-suspenders: should already be
                return 0;                       // closed via its own subclass handler
            case VK_UP:     ScrollTo(hwnd, g_scrollY - 40); return 0;
            case VK_DOWN:   ScrollTo(hwnd, g_scrollY + 40); return 0;
            case VK_PRIOR:  ScrollTo(hwnd, g_scrollY - (g_clientH - 40)); return 0; // PgUp
            case VK_NEXT:   ScrollTo(hwnd, g_scrollY + (g_clientH - 40)); return 0; // PgDn
            case VK_HOME:   ScrollTo(hwnd, 0); return 0;
            case VK_END:    ScrollTo(hwnd, MaxScroll()); return 0;
            case VK_SPACE:  ScrollTo(hwnd, g_scrollY + (g_clientH - 40)); return 0;
        }
        break;

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, g_theme.text);
        SetBkColor(dc, g_theme.bg3);
        if (!g_editBrush) g_editBrush = CreateSolidBrush(g_theme.bg3);
        return (LRESULT)g_editBrush;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), yy = GET_Y_LPARAM(lp);
        if (g_showToc && x < TocW()) {
            int rowH = MulDiv(TOC_ROW_H, g_dpi, 96), padTop = MulDiv(TOC_PAD_TOP, g_dpi, 96);
            int idx = (yy - padTop) / rowH;
            if (idx >= 0 && idx < (int)g_toc.size()) ScrollTo(hwnd, g_toc[idx].docY);
            return 0;
        }
        if (g_editing && x >= g_splitX - 3 && x <= g_splitX + DIVIDER_W + 3) {
            g_dragging = true;
            SetCapture(hwnd);
            return 0;
        }
        // begin a potential text selection in the preview
        if (x >= PreviewLeft()) {
            // triple-click (a click quickly after a double-click) selects the line
            if (g_lastDblTime && (GetTickCount() - g_lastDblTime) < GetDoubleClickTime()) {
                g_lastDblTime = 0;
                SelectLineAt(hwnd, x, yy);
                g_selAnchor.frag = -1;
                return 0;
            }
            g_downPt = POINT{ x, yy };
            g_selAnchor = PointToSel(hwnd, x, yy);
            g_selecting = false;
            if (g_sel.active) { g_sel.active = false; InvalidateRect(hwnd, nullptr, FALSE); }
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), yy = GET_Y_LPARAM(lp);
        if (x >= PreviewLeft()) {
            SelectWordAt(hwnd, x, yy);
            g_lastDblTime = GetTickCount(); // arm triple-click window
            g_selAnchor.frag = -1;
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_showToc) {
            int x = GET_X_LPARAM(lp), yy = GET_Y_LPARAM(lp);
            int newHover = -1;
            if (x < TocW()) {
                if (!g_tocTrackingLeave) {
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    g_tocTrackingLeave = true;
                }
                int rowH = MulDiv(TOC_ROW_H, g_dpi, 96), padTop = MulDiv(TOC_PAD_TOP, g_dpi, 96);
                int idx = (yy - padTop) / rowH;
                if (idx >= 0 && idx < (int)g_toc.size()) newHover = idx;
            }
            if (newHover != g_tocHoverIdx) { g_tocHoverIdx = newHover; InvalidateRect(hwnd, nullptr, FALSE); }
        }
        if (g_dragging) {
            g_splitX = GET_X_LPARAM(lp);
            ClampSplit();
            PositionEdit();
            UpdateLayout(hwnd, false);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if ((wp & MK_LBUTTON) && GetCapture() == hwnd && g_selAnchor.frag >= 0) {
            int x = GET_X_LPARAM(lp), yy = GET_Y_LPARAM(lp);
            if (!g_selecting && (abs(x - g_downPt.x) > 3 || abs(yy - g_downPt.y) > 3))
                g_selecting = true;
            if (g_selecting) {
                g_dragPt = POINT{ x, yy };
                // auto-scroll when dragging above/below the viewport
                bool outOfBounds = (yy < 0 || yy > g_clientH);
                if (outOfBounds && !g_autoScroll) { SetTimer(hwnd, AUTOSCROLL_TIMER, 40, nullptr); g_autoScroll = true; }
                else if (!outOfBounds && g_autoScroll) { KillTimer(hwnd, AUTOSCROLL_TIMER); g_autoScroll = false; }

                SelPoint caret = PointToSel(hwnd, x, yy);
                if (caret.frag >= 0) {
                    if (SelLess(caret, g_selAnchor)) { g_sel.a = caret; g_sel.b = g_selAnchor; }
                    else { g_sel.a = g_selAnchor; g_sel.b = caret; }
                    g_sel.active = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        break;

    case WM_MOUSELEAVE:
        g_tocTrackingLeave = false;
        if (g_tocHoverIdx != -1) { g_tocHoverIdx = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        break;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            if (g_clientW > 0) { g_prefs.splitPct = g_splitX * 100 / g_clientW; SavePrefs(g_prefs); }
            UpdateLayout(hwnd); // final pass: scrollbar was skipping redraws during the drag
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (GetCapture() == hwnd) {
            ReleaseCapture();
            if (g_autoScroll) { KillTimer(hwnd, AUTOSCROLL_TIMER); g_autoScroll = false; }
            g_selAnchor.frag = -1;
            if (!g_selecting) {
                // a plain click (no drag): follow a link if one is here
                std::wstring href = LinkAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (!href.empty())
                    ShellExecuteW(hwnd, L"open", href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            g_selecting = false;
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            if (g_showToc && pt.x < TocW()) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (g_editing && pt.x >= g_splitX - 3 && pt.x <= g_splitX + DIVIDER_W + 3) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            if (!LinkAt(pt.x, pt.y).empty()) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (pt.x >= PreviewLeft()) { // text area → I-beam
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
        }
        break;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        ApplyScale();
        RECT* nr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, nr->left, nr->top,
                     nr->right - nr->left, nr->bottom - nr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateLayout(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_TIMER:
        if (wp == AUTOSCROLL_TIMER) {
            if (!g_selecting) { KillTimer(hwnd, AUTOSCROLL_TIMER); g_autoScroll = false; return 0; }
            int step = (g_dragPt.y < 0) ? -40 : 40;
            int before = g_scrollY;
            ScrollTo(hwnd, g_scrollY + step);
            if (g_scrollY == before) return 0; // hit top/bottom
            UpdateWindow(hwnd); // refresh g_frags at the new scroll before hit-testing
            int cy = (g_dragPt.y < 0) ? 0 : g_clientH - 1;
            SelPoint caret = PointToSel(hwnd, g_dragPt.x, cy);
            if (caret.frag >= 0) {
                if (SelLess(caret, g_selAnchor)) { g_sel.a = caret; g_sel.b = g_selAnchor; }
                else { g_sel.a = g_selAnchor; g_sel.b = caret; }
                g_sel.active = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (wp == UPDATE_TIMER) { // one-shot, 2.5s after first paint
            KillTimer(hwnd, UPDATE_TIMER);
            CleanupOldExe();
            if (g_prefs.updateMode != UPDATE_PIN)
                StartUpdateCheck(g_prefs.updateMode == UPDATE_AUTO);
            return 0;
        }
        if (wp == WATCH_TIMER && !g_editing && !g_filePath.empty()) {
            FILETIME ft = FileMtime(g_filePath);
            if ((ft.dwLowDateTime || ft.dwHighDateTime) && CompareFileTime(&ft, &g_fileTime) != 0) {
                g_fileTime = ft;
                std::wstring text;
                if (ReadFileUtf8(g_filePath, text)) {
                    g_rawText = text;
                    g_doc = ParseMarkdown(text);
                    UpdateLayout(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }
        return 0;

    case WM_DESTROY:
        FreeBackBuffer();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int Run(int argc, wchar_t** argv) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = (double)f.QuadPart;
    QueryPerformanceCounter(&g_start);

    {
        wchar_t* t = _wgetenv(L"FMDV_TIMING");
        g_timing = (t && t[0] == L'1');
    }

    // Per-monitor DPI awareness (must be set before any window is created)
    {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        if (u) {
            auto setCtx = (SetCtxFn)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
            if (setCtx) setCtx((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }

    // Load persisted preferences (dark mode, split, zoom)
    g_prefs = LoadPrefs();
    double prefsLoadedMs = NowMs();
    g_dark = g_prefs.dark;
    g_zoomPct = g_prefs.zoomPct;

    bool parseDump = false;
    bool benchRender = false;
    bool benchMode = false;
    std::wstring dumpPath;
    int dumpWidth = 900;
    int dumpViewportH = 0;
    int dumpScroll = 0;
    int scrollRuns = 200;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--parse-dump") == 0) { parseDump = true; continue; }
        if (wcscmp(argv[i], L"--bench-startup") == 0) { g_benchStartup = true; benchMode = true; continue; }
        if (wcscmp(argv[i], L"--bench-render") == 0) { benchRender = true; benchMode = true; continue; }
        if (wcscmp(argv[i], L"--benchpaint") == 0) { benchRender = true; benchMode = true; if (dumpWidth == 900) dumpWidth = 1000; if (dumpViewportH == 0) dumpViewportH = 800; continue; }
        if (wcscmp(argv[i], L"--dump") == 0 && i + 1 < argc) { dumpPath = argv[++i]; continue; }
        if (wcscmp(argv[i], L"--width") == 0 && i + 1 < argc) { dumpWidth = _wtoi(argv[++i]); g_benchWindowW = dumpWidth; continue; }
        if (wcscmp(argv[i], L"--height") == 0 && i + 1 < argc) { g_benchWindowH = _wtoi(argv[++i]); continue; }
        if (wcscmp(argv[i], L"--viewport") == 0 && i + 1 < argc) { dumpViewportH = _wtoi(argv[++i]); continue; }
        if (wcscmp(argv[i], L"--scroll-runs") == 0 && i + 1 < argc) { scrollRuns = _wtoi(argv[++i]); continue; }
        if (wcscmp(argv[i], L"--scroll") == 0 && i + 1 < argc) { dumpScroll = _wtoi(argv[++i]); continue; }
        if (wcscmp(argv[i], L"--dark") == 0) { g_dark = true; continue; }
        if (argv[i][0] == L'-') continue; // other flags handled later
        if (g_filePath.empty()) g_filePath = argv[i];
    }
    double argsParsedMs = NowMs();
    InitBenchLog(benchMode);
    BenchLog("process_start", 0.0, g_benchWindowW, g_benchWindowH, 0, "elapsed");
    BenchLog("prefs_loaded", prefsLoadedMs, g_benchWindowW, g_benchWindowH, 0, "elapsed");
    BenchLog("args_parsed", argsParsedMs, g_benchWindowW, g_benchWindowH, 0, "elapsed");
    Timing("args-parsed");

#ifdef FMDV_CONSOLE
    // updater test/inspection flags (debug build)
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--version") == 0) {
            wprintf(L"%ls\n", CurrentVersion().c_str());
            return 0;
        }
        if (wcscmp(argv[i], L"--test-updater") == 0) {
            int fails = 0;
            auto chk = [&](const char* name, bool ok) {
                wprintf(L"%hs  %hs\n", ok ? "OK  " : "FAIL", name);
                if (!ok) fails++;
            };
            chk("vercmp equal ignores v", CompareVersions(L"v1.0.0", L"1.0.0") == 0);
            chk("vercmp 1.10 > 1.9",      CompareVersions(L"v1.10.0", L"v1.9.9") > 0);
            chk("vercmp 0.9 < 1.0",       CompareVersions(L"0.9", L"1.0") < 0);
            chk("vercmp 1.0.1 > 1.0",     CompareVersions(L"1.0.1", L"1.0") > 0);
            chk("vercmp 1.0 == 1.0.0",    CompareVersions(L"1.0", L"1.0.0") == 0);
            const char* sample =
                "[{\"tag_name\":\"v1.1.0\",\"assets\":["
                "{\"name\":\"notes.txt\",\"browser_download_url\":\"https://x/y/notes.txt\"},"
                "{\"name\":\"fmdv.exe\",\"browser_download_url\":\"https://x/v1.1.0/fmdv.exe\"}]},"
                "{\"tag_name\":\"v1.0.0\",\"assets\":[]}]";
            std::vector<ReleaseInfo> rel;
            bool p = ParseReleasesJson(sample, rel);
            chk("parse: two releases",   p && rel.size() == 2);
            chk("parse: tags",           rel.size() == 2 && rel[0].tag == L"v1.1.0" && rel[1].tag == L"v1.0.0");
            chk("parse: exe url",        rel.size() == 2 && rel[0].exeUrl == L"https://x/v1.1.0/fmdv.exe");
            chk("parse: no-asset empty", rel.size() == 2 && rel[1].exeUrl.empty());
            wprintf(L"%d failures\n", fails);
            return fails;
        }
        if (wcscmp(argv[i], L"--check-updates") == 0) {
            std::vector<ReleaseInfo> rel;
            if (!FetchReleases(rel)) { wprintf(L"fetch FAILED\n"); return 2; }
            std::wstring cur = CurrentVersion();
            wprintf(L"current %ls\n", cur.c_str());
            for (auto& r : rel)
                wprintf(L"%ls  exe=%ls  %ls\n", r.tag.c_str(),
                        r.exeUrl.empty() ? L"no" : L"yes",
                        CompareVersions(r.tag, cur) > 0 ? L"NEWER" : L"");
            return 0;
        }
        if (wcscmp(argv[i], L"--install-tag") == 0 && i + 1 < argc) {
            std::vector<ReleaseInfo> rel;
            if (!FetchReleases(rel)) { wprintf(L"fetch FAILED\n"); return 2; }
            for (auto& r : rel) {
                if (r.tag == argv[i + 1] && !r.exeUrl.empty()) {
                    UpdateResult res = DownloadAndInstall(r.exeUrl);
                    if (res == UpdateResult::Ok) wprintf(L"install %ls: OK\n", r.tag.c_str());
                    else wprintf(L"install %ls: FAILED (%ls)\n", r.tag.c_str(), UpdateResultMessage(res));
                    return res == UpdateResult::Ok ? 0 : 2;
                }
            }
            wprintf(L"tag not found or has no exe asset\n");
            return 2;
        }
    }
#endif

    // Read + parse the file
    std::wstring text;
    double fileStart = NowMs();
    bool fileRead = false;
    if (!g_filePath.empty()) {
        fileRead = ReadFileUtf8(g_filePath, text);
    }
    BenchLog("file_read", NowMs() - fileStart, g_benchWindowW, g_benchWindowH, 0,
             g_filePath.empty() ? "no_file" : (fileRead ? "duration" : "failed"));
    if (fileRead) {
        double parseStart = NowMs();
        g_doc = ParseMarkdown(text);
        BenchLog("parsed", NowMs() - parseStart, g_benchWindowW, g_benchWindowH, 0, "duration");
    }
    g_rawText = text;
    Timing("parsed");

#ifdef FMDV_CONSOLE
    if (parseDump) { ParseDump(g_doc); return 0; }
#else
    (void)parseDump;
#endif

#ifdef FMDV_CONSOLE
    if (benchRender) return RunBenchRender(dumpWidth, dumpViewportH, scrollRuns);
#else
    (void)benchRender; (void)scrollRuns;
#endif

    // headless PNG dump (visual test)
    if (!dumpPath.empty()) {
        Gdiplus::GdiplusStartupInput gi;
        ULONG_PTR token;
        Gdiplus::GdiplusStartup(&token, &gi, nullptr);
        bool ok = DumpToPng(dumpPath, dumpWidth, dumpViewportH, dumpScroll);
        Gdiplus::GdiplusShutdown(token);
        FreeFontCache();
        return ok ? 0 : 2;
    }

    g_theme = g_dark ? DarkTheme() : LightTheme();

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS; // deliver double-click messages (word/line selection)
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint it ourselves
    wc.lpszClassName = L"FMDV_Window";
    RegisterClassExW(&wc);
    Timing("class-reg");

    std::wstring title = L"FMDV";
    if (!g_filePath.empty()) {
        size_t slash = g_filePath.find_last_of(L"\\/");
        std::wstring base = (slash == std::wstring::npos) ? g_filePath : g_filePath.substr(slash + 1);
        title = base + L" — FMDV";
    }

    // Test suites can set this to launch off the visible screen entirely
    // (rather than on-screen then relocated, which leaves a visible flash).
    int initX = CW_USEDEFAULT, initY = CW_USEDEFAULT;
    if (GetEnvironmentVariableW(L"FMDV_TEST_OFFSCREEN", nullptr, 0) > 0) initX = initY = -32000;
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
        initX, initY, g_benchWindowW, g_benchWindowH,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    Timing("window-created");
    BenchLog("window_created", NowMs(), g_benchWindowW, g_benchWindowH, 0, "elapsed");

    // Kill the open-transition animation (saves ~100-150ms of synchronous DWM work)
    BOOL disable = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));

    // DPI + zoom scale (before first layout), dark title bar, live-reload watch
    { HDC dc0 = GetDC(hwnd); g_dpi = GetDeviceCaps(dc0, LOGPIXELSX); ReleaseDC(hwnd, dc0); }
    ApplyScale();
    ApplyTitleBar(hwnd);
    g_fileTime = FileMtime(g_filePath);
    SetTimer(hwnd, WATCH_TIMER, 500, nullptr);
    InitializeCriticalSection(&g_updLock);
    g_mainHwnd = hwnd;
    SetTimer(hwnd, UPDATE_TIMER, 2500, nullptr); // update check runs well after first paint

    ShowWindow(hwnd, SW_SHOWNA);   // no-activate show: paints fast (activation handshake is the slow part)
    Timing("showwindow");
    BenchLog("window_shown", NowMs(), g_benchWindowW, g_benchWindowH, 0, "elapsed");
    UpdateWindow(hwnd);            // forces immediate WM_PAINT -> first-paint timing
    Timing("updatewindow");
    if (g_benchStartup) {
        DestroyWindow(hwnd);
        DeleteCriticalSection(&g_updLock);
        FreeFontCache();
        return 0;
    }
    // Bring to foreground AFTER the content is already on screen, so perceived
    // startup stays instant while the window still ends up focused.
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    // accelerators: Ctrl+E edit, Ctrl+D dark, Ctrl+S save, Ctrl+Shift+S save&close
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'E', ID_EDIT_TOGGLE },
        { FCONTROL | FVIRTKEY, 'D', ID_DARK },
        { FCONTROL | FVIRTKEY, 'S', ID_SAVE },
        { (BYTE)(FCONTROL | FSHIFT | FVIRTKEY), 'S', ID_SAVE_CLOSE },
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS,  ID_ZOOM_IN },
        { FCONTROL | FVIRTKEY, VK_ADD,       ID_ZOOM_IN },
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, ID_ZOOM_OUT },
        { FCONTROL | FVIRTKEY, VK_SUBTRACT,  ID_ZOOM_OUT },
        { FCONTROL | FVIRTKEY, '0',          ID_ZOOM_RESET },
        { FCONTROL | FVIRTKEY, 'T',          ID_INSERT_TABLE },
        { FCONTROL | FVIRTKEY, 'U',          ID_UPDATES },
        { (BYTE)(FCONTROL | FSHIFT | FVIRTKEY), 'O', ID_TOC },
        { FCONTROL | FVIRTKEY, 'F',          ID_FIND },
    };
    HACCEL hAccel = CreateAcceleratorTableW(accels, (int)(sizeof(accels)/sizeof(accels[0])));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    if (g_editFont) DeleteObject(g_editFont);
    if (g_ghostFont) DeleteObject(g_ghostFont);
    if (g_editBrush) DeleteObject(g_editBrush);
    FreeFontCache();
    return (int)m.wParam;
}

#ifdef FMDV_CONSOLE
int wmain(int argc, wchar_t** argv) {
    return Run(argc, argv);
}
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = Run(argc, argv);
    if (argv) LocalFree(argv);
    return rc;
}
#endif
