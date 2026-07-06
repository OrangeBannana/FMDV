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
#include <string>
#include <vector>
#include "theme.h"
#include "markdown.h"
#include "render.h"
#include "prefs.h"
#include "updater.h"
#include "version.h"

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
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

// command IDs (driven by the accelerator table so they work even while typing)
enum { ID_EDIT_TOGGLE = 2001, ID_DARK = 2002, ID_SAVE = 2003, ID_SAVE_CLOSE = 2004,
       ID_ZOOM_IN = 2005, ID_ZOOM_OUT = 2006, ID_ZOOM_RESET = 2007, ID_COPY = 2008,
       ID_SELECT_ALL = 2009, ID_INSERT_TABLE = 2010, ID_UPDATES = 2011 };

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// forward declarations (used by helpers defined above their definitions)
static int PreviewLeft();
static void UpdateLayout(HWND hwnd);

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
static SelPoint PointToSel(HWND hwnd, int clientX, int clientY) {
    SelPoint sp;
    if (g_frags.empty()) return sp;
    int bx = clientX - PreviewLeft();
    HDC dc = GetDC(hwnd);
    int best = -1; long bestDist = 0x7fffffff; bool rowHit = false;
    for (int i = 0; i < (int)g_frags.size(); i++) {
        const RECT& r = g_frags[i].rc;
        bool inRow = clientY >= r.top && clientY < r.bottom;
        if (inRow && bx >= r.left && bx <= r.right) { best = i; rowHit = true; break; }
        long dist;
        if (inRow) dist = (bx < r.left) ? (r.left - bx) : (bx - r.right);
        else       dist = 100000 + labs((long)((r.top + r.bottom) / 2 - clientY));
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

static int PreviewLeft() { return g_editing ? g_splitX + DIVIDER_W : 0; }
static int PreviewWidth() { int w = g_clientW - PreviewLeft(); return w > 0 ? w : 0; }

static void ClampSplit() {
    if (g_splitX < MIN_PANE) g_splitX = MIN_PANE;
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
        MoveWindow(g_hEdit, 0, 0, g_splitX, g_clientH, TRUE);
        // inset the text with a little breathing room (left/top padding)
        RECT fr{ 14, 12, g_splitX - 8, g_clientH };
        SendMessageW(g_hEdit, EM_SETRECT, 0, (LPARAM)&fr);
        ShowWindow(g_hEdit, SW_SHOW);
    } else {
        ShowWindow(g_hEdit, SW_HIDE);
    }
}

// Recompute content height for the current preview width and update the scrollbar.
static void UpdateLayout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    g_clientW = rc.right; g_clientH = rc.bottom;
    g_sel.active = false; // re-layout invalidates fragment indices
    if (g_editing) { ClampSplit(); PositionEdit(); }
    HDC dc = GetDC(hwnd);
    g_contentH = LayoutDocument(dc, PreviewWidth(), g_doc, g_theme, &g_links, &g_frags);
    ReleaseDC(hwnd, dc);

    if (g_scrollY > MaxScroll()) g_scrollY = MaxScroll();

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (g_contentH > 0) ? g_contentH - 1 : 0;
    si.nPage = g_clientH;
    si.nPos = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
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
    HANDLE h = CreateFileW(g_filePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    if (!utf8.empty()) WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
    CloseHandle(h);
    g_fileTime = FileMtime(g_filePath); // don't let our own write trigger a live-reload
    return true;
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

struct Sugg { std::wstring text; int caret; };

// Given the current line text up to the caret, return a completion suggestion.
// `caret` is where the caret lands (offset into text) after Tab-commit.
static Sugg SuggestClose(const std::wstring& line) {
    auto endsWith = [&](const wchar_t* d) {
        size_t n = wcslen(d);
        return line.size() >= n && line.compare(line.size() - n, n, d) == 0;
    };
    auto count = [&](const std::wstring& d) {
        int c = 0; size_t p = 0;
        while ((p = line.find(d, p)) != std::wstring::npos) { c++; p += d.size(); }
        return c;
    };
    auto countCh = [&](wchar_t ch) { int c = 0; for (wchar_t x : line) if (x == ch) c++; return c; };

    // fenced code block: only right after typing the opening ``` (no lang yet).
    // close on its own line, caret on the blank middle line.
    {
        std::wstring t = line; size_t i = 0; while (i < t.size() && (t[i]==L' '||t[i]==L'\t')) i++;
        if (t.substr(i) == L"```") return { L"\n\n```", 1 };
    }
    if (endsWith(L"**") && !endsWith(L"***") && (count(L"**") % 2)) return { L"**", 0 };
    if (endsWith(L"__") && (count(L"__") % 2)) return { L"__", 0 };
    if (endsWith(L"~~") && (count(L"~~") % 2)) return { L"~~", 0 };
    if (endsWith(L"``") && !endsWith(L"```") && (count(L"``") % 2)) return { L"``", 0 };
    if (endsWith(L"`") && !endsWith(L"``") && (count(L"`") % 2)) return { L"`", 0 };
    if (endsWith(L"*") && !endsWith(L"**") && (countCh(L'*') % 2)) return { L"*", 0 };
    if (endsWith(L"(") && (countCh(L'(') > countCh(L')'))) return { L")", 0 };
    if (endsWith(L"[")) {
        // context split: checkbox after a list marker, else a link
        std::wstring rest = line.substr(0, line.size() - 1); // drop trailing '['
        size_t i = 0; while (i < rest.size() && (rest[i]==L' '||rest[i]==L'\t')) i++;
        bool listStart = false;
        if (i < rest.size()) {
            size_t j = i;
            if (rest[j]==L'-'||rest[j]==L'*'||rest[j]==L'+') j++;
            else { size_t d=j; while (d<rest.size() && iswdigit(rest[d])) d++; if (d>j && d<rest.size() && rest[d]==L'.') j=d+1; else j=rest.size()+1; }
            if (j <= rest.size() && j < rest.size() && rest[j]==L' ') { listStart = (j+1 == rest.size()); }
        }
        if (listStart) return { L" ] ", 3 };          // "- [ ] |" checkbox, caret after
        int ob = countCh(L'['), cb = countCh(L']');
        if (ob > cb) return { L"]()", 0 };             // "[|]()" link, caret inside brackets
    }
    return {};
}

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
                Sugg sg = SuggestClose(text.substr(ls, caret - ls));
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
    std::wstring line = text.substr(ls, caret - ls);

    size_t i = 0; while (i < line.size() && (line[i]==L' '||line[i]==L'\t')) i++;
    std::wstring indent = line.substr(0, i);
    std::wstring marker, rest;

    if (i < line.size() && (line[i]==L'-'||line[i]==L'*'||line[i]==L'+')
        && i+1 < line.size() && line[i+1]==L' ') {
        wchar_t bullet = line[i]; size_t after = i + 2;
        if (line.compare(after, 4, L"[ ] ") == 0 || line.compare(after, 4, L"[x] ") == 0 ||
            line.compare(after, 4, L"[X] ") == 0) {
            marker = std::wstring(1, bullet) + L" [ ] "; rest = line.substr(after + 4);
        } else {
            marker = std::wstring(1, bullet) + L" "; rest = line.substr(after);
        }
    } else if (i < line.size() && iswdigit(line[i])) {
        size_t d = i; while (d < line.size() && iswdigit(line[d])) d++;
        if (d < line.size() && line[d]==L'.' && d+1 < line.size() && line[d+1]==L' ') {
            int num = _wtoi(line.substr(i, d - i).c_str());
            marker = std::to_wstring(num + 1) + L". "; rest = line.substr(d + 2);
        } else return false;
    } else return false;

    std::wstring trimmed = rest;
    while (!trimmed.empty() && (trimmed.back()==L' '||trimmed.back()==L'\t')) trimmed.pop_back();
    if (trimmed.empty()) { // empty item -> end the list (clear the marker)
        SendMessageW(g_hEdit, EM_SETSEL, ls, caret);
        SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)L"");
        return true;
    }
    std::wstring ins = L"\r\n" + indent + marker;
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
// A small popup grid: arrow keys size the table (1..8 cols/rows), Enter inserts
// a markdown table skeleton at the caret, Esc cancels.

