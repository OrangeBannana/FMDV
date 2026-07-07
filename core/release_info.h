#pragma once
// Platform-neutral release parsing + version comparison (macOS impl guide,
// "core/releases"). The network fetch and the on-disk exe swap stay in each
// frontend (WinHTTP on Windows, URLSession on macOS); only the pure data logic
// lives here so all frontends parse and order releases identically.
//
// Strings are std::wstring for now, matching the rest of the transitional core;
// they migrate to the core Str type with the parser in the string-type milestone.
#include <string>
#include <vector>

struct ReleaseInfo {
    std::wstring tag;     // release tag, e.g. L"v1.0.0"
    std::wstring exeUrl;  // browser_download_url of the fmdv.exe asset ("" if none)
};

// Extract tag + fmdv.exe asset URL pairs from a GitHub releases JSON payload
// (UTF-8), newest first. Returns false if no releases were found.
bool ParseReleasesJson(const std::string& json, std::vector<ReleaseInfo>& out);

// Compare dotted versions; a leading 'v'/'V' is ignored and non-numeric
// suffixes (e.g. "-rc1") don't affect ordering. Returns <0, 0, or >0.
int CompareVersions(const std::wstring& a, const std::wstring& b);
