#pragma once
#include <string>

// Structured benchmark CSV logging for the Win32 frontend. Kept as a plain-
// parameter module -- like render.cpp/updater.cpp -- rather than reaching
// into fmdv.cpp's globals, so it stays a self-contained TU.

// Resolves FMDV_BENCH_LOG (or, when forceDefaultPath is set and the env var
// is unset, bench\results\windows-baseline.csv) plus FMDV_BENCH_LABEL /
// FMDV_BENCH_COMMIT (falling back to GITHUB_SHA). BenchLog() stays a no-op
// if no path resolves.
void InitBenchLog(bool forceDefaultPath);

// True once InitBenchLog has resolved a log path; lets callers gate one-shot
// logging decisions (e.g. "log the first layout") without a BenchLog() call.
bool BenchLogActive();

// Appends one CSV row (writes the header first if the file is new/empty).
// No-op when BenchLogActive() is false. filePath/dark/blockCount are the
// caller's current-document state, passed explicitly since this module
// holds no app globals of its own.
void BenchLog(const char* event, double durationMs, int width, int height,
              int contentH, const char* notes,
              const std::wstring& filePath, bool dark, size_t blockCount);
