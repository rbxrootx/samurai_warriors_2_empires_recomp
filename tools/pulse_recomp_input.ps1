param(
  [string]$ProcessName = "samurai_warriors_2_empires",
  [string]$Key = "Space",
  [int]$DurationSeconds = 30,
  [int]$IntervalMs = 700,
  [int]$KeyDownMs = 60,
  [int]$InitialDelaySeconds = 0,
  [ValidateSet("SendInput", "PostMessage")]
  [string]$Method = "SendInput"
)

$ErrorActionPreference = "Stop"

$source = @"
using System;
using System.Runtime.InteropServices;

public static class RecompInputPulse {
  [DllImport("user32.dll", SetLastError = true)]
  public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

  [DllImport("user32.dll")]
  public static extern bool IsWindow(IntPtr hWnd);

  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);

  [DllImport("user32.dll", SetLastError = true)]
  public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
}
"@

if (-not ("RecompInputPulse" -as [type])) {
  Add-Type -TypeDefinition $source
}

function Get-VirtualKey([string]$Name) {
  $upper = $Name.ToUpperInvariant()
  if ($upper.Length -eq 1) {
    $c = [int][char]$upper
    if (($c -ge [int][char]'A' -and $c -le [int][char]'Z') -or
        ($c -ge [int][char]'0' -and $c -le [int][char]'9')) {
      return $c
    }
  }

  switch ($upper) {
    "ENTER" { return 0x0D }
    "RETURN" { return 0x0D }
    "SPACE" { return 0x20 }
    "ESC" { return 0x1B }
    "ESCAPE" { return 0x1B }
    "UP" { return 0x26 }
    "DOWN" { return 0x28 }
    "LEFT" { return 0x25 }
    "RIGHT" { return 0x27 }
    default { throw "Unsupported key '$Name'" }
  }
}

function Get-RecompWindow([string]$Name) {
  $deadline = (Get-Date).AddSeconds(20)
  do {
    $process = Get-Process -Name $Name -ErrorAction SilentlyContinue |
      Where-Object { $_.MainWindowHandle -ne 0 } |
      Select-Object -First 1
    if ($process) {
      return $process
    }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)

  throw "No visible window found for process '$Name'"
}

if ($InitialDelaySeconds -gt 0) {
  Start-Sleep -Seconds $InitialDelaySeconds
}

$process = Get-RecompWindow $ProcessName
$hwnd = $process.MainWindowHandle
$vk = Get-VirtualKey $Key
$wmKeyDown = 0x0100
$wmKeyUp = 0x0101
$keyEventKeyUp = 0x0002
$endTime = (Get-Date).AddSeconds($DurationSeconds)
$pulses = 0

while ((Get-Date) -lt $endTime) {
  if ($process.HasExited -or -not [RecompInputPulse]::IsWindow($hwnd)) {
    break
  }

  if ($Method -eq "SendInput") {
    [void][RecompInputPulse]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 20
    [RecompInputPulse]::keybd_event([byte]$vk, [byte]0, [uint32]0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $KeyDownMs
    [RecompInputPulse]::keybd_event([byte]$vk, [byte]0, [uint32]$keyEventKeyUp, [UIntPtr]::Zero)
  } else {
    [void][RecompInputPulse]::PostMessage($hwnd, $wmKeyDown, [IntPtr]$vk, [IntPtr]0)
    Start-Sleep -Milliseconds $KeyDownMs
    [void][RecompInputPulse]::PostMessage($hwnd, $wmKeyUp, [IntPtr]$vk, [IntPtr]0)
  }

  $pulses++
  Start-Sleep -Milliseconds $IntervalMs
}

[pscustomobject]@{
  ProcessName = $ProcessName
  ProcessId = $process.Id
  Key = $Key
  Method = $Method
  Pulses = $pulses
  DurationSeconds = $DurationSeconds
}
