// FMDV command-line frontend — the first non-Windows consumer of the core.
//
// Purpose (macOS implementation guide, Phase 3): prove the platform-neutral
// markdown parser compiles and runs off Windows, and exercise the shared
// bench-logging schema (Phase 0) before any macOS UI is written. Builds on
// macOS/Linux with clang or gcc via the repo Makefile; the Windows GUI is
// untouched and still builds via cpp/build.ps1.
//
// Commands:
//   fmdv-cli parse <file.md>                    dump the parsed Document model
//   fmdv-cli bench-parse <file.md> [--runs N]   time parsing, log to FMDV_BENCH_LOG
//
// The `parse` output mirrors the Windows debug build's --parse-dump so the two
// can be diffed for byte-for-byte parser equivalence across platforms.

#include "markdown.h"
#include "bench_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
static const char* kPlatform = "windows";
#elif defined(__APPLE__)
static const char* kPlatform = "macos";
#else
static const char* kPlatform = "linux";
#endif

#ifndef FMDV_COMMIT
#define FMDV_COMMIT ""
#endif
#ifndef FMDV_BUILD
#define FMDV_BUILD "release"
#endif

// ---------------- UTF-8 <-> wstring bridge (frontend boundary) ----------------
// The core parser takes std::wstring. This is the frontend-boundary conversion
// the guide describes: the CLI reads/writes UTF-8 and bridges here. Handles both
// 32-bit wchar_t (macOS/Linux) and 16-bit wchar_t (Windows, surrogate pairs).

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

