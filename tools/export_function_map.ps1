param(
  [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
  [string]$OutputPath = (Join-Path $ProjectRoot 'docs\function_map.generated.csv')
)

$initPath = Join-Path $ProjectRoot 'generated\default\samurai_warriors_2_empires_init.cpp'
if (-not (Test-Path -LiteralPath $initPath)) {
  throw "Generated init file was not found: $initPath"
}

$rows = foreach ($line in Get-Content -LiteralPath $initPath) {
  if ($line -match '\{\s*0x([0-9A-Fa-f]+),\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}') {
    [pscustomobject]@{
      address = ('0x{0}' -f $matches[1].ToUpperInvariant())
      symbol = $matches[2]
      label = ''
      confidence = 'generated'
      notes = ''
    }
  }
}

$outDir = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$rows | Export-Csv -NoTypeInformation -LiteralPath $OutputPath
Write-Host "Wrote $($rows.Count) functions to $OutputPath"