static HWND g_tpHwnd = nullptr;
static int g_tpCols = 2, g_tpRows = 3;
static const int TP_MAX = 8, TP_CELL = 20, TP_GAP = 3, TP_PAD = 10, TP_LABEL = 22;

static void InsertTableMarkdown(int cols, int rows) {
    if (!g_hEdit) return;
    std::wstring t;
    // if the caret isn't at the start of a line, drop to a fresh line first
    DWORD s = 0, e = 0; SendMessageW(g_hEdit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int len = GetWindowTextLengthW(g_hEdit);
    std::wstring all(len + 1, L'\0'); GetWindowTextW(g_hEdit, &all[0], len + 1); all.resize(len);
    if (e > 0 && e <= (DWORD)all.size() && all[e-1] != L'\n') t += L"\r\n";
    t += L"|";
    for (int c = 0; c < cols; c++) t += L" Column " + std::to_wstring(c + 1) + L" |";
    t += L"\r\n|";
    for (int c = 0; c < cols; c++) t += L" --- |";
    t += L"\r\n";
    for (int r = 0; r < rows; r++) {
        t += L"|";
        for (int c = 0; c < cols; c++) t += L"   |";
        t += L"\r\n";
    }
    SendMessageW(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)t.c_str());
}

static void CloseTablePicker() {
    if (g_tpHwnd) { HWND h = g_tpHwnd; g_tpHwnd = nullptr; DestroyWindow(h); }
    if (g_hEdit) SetFocus(g_hEdit);
}

