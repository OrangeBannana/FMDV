#pragma once
// Shared, platform-neutral benchmark logging for FMDV.
//
// One CSV row per event, appended to the path in the FMDV_BENCH_LOG env var
// (logging is a no-op when it is unset). The schema is identical across the
// Windows, CLI, and macOS frontends so results are directly comparable — see
// docs/macos-implementation-guide.md, "Phase 0: Add Timing and Logging First".
//
// Header-only and dependency-free (no windows.h) so every frontend can include
// it. Durations use a monotonic clock; the timestamp column is wall-clock and
// exists only for ordering and human reading.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

namespace fmdv {

// Monotonic milliseconds for measuring durations. The guide calls for
// steady_clock / mach_absolute_time / clock_gettime; steady_clock wraps the
// best available monotonic source on each platform.
inline double NowMonotonicMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Wall-clock ISO-8601 UTC timestamp for the `timestamp` column.
inline std::string WallTimestamp() {
    using namespace std::chrono;
    std::time_t t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// One benchmark row. Numeric fields left negative are emitted as empty cells.
struct BenchRecord {
    std::string platform;   // windows | macos | linux
    std::string frontend;   // win32 | macos | cli
    std::string build;      // release | debug
    std::string commit;
    std::string label;      // defaults to FMDV_BENCH_LABEL when left empty
    std::string file;
    std::string theme;      // light | dark | ""
    int width = -1;
    int height = -1;
    std::string event;      // process_start, parsed, first_paint, ...
    double duration_ms = -1;
    int blocks = -1;
    int content_height = -1;
    std::string notes;
};

class BenchLog {
public:
    // Opens FMDV_BENCH_LOG for append, writing the header row if the file is new
    // or empty. When the env var is unset, active() is false and write() is a
    // no-op, so callers can log unconditionally.
    BenchLog() {
        const char* path = std::getenv("FMDV_BENCH_LOG");
        if (!path || !*path) return;
        const char* lbl = std::getenv("FMDV_BENCH_LABEL");
        label_ = lbl ? lbl : "";
        bool nonEmpty = false;
        {
            std::ifstream in(path);
            nonEmpty = in.good() && in.peek() != std::ifstream::traits_type::eof();
        }
        out_.open(path, std::ios::app);
        if (out_ && !nonEmpty) out_ << Header() << "\n";
    }

    bool active() const { return out_.is_open(); }
    const std::string& label() const { return label_; }

    void write(BenchRecord r) {
        if (!out_) return;
        if (r.label.empty()) r.label = label_;
        out_ << WallTimestamp() << ','
             << Esc(r.platform) << ',' << Esc(r.frontend) << ',' << Esc(r.build) << ','
             << Esc(r.commit) << ',' << Esc(r.label) << ',' << Esc(r.file) << ',' << Esc(r.theme) << ','
             << OptInt(r.width) << ',' << OptInt(r.height) << ','
             << Esc(r.event) << ',' << OptDbl(r.duration_ms) << ','
             << OptInt(r.blocks) << ',' << OptInt(r.content_height) << ',' << Esc(r.notes) << "\n";
        out_.flush();
    }

    static const char* Header() {
        return "timestamp,platform,frontend,build,commit,label,file,theme,width,height,"
               "event,duration_ms,blocks,content_height,notes";
    }

private:
    static std::string Esc(const std::string& s) {
        if (s.find_first_of(",\"\n") == std::string::npos) return s;
        std::string o = "\"";
        for (char c : s) { if (c == '"') o += '"'; o += c; }
        o += '"';
        return o;
    }
    static std::string OptInt(int v) { return v < 0 ? std::string() : std::to_string(v); }
    static std::string OptDbl(double v) {
        if (v < 0) return {};
        char b[32];
        std::snprintf(b, sizeof b, "%.4f", v);
        return b;
    }

    std::ofstream out_;
    std::string label_;
};

} // namespace fmdv
