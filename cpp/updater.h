#pragma once
#include <string>
#include <vector>
#include "release_info.h"  // ReleaseInfo, ParseReleasesJson, CompareVersions (shared core)

// In-app updates (issue #9): fetch releases from the GitHub API, download an
// exe asset, and swap it in place of the running binary. All functions block;
// call them off the UI thread.

// GET /repos/<owner>/<repo>/releases. Newest first. False on network/parse failure.
bool FetchReleases(std::vector<ReleaseInfo>& out);

// Which step DownloadAndInstall failed at, so the UI can say something more
// useful than "update failed".
enum class UpdateResult {
    Ok,
    BadUrl,      // couldn't parse the asset URL
    Download,    // network/HTTP failure fetching the asset
    BadImage,    // response wasn't a plausible PE (too small, wrong magic)
    Write,       // couldn't write the downloaded exe to <exe>.new
    SwapOld,     // couldn't rename the running exe to <exe>.old
    SwapNew,     // couldn't move <exe>.new into place (rolled back)
};

const wchar_t* UpdateResultMessage(UpdateResult r);

// Download url and swap it in as the running exe: current exe is renamed to
// <exe>.old (renaming a running image is allowed on Windows), the download is
// moved into its place. On failure the running exe is left untouched.
UpdateResult DownloadAndInstall(const std::wstring& url);

// Delete a leftover <exe>.old from a previous swap. Silent if locked/missing.
void CleanupOldExe();

// Version of the running binary ("1.1.0"). FMDV_VERSION_OVERRIDE env var wins
// (test hook: lets the suite simulate an outdated install).
std::wstring CurrentVersion();