static LRESULT CALLBACK TablePickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        switch (wp) {
            case VK_RIGHT: if (g_tpCols < TP_MAX) g_tpCols++; break;
            case VK_LEFT:  if (g_tpCols > 1) g_tpCols--; break;
            case VK_DOWN:  if (g_tpRows < TP_MAX) g_tpRows++; break;
            case VK_UP:    if (g_tpRows > 1) g_tpRows--; break;
            case VK_RETURN: InsertTableMarkdown(g_tpCols, g_tpRows); CloseTablePicker(); return 0;
            case VK_ESCAPE: CloseTablePicker(); return 0;
            default: return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        CloseTablePicker();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        Theme th = g_dark ? DarkTheme() : LightTheme();
        HBRUSH bg = CreateSolidBrush(th.bg); FillRect(hdc, &rc, bg); DeleteObject(bg);
        for (int r = 0; r < TP_MAX; r++) for (int c = 0; c < TP_MAX; c++) {
            int x = TP_PAD + c * (TP_CELL + TP_GAP);
            int y = TP_PAD + r * (TP_CELL + TP_GAP);
            RECT cell{ x, y, x + TP_CELL, y + TP_CELL };
            bool on = (c < g_tpCols && r < g_tpRows);
            HBRUSH b = CreateSolidBrush(on ? th.link : th.bg2);
            FillRect(hdc, &cell, b); DeleteObject(b);
            HBRUSH bd = CreateSolidBrush(th.border); FrameRect(hdc, &cell, bd); DeleteObject(bd);
        }
        wchar_t lbl[32]; _snwprintf_s(lbl, 32, _TRUNCATE, L"%d x %d table", g_tpCols, g_tpRows);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, th.text);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT); HFONT of = (HFONT)SelectObject(hdc, f);
        int gridH = TP_MAX * (TP_CELL + TP_GAP);
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
    g_tpCols = 2; g_tpRows = 3;
    int gridW = TP_MAX * (TP_CELL + TP_GAP) - TP_GAP;
    int w = TP_PAD * 2 + gridW;
    int h = TP_PAD * 2 + TP_MAX * (TP_CELL + TP_GAP) + TP_LABEL;
    POINT pt; GetCaretPos(&pt); ClientToScreen(g_hEdit, &pt);
    g_tpHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"FMDV_TablePicker", L"",
        WS_POPUP | WS_BORDER, pt.x, pt.y + 18, w, h, main, nullptr, GetModuleHandleW(nullptr), nullptr);
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

