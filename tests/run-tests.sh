#!/bin/bash
# FMDV macOS live-UI test suite — the AppKit analog of cpp/tests/run-tests.ps1.
#
# Drives the real app through its --test-drive stdin channel (see app.mm):
# every keystroke is a real NSEvent routed the way user input is, and every
# command is answered synchronously on stdout, so no sleep-and-pray. Needs no
# Accessibility/TCC permission, so it runs on hosted CI (window server session
# required — any logged-in macOS, including GitHub macos-* runners).
#
# Usage:  tests/run-tests.sh          (from the repo root)
set -u
cd "$(dirname "$0")/.."
BIN=build/fmdv-macos
CLI=build/fmdv-cli
FIX=$(mktemp -d "${TMPDIR:-/tmp}/fmdv-uitests.XXXXXX")
PORT=8799
PASS=0; FAIL=0
APP_PID=""

check() { # name, condition (0/1), detail
    if [ "$2" = 1 ]; then printf '  PASS  %s\n' "$1"; PASS=$((PASS+1));
    else printf '  FAIL  %s  %s\n' "$1" "${3:-}"; FAIL=$((FAIL+1)); fi
}
eq() { check "$1" "$([ "$2" = "$3" ] && echo 1 || echo 0)" "got '$2' want '$3'"; }
has() { case "$2" in *"$3"*) check "$1" 1;; *) check "$1" 0 "got '$2'";; esac; }

# ---- app session over two FIFOs: fd3 = commands in, fd4 = replies out ----
start_app() { # file [extra argv...]
    rm -f "$FIX/in" "$FIX/out"; mkfifo "$FIX/in" "$FIX/out"
    "$BIN" "$@" --test-drive < "$FIX/in" > "$FIX/out" 2>/dev/null &
    APP_PID=$!
    exec 3>"$FIX/in" 4<"$FIX/out"
}
cmd() { # send one command, reply lands in $R (10s timeout = harness bug)
    R=""
    echo "$1" >&3
    IFS= read -r -t 10 R <&4 || R="err timeout"
    R=${R#ok }; [ "$R" = ok ] && R=""
}
poll() { # poll "query X" until value equals $2 (or 4s); result in $R
    for _ in $(seq 1 40); do cmd "$1"; [ "$R" = "$2" ] && return 0; sleep 0.1; done
    return 1
}
stop_app() {
    cmd "quit" 2>/dev/null
    exec 3>&- 4<&-
    wait "$APP_PID" 2>/dev/null
    APP_PID=""
}
alive() { kill -0 "$APP_PID" 2>/dev/null && echo 1 || echo 0; }
frag_with() { # substring -> index of the first selectable fragment containing it (-1 if none)
    local n i; cmd "query fragcount"; n=$R
    for ((i=0; i<n; i++)); do cmd "query fragtext $i"; case "$R" in *"$1"*) printf '%s' "$i"; return;; esac; done
    printf -- -1
}

