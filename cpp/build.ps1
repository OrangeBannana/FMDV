# FMDV native build script
param([switch]$Debug)

$ErrorActionPreference = "Stop"
$mingw = "C:\Users\<user>\mingw\mingw64\bin"
$env:PATH = "$mingw;$env:PATH"
Set-Location $PSScriptRoot

# Compile resource (icon + version info)
& windres fmdv.rc -O coff -o fmdv_res.o
if ($LASTEXITCODE -ne 0) { Write-Host "windres FAILED"; exit 1 }

$srcs = @("fmdv.cpp", "markdown.cpp", "layout.cpp", "render.cpp", "prefs.cpp")
$srcs = $srcs | Where-Object { Test-Path $_ }

$common = @(
    "-municode", "-std=c++17", "-Wall", "-Wextra",
    "-lgdi32", "-lgdiplus", "-lcomctl32", "-luser32", "-lshell32", "-lole32", "-ldwmapi"
)

if ($Debug) {
    # console subsystem so stdout/stderr are visible; with symbols
    $args = @("-g", "-O0", "-DFMDV_CONSOLE") + $srcs + @("fmdv_res.o", "-o", "fmdv_dbg.exe") + $common
    Write-Host "Building DEBUG (console) ..."
    & g++ @args
    if ($LASTEXITCODE -eq 0) { Write-Host "OK -> fmdv_dbg.exe" } else { Write-Host "BUILD FAILED" }
} else {
    $args = @("-O2", "-mwindows", "-static", "-s") + $srcs + @("fmdv_res.o", "-o", "fmdv.exe") + $common
    Write-Host "Building RELEASE (gui) ..."
    & g++ @args
    if ($LASTEXITCODE -eq 0) {
        $kb = [math]::Round((Get-Item fmdv.exe).Length / 1KB)
        Write-Host "OK -> fmdv.exe ($kb KB)"
    } else { Write-Host "BUILD FAILED" }
}
