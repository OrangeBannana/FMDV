# Runs run-tests.ps1 with every launched fmdv.exe window relocated off the
# visible screen, so the suite doesn't pop windows over whatever you're doing.
# Same checks, same pass/fail output -- just off-screen instead of on top.
#
# (An earlier version of this tried a separate hidden desktop via CreateProcess,
# but process creation on a freshly-created desktop reliably failed here with
# STATUS_DLL_INIT_FAILED (0xC0000142) -- looks like an environment restriction
# on this machine/sandbox. Off-screen positioning sidesteps that: the window is
# real and on the normal desktop, just parked outside any monitor's bounds, so
# PostMessage/GDI all still work exactly like an on-screen run.)
#
# Usage: powershell -ExecutionPolicy Bypass -File tests\run-tests-hidden.ps1
$env:FMDV_TEST_OFFSCREEN = "1"
& "$PSScriptRoot\run-tests.ps1"
exit $LASTEXITCODE
