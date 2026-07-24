# FMDV native build script
# Toolchain: MinGW-w64 (g++ + windres). Resolved from, in order:
#   1. -MinGW <path-to-bin> parameter
#   2. FMDV_MINGW environment variable
#   3. g++ already on PATH
param([switch]$Debug, [string]$MinGW = $env:FMDV_MINGW)

$ErrorActionPreference = "Stop"
if ($MinGW) { $env:PATH = "$MinGW;$env:PATH" }
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Write-Host "g++ not found. Install MinGW-w64 and add its bin dir to PATH, set FMDV_MINGW, or pass -MinGW <path>." -ForegroundColor Red
    exit 1
}
# Build inside cpp/ without leaking the cwd change to the caller
Push-Location $PSScriptRoot
try {

# Compile resource (icon + version info)
& windres fmdv.rc -O coff -o fmdv_res.o
if ($LASTEXITCODE -ne 0) { Write-Host "windres FAILED"; exit 1 }

# Shared platform-neutral core lives in ..\core (see docs/macos-implementation-guide.md).
$srcs = @("fmdv.cpp", "render.cpp", "prefs.cpp", "updater.cpp", "bench.cpp",
          "..\core\str.cpp", "..\core\markdown.cpp", "..\core\edit_assist.cpp", "..\core\release_info.cpp",
          "..\core\layout.cpp", "..\core\text_select.cpp")

$common = @(
    "-municode", "-std=c++17", "-Wall", "-Wextra", "-I..\core",
    "-lgdi32", "-lgdiplus", "-lcomctl32", "-luser32", "-lshell32", "-lole32", "-ldwmapi", "-lwinhttp", "-lcomdlg32"
)

if ($Debug) {
    # console subsystem so stdout/stderr are visible; with symbols
    $buildArgs = @("-g", "-O0", "-DFMDV_CONSOLE") + $srcs + @("fmdv_res.o", "-o", "fmdv_dbg.exe") + $common
    Write-Host "Building DEBUG (console) ..."
    & g++ @buildArgs
    if ($LASTEXITCODE -eq 0) { Write-Host "OK -> fmdv_dbg.exe" } else { Write-Host "BUILD FAILED" }
} else {
    $buildArgs = @("-O2", "-mwindows", "-static", "-s") + $srcs + @("fmdv_res.o", "-o", "fmdv.exe") + $common
    Write-Host "Building RELEASE (gui) ..."
    & g++ @buildArgs
    if ($LASTEXITCODE -eq 0) {
        $kb = [math]::Round((Get-Item fmdv.exe).Length / 1KB)
        Write-Host "OK -> fmdv.exe ($kb KB)"
    } else { Write-Host "BUILD FAILED" }
}

} finally { Pop-Location }