static DWORD WINAPI CheckThread(LPVOID autoInstall) {
    std::vector<ReleaseInfo> rel;
    bool ok = FetchReleases(rel);
    EnterCriticalSection(&g_updLock);
    g_relPending = rel;
    LeaveCriticalSection(&g_updLock);
    PostMessageW(g_mainHwnd, WM_APP_UPDATE, UPD_CHECK_DONE, ok ? 0 : 1);
    if (ok && autoInstall) {
        // install the newest exe-bearing release if it's newer than us
        const ReleaseInfo* best = nullptr;
        for (const auto& r : rel) {
            if (r.exeUrl.empty()) continue;
            if (!best || CompareVersions(r.tag, best->tag) > 0) best = &r;
        }
        if (best && CompareVersions(best->tag, CurrentVersion()) > 0) {
            g_installTag = best->tag;
            bool ins = DownloadAndInstall(best->exeUrl);
            PostMessageW(g_mainHwnd, WM_APP_UPDATE, ins ? UPD_INSTALL_OK : UPD_INSTALL_FAIL, 0);
        }
    }
    return 0;
}

static DWORD WINAPI InstallThread(LPVOID) {
    bool ok = DownloadAndInstall(g_installUrl);
    PostMessageW(g_mainHwnd, WM_APP_UPDATE, ok ? UPD_INSTALL_OK : UPD_INSTALL_FAIL, 0);
    return 0;
}

static void StartUpdateCheck(bool autoInstall) {
    if (g_fetchRunning) return;
    g_fetchRunning = true;
    HANDLE h = CreateThread(nullptr, 0, CheckThread, autoInstall ? (LPVOID)1 : nullptr, 0, nullptr);
    if (h) CloseHandle(h); else g_fetchRunning = false;
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

    g_installTag = r.tag;
    g_installUrl = r.exeUrl;
    g_installRunning = true;
    HANDLE h = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else g_installRunning = false;
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

        PaintDocument(mem, g_scrollY, pw, h, g_theme, &g_sel, g_frags);
        BitBlt(hdc, previewLeft, 0, pw, h, mem, 0, 0, SRCCOPY);

        // divider bar
        if (g_editing) {
            RECT dv{ g_splitX, 0, g_splitX + DIVIDER_W, h };
            HBRUSH db = CreateSolidBrush(g_theme.border);
            FillRect(hdc, &dv, db);
            DeleteObject(db);
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
        if (!firstPaintDone) { firstPaintDone = true; Timing("first-paint"); FlushTiming(); }
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
        int lines = delta / WHEEL_DELTA;
        ScrollTo(hwnd, g_scrollY - lines * 60);
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
                ApplyTitleBar(hwnd);
                UpdateLayout(hwnd); // colors are baked into the cached display list
                InvalidateRect(hwnd, nullptr, TRUE);
                if (g_hEdit) InvalidateRect(g_hEdit, nullptr, TRUE);
                return 0;
            case ID_SAVE:       if (g_editing) SaveToFile(); return 0;
            case ID_SAVE_CLOSE: if (g_editing) { SaveToFile(); ToggleEditor(hwnd); } return 0;
            case ID_ZOOM_IN:    ApplyZoom(hwnd, g_zoomPct + 10); return 0;
            case ID_ZOOM_OUT:   ApplyZoom(hwnd, g_zoomPct - 10); return 0;
            case ID_ZOOM_RESET: ApplyZoom(hwnd, 100); return 0;
            case ID_COPY:       CopySelection(hwnd); return 0;
            case ID_SELECT_ALL: SelectAll(hwnd); return 0;
            case ID_INSERT_TABLE: ShowTablePicker(hwnd); return 0;
            case ID_UPDATES:      ShowUpdatePicker(hwnd); return 0;
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
            SetBanner(hwnd, L"update failed — check github.com/OrangeBannana/FMDV/releases");
            if (g_upHwnd) InvalidateRect(g_upHwnd, nullptr, TRUE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'C') { CopySelection(hwnd); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'A') { SelectAll(hwnd); return 0; }
        switch (wp) {
            case VK_ESCAPE: DestroyWindow(hwnd); return 0;
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
        if (g_dragging) {
            g_splitX = GET_X_LPARAM(lp);
            ClampSplit();
            PositionEdit();
            UpdateLayout(hwnd);
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

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            if (g_clientW > 0) { g_prefs.splitPct = g_splitX * 100 / g_clientW; SavePrefs(g_prefs); }
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
    g_dark = g_prefs.dark;
    g_zoomPct = g_prefs.zoomPct;

    bool parseDump = false;
    std::wstring dumpPath;
    int dumpWidth = 900;
    int dumpViewportH = 0;
    int dumpScroll = 0;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--parse-dump") == 0) { parseDump = true; continue; }
        if (wcscmp(argv[i], L"--dump") == 0 && i + 1 < argc) { dumpPath = argv[++i]; continue; }
        if (wcscmp(argv[i], L"--width") == 0 && i + 1 < argc) { dumpWidth = _wtoi(argv[++i]); continue; }
        if (wcscmp(argv[i], L"--viewport") == 0 && i + 1 < argc) { dumpViewportH = _wtoi(argv[++i]); continue; }
        if (wcscmp(argv[i], L"--scroll") == 0 && i + 1 < argc) { dumpScroll = _wtoi(argv[++i]); continue; }
        if (wcscmp(argv[i], L"--dark") == 0) { g_dark = true; continue; }
        if (argv[i][0] == L'-') continue; // other flags handled later
        if (g_filePath.empty()) g_filePath = argv[i];
    }
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
                    bool ok = DownloadAndInstall(r.exeUrl);
                    wprintf(L"install %ls: %hs\n", r.tag.c_str(), ok ? "OK" : "FAILED");
                    return ok ? 0 : 2;
                }
            }
            wprintf(L"tag not found or has no exe asset\n");
            return 2;
        }
    }
