# FMDV test suite. Builds release+debug, then runs functional checks.
# Usage:  powershell -ExecutionPolicy Bypass -File tests\run-tests.ps1
$ErrorActionPreference = "Stop"
$cpp = Split-Path $PSScriptRoot -Parent
# Toolchain is resolved by build.ps1 (PATH, FMDV_MINGW env var, or -MinGW param).

$exe = "$cpp\fmdv.exe"
$dbg = "$cpp\fmdv_dbg.exe"

# ---- Win32 helpers for driving the live window ----
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class T {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr SendInt(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, string cls, string win);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
}
public struct RECT { public int Left, Top, Right, Bottom; }
"@
$WM_COMMAND=0x0111; $WM_SETTEXT=0x000C; $WM_CHAR=0x0102; $WM_KEYDOWN=0x0100; $VK_TAB=0x09
$WM_VSCROLL=0x0115; $SB_PAGEDOWN=3
$WM_LBUTTONDOWN=0x0201; $WM_MOUSEMOVE=0x0200; $WM_LBUTTONUP=0x0202
$ID_EDIT=2001; $ID_SAVE=2003; $ID_COPY=2008; $ID_SELALL=2009

function MakeLParam($x, $y) { return [IntPtr]((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF)) }

$script:pass = 0; $script:fail = 0
function Check($name, $cond, $detail="") {
    if ($cond) { Write-Host "  PASS  $name" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "  FAIL  $name  $detail" -ForegroundColor Red;  $script:fail++ }
}
# FMDV_TEST_OFFSCREEN=1 (see run-tests-hidden.ps1) makes fmdv.exe create its
# window off the visible screen from the first frame -- no on-screen flash to
# relocate after the fact. Popups (table picker, find bar, update picker) all
# position themselves via GetWindowRect(mainHwnd)/GetCaretPos, so they follow
# the main window off-screen automatically.
function Launch($file) {
    $p = Start-Process -FilePath $exe -ArgumentList "`"$file`"" -PassThru
    for ($i=0; $i -lt 15 -and $p.MainWindowHandle -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 150; $p.Refresh() }
    Start-Sleep -Milliseconds 300
    return $p
}

# ---- build ----
Write-Host "Building..." -ForegroundColor Cyan
& "$cpp\build.ps1"        | Out-Null
& "$cpp\build.ps1" -Debug | Out-Null
if (-not (Test-Path $exe)) { Write-Host "RELEASE BUILD FAILED"; exit 1 }
if (-not (Test-Path $dbg)) { Write-Host "DEBUG BUILD FAILED"; exit 1 }

# ---- fixture ----
$fix = "$env:TEMP\fmdv_tests"; New-Item -ItemType Directory -Force $fix | Out-Null
$basic = "$fix\basic.md"
@'
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
'@ | Set-Content $basic -Encoding utf8

# ============ TESTS ============
Write-Host "`nParser:" -ForegroundColor Cyan
$pd = & $dbg $basic --parse-dump 2>&1 | Out-String
Check "parse: H1"          ($pd -match "Heading level=1")
Check "parse: H2"          ($pd -match "Heading level=2")
Check "parse: bold run"    ($pd -match "run \[b\] .bold.")
Check "parse: italic run"  ($pd -match "run \[i\] .italic.")
Check "parse: inline code" ($pd -match "run \[c\] .code.")
Check "parse: bullet list" ($pd -match "ListItem bullet")
Check "parse: ordered list" ($pd -match "ListItem ordered")
Check "parse: task checked" ($pd -match "task=1")
Check "parse: task unchecked" ($pd -match "task=0")
Check "parse: code lang"   ($pd -match "CodeBlock lang=python")
Check "parse: table"       ($pd -match "Table")
Check "parse: blockquote"  ($pd -match "BlockQuote")
Check "parse: hrule"       ($pd -match "HRule")
Check "parse: link"        ($pd -match "link=https://example.com")

Write-Host "`nRendering:" -ForegroundColor Cyan
& $exe $basic --dump "$fix\light.png" --width 800 | Out-Null
Check "render light png"   ((Test-Path "$fix\light.png") -and (Get-Item "$fix\light.png").Length -gt 3000)
& $exe $basic --dump "$fix\dark.png" --width 800 --dark | Out-Null
Check "render dark png"    ((Test-Path "$fix\dark.png") -and (Get-Item "$fix\dark.png").Length -gt 3000)
& $exe $basic --dump "$fix\scroll.png" --width 800 --viewport 400 --scroll 200 | Out-Null
Check "render scrolled viewport" ((Test-Path "$fix\scroll.png") -and (Get-Item "$fix\scroll.png").Length -gt 1500)

# content-aware table columns: mismatched column widths + a cell long enough to
# need wrapping once the pane is narrow. Smoke test (exercises both the
# stretch-to-fill and shrink-and-wrap code paths) rather than pixel-exact.
$tblWrap = "$fix\tblwrap.md"
@'
| ID | Description | Notes |
| --- | --- | --- |
| 1 | A long description cell that should force this column wider than the others and wrap in a narrow pane | short |
| 22 | Short | Also short |
'@ | Set-Content $tblWrap -Encoding utf8
& $exe $tblWrap --dump "$fix\tblwide.png" --width 900 | Out-Null
Check "render table (fits, no wrap)" ((Test-Path "$fix\tblwide.png") -and (Get-Item "$fix\tblwide.png").Length -gt 2000)
& $exe $tblWrap --dump "$fix\tblnarrow.png" --width 420 | Out-Null
Check "render table (shrink + wrap)" ((Test-Path "$fix\tblnarrow.png") -and (Get-Item "$fix\tblnarrow.png").Length -gt 2000)

Write-Host "`nLaunch / stability:" -ForegroundColor Cyan
$p = Launch $basic
Check "window stays open"  (-not $p.HasExited)

Write-Host "`nTable of contents sidebar:" -ForegroundColor Cyan
$ID_TOC = 2012
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_TOC, [IntPtr]::Zero) | Out-Null  # show
Start-Sleep -Milliseconds 200
Check "TOC toggle on: window stable"  (-not $p.HasExited)
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_EDIT, [IntPtr]::Zero) | Out-Null   # + editor (3-pane layout)
Start-Sleep -Milliseconds 300
Check "TOC + editor: window stable"    (-not $p.HasExited)
$edit = [T]::FindWindowExW($p.MainWindowHandle, [IntPtr]::Zero, "Edit", $null)
$rc = New-Object RECT
[T]::GetWindowRect($edit, [ref]$rc) | Out-Null
Check "TOC + editor: editor pane visible" (($rc.Right - $rc.Left) -gt 20)
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_EDIT, [IntPtr]::Zero) | Out-Null   # close editor
Start-Sleep -Milliseconds 200
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_TOC, [IntPtr]::Zero) | Out-Null    # hide
Start-Sleep -Milliseconds 200
Check "TOC toggle off: window stable" (-not $p.HasExited)

