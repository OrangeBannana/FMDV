// Unit tests for the shared release parsing + version ordering
// (core/release_info). The Windows updater and the macOS update banner both
// decide "is there something newer?" through these two functions.
#include "release_info.h"
#include "test_check.h"
#include <string>

static std::string u8(const Str& s) { return ToUtf8(s); }

int main() {
    // ---- ParseReleasesJson ----
    {
        const char* json =
            "[{\"tag_name\":\"v1.1.0\",\"assets\":["
            "{\"name\":\"notes.txt\",\"browser_download_url\":\"https://x/y/notes.txt\"},"
            "{\"name\":\"fmdv.exe\",\"browser_download_url\":\"https://x/v1.1.0/fmdv.exe\"},"
            "{\"name\":\"FMDV-macos.zip\",\"browser_download_url\":\"https://x/v1.1.0/FMDV-macos.zip\"}]},"
            "{\"tag_name\":\"v1.0.0\",\"assets\":[]}]";
        std::vector<ReleaseInfo> rel;
        bool ok = ParseReleasesJson(json, rel);
        check(ok && rel.size() == 2, "releases: two releases found");
        check(rel.size() == 2 && u8(rel[0].tag) == "v1.1.0" && u8(rel[1].tag) == "v1.0.0",
              "releases: tags in payload order (newest first)");
        check(rel.size() == 2 && u8(rel[0].exeUrl) == "https://x/v1.1.0/fmdv.exe",
              "releases: exe asset url extracted, non-exe assets skipped");
        check(rel.size() == 2 && u8(rel[0].macUrl) == "https://x/v1.1.0/FMDV-macos.zip",
              "releases: macos zip asset url extracted alongside exe");
        check(rel.size() == 2 && rel[1].exeUrl.empty() && rel[1].macUrl.empty(),
              "releases: release without assets has empty urls");
    }
    {
        // macOS asset only (a mac-only release), and order independence: the
        // zip may precede the exe in the payload.
        const char* json =
            "[{\"tag_name\":\"v1.2.0\",\"assets\":["
            "{\"browser_download_url\":\"https://x/v1.2.0/FMDV-macos.zip\"},"
            "{\"browser_download_url\":\"https://x/v1.2.0/fmdv.exe\"}]},"
            "{\"tag_name\":\"v1.1.0\",\"assets\":["
            "{\"browser_download_url\":\"https://x/v1.1.0/FMDV-macos.zip\"}]}]";
        std::vector<ReleaseInfo> rel;
        check(ParseReleasesJson(json, rel) && rel.size() == 2, "releases: mac-first payload parses");
        check(rel.size() == 2 && u8(rel[0].exeUrl) == "https://x/v1.2.0/fmdv.exe" &&
                  u8(rel[0].macUrl) == "https://x/v1.2.0/FMDV-macos.zip",
              "releases: both assets found regardless of order");
        check(rel.size() == 2 && rel[1].exeUrl.empty() &&
                  u8(rel[1].macUrl) == "https://x/v1.1.0/FMDV-macos.zip",
              "releases: mac-only release keeps zip, empty exe");
    }
    {
        // An exe asset that belongs to the NEXT release must not be attributed
        // to a release that has none of its own.
        const char* json =
            "[{\"tag_name\":\"v2.0.0\",\"assets\":[]},"
            "{\"tag_name\":\"v1.0.0\",\"assets\":["
            "{\"browser_download_url\":\"https://x/v1.0.0/fmdv.exe\"}]}]";
        std::vector<ReleaseInfo> rel;
        check(ParseReleasesJson(json, rel) && rel.size() == 2, "releases: two entries");
        check(rel.size() == 2 && rel[0].exeUrl.empty() && rel[0].macUrl.empty(),
              "releases: asset search stops at the next tag_name");
        check(rel.size() == 2 && u8(rel[1].exeUrl) == "https://x/v1.0.0/fmdv.exe",
              "releases: later release keeps its own asset");
    }
    {
        const char* json =
            "[{\"tag_name\":\"v1.0.0\",\"assets\":["
            "{\"browser_download_url\":\"https://x/fmdv.exe.sig\"}]}]";
        std::vector<ReleaseInfo> rel;
        ParseReleasesJson(json, rel);
        check(rel.size() == 1 && rel[0].exeUrl.empty(),
              "releases: url must end with /fmdv.exe (not .exe.sig)");
    }
    {
        std::vector<ReleaseInfo> rel;
        check(!ParseReleasesJson("[]", rel) && rel.empty(),
              "releases: empty payload returns false");
        check(!ParseReleasesJson("not json at all", rel),
              "releases: garbage payload returns false");
    }

    // ---- CompareVersions ----
    check(CompareVersions(U16("v1.0.0"), U16("1.0.0")) == 0, "vercmp: leading v ignored");
    check(CompareVersions(U16("V2.0"), U16("v2.0")) == 0, "vercmp: capital V ignored");
    check(CompareVersions(U16("v1.10.0"), U16("v1.9.9")) > 0, "vercmp: numeric, not lexicographic");
    check(CompareVersions(U16("0.9"), U16("1.0")) < 0, "vercmp: major compares first");
    check(CompareVersions(U16("1.0.1"), U16("1.0")) > 0, "vercmp: longer version with extra nonzero part wins");
    check(CompareVersions(U16("1.0"), U16("1.0.0")) == 0, "vercmp: trailing zero part is equal");
    check(CompareVersions(U16("1.2.3"), U16("1.2.4")) < 0, "vercmp: patch compares last");
    check(CompareVersions(U16("2.0"), U16("10.0")) < 0, "vercmp: 2 < 10");
    check(CompareVersions(U16("1.0.0-rc1"), U16("1.0.0")) == 0, "vercmp: suffix ignored");
    check(CompareVersions(U16("1.0.0-rc1"), U16("1.0.0-rc2")) == 0, "vercmp: suffixes never ordered");
    check(CompareVersions(U16(""), U16("")) == 0, "vercmp: empty inputs equal");
    check(CompareVersions(U16("v"), U16("")) == 0, "vercmp: bare v equals empty");

    return summary();
}
