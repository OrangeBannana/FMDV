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
}
"@
$WM_COMMAND=0x0111; $WM_SETTEXT=0x000C; $WM_CHAR=0x0102; $WM_KEYDOWN=0x0100; $VK_TAB=0x09
$ID_EDIT=2001; $ID_SAVE=2003; $ID_COPY=2008; $ID_SELALL=2009

$script:pass = 0; $script:fail = 0
function Check($name, $cond, $detail="") {
    if ($cond) { Write-Host "  PASS  $name" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "  FAIL  $name  $detail" -ForegroundColor Red;  $script:fail++ }
}
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

Write-Host "`nLaunch / stability:" -ForegroundColor Cyan
$p = Launch $basic
Check "window stays open"  (-not $p.HasExited)

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

# ---- summary ----
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host ("  {0} passed, {1} failed" -f $script:pass, $script:fail) -ForegroundColor ($(if($script:fail){"Red"}else{"Green"}))
Write-Host "========================================"
exit $script:fail
