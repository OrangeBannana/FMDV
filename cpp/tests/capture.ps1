# Launch fmdv, optionally post a WM_COMMAND, and capture the window via PrintWindow
# (works regardless of z-order / occlusion — no foreground battle needed).
param(
    [string]$Exe,
    [string]$File,
    [string]$Out,
    [int]$Command = 0,      # WM_COMMAND id to post after launch (0 = none). 2001=edit,2002=dark
    [int]$Command2 = 0,     # second WM_COMMAND id
    [switch]$Screen,        # use real on-screen grab (topmost + move to 0,0) instead of PrintWindow
    [int]$WaitMs = 900
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    public struct RECT { public int L, T, R, B; }
}
"@

$WM_COMMAND = 0x0111

$p = Start-Process -FilePath $Exe -ArgumentList "`"$File`"" -PassThru
Start-Sleep -Milliseconds 800
$p.Refresh()
$h = $p.MainWindowHandle
$tries = 0
while ($h -eq [IntPtr]::Zero -and $tries -lt 10) { Start-Sleep -Milliseconds 200; $p.Refresh(); $h = $p.MainWindowHandle; $tries++ }

if ($Screen) {
    # move to origin and force topmost so it renders above everything for a real grab
    [W]::SetWindowPos($h, [IntPtr](-1), 0, 0, 1100, 820, 0x0040) | Out-Null # SWP_SHOWWINDOW
    Start-Sleep -Milliseconds 300
}

if ($Command -ne 0) {
    [W]::PostMessage($h, $WM_COMMAND, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 400
}
if ($Command2 -ne 0) {
    [W]::PostMessage($h, $WM_COMMAND, [IntPtr]$Command2, [IntPtr]::Zero) | Out-Null
}
if ($Command -ne 0 -or $Command2 -ne 0) { Start-Sleep -Milliseconds $WaitMs }

$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hgt = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $hgt)
$g = [System.Drawing.Graphics]::FromImage($bmp)
if ($Screen) {
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $hgt)))
} else {
    $hdc = $g.GetHdc()
    [W]::PrintWindow($h, $hdc, 2) | Out-Null   # 2 = PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Start-Sleep -Milliseconds 150
if (-not $p.HasExited) { $p.Kill() }
Write-Host "captured $w x $hgt -> $Out"
