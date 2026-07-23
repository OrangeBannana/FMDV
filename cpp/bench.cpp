#include "bench.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace {

bool g_benchEnabled = false;
std::wstring g_benchLogPath;
std::string g_benchLabel;
std::string g_benchCommit;

std::string WideToUtf8(const std::wstring& w) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(need, '\0');
    if (need) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], need, nullptr, nullptr);
    return s;
}

std::string EnvUtf8(const wchar_t* name) {
    wchar_t* value = _wgetenv(name);
    return (value && value[0]) ? WideToUtf8(value) : std::string();
}

std::string Csv(const std::string& s) {
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

void EnsureParentDirs(const std::wstring& path) {
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] != L'\\' && path[i] != L'/') continue;
        if (i == 0 || (i == 2 && path[1] == L':')) continue;
        std::wstring dir = path.substr(0, i);
        if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    }
}

bool FileIsEmptyOrMissing(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return true;
    return fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0;
}

std::string IsoUtcNow() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
             (unsigned)st.wMilliseconds);
    return buf;
}

} // namespace

void InitBenchLog(bool forceDefaultPath) {
    wchar_t* path = _wgetenv(L"FMDV_BENCH_LOG");
    if (path && path[0]) g_benchLogPath = path;
    else if (forceDefaultPath) g_benchLogPath = L"bench\\results\\windows-baseline.csv";
    if (g_benchLogPath.empty()) return;

    g_benchEnabled = true;
    g_benchLabel = EnvUtf8(L"FMDV_BENCH_LABEL");
    g_benchCommit = EnvUtf8(L"FMDV_BENCH_COMMIT");
    if (g_benchCommit.empty()) g_benchCommit = EnvUtf8(L"GITHUB_SHA");
}

bool BenchLogActive() { return g_benchEnabled; }

void BenchLog(const char* event, double durationMs, int width, int height,
              int contentH, const char* notes,
              const std::wstring& filePath, bool dark, size_t blockCount) {
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

    fprintf(f, "%s,windows,win32,%s,%s,%s,%s,%s,%d,%d,%s,%.3f,%zu,%d,%s\n",
            Csv(IsoUtcNow()).c_str(),
            Csv(build).c_str(),
            Csv(g_benchCommit).c_str(),
            Csv(g_benchLabel).c_str(),
            Csv(WideToUtf8(filePath)).c_str(),
            dark ? "dark" : "light",
            width,
            height,
            Csv(event).c_str(),
            durationMs,
            blockCount,
            contentH,
            Csv(notes).c_str());
    fclose(f);
}
