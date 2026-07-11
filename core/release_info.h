#pragma once
// Platform-neutral release parsing + version comparison (macOS impl guide,
// "core/releases"). The network fetch and the on-disk exe swap stay in each
// frontend (WinHTTP on Windows, URLSession on macOS); only the pure data logic
// lives here so all frontends parse and order releases identically.
#include <string>
#include <vector>
#include "str.h"

struct ReleaseInfo {
    Str tag;     // release tag, e.g. U16("v1.0.0")
    Str exeUrl;  // browser_download_url of the fmdv.exe asset ("" if none)
    Str macUrl;  // browser_download_url of the FMDV-macos.zip asset ("" if none)
};

// Extract tag + installable asset URLs (fmdv.exe, FMDV-macos.zip) from a
// GitHub releases JSON payload (UTF-8), newest first. Returns false if no
// releases were found.
bool ParseReleasesJson(const std::string& json, std::vector<ReleaseInfo>& out);

// Compare dotted versions; a leading 'v'/'V' is ignored and non-numeric
// suffixes (e.g. "-rc1") don't affect ordering. Returns <0, 0, or >0.
int CompareVersions(const Str& a, const Str& b);
