param(
  [string]$ProcessName = "samurai_warriors_2_empires",
  [string]$Monitor = "nonprimary",
  [int]$WaitSeconds = 20,
  [int]$Width = 1280,
  [int]$Height = 720
)

Add-Type -AssemblyName System.Windows.Forms

$signature = @"
using System;
using System.Runtime.InteropServices;

public static class WindowPlacement {
  [DllImport("user32.dll")]
  public static extern bool SetWindowPos(
      IntPtr hWnd,
      IntPtr hWndInsertAfter,
      int X,
      int Y,
      int cx,
      int cy,
      uint uFlags);
}
"@

Add-Type -TypeDefinition $signature

$screens = [System.Windows.Forms.Screen]::AllScreens
if ($screens.Count -eq 0) {
  exit 0
}

$screen = $null
if ($Monitor -match '^\d+$') {
  $index = [int]$Monitor - 1
  if ($index -ge 0 -and $index -lt $screens.Count) {
    $screen = $screens[$index]
  }
}

if ($null -eq $screen -and $Monitor -ieq "primary") {
  $screen = $screens | Where-Object { $_.Primary } | Select-Object -First 1
}

if ($null -eq $screen -and $Monitor -ieq "nonprimary") {
  $screen = $screens | Where-Object { -not $_.Primary } | Select-Object -First 1
}

if ($null -eq $screen) {
  $screen = $screens | Select-Object -First 1
}

$area = $screen.WorkingArea
$targetWidth = [Math]::Min($Width, $area.Width)
$targetHeight = [Math]::Min($Height, $area.Height)
$targetX = $area.X + [Math]::Max(0, [int](($area.Width - $targetWidth) / 2))
$targetY = $area.Y + [Math]::Max(0, [int](($area.Height - $targetHeight) / 2))

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$hwnd = [IntPtr]::Zero

while ((Get-Date) -lt $deadline) {
  $process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
      Where-Object { $_.MainWindowHandle -ne 0 } |
      Select-Object -First 1

  if ($null -ne $process) {
    $process.Refresh()
    $hwnd = $process.MainWindowHandle
    if ($hwnd -ne [IntPtr]::Zero) {
      break
    }
  }

  Start-Sleep -Milliseconds 100
}

if ($hwnd -ne [IntPtr]::Zero) {
  $swpNoZOrder = 0x0004
  $swpNoActivate = 0x0010
  $swpShowWindow = 0x0040
  [WindowPlacement]::SetWindowPos(
      $hwnd,
      [IntPtr]::Zero,
      $targetX,
      $targetY,
      $targetWidth,
      $targetHeight,
      $swpNoZOrder -bor $swpNoActivate -bor $swpShowWindow) | Out-Null
}
