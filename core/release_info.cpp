#include "release_info.h"
#include <cstring>
#include <cwctype>

// UTF-8 -> wstring. Matches the Windows frontend's previous MultiByteToWideChar
// (CP_UTF8) behavior, including surrogate pairs when wchar_t is 16-bit. Release
// tags/URLs are ASCII in practice, but this stays correct for any payload. This
// duplicates the CLI's bridge; both fold into core/str.h in the string-type
// milestone.
static void appendCp(std::wstring& w, char32_t cp) {
    if (sizeof(wchar_t) >= 4) { w.push_back((wchar_t)cp); return; }
    if (cp <= 0xFFFF) {
        w.push_back((wchar_t)cp);
    } else {
        cp -= 0x10000;
        w.push_back((wchar_t)(0xD800 + (cp >> 10)));
        w.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
    }
}

static std::wstring Widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int len;
        if (c < 0x80)             { cp = c;        len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E){ cp = c & 0x07; len = 4; }
        else                      { cp = 0xFFFD;   len = 1; }
        if (i + (size_t)len > n)  { cp = 0xFFFD;   len = 1; }
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        appendCp(w, cp);
        i += (size_t)len;
    }
    return w;
}

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
        r.tag = Widen(json.substr(pos, te - pos));

        size_t next = json.find(TAG, te);
        size_t u = json.find(URL, te);
        while (u != std::string::npos && (next == std::string::npos || u < next)) {
            size_t us = u + strlen(URL);
            size_t ue = json.find('"', us);
            if (ue == std::string::npos) break;
            std::string cand = json.substr(us, ue - us);
            if (cand.size() > strlen(EXE) &&
                cand.compare(cand.size() - strlen(EXE), strlen(EXE), EXE) == 0) {
                r.exeUrl = Widen(cand);
                break;
            }
            u = json.find(URL, ue);
        }
        out.push_back(std::move(r));
        pos = te;
    }
    return !out.empty();
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    if (i < a.size() && (a[i] == L'v' || a[i] == L'V')) i++;
    if (j < b.size() && (b[j] == L'v' || b[j] == L'V')) j++;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && iswdigit(a[i])) x = x * 10 + (a[i++] - L'0');
        while (j < b.size() && iswdigit(b[j])) y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        if (i < a.size() && a[i] == L'.') i++;
        if (j < b.size() && b[j] == L'.') j++;
        // non-numeric suffixes (e.g. "-rc1") are ignored for ordering
        if (i < a.size() && !iswdigit(a[i]) && a[i] != L'.') break;
        if (j < b.size() && !iswdigit(b[j]) && b[j] != L'.') break;
    }
    return 0;
}
