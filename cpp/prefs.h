#pragma once

struct Prefs {
    bool dark = false;
    int splitPct = 50;  // editor split percentage
    int zoomPct = 100;  // render zoom percentage
};

Prefs LoadPrefs();
void SavePrefs(const Prefs& p);