static std::wstring utf8ToW(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int len;
        if (c < 0x80)            { cp = c;          len = 1; }
        else if ((c >> 5) == 0x6){ cp = c & 0x1F;   len = 2; }
        else if ((c >> 4) == 0xE){ cp = c & 0x0F;   len = 3; }
        else if ((c >> 3) == 0x1E){ cp = c & 0x07;  len = 4; }
        else                     { cp = 0xFFFD;     len = 1; }
        if (i + (size_t)len > n) { cp = 0xFFFD; len = 1; }
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

static void appendUtf8(std::string& o, char32_t cp) {
    if (cp < 0x80) {
        o += (char)cp;
    } else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string wToUtf8(const std::wstring& w) {
    std::string o;
    o.reserve(w.size());
    for (size_t i = 0; i < w.size(); i++) {
        char32_t cp = (char32_t)(unsigned long)w[i];
        if (sizeof(wchar_t) < 4 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            char32_t lo = (char32_t)(unsigned long)w[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        appendUtf8(o, cp);
    }
    return o;
}

// ---------------- file loading ----------------

static bool readFileUtf8(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        out.resize((size_t)sz);
        size_t rd = std::fread(&out[0], 1, (size_t)sz, f);
        out.resize(rd);
    }
    std::fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3); // strip UTF-8 BOM
    }
    return true;
}

// The parser expects LF-normalized text; drop CR so CRLF fixtures parse the
// same as LF ones (matches the Windows frontend's normalization).
static std::wstring loadDoc(const std::string& utf8) {
    std::wstring w = utf8ToW(utf8);
    std::wstring lf;
    lf.reserve(w.size());
    for (wchar_t c : w) if (c != L'\r') lf += c;
    return lf;
}

// ---------------- parse dump (mirrors Windows --parse-dump) ----------------

static const char* blockTypeName(BlockType t) {
    switch (t) {
        case BlockType::Heading:    return "Heading";
        case BlockType::Paragraph:  return "Paragraph";
        case BlockType::CodeBlock:  return "CodeBlock";
        case BlockType::BlockQuote: return "BlockQuote";
        case BlockType::ListItem:   return "ListItem";
        case BlockType::Table:      return "Table";
        case BlockType::HRule:      return "HRule";
    }
    return "?";
}

static void dumpRuns(std::string& o, const std::vector<InlineRun>& runs) {
    for (const auto& r : runs) {
        o += "    run [";
        if (r.bold)   o += "b";
        if (r.italic) o += "i";
        if (r.code)   o += "c";
        if (r.strike) o += "s";
        if (!r.href.empty()) { o += "link="; o += wToUtf8(r.href); }
        o += "] \"";
        o += wToUtf8(r.text);
        o += "\"\n";
    }
}

static std::string dumpDoc(const Document& doc) {
    std::string o;
    char buf[128];
    std::snprintf(buf, sizeof buf, "=== Document: %zu blocks ===\n", doc.blocks.size());
    o += buf;
    for (size_t bi = 0; bi < doc.blocks.size(); bi++) {
        const Block& b = doc.blocks[bi];
        std::snprintf(buf, sizeof buf, "[%zu] %s", bi, blockTypeName(b.type));
        o += buf;
        if (b.type == BlockType::Heading) {
            std::snprintf(buf, sizeof buf, " level=%d", b.level);
            o += buf;
        }
        if (b.type == BlockType::ListItem) {
            std::snprintf(buf, sizeof buf, " %s depth=%d task=%d",
                          b.ordered ? "ordered" : "bullet", b.level, b.taskState);
            o += buf;
        }
        if (b.type == BlockType::CodeBlock) { o += " lang="; o += wToUtf8(b.lang); }
        o += "\n";
        if (b.type == BlockType::CodeBlock) {
            o += "    <code>\n";
            o += wToUtf8(b.codeText);
            o += "\n    </code>\n";
        } else if (b.type == BlockType::Table) {
            std::snprintf(buf, sizeof buf, "    headers: %zu cols, %zu rows, aligns:",
                          b.headers.size(), b.rows.size());
            o += buf;
            for (int a : b.aligns) { std::snprintf(buf, sizeof buf, " %d", a); o += buf; }
            o += "\n";
            for (const auto& h : b.headers) { o += "    H:"; dumpRuns(o, h.runs); }
            for (size_t ri = 0; ri < b.rows.size(); ri++) {
                std::snprintf(buf, sizeof buf, "    row %zu:\n", ri);
                o += buf;
                for (const auto& c : b.rows[ri].cells) dumpRuns(o, c.runs);
            }
        } else {
            dumpRuns(o, b.runs);
        }
    }
    return o;
}

// ---------------- commands ----------------

static int cmdParse(const char* path) {
    std::string utf8;
    if (!readFileUtf8(path, utf8)) {
        std::fprintf(stderr, "fmdv-cli: cannot read %s\n", path);
        return 1;
    }
    Document doc = ParseMarkdown(loadDoc(utf8));
    std::string out = dumpDoc(doc);
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

// Prefer the runtime FMDV_BENCH_COMMIT (matches the Windows frontend) so a CI
// run can stamp rows with the commit under test; fall back to the value baked
// in at build time.
static std::string resolveCommit() {
    const char* env = std::getenv("FMDV_BENCH_COMMIT");
    return (env && *env) ? env : FMDV_COMMIT;
}

static void logRow(fmdv::BenchLog& log, const char* file, const char* event,
                   double dur, int blocks, const std::string& notes) {
    fmdv::BenchRecord r;
    r.platform = kPlatform;
    r.frontend = "cli";
    r.build = FMDV_BUILD;
    r.commit = resolveCommit();
    r.file = file;
    r.event = event;
    r.duration_ms = dur;
    r.blocks = blocks;
    r.notes = notes;
    log.write(r);
}

static int cmdBenchParse(const char* path, int runs) {
    if (runs < 1) runs = 1;
    std::string utf8;
    if (!readFileUtf8(path, utf8)) {
        std::fprintf(stderr, "fmdv-cli: cannot read %s\n", path);
        return 1;
    }
    std::wstring doc = loadDoc(utf8);

    fmdv::BenchLog log;
    std::vector<double> times;
    times.reserve((size_t)runs);
    int blocks = 0;
    for (int i = 0; i < runs; i++) {
        double t0 = fmdv::NowMonotonicMs();
        Document d = ParseMarkdown(doc);
        double dt = fmdv::NowMonotonicMs() - t0;
        blocks = (int)d.blocks.size();
        times.push_back(dt);
        logRow(log, path, "parsed", dt, blocks, "run=" + std::to_string(i));
    }

    std::sort(times.begin(), times.end());
    double median = times[times.size() / 2];
    double p95 = times[(size_t)((times.size() - 1) * 0.95)];
    double mn = times.front();

    char p95buf[32];
    std::snprintf(p95buf, sizeof p95buf, "%.4f", p95);
    logRow(log, path, "parse_median", median, blocks,
           "runs=" + std::to_string(runs) + " p95=" + p95buf);

    std::printf("bench-parse %s: runs=%d blocks=%d  median=%.4f ms  p95=%.4f ms  min=%.4f ms\n",
                path, runs, blocks, median, p95, mn);
    if (!log.active()) {
        std::fprintf(stderr, "(FMDV_BENCH_LOG not set — per-run rows not written to a file)\n");
    }
    return 0;
}

static int usage() {
    std::fprintf(stderr,
        "fmdv-cli — FMDV command-line frontend\n\n"
        "Usage:\n"
        "  fmdv-cli parse <file.md>                   dump the parsed Document model\n"
        "  fmdv-cli bench-parse <file.md> [--runs N]  time parsing (default N=100)\n\n"
        "Env:\n"
        "  FMDV_BENCH_LOG    CSV path; benchmark rows are appended when set\n"
        "  FMDV_BENCH_LABEL  optional run label recorded in each row\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const char* cmd = argv[1];

    if (std::strcmp(cmd, "parse") == 0) {
        if (argc < 3) return usage();
        return cmdParse(argv[2]);
    }
    if (std::strcmp(cmd, "bench-parse") == 0) {
        if (argc < 3) return usage();
        int runs = 100;
        for (int i = 3; i < argc; i++) {
            if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) runs = std::atoi(argv[++i]);
        }
        return cmdBenchParse(argv[2], runs);
    }
    if (std::strcmp(cmd, "--help") == 0 || std::strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }
    return usage();
}
