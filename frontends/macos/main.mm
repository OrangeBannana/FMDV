// FMDV macOS frontend entry point.
//
// This milestone implements the headless renderer (--dump), which is the
// primary correctness check (parse -> core/layout -> CoreText/CoreGraphics ->
// PNG), mirroring the Windows --dump path. The AppKit window is added next.
#include "mac_render.h"
#include "markdown.h"
#include "str.h"
#include "bench_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static bool readFileUtf8(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0) { out.resize((size_t)sz); out.resize(std::fread(&out[0], 1, (size_t)sz, f)); }
    std::fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
    return true;
}

static Str loadDoc(std::string u8) {
    u8.erase(std::remove(u8.begin(), u8.end(), '\r'), u8.end());
    return FromUtf8(u8);
}

static int usage() {
    std::fprintf(stderr,
        "fmdv-macos — FMDV macOS frontend\n\n"
        "  fmdv-macos <file.md> [--dark]                 open in a window\n"
        "  fmdv-macos --dump <file.md> <out.png> [--width W] [--dark]\n"
        "  fmdv-macos --bench-render <file.md> [--width W] [--runs N] [--dark]\n");
    return 2;
}

static int benchRender(const char* file, double width, bool dark, int runs) {
    std::string u8;
    if (!readFileUtf8(file, u8)) { std::fprintf(stderr, "fmdv-macos: cannot read %s\n", file); return 1; }
    double t0 = fmdv::NowMonotonicMs();
    Document doc = ParseMarkdown(loadDoc(u8));
    double parseMs = fmdv::NowMonotonicMs() - t0;

    double layoutMs = 0, renderMs = 0;
    fmdv::BenchLayoutRender(doc, width, dark, runs, layoutMs, renderMs);

    const char* envCommit = std::getenv("FMDV_BENCH_COMMIT");
    fmdv::BenchLog log;
    auto row = [&](const char* event, double ms) {
        fmdv::BenchRecord r;
        r.platform = "macos"; r.frontend = "macos"; r.build = "release";
        r.commit = (envCommit && *envCommit) ? envCommit : "";
        r.file = file; r.theme = dark ? "dark" : "light"; r.width = (int)width;
        r.event = event; r.duration_ms = ms; r.blocks = (int)doc.blocks.size();
        log.write(r);
    };
    row("parsed", parseMs);
    row("layout_once", layoutMs);
    row("paint_viewport_avg", renderMs);

    std::printf("bench-render %s: blocks=%zu width=%.0f%s  parse=%.4f  layout=%.4f  render=%.4f ms (median of %d)\n",
                file, doc.blocks.size(), width, dark ? " dark" : "", parseMs, layoutMs, renderMs, runs);
    if (!log.active())
        std::fprintf(stderr, "(FMDV_BENCH_LOG not set — rows not written to a file)\n");
    return 0;
}

int main(int argc, char** argv) {
    const char* file = nullptr;
    const char* out = nullptr;
    double width = 900;
    int runs = 100;
    bool dark = false, dump = false, bench = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--dump") == 0 && i + 2 < argc) {
            dump = true; file = argv[++i]; out = argv[++i];
        } else if (std::strcmp(argv[i], "--bench-render") == 0 && i + 1 < argc) {
            bench = true; file = argv[++i];
        } else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--dark") == 0) {
            dark = true;
        } else if (argv[i][0] != '-' && !file) {
            file = argv[i];
        }
    }
    if (bench) {
        if (!file) return usage();
        return benchRender(file, width, dark, runs);
    }
    if (dump) {
        if (!file || !out) return usage();
        std::string u8;
        if (!readFileUtf8(file, u8)) { std::fprintf(stderr, "fmdv-macos: cannot read %s\n", file); return 1; }
        Document doc = ParseMarkdown(loadDoc(u8));
        if (!fmdv::RenderMarkdownToPng(doc, width, dark, out)) {
            std::fprintf(stderr, "fmdv-macos: render failed\n");
            return 1;
        }
        std::printf("wrote %s (%zu blocks, width %.0f%s)\n", out, doc.blocks.size(), width, dark ? ", dark" : "");
        return 0;
    }
    // Window mode: `file` may be null (a bare .app launch opens a file panel; the
    // Finder passes documents via the openFile: Apple Event, not argv).
    return fmdv::RunApp(file, dark);
}