Write-Host "`nFind in doc (Ctrl+F):" -ForegroundColor Cyan
$ID_FIND = 2013
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_FIND, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$fb = [T]::FindWindowW("FMDV_FindBar", $null)
Check "find bar opens" ($fb -ne [IntPtr]::Zero)
$fe = [T]::FindWindowExW($fb, [IntPtr]::Zero, "Edit", $null)
Check "find bar has an edit box" ($fe -ne [IntPtr]::Zero)
[T]::SendMessageW($fe, $WM_SETTEXT, [IntPtr]::Zero, "heading") | Out-Null  # matches "Heading One"/"Heading Two"
Start-Sleep -Milliseconds 250
Check "main window stable while searching" (-not $p.HasExited)
[T]::SendInt($fe, $WM_KEYDOWN, [IntPtr]0x0D, [IntPtr]0) | Out-Null  # Enter: next match
Start-Sleep -Milliseconds 200
Check "main window stable after next-match" (-not $p.HasExited)
[T]::SendInt($fe, $WM_KEYDOWN, [IntPtr]0x1B, [IntPtr]0) | Out-Null  # Esc: close
Start-Sleep -Milliseconds 250
Check "Esc closes the find bar" ([T]::FindWindowW("FMDV_FindBar", $null) -eq [IntPtr]::Zero)
Check "main window survives find bar close" (-not $p.HasExited)

