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
    Write,       // couldn't write the downloaded exe anywhere (.new nor %TEMP%)
    SwapOld,     // couldn't rename the running exe to <exe>.old
    SwapNew,     // couldn't move the new exe into place (rolled back)
    UacDeclined, // install folder needs admin and the UAC prompt was declined
    Elevated,    // the elevated helper ran but reported failure
};

const wchar_t* UpdateResultMessage(UpdateResult r);

// Download url and swap it in as the running exe: current exe is renamed to
// <exe>.old (renaming a running image is allowed on Windows), the download is
// moved into its place. On failure the running exe is left untouched.
// If the install folder denies writes (e.g. Program Files), the download is
// staged in %TEMP% and the swap re-runs this exe elevated with --apply-update
// (one UAC prompt); the app itself never needs to run as admin.
UpdateResult DownloadAndInstall(const std::wstring& url);

// The two renames of the swap, applied by the elevated "--apply-update <new>"
// re-exec. Returns false if either rename fails (running exe left in place).
bool ApplyUpdateRenames(const std::wstring& newFile);

// Delete a leftover <exe>.old from a previous swap. Silent if locked/missing.
void CleanupOldExe();

// Version of the running binary ("1.1.0"). FMDV_VERSION_OVERRIDE env var wins
// (test hook: lets the suite simulate an outdated install).
std::wstring CurrentVersion();
