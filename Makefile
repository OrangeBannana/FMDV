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
#   make app     assemble + codesign build/FMDV.app                 [macOS only]
#   make dist    zip the .app into build/FMDV-macos.zip (updater)   [macOS only]
#   make dmg     drag-to-Applications build/FMDV-macos.dmg (release)[macOS only]
#   make notarize  notarize + staple the zip & dmg (Developer ID)   [macOS only]
#   make uitest  live-UI suite driving the real app (tests/run-tests.sh)
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
COMMIT   := $(shell git rev-parse --short HEAD 2>/dev/null)
INCLUDES := -Icore
DEFS     := -DFMDV_COMMIT=\"$(COMMIT)\" -DFMDV_BUILD=\"release\"

CLI_SRCS := frontends/cli/fmdv_cli.cpp core/str.cpp core/markdown.cpp core/edit_assist.cpp core/release_info.cpp core/layout.cpp core/text_select.cpp
CLI_DEPS := core/str.h core/markdown.h core/edit_assist.h core/release_info.h core/layout.h core/bench_log.h
CLI_BIN  := build/fmdv-cli

# macOS frontend (headless renderer for now): CoreText/CoreGraphics via .mm.
MAC_SRCS := frontends/macos/main.mm frontends/macos/app.mm frontends/macos/mac_render.mm core/str.cpp core/markdown.cpp core/layout.cpp core/edit_assist.cpp core/release_info.cpp core/text_select.cpp
MAC_DEPS := frontends/macos/mac_render.h core/layout.h core/markdown.h core/str.h
MAC_FRAMEWORKS := -framework Cocoa -framework CoreGraphics -framework CoreText -framework ImageIO
MAC_BIN  := build/fmdv-macos

.PHONY: cli macos app dist dmg notarize test uitest check clean
cli: $(CLI_BIN)

$(CLI_BIN): $(CLI_SRCS) $(CLI_DEPS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFS) $(CLI_SRCS) -o $(CLI_BIN)

macos: $(MAC_BIN)

$(MAC_BIN): $(MAC_SRCS) $(MAC_DEPS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ObjC++ $(INCLUDES) $(MAC_SRCS) $(MAC_FRAMEWORKS) -o $(MAC_BIN)

# --- macOS .app bundle (build/FMDV.app) ---
# The icon is the shared FMDV brand icon (cpp/fmdv.ico, same as the Windows app),
# converted to an .icns.
APP := build/FMDV.app

app: $(APP)

build/AppIcon.icns: cpp/fmdv.ico
	@mkdir -p build/AppIcon.iconset
	sips -s format png cpp/fmdv.ico --out build/icon-src.png >/dev/null
	@for s in 16 32 128 256 512; do \
	  d=$$((s*2)); \
	  sips -z $$s $$s build/icon-src.png --out build/AppIcon.iconset/icon_$${s}x$${s}.png    >/dev/null; \
	  sips -z $$d $$d build/icon-src.png --out build/AppIcon.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	iconutil -c icns build/AppIcon.iconset -o build/AppIcon.icns

# App version comes from cpp/version.h (single source of truth); the bundled
# Info.plist is patched to match so the in-app updater's version compare and
# the plist never drift.
VERSION := $(shell awk -F'"' '/define FMDV_VERSION_STR/{print $$2}' cpp/version.h)

# Code signing: set FMDV_SIGN_ID to a "Developer ID Application: ..." identity
# for a distributable build (hardened runtime + timestamp, ready for
# notarization via scripts/notarize.sh). Unset it and the bundle is ad-hoc
# signed, which runs locally and supports the in-app updater's signature check
# but will trip Gatekeeper on other machines.
FMDV_SIGN_ID ?=

$(APP): $(MAC_BIN) build/AppIcon.icns frontends/macos/Info.plist cpp/version.h
	rm -rf $(APP)
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp $(MAC_BIN) $(APP)/Contents/MacOS/FMDV
	cp build/AppIcon.icns $(APP)/Contents/Resources/AppIcon.icns
	cp frontends/macos/Info.plist $(APP)/Contents/Info.plist
	/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $(VERSION)" $(APP)/Contents/Info.plist
	printf 'APPL????' > $(APP)/Contents/PkgInfo
	@if [ -n "$(FMDV_SIGN_ID)" ]; then \
	  codesign --force --sign "$(FMDV_SIGN_ID)" --options runtime --timestamp $(APP); \
	else \
	  codesign --force --sign - $(APP); \
	fi
	codesign --verify $(APP)
	@echo "built $(APP) (version $(VERSION), signed: $(if $(FMDV_SIGN_ID),$(FMDV_SIGN_ID),ad-hoc))"

# Release artifact: the zip GitHub releases carry and the in-app updater
# downloads. ditto preserves the bundle structure, resource forks, and the
# code signature (plain `zip` can break signed bundles).
DIST := build/FMDV-macos.zip

dist: $(APP)
	rm -f $(DIST)
	ditto -c -k --keepParent $(APP) $(DIST)
	@echo "built $(DIST)"

# Human-facing installer: a drag-to-Applications disk image for the Releases
# page (the zip above stays the in-app updater's payload). Stages the signed
# .app next to an /Applications symlink so the mounted window offers the
# conventional drag target, then packs a compressed read-only image. No custom
# background/icon layout — robust and dependency-free (hdiutil only). Inherits
# the .app's signature; `make notarize` produces the notarized release .dmg.
DMG := build/FMDV-macos.dmg

dmg: $(APP)
	rm -rf build/dmg-stage $(DMG)
	mkdir -p build/dmg-stage
	cp -R $(APP) build/dmg-stage/
	ln -s /Applications build/dmg-stage/Applications
	hdiutil create -volname "FMDV $(VERSION)" -srcfolder build/dmg-stage \
		-fs HFS+ -format UDZO -ov "$(DMG)" >/dev/null
	rm -rf build/dmg-stage
	@echo "built $(DMG) (version $(VERSION))"

# Notarize + staple both release artifacts (zip + dmg) via scripts/notarize.sh.
# Run after a Developer ID build: `make dist FMDV_SIGN_ID="Developer ID
# Application: ..."`. Needs a Developer ID cert and stored notarytool
# credentials — see the script header. Pass FMDV_NOTARY_PROFILE to override the
# keychain profile (default: fmdv-notary).
FMDV_NOTARY_PROFILE ?= fmdv-notary

notarize:
	scripts/notarize.sh "$(FMDV_NOTARY_PROFILE)"

# --- unit tests: one binary per core module (tests/<name>_test.cpp) ---
TEST_NAMES := str markdown edit_assist release_info layout text_select bench_log
TEST_BINS  := $(TEST_NAMES:%=build/%-test)
TEST_CORE  := core/str.cpp core/markdown.cpp core/edit_assist.cpp core/release_info.cpp core/layout.cpp core/text_select.cpp
TEST_HDRS  := tests/test_check.h core/str.h core/markdown.h core/edit_assist.h core/release_info.h core/layout.h core/text_select.h core/bench_log.h

build/%-test: tests/%_test.cpp $(TEST_CORE) $(TEST_HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(TEST_CORE) -o $@

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
	  echo "== $$t =="; ./$$t || fail=1; \
	done; exit $$fail

uitest:
	./tests/run-tests.sh

check: cli test
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
