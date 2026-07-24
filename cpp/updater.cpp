#include "updater.h"
#include "version.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
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
        case UpdateResult::Write:    return L"couldn't write the new exe (disk full?)";
        case UpdateResult::SwapOld:  return L"couldn't replace the running exe (in use or permissions?)";
        case UpdateResult::SwapNew:  return L"couldn't move the new exe into place (rolled back)";
        case UpdateResult::UacDeclined: return L"install folder needs admin — retry and click Yes on the prompt";
        case UpdateResult::Elevated: return L"admin-elevated install step failed";
    }
    return L"unknown error";
}

// Write `data` to `path` (CREATE_ALWAYS). Returns false on any failure;
// `deniedOut` reports whether the failure was an access-denied.
static bool WriteAll(const std::wstring& path, const std::string& data, bool* deniedOut) {
    if (deniedOut) *deniedOut = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (deniedOut) *deniedOut = (GetLastError() == ERROR_ACCESS_DENIED);
        return false;
    }
    DWORD wr = 0;
    BOOL wok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    if (!wok || wr != data.size()) { DeleteFileW(path.c_str()); return false; }
    return true;
}

// The two renames that swap `newFile` in as `exe`. `deniedOut` reports whether
// a failure was an access-denied (candidate for elevation).
static UpdateResult SwapInPlace(const std::wstring& exe, const std::wstring& newFile, bool* deniedOut) {
    if (deniedOut) *deniedOut = false;
    std::wstring old = exe + L".old";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (deniedOut) *deniedOut = (GetLastError() == ERROR_ACCESS_DENIED);
        return UpdateResult::SwapOld;
    }
    if (!MoveFileExW(newFile.c_str(), exe.c_str(), 0)) {
        if (deniedOut) *deniedOut = (GetLastError() == ERROR_ACCESS_DENIED);
        MoveFileW(old.c_str(), exe.c_str()); // roll back
        return UpdateResult::SwapNew;
    }
    return UpdateResult::Ok;
}

bool ApplyUpdateRenames(const std::wstring& newFile) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    return SwapInPlace(exe, newFile, nullptr) == UpdateResult::Ok;
}

// Re-run this exe elevated (one UAC prompt) to do just the swap renames.
// Blocks until the helper exits.
static UpdateResult ElevatedSwap(const std::wstring& exe, const std::wstring& newFile) {
    std::wstring params = L"--apply-update \"" + newFile + L"\"";
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        DeleteFileW(newFile.c_str());
        return (GetLastError() == ERROR_CANCELLED) ? UpdateResult::UacDeclined
                                                   : UpdateResult::Elevated;
    }
    DWORD code = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    if (code != 0) { DeleteFileW(newFile.c_str()); return UpdateResult::Elevated; }
    return UpdateResult::Ok;
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

    // Stage the download next to the exe when the folder allows it (same-volume
    // rename, cleaned up naturally); fall back to %TEMP% when it doesn't (the
    // swap will then need elevation anyway).
    std::wstring stage = std::wstring(exe) + L".new";
    bool denied = false;
    if (!WriteAll(stage, data, &denied)) {
        if (!denied) return UpdateResult::Write;
        wchar_t tmpDir[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, tmpDir)) return UpdateResult::Write;
        stage = std::wstring(tmpDir) + L"fmdv-update.new";
        if (!WriteAll(stage, data, nullptr)) return UpdateResult::Write;
    }

    UpdateResult r = SwapInPlace(exe, stage, &denied);
    if (r == UpdateResult::Ok) return r;
    if (!denied) { DeleteFileW(stage.c_str()); return r; }
    // Folder needs admin: one UAC prompt, elevated helper does just the renames.
    return ElevatedSwap(exe, stage);
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