Write-Host "`nSelection + copy (all block types):" -ForegroundColor Cyan
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_SELALL, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 150
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_COPY, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 250
$clip = Get-Clipboard -Raw
Check "copy: heading"      ($clip -match "Heading One")
Check "copy: bold word"    ($clip -match "bold")
Check "copy: list item"    ($clip -match "First bullet")
Check "copy: code text"    ($clip -match "print\(")
Check "copy: table cell"   ($clip -match "Cell A1" -and $clip -match "Cell B1")
Check "copy: link text"    ($clip -match "Example Link")
Check "copy: multi-line"   (($clip -split "`n").Count -ge 8)
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`nSelection while scrolled (g_frags is doc-space, clicks are client-space):" -ForegroundColor Cyan
$scrollFile = "$fix\scroll.md"
(1..150 | ForEach-Object { "Line{0:D3} marker unique text here.`n" -f $_ }) -join "`n" | Set-Content $scrollFile -Encoding utf8
$p = Launch $scrollFile
for ($i = 0; $i -lt 6; $i++) {
    [T]::PostMessage($p.MainWindowHandle, $WM_VSCROLL, [IntPtr]$SB_PAGEDOWN, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 80
}
Start-Sleep -Milliseconds 250
[T]::PostMessage($p.MainWindowHandle, $WM_LBUTTONDOWN, [IntPtr]1, (MakeLParam 40 300)) | Out-Null
Start-Sleep -Milliseconds 80
[T]::PostMessage($p.MainWindowHandle, $WM_MOUSEMOVE, [IntPtr]1, (MakeLParam 700 300)) | Out-Null
Start-Sleep -Milliseconds 80
[T]::PostMessage($p.MainWindowHandle, $WM_LBUTTONUP, [IntPtr]0, (MakeLParam 700 300)) | Out-Null
Start-Sleep -Milliseconds 150
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_COPY, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 250
$clip = Get-Clipboard -Raw
# a click at the same screen y before/after scrolling must not select the same
# doc-space text -- if it does, scroll offset isn't being applied to the click
Check "scrolled selection grabs a Line### marker" ($clip -match "Line\d{3} marker unique")
Check "scrolled selection isn't stuck at the top of the doc" ($clip -notmatch "Line00[1-9] marker")
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`nSave round-trip:" -ForegroundColor Cyan
$saveFile = "$fix\save.md"; Set-Content $saveFile "# Original" -Encoding utf8
$p = Launch $saveFile
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_EDIT, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$edit = [T]::FindWindowExW($p.MainWindowHandle, [IntPtr]::Zero, "Edit", $null)
[T]::SendMessageW($edit, $WM_SETTEXT, [IntPtr]::Zero, "# Saved By Test`r`nbody") | Out-Null
Start-Sleep -Milliseconds 200
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_SAVE, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$saved = Get-Content $saveFile -Raw
Check "save wrote new text" ($saved -match "Saved By Test")
Check "save normalized LF"  (-not ($saved -match "`r`n"))
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`nMarkdown autocomplete (ghost text):" -ForegroundColor Cyan
function AcCase($chars, $pressTab) {
    $f = "$fix\ac.md"; Set-Content $f "" -Encoding utf8
    $p = Launch $f
    [T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_EDIT, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300
    $e = [T]::FindWindowExW($p.MainWindowHandle, [IntPtr]::Zero, "Edit", $null)
    [T]::SendMessageW($e, $WM_SETTEXT, [IntPtr]::Zero, "") | Out-Null
    foreach ($c in $chars) { [T]::SendInt($e, $WM_CHAR, [IntPtr]$c, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 35 }
    if ($pressTab) { [T]::SendInt($e, $WM_KEYDOWN, [IntPtr]$VK_TAB, [IntPtr]0) | Out-Null; [T]::SendInt($e, $WM_CHAR, [IntPtr]0x09, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 50 }
    [T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_SAVE, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300
    $got = Get-Content $f -Raw; if ($got -eq $null) { $got = "" }
    if (-not $p.HasExited) { $p.Kill() }
    return $got.TrimEnd("`r","`n")
}
Check "ghost not committed without Tab" ((AcCase @(42,42) $false) -eq "**")
Check "Tab commits ** closer"           ((AcCase @(42,42) $true)  -eq "****")
Check "Tab commits [ -> []()"           ((AcCase @(91)    $true)  -eq "[]()")
Check "Tab commits backtick"            ((AcCase @(96)    $true)  -eq '``')
Check "no closer mid-word; Tab -> tab char" ((AcCase @(104,105) $true) -eq "hi`t")

Write-Host "`nMore autocomplete triggers:" -ForegroundColor Cyan
Check "( -> ()"           ((AcCase @(40)       $true) -eq "()")
Check "* -> ** (italic)"  ((AcCase @(42)       $true) -eq "**")
Check "double backtick"   ((AcCase @(96,96)    $true) -eq '````')
Check "fence ``` (3 lines)" ((($t = AcCase @(96,96,96) $true) -split "`n").Count -eq 3 -and $t.StartsWith('```'))
Check "- [ -> checkbox"   ((AcCase @(45,32,91) $true).TrimEnd() -eq "- [ ]")
Check "x[ -> link []()"   ((AcCase @(120,91)   $true) -eq "x[]()")

