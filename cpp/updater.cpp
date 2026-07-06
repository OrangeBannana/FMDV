#include "updater.h"
#include "version.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include <cwctype>

static const wchar_t* API_HOST = L"api.github.com";
static const wchar_t* API_PATH = L"/repos/OrangeBannana/FMDV/releases?per_page=30";
static const wchar_t* USER_AGENT = L"fmdv/" FMDV_VERSION_WSTR;

// Blocking HTTPS GET. Follows redirects (GitHub asset downloads 302 to a CDN).
static bool HttpGet(const std::wstring& host, const std::wstring& path, std::string& out) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr,
                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr;
    if (req &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                size_t off = out.size();
                out.resize(off + avail);
                DWORD rd = 0;
                if (!WinHttpReadData(req, &out[off], avail, &rd)) { out.resize(off); break; }
                out.resize(off + rd);
            }
            ok = !out.empty();
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

static std::wstring Widen(const std::string& s) {
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(need, L'\0');
    if (need) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], need);
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

bool FetchReleases(std::vector<ReleaseInfo>& out) {
    std::string body;
    if (!HttpGet(API_HOST, API_PATH, body)) return false;
    return ParseReleasesJson(body, out);
}

bool DownloadAndInstall(const std::wstring& url) {
    // split the URL into host + path
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"", path[2048] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    std::string data;
    if (!HttpGet(host, path, data)) return false;
    // sanity: a real PE, not an error page
    if (data.size() < 100 * 1024 || data[0] != 'M' || data[1] != 'Z') return false;

    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring tmp = std::wstring(exe) + L".new";
    std::wstring old = std::wstring(exe) + L".old";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL wok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    if (!wok || wr != data.size()) { DeleteFileW(tmp.c_str()); return false; }

    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe, old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), exe, 0)) {
        MoveFileW(old.c_str(), exe); // roll back
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void CleanupOldExe() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring old = std::wstring(exe) + L".old";
    DeleteFileW(old.c_str()); // fails silently if still locked by an old process
}

std::wstring CurrentVersion() {
    wchar_t* o = _wgetenv(L"FMDV_VERSION_OVERRIDE");
    if (o && o[0]) return o;
    return FMDV_VERSION_WSTR;
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