cleanup() {
    [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    [ -n "${CLIP:-}" ] && printf '%s' "$CLIP" | pbcopy   # restore the user's clipboard
    rm -rf "$FIX"
}
trap cleanup EXIT

# ---- build ----
echo "Building..."
make macos >/dev/null || { echo "BUILD FAILED"; exit 1; }
make cli   >/dev/null || { echo "CLI BUILD FAILED"; exit 1; }

# ---- fixture (same document as the Windows suite) ----
cat > "$FIX/basic.md" <<'EOF'
# Heading One

Some **bold** and *italic* and `code` text.

## Heading Two

- First bullet
- Second bullet

1. First ordered
2. Second ordered

- [x] Task done
- [ ] Task todo

```python
def greet():
    print("hi there")
```

| ColA | ColB |
|------|------|
| Cell A1 | Cell B1 |

> A blockquote line.

Visit [Example Link](https://example.com).

---
EOF

# All window launches point the updater at localhost so no test ever touches
# the live GitHub API; sessions that don't test the updater get a dead port.
export FMDV_RELEASES_URL="http://127.0.0.1:1/nope"

echo
echo "Parser:"
PD=$("$CLI" parse "$FIX/basic.md")
has "parse: H1"             "$PD" "Heading level=1"
has "parse: H2"             "$PD" "Heading level=2"
has "parse: bold run"       "$PD" "run [b] \"bold\""
has "parse: italic run"     "$PD" "run [i] \"italic\""
has "parse: inline code"    "$PD" "run [c] \"code\""
has "parse: bullet list"    "$PD" "ListItem bullet"
has "parse: ordered list"   "$PD" "ListItem ordered"
has "parse: task checked"   "$PD" "task=1"
has "parse: task unchecked" "$PD" "task=0"
has "parse: code lang"      "$PD" "CodeBlock lang=python"
has "parse: table"          "$PD" "Table"
has "parse: blockquote"     "$PD" "BlockQuote"
has "parse: hrule"          "$PD" "HRule"
has "parse: link"           "$PD" "link=https://example.com"

echo
echo "Rendering (headless dumps):"
"$BIN" --dump "$FIX/basic.md" "$FIX/light.png" --width 800 >/dev/null
check "render light png" "$([ -f "$FIX/light.png" ] && [ "$(stat -f%z "$FIX/light.png")" -gt 3000 ] && echo 1 || echo 0)"
"$BIN" --dump "$FIX/basic.md" "$FIX/dark.png" --width 800 --dark >/dev/null
check "render dark png" "$([ -f "$FIX/dark.png" ] && [ "$(stat -f%z "$FIX/dark.png")" -gt 3000 ] && echo 1 || echo 0)"
cat > "$FIX/tblwrap.md" <<'EOF'
| ID | Description | Notes |
| --- | --- | --- |
| 1 | A long description cell that should force this column wider than the others and wrap in a narrow pane | short |
| 22 | Short | Also short |
EOF
"$BIN" --dump "$FIX/tblwrap.md" "$FIX/tblwide.png" --width 900 >/dev/null
check "render table (fits, no wrap)" "$([ "$(stat -f%z "$FIX/tblwide.png")" -gt 2000 ] && echo 1 || echo 0)"
"$BIN" --dump "$FIX/tblwrap.md" "$FIX/tblnarrow.png" --width 420 >/dev/null
check "render table (shrink + wrap)" "$([ "$(stat -f%z "$FIX/tblnarrow.png")" -gt 2000 ] && echo 1 || echo 0)"

echo
echo "Launch / stability:"
start_app "$FIX/basic.md"
cmd "query title";    eq "window title is the file name" "$R" "basic.md"
check "window stays open" "$(alive)"
cmd "query headings"; eq "document parsed (2 headings)" "$R" "2"
cmd "capture $FIX/live.png"
check "live window capture" "$([ -f "$FIX/live.png" ] && [ "$(stat -f%z "$FIX/live.png")" -gt 3000 ] && echo 1 || echo 0)"

echo
echo "Dark mode + zoom (live):"
cmd "query dark"; DARK0=$R
cmd "key cmd+d"; cmd "query dark"
check "Cmd+D toggles dark" "$([ "$R" != "$DARK0" ] && echo 1 || echo 0)"
cmd "key cmd+d"   # restore
cmd "key cmd+="; cmd "key cmd+0"
check "zoom keys: window stable" "$(alive)"

echo
echo "Table of contents sidebar:"
cmd "query toc"; eq "TOC hidden initially" "$R" "0"
cmd "key cmd+shift+o"; cmd "query toc"; eq "Cmd+Shift+O shows TOC" "$R" "1"
cmd "key cmd+e"; cmd "query editor"; eq "TOC + editor (3-pane)" "$R" "1"
check "3-pane: window stable" "$(alive)"
cmd "key cmd+e"; cmd "key cmd+shift+o"; cmd "query toc"; eq "TOC toggles back off" "$R" "0"

echo
echo "Find in doc (Cmd+F):"
cmd "key cmd+f"; cmd "query findbar"; eq "find bar opens" "$R" "1"
cmd "type heading"; cmd "query findlabel"; eq "matches counted" "$R" "1/2"
cmd "key enter"; cmd "query findlabel"; eq "Enter steps to next" "$R" "2/2"
cmd "key enter"; cmd "query findlabel"; eq "stepping wraps around" "$R" "1/2"
cmd "find-step -1"; cmd "query findlabel"; eq "step back (Shift+Enter path)" "$R" "2/2"
cmd "key esc"; cmd "query findbar"; eq "Esc closes the find bar" "$R" "0"
check "window survives find" "$(alive)"

echo
echo "Selection + copy (all block types):"
CLIP=$(pbpaste 2>/dev/null || true)   # restored on exit
cmd "key cmd+a"; cmd "key cmd+c"
GOT=$(pbpaste)
has "copy: heading"    "$GOT" "Heading One"
has "copy: bold word"  "$GOT" "bold"
has "copy: list item"  "$GOT" "First bullet"
has "copy: code text"  "$GOT" "print("
has "copy: table cells" "$GOT" "Cell A1"
has "copy: link text"  "$GOT" "Example Link"
check "copy: multi-line" "$([ "$(printf '%s' "$GOT" | wc -l | tr -d ' ')" -ge 8 ] && echo 1 || echo 0)"
stop_app

echo
echo "Mouse selection (click / double / triple / drag / quoted phrase):"
cat > "$FIX/sel.md" <<'EOF'
# Sel

alpha beta gamma

He said "hello world" now
EOF
start_app "$FIX/sel.md"
cmd "resize 900 700"                     # keep the whole fixture on-screen for hit points
PARA=$(frag_with "alpha beta gamma")     # a plain paragraph fragment
QUOTE=$(frag_with "hello world")         # the fragment with a quoted phrase
check "located paragraph fragment" "$([ "$PARA" -ge 0 ] && echo 1 || echo 0)" "PARA=$PARA"
check "located quoted fragment"    "$([ "$QUOTE" -ge 0 ] && echo 1 || echo 0)" "QUOTE=$QUOTE"
# double-click selects the word under the cursor (center of the line = "beta")
cmd "dblclick-frag $PARA"; cmd "query selection"; eq "double-click selects a word" "$R" "beta"
cmd "query selrects"; check "double-click draws a highlight" "$([ "${R:-0}" -ge 1 ] && echo 1 || echo 0)" "selrects=$R"
# triple-click selects the whole line
cmd "tripleclick-frag $PARA"; cmd "query selection"; eq "triple-click selects the line" "$R" "alpha beta gamma"
# double-click inside quotes selects the quoted phrase (marks excluded)
cmd "dblclick-frag $QUOTE"; cmd "query selection"; eq "double-click selects quoted phrase" "$R" "hello world"
# Cmd+C copies the live selection to the pasteboard (Edit-menu -> copy: routing)
cmd "key cmd+c"; cmd "query clipboard"; eq "Cmd+C copies the selection" "$R" "hello world"
# drag from one paragraph to the next spans both lines
cmd "drag-frag $PARA $QUOTE"; cmd "query selection"
has "drag selects across lines (start)" "$R" "alpha beta gamma"
has "drag selects across lines (end)"   "$R" "now"
# a single click clears the selection (and its highlight)
cmd "click-frag $PARA"; cmd "query selection"; eq "single click clears selection" "$R" ""
cmd "query selrects"; eq "cleared selection draws no highlight" "$R" "0"
check "window survives mouse selection" "$(alive)"
stop_app

echo
echo "Reflow on resize (WM_SIZE analog):"
cat > "$FIX/reflow.md" <<'EOF'
# Reflow

This is a fairly long paragraph of body text that wraps to a different number of
lines depending on the available content width, so a narrower window produces a
taller laid-out document than a wide one — no maximum content width, matching the
Windows frontend which reflows on every WM_SIZE.

A second paragraph so the wide/narrow height difference is unambiguous and easy to
assert on from the harness driving this window over its stdin channel.
EOF
start_app "$FIX/reflow.md"
cmd "resize 1000 700"; cmd "query laidout"; WIDE=$R
eq "layout tracks the new width (wide)" "${WIDE%% *}" "1000"
WIDE_H=${WIDE#* }
cmd "resize 480 700"; cmd "query laidout"; NARROW=$R
eq "layout tracks the new width (narrow)" "${NARROW%% *}" "480"
NARROW_H=${NARROW#* }
check "narrower width reflows taller" "$([ "$NARROW_H" -gt "$WIDE_H" ] && echo 1 || echo 0)" "wide=$WIDE_H narrow=$NARROW_H"
cmd "resize 1000 700"; cmd "query laidout"
eq "widening reflows back to the wide height" "$R" "$WIDE"
stop_app

echo
echo "Save round-trip:"
printf '# Original\n' > "$FIX/save.md"
start_app "$FIX/save.md"
cmd "key cmd+e"
cmd "key cmd+a"; cmd "type # Saved By Test\nbody"   # replace-all, then type
cmd "key cmd+s"
SAVED=$(cat "$FIX/save.md")
has "save wrote new text" "$SAVED" "Saved By Test"
check "save normalized LF" "$([ "$(grep -c $'\r' "$FIX/save.md")" = 0 ] && echo 1 || echo 0)"
stop_app

echo
echo "Save failure handling (editor stays open, edits intact):"
# `save-close` invokes saveAndClose: directly: a synthetic Cmd+Shift+S can't be
# distinguished from Cmd+S (both menu items use keyEquivalent "s"), so routing it
# through NSEvents would exercise plain Save instead. (Same reason the find suite
# uses `find-step` for Shift+Enter.)
mkdir -p "$FIX/rodir"
printf '# Keep Me\n' > "$FIX/rodir/ro.md"
start_app "$FIX/rodir/ro.md"
cmd "key cmd+e"; cmd "type # edit"
chmod 0500 "$FIX/rodir"                 # dir unwritable → temp-file create fails
cmd "save-close"                        # save & close attempt
cmd "query editor"; eq "editor stays open when save fails" "$R" "1"
chmod 0700 "$FIX/rodir"                 # restore for read + cleanup
eq "file unchanged after failed save" "$(cat "$FIX/rodir/ro.md")" "# Keep Me"
stop_app
# the success path of save & close writes the file and closes the editor
printf '# sc\n' > "$FIX/sc.md"
start_app "$FIX/sc.md"
cmd "key cmd+e"; cmd "key cmd+a"; cmd "type # Closed And Saved"
cmd "query editor"; eq "editor open before save & close" "$R" "1"
cmd "save-close"
cmd "query editor"; eq "save & close closes the editor on success" "$R" "0"
has "save & close wrote the edit" "$(cat "$FIX/sc.md")" "Closed And Saved"
stop_app

echo
echo "Markdown autocomplete (ghost text):"
ac() { # ac <name> <typed> <press-tab 0|1> <want-file-content>
    : > "$FIX/ac.md"
    start_app "$FIX/ac.md"
    cmd "key cmd+e"
    cmd "type $2"
    [ "$3" = 1 ] && cmd "key tab"
    cmd "key cmd+s"
    stop_app
    eq "$1" "$(cat "$FIX/ac.md")" "$4"
}
# ghost visible but not in the buffer until Tab
: > "$FIX/ac.md"
start_app "$FIX/ac.md"
cmd "key cmd+e"; cmd 'type **'
cmd "query ghost"; eq "ghost suggested for **" "$R" "**"
cmd "query editor-text"; eq "ghost not in the buffer" "$R" "**"
cmd "key cmd+s"; stop_app
eq "ghost not committed without Tab" "$(cat "$FIX/ac.md")" "**"
ac "Tab commits ** closer"     '**' 1 '****'
ac "Tab commits [ -> []()"     '[' 1 '[]()'
ac "Tab commits backtick"      '`' 1 '``'
ac "no closer mid-word; Tab -> tab char" 'hi' 1 "$(printf 'hi\t')"
ac "( -> ()"                   '(' 1 '()'
ac "* -> ** (italic)"          '*' 1 '**'
ac "double backtick"           '``' 1 '````'
ac "- [ -> checkbox"           '- [' 1 '- [ ] '   # trailing space: caret lands after the marker
ac "x[ -> link []()"           'x[' 1 'x[]()'
: > "$FIX/ac.md"   # fence: 3-line block (opening fence, blank code line, closing fence)
start_app "$FIX/ac.md"
cmd "key cmd+e"; cmd 'type ```'; cmd "key tab"; cmd "key cmd+s"; stop_app
FLINES=$(wc -l < "$FIX/ac.md" | tr -d ' ')
FFIRST=$(head -1 "$FIX/ac.md")
FOK=0; [ "$FLINES" -ge 2 ] && [ "$FFIRST" = '```' ] && FOK=1
check "fence: Tab commits a 3-line block" "$FOK" "lines=$FLINES first=$FFIRST"

echo
echo "List continuation + Tab:"
seqtest() { # name, typed (with \n), want
    : > "$FIX/seq.md"
    start_app "$FIX/seq.md"
    cmd "key cmd+e"; cmd "type $2"; cmd "key cmd+s"; stop_app
    eq "$1" "$(cat "$FIX/seq.md")" "$3"
}
seqtest "bullet continuation"  '- a\nb' '- a
- b'
seqtest "ordered continuation" '1. a\nb' '1. a
2. b'
seqtest "empty item ends list" '- a\n\n' '- a'
: > "$FIX/seq.md"
start_app "$FIX/seq.md"
cmd "key cmd+e"; cmd "type x"; cmd "key tab"; cmd "type y"; cmd "key cmd+s"; stop_app
eq "Tab inserts tab (no ghost)" "$(cat "$FIX/seq.md")" "$(printf 'x\ty')"

echo
echo "Table insert (Cmd+T):"
: > "$FIX/tbl.md"
start_app "$FIX/tbl.md"
cmd "key cmd+e"; cmd "key cmd+t"; cmd "key cmd+s"; stop_app
TBL=$(cat "$FIX/tbl.md")
has "table has 3 columns"  "$TBL" "| Column 1 | Column 2 | Column 3 |"
has "table has separator"  "$TBL" "| --- | --- | --- |"
check "table has body rows" "$([ "$(grep -c '^|   |' "$FIX/tbl.md")" -ge 3 ] && echo 1 || echo 0)"

echo
echo "Live reload (external edit):"
printf '# One\n' > "$FIX/reload.md"
start_app "$FIX/reload.md"
cmd "query headings"; eq "one heading before edit" "$R" "1"
printf '# One\n\n## Two\n' > "$FIX/reload.md"
poll "query headings" "2"
eq "external edit reparsed into preview" "$R" "2"
stop_app

echo
echo "Updater (local fixture server):"
mkdir -p "$FIX/www" "$FIX/install"
make dist >/dev/null 2>&1 || { echo "make dist FAILED"; exit 1; }
ditto -x -k build/FMDV-macos.zip "$FIX/install"
cp -R build/FMDV.app "$FIX/www/FMDV.app"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString 9.9.9" "$FIX/www/FMDV.app/Contents/Info.plist"
codesign --force --sign - "$FIX/www/FMDV.app" 2>/dev/null
ditto -c -k --keepParent "$FIX/www/FMDV.app" "$FIX/www/FMDV-macos.zip"
rm -rf "$FIX/www/FMDV.app"
printf '[{"tag_name":"v9.9.9","assets":[{"name":"FMDV-macos.zip","browser_download_url":"http://127.0.0.1:%s/FMDV-macos.zip"}]},{"tag_name":"v1.1.0","assets":[]}]' "$PORT" > "$FIX/www/releases.json"
# exec so $! is python itself, not a wrapper subshell (killing the subshell
# would orphan the server and leave the port squatted for the next run)
(cd "$FIX/www" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
SRV_PID=$!
disown "$SRV_PID" 2>/dev/null   # no "Terminated" job noise when we kill it
for _ in $(seq 1 30); do curl -sf "http://127.0.0.1:$PORT/releases.json" >/dev/null && break; sleep 0.1; done

# picker UI on the raw binary (no bundle → install degrades, so don't press Enter)
FMDV_RELEASES_URL="http://127.0.0.1:$PORT/releases.json" FMDV_VERSION_OVERRIDE=0.0.1 \
    start_app "$FIX/basic.md"
cmd "key cmd+u"; cmd "query picker"; eq "Cmd+U opens the picker" "$R" "1"
poll "query releases" "2"
eq "release list arrives" "$R" "2"
poll "query banner" "FMDV v9.9.9 is available"
has "notify banner appears (newer release)" "$R" "available"
# banner tracks the bottom edge on resize (its origin.y stays scrollHeight-44,
# vs. Windows repainting the strip from the live client size each WM_PAINT)
banner_pinned() { cmd "query bannerpos"; set -- $R; [ "$1" = "$(( $2 - 44 ))" ] && echo 1 || echo 0; }
cmd "resize 900 640"; check "banner pinned to bottom (tall)"  "$(banner_pinned)" "got '$R'"
cmd "resize 700 900"; check "banner tracks a taller resize"    "$(banner_pinned)" "got '$R'"
cmd "resize 800 500"; check "banner tracks a shorter resize"   "$(banner_pinned)" "got '$R'"
cmd "key esc"; cmd "query picker"; eq "Esc closes the picker" "$R" "0"
stop_app

# full install E2E on a staged bundle: auto mode downloads + swaps on launch
INST_BIN="$FIX/install/FMDV.app/Contents/MacOS/FMDV"
rm -f "$FIX/in" "$FIX/out"; mkfifo "$FIX/in" "$FIX/out"
FMDV_RELEASES_URL="http://127.0.0.1:$PORT/releases.json" \
    "$INST_BIN" "$FIX/basic.md" --test-drive -FMDVUpdateMode auto < "$FIX/in" > "$FIX/out" 2>/dev/null &
APP_PID=$!
exec 3>"$FIX/in" 4<"$FIX/out"
poll "query installing" "1" >/dev/null   # install kicked off...
poll "query installing" "0" >/dev/null   # ...and finished
cmd "query banner"
has "install completion banner" "$R" "installed"
stop_app
V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$FIX/install/FMDV.app/Contents/Info.plist")
eq "bundle swapped to the new version" "$V" "9.9.9"
check "previous version kept as .old" "$([ -d "$FIX/install/FMDV.app.old" ] && echo 1 || echo 0)"
"$INST_BIN" --dump "$FIX/basic.md" "$FIX/postswap.png" --width 600 >/dev/null
check "swapped app still renders" "$([ "$(stat -f%z "$FIX/postswap.png")" -gt 3000 ] && echo 1 || echo 0)"
# next launch sweeps the .old backup
rm -f "$FIX/in" "$FIX/out"; mkfifo "$FIX/in" "$FIX/out"
"$INST_BIN" "$FIX/basic.md" --test-drive -FMDVUpdateNotify 0 < "$FIX/in" > "$FIX/out" 2>/dev/null &
APP_PID=$!
exec 3>"$FIX/in" 4<"$FIX/out"
for _ in $(seq 1 40); do [ ! -d "$FIX/install/FMDV.app.old" ] && break; sleep 0.1; done
check ".old swept on next launch" "$([ ! -d "$FIX/install/FMDV.app.old" ] && echo 1 || echo 0)"
stop_app
kill "$SRV_PID" 2>/dev/null; SRV_PID=""

# ---- summary ----
echo
echo "========================================"
echo "  $PASS passed, $FAIL failed"
echo "========================================"
exit $((FAIL > 0 ? 1 : 0))
