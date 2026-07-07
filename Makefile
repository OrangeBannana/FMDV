# Portable build for the FMDV CLI + shared core (macOS/Linux, clang or gcc).
#
# This is additive: the Windows GUI still builds via cpp/build.ps1. The CLI is
# the first non-Windows consumer of the platform-neutral parser and exists to
# prove the core compiles off Windows and to run parser benchmarks. See
# docs/macos-implementation-guide.md.
#
#   make cli     build build/fmdv-cli
#   make check   build, then smoke-test parse + bench-parse on test.md
#   make macos   build the native macOS binary (build/fmdv-macos)  [macOS only]
#   make app     assemble build/FMDV.app with a generated icon     [macOS only]
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
COMMIT   := $(shell git rev-parse --short HEAD 2>/dev/null)
INCLUDES := -Icore
DEFS     := -DFMDV_COMMIT=\"$(COMMIT)\" -DFMDV_BUILD=\"release\"

CLI_SRCS := frontends/cli/fmdv_cli.cpp core/str.cpp core/markdown.cpp core/edit_assist.cpp core/release_info.cpp core/layout.cpp
CLI_DEPS := core/str.h core/markdown.h core/edit_assist.h core/release_info.h core/layout.h core/bench_log.h
CLI_BIN  := build/fmdv-cli

# macOS frontend (headless renderer for now): CoreText/CoreGraphics via .mm.
MAC_SRCS := frontends/macos/main.mm frontends/macos/app.mm frontends/macos/mac_render.mm core/str.cpp core/markdown.cpp core/layout.cpp core/edit_assist.cpp
MAC_DEPS := frontends/macos/mac_render.h core/layout.h core/markdown.h core/str.h
MAC_FRAMEWORKS := -framework Cocoa -framework CoreGraphics -framework CoreText -framework ImageIO
MAC_BIN  := build/fmdv-macos

.PHONY: cli macos check clean
cli: $(CLI_BIN)

$(CLI_BIN): $(CLI_SRCS) $(CLI_DEPS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFS) $(CLI_SRCS) -o $(CLI_BIN)

macos: $(MAC_BIN)

$(MAC_BIN): $(MAC_SRCS) $(MAC_DEPS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ObjC++ $(INCLUDES) $(MAC_SRCS) $(MAC_FRAMEWORKS) -o $(MAC_BIN)

# --- macOS .app bundle (build/FMDV.app) with generated icon ---
ICON_FRAMEWORKS := -framework CoreGraphics -framework CoreText -framework ImageIO -framework CoreFoundation
APP := build/FMDV.app

app: $(APP)

build/AppIcon.icns: frontends/macos/appicon/make_icon.mm
	@mkdir -p build/AppIcon.iconset
	$(CXX) -std=c++17 -ObjC++ -w frontends/macos/appicon/make_icon.mm $(ICON_FRAMEWORKS) -o build/make_icon
	./build/make_icon build/icon-1024.png
	@for s in 16 32 128 256 512; do \
	  d=$$((s*2)); \
	  sips -z $$s $$s   build/icon-1024.png --out build/AppIcon.iconset/icon_$${s}x$${s}.png    >/dev/null; \
	  sips -z $$d $$d   build/icon-1024.png --out build/AppIcon.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	iconutil -c icns build/AppIcon.iconset -o build/AppIcon.icns

$(APP): $(MAC_BIN) build/AppIcon.icns frontends/macos/Info.plist
	rm -rf $(APP)
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp $(MAC_BIN) $(APP)/Contents/MacOS/FMDV
	cp build/AppIcon.icns $(APP)/Contents/Resources/AppIcon.icns
	cp frontends/macos/Info.plist $(APP)/Contents/Info.plist
	printf 'APPL????' > $(APP)/Contents/PkgInfo
	@echo "built $(APP)"

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
	@echo "== releases/vercmp =="
	@test "`$(CLI_BIN) vercmp v1.10.0 v1.9.9`" = "1" || { echo "vercmp FAILED"; exit 1; }
	@test "`$(CLI_BIN) vercmp 1.0 1.0.0`" = "0" || { echo "vercmp eq FAILED"; exit 1; }
	@echo "releases/vercmp OK"

clean:
	rm -rf build
