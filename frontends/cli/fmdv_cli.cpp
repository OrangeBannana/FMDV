// FMDV command-line frontend — the first non-Windows consumer of the core.
//
// Purpose (macOS implementation guide, Phase 3): prove the platform-neutral
// core compiles and runs off Windows, and exercise the shared bench-logging
// schema (Phase 0) before any macOS UI is written. Builds on macOS/Linux with
// clang or gcc via the repo Makefile; the Windows GUI is untouched.
//
// Commands:
//   fmdv-cli parse <file.md>                    dump the parsed Document model
//   fmdv-cli bench-parse <file.md> [--runs N]   time parsing, log to FMDV_BENCH_LOG
//   fmdv-cli suggest --line "<text>"            autocomplete for a line
//   fmdv-cli table --cols N --rows M            markdown for a table
//   fmdv-cli releases <releases.json>           parse a GitHub releases payload
//   fmdv-cli vercmp <a> <b>                     compare versions (-1/0/1)
//
// The `parse` output mirrors the Windows debug build's --parse-dump so the two
// can be diffed for byte-for-byte parser equivalence across platforms. UTF-8
// <-> core-string conversion is the frontend boundary; it uses core/str.h.

#include "markdown.h"
#include "edit_assist.h"
#include "release_info.h"
#include "layout.h"
#include "str.h"
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

// The parser expects LF-normalized text; drop CR bytes (ASCII 0x0D) so CRLF
// fixtures parse the same as LF ones, then convert to the core string type.
static Str loadDoc(std::string utf8) {
    utf8.erase(std::remove(utf8.begin(), utf8.end(), '\r'), utf8.end());
    return FromUtf8(utf8);
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
        if (!r.href.empty()) { o += "link="; o += ToUtf8(r.href); }
        o += "] \"";
        o += ToUtf8(r.text);
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
        if (b.type == BlockType::CodeBlock) { o += " lang="; o += ToUtf8(b.lang); }
        o += "\n";
        if (b.type == BlockType::CodeBlock) {
            o += "    <code>\n";
            o += ToUtf8(b.codeText);
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
    Str doc = loadDoc(utf8);

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

// Render a suggestion/continuation string on one line, showing '\n' as "\\n"
// so multi-line results stay greppable in tests and shells.
static std::string escapeNl(const Str& w) {
    std::string u = ToUtf8(w);
    std::string o;
    for (char c : u) {
        if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

static int cmdSuggest(const std::string& line) {
    fmdv::Suggestion sg = fmdv::SuggestClose(FromUtf8(line));
    std::printf("caret=%d text=%s\n", sg.caret, escapeNl(sg.text).c_str());
    return 0;
}

static int cmdTable(int cols, int rows) {
    std::string out = ToUtf8(fmdv::MakeTableMarkdown(cols, rows));
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

static int cmdReleases(const char* path) {
    std::string json;
    if (!readFileUtf8(path, json)) {
        std::fprintf(stderr, "fmdv-cli: cannot read %s\n", path);
        return 1;
    }
    std::vector<ReleaseInfo> rel;
    ParseReleasesJson(json, rel);
    std::printf("releases: %zu\n", rel.size());
    for (const auto& r : rel) {
        std::string exe = ToUtf8(r.exeUrl);
        std::printf("%s\t%s\n", ToUtf8(r.tag).c_str(), exe.empty() ? "(no exe asset)" : exe.c_str());
    }
    return 0;
}

static int cmdVercmp(const char* a, const char* b) {
    int c = CompareVersions(FromUtf8(a), FromUtf8(b));
    std::printf("%d\n", c < 0 ? -1 : (c > 0 ? 1 : 0));
    return 0;
}

// Approximate metrics so the platform-neutral layout engine can be exercised
// without a real font system. The macOS frontend supplies a CoreText measurer.
struct StubMeasurer : fmdv::TextMeasurer {
    double textWidth(const fmdv::FontSpec& f, StrView s) override { return s.size() * f.px * 0.55; }
    double lineHeight(const fmdv::FontSpec& f) override { return f.px * 1.4; }
    double ascent(const fmdv::FontSpec& f) override { return f.px * 1.1; }
};

static int cmdLayout(const char* path) {
    std::string utf8;
    if (!readFileUtf8(path, utf8)) {
        std::fprintf(stderr, "fmdv-cli: cannot read %s\n", path);
        return 1;
    }
    Document doc = ParseMarkdown(loadDoc(utf8));
    StubMeasurer tm;
    fmdv::LayoutResult r = fmdv::LayoutDocument(doc, 900, fmdv::LightLayoutTheme(), tm);
    int text = 0, fill = 0, frame = 0, line = 0;
    for (const auto& c : r.cmds) {
        switch (c.kind) {
            case fmdv::DrawCommand::Text:      text++;  break;
            case fmdv::DrawCommand::FillRect:  fill++;  break;
            case fmdv::DrawCommand::FrameRect: frame++; break;
            case fmdv::DrawCommand::Line:      line++;  break;
        }
    }
    std::printf("blocks=%zu commands=%zu text=%d fill=%d frame=%d line=%d links=%zu content_height=%.1f\n",
                doc.blocks.size(), r.cmds.size(), text, fill, frame, line, r.links.size(), r.contentHeight);
    return 0;
}

static int usage() {
    std::fprintf(stderr,
        "fmdv-cli — FMDV command-line frontend\n\n"
        "Usage:\n"
        "  fmdv-cli parse <file.md>                   dump the parsed Document model\n"
        "  fmdv-cli bench-parse <file.md> [--runs N]  time parsing (default N=100)\n"
        "  fmdv-cli suggest --line \"<text>\"           autocomplete for a line\n"
        "  fmdv-cli table --cols N --rows M           markdown for a table\n"
        "  fmdv-cli releases <releases.json>          parse a GitHub releases payload\n"
        "  fmdv-cli vercmp <a> <b>                    compare versions (-1/0/1)\n"
        "  fmdv-cli layout <file.md>                  lay out (stub metrics) + dump stats\n\n"
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
    if (std::strcmp(cmd, "suggest") == 0) {
        const char* line = nullptr;
        for (int i = 2; i < argc; i++) {
            if (std::strcmp(argv[i], "--line") == 0 && i + 1 < argc) line = argv[++i];
        }
        if (!line) return usage();
        return cmdSuggest(line);
    }
    if (std::strcmp(cmd, "table") == 0) {
        int cols = 3, rows = 3;
        for (int i = 2; i < argc; i++) {
            if (std::strcmp(argv[i], "--cols") == 0 && i + 1 < argc) cols = std::atoi(argv[++i]);
            else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc) rows = std::atoi(argv[++i]);
        }
        return cmdTable(cols, rows);
    }
    if (std::strcmp(cmd, "releases") == 0) {
        if (argc < 3) return usage();
        return cmdReleases(argv[2]);
    }
    if (std::strcmp(cmd, "vercmp") == 0) {
        if (argc < 4) return usage();
        return cmdVercmp(argv[2], argv[3]);
    }
    if (std::strcmp(cmd, "layout") == 0) {
        if (argc < 3) return usage();
        return cmdLayout(argv[2]);
    }
    if (std::strcmp(cmd, "--help") == 0 || std::strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }
    return usage();
}
