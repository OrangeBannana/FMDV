#include "release_info.h"
#include <cstring>
#include <cwctype>

// Minimal scan of the releases payload: one "tag_name" per release, asset
// "browser_download_url"s follow it. Escaped quotes in body text can't false-
// match because the escape backslash breaks the `":"` pattern.
bool ParseReleasesJson(const std::string& json, std::vector<ReleaseInfo>& out) {
    static const char* TAG = "\"tag_name\":\"";
    static const char* URL = "\"browser_download_url\":\"";
    static const char* EXE = "/fmdv.exe";
    size_t pos = 0;
    while ((pos = json.find(TAG, pos)) != std::string::npos) {
        pos += strlen(TAG);
        size_t te = json.find('"', pos);
        if (te == std::string::npos) break;
        ReleaseInfo r;
        r.tag = FromUtf8(json.substr(pos, te - pos));

        size_t next = json.find(TAG, te);
        size_t u = json.find(URL, te);
        while (u != std::string::npos && (next == std::string::npos || u < next)) {
            size_t us = u + strlen(URL);
            size_t ue = json.find('"', us);
            if (ue == std::string::npos) break;
            std::string cand = json.substr(us, ue - us);
            if (cand.size() > strlen(EXE) &&
                cand.compare(cand.size() - strlen(EXE), strlen(EXE), EXE) == 0) {
                r.exeUrl = FromUtf8(cand);
                break;
            }
            u = json.find(URL, ue);
        }
        out.push_back(std::move(r));
        pos = te;
    }
    return !out.empty();
}

int CompareVersions(const Str& a, const Str& b) {
    size_t i = 0, j = 0;
    if (i < a.size() && (a[i] == U16('v') || a[i] == U16('V'))) i++;
    if (j < b.size() && (b[j] == U16('v') || b[j] == U16('V'))) j++;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && iswdigit(a[i])) x = x * 10 + (a[i++] - U16('0'));
        while (j < b.size() && iswdigit(b[j])) y = y * 10 + (b[j++] - U16('0'));
        if (x != y) return x < y ? -1 : 1;
        if (i < a.size() && a[i] == U16('.')) i++;
        if (j < b.size() && b[j] == U16('.')) j++;
        // non-numeric suffixes (e.g. "-rc1") are ignored for ordering
        if (i < a.size() && !iswdigit(a[i]) && a[i] != U16('.')) break;
        if (j < b.size() && !iswdigit(b[j]) && b[j] != U16('.')) break;
    }
    return 0;
}