Write-Host "`nTable grid-picker:" -ForegroundColor Cyan
$tf = "$fix\tbl.md"; Set-Content $tf "" -Encoding utf8
$p = Launch $tf
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_EDIT, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]2010, [IntPtr]::Zero) | Out-Null  # ID_INSERT_TABLE
Start-Sleep -Milliseconds 350
$tp = [T]::FindWindowW("FMDV_TablePicker", $null)
Check "picker window opened" ($tp -ne [IntPtr]::Zero)
[T]::SendInt($tp, $WM_KEYDOWN, [IntPtr]0x27, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 60  # Right -> 3 cols
[T]::SendInt($tp, $WM_KEYDOWN, [IntPtr]0x0D, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 150 # Enter
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_SAVE, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$tbl = Get-Content $tf -Raw
Check "table has 3 columns"  ($tbl -match "Column 1" -and $tbl -match "Column 3")
Check "table has separator"  ($tbl -match "\| --- \| --- \| --- \|")
Check "table has body rows"  ((($tbl -split "`n") | Where-Object { $_ -match '^\|   \|' }).Count -ge 3)
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`nTable resize (edit existing table):" -ForegroundColor Cyan
$trFile = "$fix\tresize.md"
$trText = "# Doc`r`n`r`n| A | B |`r`n| --- | --- |`r`n| x | **y** |`r`n| z | w |`r`n`r`nafter`r`n"
Set-Content $trFile $trText -Encoding utf8 -NoNewline
$p = Launch $trFile
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_EDIT, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$e = [T]::FindWindowExW($p.MainWindowHandle, [IntPtr]::Zero, "Edit", $null)
$caret = $trText.IndexOf("**y**") + 1  # land inside the table's second column, second row
[T]::SendInt($e, 0x00B1, [IntPtr]$caret, [IntPtr]$caret) | Out-Null  # EM_SETSEL
Start-Sleep -Milliseconds 100
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]2010, [IntPtr]::Zero) | Out-Null  # ID_INSERT_TABLE
Start-Sleep -Milliseconds 400
$tp = [T]::FindWindowW("FMDV_TablePicker", $null)
Check "resize picker opens on caret-in-table" ($tp -ne [IntPtr]::Zero)
# grow 2x2 -> 3x3: one Right, one Down
[T]::SendInt($tp, $WM_KEYDOWN, [IntPtr]0x27, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 80  # Right
[T]::SendInt($tp, $WM_KEYDOWN, [IntPtr]0x28, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 80  # Down
[T]::SendInt($tp, $WM_KEYDOWN, [IntPtr]0x0D, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 250 # Enter
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_SAVE, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$resized = Get-Content $trFile -Raw
Check "resize preserved bold cell text"  ($resized -match '\*\*y\*\*')
Check "resize preserved plain cell text" ($resized -match '\bx\b' -and $resized -match '\bz\b' -and $resized -match '\bw\b')
Check "resize grew header to 3 columns" ($resized -match "Column 3")
$pipeLines = ($resized -split "`n") | Where-Object { $_.TrimStart() -match '^\|' }
Check "resize grew to exactly 3 body rows" ($pipeLines.Count -eq 5)  # header + separator + 3 body rows
Check "resize kept surrounding content"  ($resized -match "# Doc" -and $resized -match "after")
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`nList continuation + Tab:" -ForegroundColor Cyan
# token 'TAB'/'ENTER' send KEYDOWN+CHAR (as TranslateMessage would); ints are WM_CHAR
function Seq($tokens) {
    $f = "$fix\seq.md"; Set-Content $f "" -Encoding utf8
    $p = Launch $f
    [T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_EDIT, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300
    $e = [T]::FindWindowExW($p.MainWindowHandle, [IntPtr]::Zero, "Edit", $null)
    [T]::SendMessageW($e, $WM_SETTEXT, [IntPtr]::Zero, "") | Out-Null
    foreach ($t in $tokens) {
        if ($t -eq 'TAB')   { [T]::SendInt($e,$WM_KEYDOWN,[IntPtr]0x09,[IntPtr]0)|Out-Null; [T]::SendInt($e,$WM_CHAR,[IntPtr]0x09,[IntPtr]0)|Out-Null }
        elseif ($t -eq 'ENTER') { [T]::SendInt($e,$WM_KEYDOWN,[IntPtr]0x0D,[IntPtr]0)|Out-Null; [T]::SendInt($e,$WM_CHAR,[IntPtr]0x0D,[IntPtr]0)|Out-Null }
        else { [T]::SendInt($e,$WM_CHAR,[IntPtr]$t,[IntPtr]0)|Out-Null }
        Start-Sleep -Milliseconds 30
    }
    [T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_SAVE, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 250
    $g = Get-Content $f -Raw; if ($g -eq $null) { $g = "" }
    if (-not $p.HasExited) { $p.Kill() }
    return $g.TrimEnd("`r","`n")
}
Check "bullet continuation"    ((Seq @(45,32,97,'ENTER',98)) -eq "- a`n- b")
Check "ordered continuation"   ((Seq @(49,46,32,97,'ENTER',98)) -eq "1. a`n2. b")
Check "empty item ends list"   ((Seq @(45,32,97,'ENTER','ENTER')) -eq "- a")
Check "Tab inserts tab (no ghost)" ((Seq @(120,'TAB',121)) -eq "x`ty")
Check "Tab commits ghost (no tab char)" ((Seq @(42,42,'TAB')) -eq "****")

Write-Host "`nUpdater:" -ForegroundColor Cyan
$ut = & $dbg --test-updater 2>&1 | Out-String
Check "updater unit checks"    ($LASTEXITCODE -eq 0 -and $ut -match "0 failures")
$ver = (& $dbg --version | Out-String).Trim()
Check "version flag"           ($ver -match '^\d+\.\d+\.\d+$')

$p = Launch $basic
[T]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]2011, [IntPtr]::Zero) | Out-Null  # ID_UPDATES
Start-Sleep -Milliseconds 1500  # let the release fetch land so v1.0.0 is in the list
$up = [T]::FindWindowW("FMDV_UpdatePicker", $null)
Check "update picker opens"    ($up -ne [IntPtr]::Zero)
if ($up -ne [IntPtr]::Zero) {
    # The picker defaults to selecting whichever row matches the running
    # version, which isn't necessarily the oldest release. Force selection to
    # the last (oldest) row — the releases API returns newest-first, so this
    # is always a release that predates the updater, regardless of how many
    # newer releases exist by the time this runs.
    for ($i = 0; $i -lt 10; $i++) { [T]::SendInt($up, $WM_KEYDOWN, [IntPtr]0x28, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 25 }
    # first Enter on that row must only arm a confirmation, not install (a
    # real install would replace the exe this suite is running).
    [T]::SendInt($up, $WM_KEYDOWN, [IntPtr]0x0D, [IntPtr]0) | Out-Null
    Start-Sleep -Milliseconds 200
    Check "downgrade requires confirm (still open)" ([T]::FindWindowW("FMDV_UpdatePicker", $null) -ne [IntPtr]::Zero)
    [T]::SendInt($up, $WM_KEYDOWN, [IntPtr]0x1B, [IntPtr]0) | Out-Null  # Esc cancels the arm, not the picker
    Start-Sleep -Milliseconds 200
    Check "Esc cancels the arm (still open)" ([T]::FindWindowW("FMDV_UpdatePicker", $null) -ne [IntPtr]::Zero)
    [T]::SendInt($up, $WM_KEYDOWN, [IntPtr]0x1B, [IntPtr]0) | Out-Null  # Esc closes the picker
}
Start-Sleep -Milliseconds 250
Check "update picker Esc closes" ([T]::FindWindowW("FMDV_UpdatePicker", $null) -eq [IntPtr]::Zero)
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`nTask checkboxes (click-to-toggle):" -ForegroundColor Cyan
# Preview-pane click hit-testing lives in fmdv.cpp (ToggleTaskAt) and must map the
# raw client point into document space (client y + scroll offset) before testing
# the document-space checkbox rects. Regression guard for that scroll adjustment.
$WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202; $VK_HOME = 0x24; $VK_DOWN = 0x28
function LParam($x, $y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function ClickAt($h, $x, $y) {
    [T]::PostMessage($h, $WM_LBUTTONDOWN, [IntPtr]1, (LParam $x $y)) | Out-Null; Start-Sleep -Milliseconds 25
    [T]::PostMessage($h, $WM_LBUTTONUP,   [IntPtr]0, (LParam $x $y)) | Out-Null; Start-Sleep -Milliseconds 120
}
# read even while the app is mid-write (atomic replace briefly locks the target)
function ReadMd($path) {
    for ($i = 0; $i -lt 40; $i++) {
        try { $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite');
              $sr = New-Object System.IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t }
        catch { Start-Sleep -Milliseconds 25 }
    }
    return $null
}
$cbx = 60  # x inside the checkbox glyph column at 1x/1.5x scale

# 1) plain toggle + byte integrity (unscrolled)
$tf = "$fix\clicktasks.md"
$torig = "# Tasks`n`n- [ ] first`n- [x] second`n- [ ] third`n"
[System.IO.File]::WriteAllText($tf, $torig)
$p = Launch $tf; $h = $p.MainWindowHandle
[T]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$VK_HOME, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 200
# find the three checkbox rows by sweeping y; record which source line each toggles
$before = ReadMd $tf; $rows = @{}
for ($y = 18; $y -lt 240 -and $rows.Count -lt 3; $y++) {
    ClickAt $h $cbx $y; $now = ReadMd $tf
    if ($now -ne $before) {
        $ob = $before -split "`n"; $nw = $now -split "`n"
        for ($li = 0; $li -lt $ob.Count; $li++) { if ($ob[$li] -ne $nw[$li]) { if (-not $rows.ContainsKey($li)) { $rows[$li] = $y }; break } }
        $before = $now
    }
}
Check "task rows map to source lines 2/3/4" ($rows.ContainsKey(2) -and $rows.ContainsKey(3) -and $rows.ContainsKey(4))
if (-not $p.HasExited) { $p.Kill() }; Start-Sleep -Milliseconds 250
# fresh doc, toggle only the middle item and verify the other lines are byte-identical
[System.IO.File]::WriteAllText($tf, $torig)
$p = Launch $tf; $h = $p.MainWindowHandle
[T]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$VK_HOME, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 200
if ($rows.ContainsKey(3)) { ClickAt $h $cbx $rows[3] }
$mid = ReadMd $tf; $ml = $mid -split "`n"
Check "middle toggles [x]->[ ]"        ($ml[3] -eq '- [ ] second')
Check "other task lines byte-identical" ($ml[0] -eq '# Tasks' -and $ml[2] -eq '- [ ] first' -and $ml[4] -eq '- [ ] third')
Check "save keeps LF endings"           (-not ($mid -match "`r"))
if (-not $p.HasExited) { $p.Kill() }; Start-Sleep -Milliseconds 250

# 2) SCROLLED hit-testing: a checkbox in the middle of a long doc must toggle when
# clicked where it is *visible*, and an off-screen checkbox must never be hit.
$sf = "$fix\clicktasks_scroll.md"
$sfsb = "- [ ] AAA`n`n"
for ($i = 0; $i -lt 6;   $i++) { $sfsb += "Filler A $i`n`n" }
$sfsb += "- [ ] MID`n`n"
for ($i = 0; $i -lt 200; $i++) { $sfsb += "Filler B $i`n`n" }
[System.IO.File]::WriteAllText($sf, $sfsb)
$p = Launch $sf; $h = $p.MainWindowHandle
[T]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$VK_HOME, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 200
for ($i = 0; $i -lt 5; $i++) { [T]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$VK_DOWN, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 70 }
Start-Sleep -Milliseconds 200
# sweep the viewport; the first checkbox we hit should be MID (AAA is scrolled off the top)
$prev = ReadMd $sf; $midY = $null; $hitAAA = $false
for ($y = 6; $y -lt 460; $y++) {
    ClickAt $h $cbx $y; $now = ReadMd $sf
    $ma = [bool]($prev -match '\[x\] AAA'); $mb = [bool]($now -match '\[x\] AAA')
    $na = [bool]($prev -match '\[x\] MID'); $nb = [bool]($now -match '\[x\] MID')
    if ($ma -ne $mb) { $hitAAA = $true }
    if ($na -ne $nb -and -not $midY) { $midY = $y; break }
    $prev = $now
}
Check "scrolled click toggles the VISIBLE checkbox (MID)"      ($null -ne $midY)
Check "scrolled click never hits the off-screen checkbox (AAA)" (-not $hitAAA)
if (-not $p.HasExited) { $p.Kill() }

# ---- summary ----
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host ("  {0} passed, {1} failed" -f $script:pass, $script:fail) -ForegroundColor ($(if($script:fail){"Red"}else{"Green"}))
Write-Host "========================================"
exit $script:fail
