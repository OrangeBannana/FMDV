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

// ReleaseInfo, ParseReleasesJson, and CompareVersions now live in the shared
// core (core/release_info.*); this file keeps only the Win32 network/install glue.

bool FetchReleases(std::vector<ReleaseInfo>& out) {
    std::string body;
    if (!HttpGet(API_HOST, API_PATH, body)) return false;
    return ParseReleasesJson(body, out);
}

const wchar_t* UpdateResultMessage(UpdateResult r) {
    switch (r) {
        case UpdateResult::Ok:       return L"ok";
        case UpdateResult::BadUrl:   return L"couldn't read the download URL";
        case UpdateResult::Download: return L"download failed (network or GitHub unreachable)";
        case UpdateResult::BadImage: return L"downloaded file wasn't a valid exe (blocked or truncated?)";
        case UpdateResult::Write:    return L"couldn't write the new exe (disk full or permissions?)";
        case UpdateResult::SwapOld:  return L"couldn't replace the running exe (in use or permissions?)";
        case UpdateResult::SwapNew:  return L"couldn't move the new exe into place (rolled back)";
    }
    return L"unknown error";
}

UpdateResult DownloadAndInstall(const std::wstring& url) {
    // split the URL into host + path
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"", path[2048] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return UpdateResult::BadUrl;

    std::string data;
    if (!HttpGet(host, path, data)) return UpdateResult::Download;
    // sanity: a real PE, not an error page
    if (data.size() < 100 * 1024 || data[0] != 'M' || data[1] != 'Z') return UpdateResult::BadImage;

    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return UpdateResult::Write;
    std::wstring tmp = std::wstring(exe) + L".new";
    std::wstring old = std::wstring(exe) + L".old";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return UpdateResult::Write;
    DWORD wr = 0;
    BOOL wok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    if (!wok || wr != data.size()) { DeleteFileW(tmp.c_str()); return UpdateResult::Write; }

    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe, old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return UpdateResult::SwapOld;
    }
    if (!MoveFileExW(tmp.c_str(), exe, 0)) {
        MoveFileW(old.c_str(), exe); // roll back
        DeleteFileW(tmp.c_str());
        return UpdateResult::SwapNew;
    }
    return UpdateResult::Ok;
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
