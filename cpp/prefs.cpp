#include "prefs.h"
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>

// Prefs are stored as a tiny key=value text file in %APPDATA%\fmdv\prefs.txt

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

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64]; int val = 0;
        if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
            std::string k(key);
            if (k == "dark") p.dark = (val != 0);
            else if (k == "split") p.splitPct = val;
            else if (k == "zoom") p.zoomPct = val;
        }
    }
    fclose(f);
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
    fclose(f);
}
