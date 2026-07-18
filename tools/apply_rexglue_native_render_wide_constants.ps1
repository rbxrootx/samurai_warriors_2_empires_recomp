param(
  [string]$RexSdkDir = "L:\SM2\thirdparty\rexglue-sdk-source-v0.8.0",
  [int]$ConstantSummaryLimit = 128
)

$ErrorActionPreference = "Stop"

if ($ConstantSummaryLimit -lt 8 -or $ConstantSummaryLimit -gt 256) {
  throw "ConstantSummaryLimit must be between 8 and 256."
}

$sdkRoot = (Resolve-Path -LiteralPath $RexSdkDir).Path
$header = Join-Path $sdkRoot "include\rex\graphics\native_render_event_stream.h"
if (-not (Test-Path -LiteralPath $header)) {
  throw "native_render_event_stream.h was not found under $sdkRoot"
}

$text = Get-Content -LiteralPath $header -Raw
$pattern = "constexpr uint32_t kMaxFloatConstantSummariesPerDraw = \d+;"
$replacement = "constexpr uint32_t kMaxFloatConstantSummariesPerDraw = $ConstantSummaryLimit;"
if ($text -notmatch $pattern) {
  throw "Could not find kMaxFloatConstantSummariesPerDraw in $header"
}

$updated = [regex]::Replace($text, $pattern, $replacement, 1)
if ($updated -eq $text) {
  Write-Host "ReXGlue native-render constant summary limit already set to $ConstantSummaryLimit."
  exit 0
}

Set-Content -LiteralPath $header -Value $updated -NoNewline
Write-Host "Set ReXGlue native-render constant summary limit to $ConstantSummaryLimit in $header"
