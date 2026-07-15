#include "prefs.h"
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>

// Prefs are stored as a tiny key=value text file in %APPDATA%\fmdv\prefs.txt

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(need, '\0');
    if (need) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], need, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(need, L'\0');
    if (need) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], need);
    return w;
}

static std::wstring PrefsPath(bool createDir) {
    wchar_t* appdata = _wgetenv(L"APPDATA");
    if (!appdata || !appdata[0]) return L"";
    std::wstring dir = appdata;
    dir += L"\\fmdv";
    if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\prefs.txt";
}

Prefs LoadPrefs() {
    Prefs p;
    std::wstring path = PrefsPath(false);
    if (path.empty()) return p;

    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return p;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[900];
        if (sscanf(line, "%63[^=]=%899[^\r\n]", key, val) == 2) {
            std::string k(key), v(val);
            if (k == "dark") p.dark = (atoi(val) != 0);
            else if (k == "split") p.splitPct = atoi(val);
            else if (k == "zoom") p.zoomPct = atoi(val);
            else if (k == "update") {
                if (v == "auto") p.updateMode = UPDATE_AUTO;
                else if (v == "pin") p.updateMode = UPDATE_PIN;
                else p.updateMode = UPDATE_NOTIFY;
            }
            else if (k == "pin") p.pinTag = v;
            else if (k == "lastdir") p.lastOpenDir = Utf8ToWide(v);
        }
    }
    fclose(f);
    if (p.updateMode == UPDATE_PIN && p.pinTag.empty()) p.updateMode = UPDATE_NOTIFY;
    return p;
}

void SavePrefs(const Prefs& p) {
    std::wstring path = PrefsPath(true);
    if (path.empty()) return;
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    fprintf(f, "dark=%d\n", p.dark ? 1 : 0);
    fprintf(f, "split=%d\n", p.splitPct);
    fprintf(f, "zoom=%d\n", p.zoomPct);
    fprintf(f, "update=%s\n", p.updateMode == UPDATE_AUTO ? "auto"
                            : p.updateMode == UPDATE_PIN  ? "pin" : "notify");
    if (!p.pinTag.empty()) fprintf(f, "pin=%s\n", p.pinTag.c_str());
    if (!p.lastOpenDir.empty()) fprintf(f, "lastdir=%s\n", WideToUtf8(p.lastOpenDir).c_str());
    fclose(f);
}
