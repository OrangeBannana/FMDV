#pragma once
// Minimal shared harness for the core unit tests: check() prints one line per
// assertion, and the suite's exit code is nonzero when anything failed. Same
// zero-dependency style the first test file (text_select_test.cpp) used.
#include <cstdio>

static int failures = 0;

static void check(bool ok, const char* name) {
    std::printf("%s  %s\n", ok ? "OK  " : "FAIL", name);
    if (!ok) failures++;
}

static int summary() {
    std::printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
