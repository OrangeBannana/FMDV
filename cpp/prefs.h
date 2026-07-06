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
};

Prefs LoadPrefs();
void SavePrefs(const Prefs& p);
