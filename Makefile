# Portable build for the FMDV CLI + shared core (macOS/Linux, clang or gcc).
#
# This is additive: the Windows GUI still builds via cpp/build.ps1. The CLI is
# the first non-Windows consumer of the platform-neutral parser and exists to
# prove the core compiles off Windows and to run parser benchmarks. See
# docs/macos-implementation-guide.md.
#
#   make cli     build build/fmdv-cli
#   make check   build, then smoke-test parse + bench-parse on test.md
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
COMMIT   := $(shell git rev-parse --short HEAD 2>/dev/null)
INCLUDES := -Icpp -Icore
DEFS     := -DFMDV_COMMIT=\"$(COMMIT)\" -DFMDV_BUILD=\"release\"

CLI_SRCS := frontends/cli/fmdv_cli.cpp cpp/markdown.cpp core/edit_assist.cpp
CLI_DEPS := core/bench_log.h core/edit_assist.h cpp/markdown.h
CLI_BIN  := build/fmdv-cli

.PHONY: cli check clean
cli: $(CLI_BIN)

$(CLI_BIN): $(CLI_SRCS) $(CLI_DEPS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFS) $(CLI_SRCS) -o $(CLI_BIN)

check: cli
	@echo "== parse test.md =="
	@$(CLI_BIN) parse test.md | head -n 5
	@echo "== bench-parse test.md =="
	@FMDV_BENCH_LOG=build/bench-check.csv FMDV_BENCH_LABEL=make-check \
		$(CLI_BIN) bench-parse test.md --runs 50
	@echo "== edit helpers =="
	@test "`$(CLI_BIN) suggest --line '**'`" = "caret=0 text=**" || { echo "suggest ** FAILED"; exit 1; }
	@test "`$(CLI_BIN) suggest --line '- ['`" = "caret=3 text= ] " || { echo "suggest checkbox FAILED"; exit 1; }
	@$(CLI_BIN) table --cols 2 --rows 1 | grep -q '| Column 1 | Column 2 |' || { echo "table FAILED"; exit 1; }
	@echo "edit helpers OK"

clean:
	rm -rf build
