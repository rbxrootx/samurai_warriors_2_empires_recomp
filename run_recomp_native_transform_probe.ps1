param(
  [int]$DurationSeconds = 90,
  [int]$InitialDelaySeconds = 8,
  [string]$Monitor = "nonprimary",
  [switch]$DumpGapSamples,
  [int]$SampleLimit = 128,
  [int]$SampleBytes = 65536,
  [switch]$ExternalPulseInput,
  [switch]$NoMoveWindow,
  [switch]$KeepOpen
)

$ErrorActionPreference = "Stop"

if ($DurationSeconds -lt 20) {
  throw "DurationSeconds must be at least 20 seconds."
}

$root = $PSScriptRoot
$processName = "samurai_warriors_2_empires"
$buildDir = Join-Path $root "out\build\win-amd64-debug"
$exe = Join-Path $buildDir "samurai_warriors_2_empires.exe"
$gameRoot = Join-Path (Split-Path $root -Parent) "game_files"
$modRoot = Join-Path $root "mods\loose"
$moveScript = Join-Path $root "tools\move_recomp_window.ps1"
$pulseScript = Join-Path $root "tools\pulse_recomp_input.ps1"
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logFile = Join-Path $root "runtime.native-transform-probe-$timestamp.log"
$sampleRoot = Join-Path $root "extracted\native_render_samples\native_gap_probe_$timestamp"

if (-not (Test-Path $exe)) {
  throw "Recomp executable was not found: $exe"
}

if (-not (Test-Path (Join-Path $gameRoot "default.xex"))) {
  throw "Extracted game files were not found: $gameRoot"
}

if ($ExternalPulseInput -and -not (Test-Path $pulseScript)) {
  throw "Input pulse helper was not found: $pulseScript"
}

$existing = Get-Process -Name $processName -ErrorAction SilentlyContinue
if ($existing) {
  $pids = ($existing | ForEach-Object { $_.Id }) -join ", "
  throw "A recomp process is already running (PID $pids). Close it before starting the probe."
}

$eventCandidates = @(
  (Join-Path $root "native_render_events.jsonl"),
  (Join-Path $buildDir "native_render_events.jsonl"),
  (Join-Path $root "extracted\native_render_events\native_render_events.jsonl")
)
$eventWriteTimes = @{}
foreach ($candidate in $eventCandidates) {
  if (Test-Path $candidate) {
    $eventWriteTimes[$candidate] = (Get-Item $candidate).LastWriteTimeUtc
  }
}

$inputArgs = @(
  "--audio_mute=true",
  "--mnk_mode=true",
  "--mnk_capture_mouse=false",
  "--mnk_sensitivity=1.0",
  "--keybind_a=Space",
  "--keybind_b=Shift",
  "--keybind_x=LMB",
  "--keybind_y=RMB",
  "--keybind_left_trigger=Z",
  "--keybind_right_trigger=Control",
  "--keybind_left_shoulder=Q",
  "--keybind_right_shoulder=F",
  "--keybind_lstick_up=W",
  "--keybind_lstick_down=S",
  "--keybind_lstick_left=A",
  "--keybind_lstick_right=D",
  "--keybind_lstick_press=C",
  "--keybind_rstick_press=MMB",
  "--keybind_dpad_up=Up",
  "--keybind_dpad_down=Down",
  "--keybind_dpad_left=Left",
  "--keybind_dpad_right=Right",
  "--keybind_back=Tab",
  "--keybind_start=Enter"
)

$probeArgs = @(
  "--native_render_events=false",
  "--sw2e_native_renderer=true",
  "--sw2e_native_renderer_log_interval=15",
  "--sw2e_native_renderer_hash_memory=false",
  "--sw2e_native_renderer_dump_samples=false",
  "--sw2e_native_renderer_gpu_replay=false",
  "--sw2e_auto_boot_input=true",
  "--sw2e_auto_probe_input=true"
)

if ($DumpGapSamples) {
  $probeArgs = @(
    "--native_render_events=false",
    "--sw2e_native_renderer=true",
    "--sw2e_native_renderer_log_interval=15",
    "--sw2e_native_renderer_hash_memory=true",
    "--sw2e_native_renderer_memory_hash_bytes=$SampleBytes",
    "--sw2e_native_renderer_dump_samples=true",
    "--sw2e_native_renderer_dump_gap_samples_only=true",
    "--sw2e_native_renderer_dump_priority_samples_only=false",
    "--sw2e_native_renderer_dump_sample_limit=$SampleLimit",
    "--sw2e_native_renderer_sample_root=$sampleRoot",
    "--sw2e_native_renderer_gpu_replay=false",
    "--sw2e_auto_boot_input=true",
    "--sw2e_auto_probe_input=true"
  )
}

$gameArgs = @(
  "--game_data_root=$gameRoot",
  "--log_file=$logFile",
  "--log_level=info"
)
if (Test-Path $modRoot) {
  $gameArgs += "--mod_data_root=$modRoot"
}
$gameArgs += $inputArgs
$gameArgs += $probeArgs

function Invoke-RecompPulse {
  param(
    [System.Diagnostics.Process]$Process,
    [string]$Key,
    [int]$Seconds,
    [int]$IntervalMs
  )

  if ($Seconds -le 0) {
    return
  }

  $Process.Refresh()
  if ($Process.HasExited) {
    return
  }

  Write-Host "Pulsing $Key for $Seconds seconds..."
  $pulseArgs = @(
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    $pulseScript,
    "-ProcessName",
    $processName,
    "-Key",
    $Key,
    "-DurationSeconds",
    [string]$Seconds,
    "-IntervalMs",
    [string]$IntervalMs,
    "-KeyDownMs",
    "60",
    "-Method",
    "SendInput"
  )

  $pulseOutput = & powershell.exe @pulseArgs 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Warning (($pulseOutput | Out-String).Trim())
  }
}