#endif

    // Read + parse the file
    std::wstring text;
    if (!g_filePath.empty() && ReadFileUtf8(g_filePath, text)) {
        g_doc = ParseMarkdown(text);
    }
    g_rawText = text;
    Timing("parsed");

#ifdef FMDV_CONSOLE
    if (parseDump) { ParseDump(g_doc); return 0; }
#else
    (void)parseDump;
#endif

#ifdef FMDV_CONSOLE
    // Bench: lay out once, then time repeated culled paints (the per-scroll cost).
    for (int i = 1; i < argc; i++) if (wcscmp(argv[i], L"--benchpaint") == 0) {
        Theme th = g_dark ? DarkTheme() : LightTheme();
        int W = 1000, H = 800;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(mem, dib);
        std::vector<TextFrag> frags;
        double t0 = NowMs();
        int contentH = LayoutDocument(mem, W, g_doc, th, nullptr, &frags);
        double layoutMs = NowMs() - t0;
        int N = 200; double t1 = NowMs();
        for (int i2 = 0; i2 < N; i2++) {
            int scroll = (contentH > H) ? (i2 * (contentH - H) / N) : 0;
            RECT full{0,0,W,H}; HBRUSH bg = CreateSolidBrush(th.bg); FillRect(mem,&full,bg); DeleteObject(bg);
            PaintDocument(mem, scroll, W, H, th, nullptr, frags);
        }
        double paintMs = (NowMs() - t1) / N;
        wprintf(L"blocks=%zu contentH=%d  layout(once)=%.2f ms  paint(per-scroll avg over %d)=%.3f ms\n",
                g_doc.blocks.size(), contentH, layoutMs, N, paintMs);
        DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
        FreeFontCache();
        return 0;
    }
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

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    Timing("window-created");

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
    UpdateWindow(hwnd);            // forces immediate WM_PAINT -> first-paint timing
    Timing("updatewindow");
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
