#pragma once
#include <string>

// update behavior (issue #9)
enum UpdateMode { UPDATE_NOTIFY = 0, UPDATE_AUTO = 1, UPDATE_PIN = 2 };

struct Prefs {
    bool dark = false;
    int splitPct = 50;  // editor split percentage
    int zoomPct = 100;  // render zoom percentage
    int updateMode = UPDATE_NOTIFY;
    std::string pinTag; // pinned release tag when updateMode == UPDATE_PIN
    std::wstring lastOpenDir; // folder the "Open" dialog last opened a file from

    // Last normal (non-maximized) window position/size in screen pixels; 0
    // means "not set yet" (first run), in which case the caller falls back to
    // a percentage of the current monitor's work area.
    int winX = 0, winY = 0, winW = 0, winH = 0;
};

Prefs LoadPrefs();
void SavePrefs(const Prefs& p);
