// Unit tests for the shared benchmark logger (core/bench_log.h): env-driven
// activation, the CSV schema, quoting, and header-once-per-file behavior. The
// cross-platform bench comparison relies on every frontend emitting this exact
// schema.
#include "bench_log.h"
#include "test_check.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static const char* kPath = "bench_log_test.tmp.csv";

static void setEnv(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    if (v && *v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

static std::vector<std::string> readLines() {
    std::vector<std::string> lines;
    std::ifstream in(kPath);
    std::string l;
    while (std::getline(in, l)) lines.push_back(l);
    return lines;
}

int main() {
    std::remove(kPath);

    // ---- inactive without FMDV_BENCH_LOG ----
    {
        setEnv("FMDV_BENCH_LOG", "");
        fmdv::BenchLog log;
        check(!log.active(), "env unset: logger inactive");
        fmdv::BenchRecord r;
        r.event = "noop";
        log.write(r); // must be a harmless no-op
        check(readLines().empty(), "env unset: nothing written");
    }

    // ---- header + row schema ----
    {
        setEnv("FMDV_BENCH_LOG", kPath);
        setEnv("FMDV_BENCH_LABEL", "unit");
        fmdv::BenchLog log;
        check(log.active(), "env set: logger active");
        check(log.label() == "unit", "env set: label picked up");

        fmdv::BenchRecord r;
        r.platform = "windows";
        r.frontend = "cli";
        r.build = "release";
        r.event = "parsed";
        r.duration_ms = 1.5;
        r.blocks = 3;
        log.write(r);

        auto lines = readLines();
        check(lines.size() == 2, "write: header + one row");
        check(!lines.empty() && lines[0] == fmdv::BenchLog::Header(), "write: header row matches schema");
        if (lines.size() == 2) {
            const std::string& row = lines[1];
            check(row.find(",windows,cli,release,") != std::string::npos,
                  "write: identity fields in order");
            check(row.find(",unit,") != std::string::npos, "write: empty label defaults to env label");
            check(row.find(",parsed,1.5000,3,") != std::string::npos,
                  "write: duration formatted to 4 decimals");
            int commas = 0;
            for (char c : row) if (c == ',') commas++;
            check(commas == 14, "write: 15 columns");
            check(row.size() > 4 && row.find("T") != std::string::npos
                      && row.find("Z") < row.find(','),
                  "write: ISO-8601 UTC timestamp first");
            // width/height/content_height left -1 -> empty cells => ",," runs
            check(row.find(",,parsed") != std::string::npos,
                  "write: unset numeric fields are empty cells");
        }
    }

    // ---- quoting + append without duplicate header ----
    {
        fmdv::BenchLog log; // same path, now non-empty
        fmdv::BenchRecord r;
        r.event = "e";
        r.label = "explicit";       // overrides the env label
        r.notes = "a,b \"q\"";      // needs CSV quoting
        log.write(r);

        auto lines = readLines();
        check(lines.size() == 3, "append: no duplicate header on existing file");
        if (lines.size() == 3) {
            check(lines[2].find(",explicit,") != std::string::npos,
                  "append: explicit label wins over env label");
            check(lines[2].find("\"a,b \"\"q\"\"\"") != std::string::npos,
                  "append: commas and quotes escaped CSV-style");
        }
    }

    std::remove(kPath);
    return summary();
}