if (-not $NoMoveWindow -and (Test-Path $moveScript)) {
  Start-Process -FilePath "powershell.exe" -WindowStyle Hidden -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    $moveScript,
    "-Monitor",
    $Monitor,
    "-WaitSeconds",
    "20"
  ) | Out-Null
}

Push-Location $buildDir
try {
  Write-Host "Starting native transform probe..."
  Write-Host "Log: $logFile"
  $process = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $buildDir -PassThru
} finally {
  Pop-Location
}

$deadline = (Get-Date).AddSeconds($DurationSeconds)
$closedByProbe = $false

try {
  if ($ExternalPulseInput) {
    Start-Sleep -Seconds $InitialDelaySeconds
    Invoke-RecompPulse -Process $process -Key "Enter" -Seconds 4 -IntervalMs 500

    $remainingForConfirm = [int][Math]::Max(5, ($deadline - (Get-Date)).TotalSeconds - 2)
    Invoke-RecompPulse -Process $process -Key "Space" -Seconds $remainingForConfirm -IntervalMs 650
  } elseif ($InitialDelaySeconds -gt 0) {
    Start-Sleep -Seconds $InitialDelaySeconds
  }

  while ((Get-Date) -lt $deadline) {
    $process.Refresh()
    if ($process.HasExited) {
      break
    }
    Start-Sleep -Milliseconds 500
  }
} finally {
  $process.Refresh()
  if (-not $process.HasExited -and -not $KeepOpen) {
    $closedByProbe = $true
    [void]$process.CloseMainWindow()
    if (-not $process.WaitForExit(15000)) {
      Stop-Process -Id $process.Id -Force
      $process.WaitForExit()
    }
  }
}

Start-Sleep -Milliseconds 500

$transformMatches = @()
$layoutMatches = @()
$summaryMatches = @()
$problemMatches = @()
if (Test-Path $logFile) {
  $transformMatches = @(Select-String -Path $logFile -Pattern "SW2E native transform gap" -ErrorAction SilentlyContinue)
  $layoutMatches = @(Select-String -Path $logFile -Pattern "SW2E native layout gap" -ErrorAction SilentlyContinue)
  $summaryMatches = @(Select-String -Path $logFile -Pattern "SW2E native (sidecar|renderer) frame" -ErrorAction SilentlyContinue | Select-Object -First 5)
  $problemMatches = @(Select-String -Path $logFile -Pattern "fatal|assert|crash|exception|\[error\]" -ErrorAction SilentlyContinue | Select-Object -First 8)
}

$sampleManifest = Join-Path $sampleRoot "samples.jsonl"
$sampleRowCount = 0
$sampleKindCounts = @()
$sampleSupportCounts = @()
$sampleVertexConstantRows = 0
$samplePixelConstantRows = 0
if (Test-Path $sampleManifest) {
  $sampleRows = @(Get-Content -Path $sampleManifest | ForEach-Object { $_ | ConvertFrom-Json })
  $sampleRowCount = $sampleRows.Count
  $sampleKindCounts = @($sampleRows | Group-Object kind | Sort-Object Count -Descending |
    ForEach-Object { "$($_.Name)=$($_.Count)" })
  $sampleSupportCounts = @($sampleRows | Group-Object native_replay_support |
    Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" })
  $sampleVertexConstantRows = @($sampleRows | Where-Object {
      $_.vertex_float_constant_values -and $_.vertex_float_constant_values.Count -gt 0
    }).Count
  $samplePixelConstantRows = @($sampleRows | Where-Object {
      $_.pixel_float_constant_values -and $_.pixel_float_constant_values.Count -gt 0
    }).Count
}

$eventTouched = @()
foreach ($candidate in $eventCandidates) {
  if (-not (Test-Path $candidate)) {
    continue
  }
  $currentWriteTime = (Get-Item $candidate).LastWriteTimeUtc
  if (-not $eventWriteTimes.ContainsKey($candidate) -or $eventWriteTimes[$candidate] -ne $currentWriteTime) {
    $eventTouched += $candidate
  }
}

$exitCode = $null
$process.Refresh()
if ($process.HasExited) {
  $exitCode = $process.ExitCode
}

[pscustomobject]@{
  LogFile = $logFile
  ProcessId = $process.Id
  ExitCode = $exitCode
  ClosedByProbe = $closedByProbe
  NativeRenderEvents = "false"
  EventJsonTouched = $eventTouched
  SampleRoot = $(if ($DumpGapSamples) { $sampleRoot } else { $null })
  SampleRows = $sampleRowCount
  SampleKindCounts = $sampleKindCounts
  SampleSupportCounts = $sampleSupportCounts
  SampleVertexConstantRows = $sampleVertexConstantRows
  SamplePixelConstantRows = $samplePixelConstantRows
  TransformGapCount = $transformMatches.Count
  LayoutGapCount = $layoutMatches.Count
  FirstTransformGaps = @($transformMatches | Select-Object -First 6 | ForEach-Object { $_.Line })
  FirstLayoutGaps = @($layoutMatches | Select-Object -First 6 | ForEach-Object { $_.Line })
  FirstFrameSummaries = @($summaryMatches | ForEach-Object { $_.Line })
  ProblemLines = @($problemMatches | ForEach-Object { $_.Line })
}
