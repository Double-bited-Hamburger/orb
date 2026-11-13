# Launches native\OrbTest.exe with given settings in an isolated ini (never touches the real
# %APPDATA%\Orb\orb.ini), screenshots the window, prints startup time + memory, then closes it.
#   .\shot.ps1 -Name ring_glass -Orb @{form=2; clockstyle=2; drift=0}
#   .\shot.ps1 -Name panel -Orb @{form=0} -Settings     # opens the settings panel, captures the whole monitor
# Output PNGs + test.ini go to tools\out\ (git-ignorable).
param([string]$Name, [hashtable]$Orb = @{}, [int]$Wait = 4, [int]$W = 1280, [int]$H = 760, [switch]$Settings,
      [string]$Exe = (Join-Path $PSScriptRoot '..\OrbTest.exe'))
$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $dir | Out-Null
$ini = Join-Path $dir 'test.ini'
$lines = @('[orb]')
foreach ($k in $Orb.Keys) { $lines += "$k=$($Orb[$k])" }
$lines += '[window]', 'x=60', 'y=60', "w=$W", "h=$H", 'fullscreen=0'
Set-Content $ini $lines -Encoding Unicode
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
if (-not ('W5' -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W5 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
"@ }
[W5]::SetProcessDPIAware() | Out-Null
$env:ORB_INI = $ini
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process $Exe -PassThru
while ($p.MainWindowHandle -eq 0 -and $sw.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 100; $p.Refresh() }
$startup = $sw.Elapsed.TotalSeconds
Start-Sleep $Wait
$p.Refresh()
$h = [IntPtr]$p.MainWindowHandle
# Windows may refuse to bring the window forward, so also make it topmost while capturing.
[W5]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0043) | Out-Null   # HWND_TOPMOST, NOSIZE|NOMOVE|SHOWWINDOW
[W5]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
if ($Settings) { [System.Windows.Forms.SendKeys]::SendWait('h'); Start-Sleep 1 }
$scr = [System.Windows.Forms.Screen]::FromHandle($h).Bounds
$r = New-Object W5+RECT
[W5]::GetWindowRect($h, [ref]$r) | Out-Null
# refuse to save a capture of some other window
$pt = New-Object W5+POINT; $pt.X = [int](($r.L + $r.R) / 2); $pt.Y = [int](($r.T + $r.B) / 2)
if ([W5]::GetAncestor([W5]::WindowFromPoint($pt), 2) -ne $h) {
    Stop-Process $p; Remove-Item Env:ORB_INI
    throw "$Name : another window is covering Orb, screenshot not taken"
}
$x = $r.L; $y = $r.T; $w = $r.R - $r.L; $hh = $r.B - $r.T   # the settings panel is drawn inside the window
$bmp = New-Object System.Drawing.Bitmap $w, $hh
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($x, $y, 0, 0, $bmp.Size)
$out = Join-Path $dir "$Name.png"
$bmp.Save($out)
$p.Refresh()
"{0}: window-up {1:N1}s, private {2:N0} MB -> {3}" -f $Name, $startup, ($p.PrivateMemorySize64 / 1MB), $out
$p.CloseMainWindow() | Out-Null
Start-Sleep -Milliseconds 800
if (-not $p.HasExited) { Stop-Process $p }
Remove-Item Env:ORB_INI
