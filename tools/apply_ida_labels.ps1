param(
  [string]$McpUrl = 'http://127.0.0.1:13337/mcp',
  [string]$MapPath = '',
  [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$script:NextRpcId = 1

if ([string]::IsNullOrWhiteSpace($MapPath)) {
  $MapPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\hooks\function_map.cpp'
}

function Invoke-IdaTool {
  param(
    [string]$Name,
    [hashtable]$Arguments
  )

  $body = @{
    jsonrpc = '2.0'
    id = $script:NextRpcId
    method = 'tools/call'
    params = @{
      name = $Name
      arguments = $Arguments
    }
  } | ConvertTo-Json -Depth 16
  $script:NextRpcId++

  $response = Invoke-WebRequest -UseBasicParsing -Uri $McpUrl -Method Post -Body $body `
    -ContentType 'application/json'
  $json = $response.Content | ConvertFrom-Json
  if ($json.error) {
    throw "IDA MCP tool '$Name' failed: $($json.error.message)"
  }
  if ($json.result.isError) {
    throw "IDA MCP tool '$Name' returned an error: $($json.result.content.text -join "`n")"
  }
  return $json.result
}

function New-IdaName {
  param(
    [string]$Label,
    [string]$Symbol
  )

  $stem = if ([string]::IsNullOrWhiteSpace($Label)) { $Symbol } else { $Label }
  $stem = $stem.ToLowerInvariant() -replace '[^a-z0-9]+', '_'
  $stem = $stem.Trim('_')
  if ([string]::IsNullOrWhiteSpace($stem)) {
    $stem = $Symbol.ToLowerInvariant() -replace '[^a-z0-9]+', '_'
  }
  return "sw2e_$stem"
}

$source = Get-Content -Raw -Path $MapPath
$pattern = '\{0x([0-9A-Fa-f]+),\s*"([^"]+)",\s*"([^"]+)",\s*FunctionConfidence::([A-Za-z]+),\s*"([^"]*)"\s*\}'
$matches = [regex]::Matches($source, $pattern, [Text.RegularExpressions.RegexOptions]::Singleline)

if ($matches.Count -eq 0) {
  throw "No curated function-map entries found in $MapPath"
}

$seenNames = @{}
$labels = foreach ($match in $matches) {
  $address = ('0x{0}' -f $match.Groups[1].Value.ToUpperInvariant())
  $name = New-IdaName -Label $match.Groups[3].Value -Symbol $match.Groups[2].Value
  if ($seenNames.ContainsKey($name)) {
    $seenNames[$name]++
    $name = '{0}_{1}' -f $name, $seenNames[$name]
  } else {
    $seenNames[$name] = 1
  }

  [pscustomobject]@{
    Address = $address
    Symbol = $match.Groups[2].Value
    Label = $match.Groups[3].Value
    Confidence = $match.Groups[4].Value.ToLowerInvariant()
    Notes = $match.Groups[5].Value
    IdaName = $name
  }
}

$renames = @()
$comments = @()
$skipped = @()

foreach ($label in $labels) {
  $lookup = Invoke-IdaTool -Name 'lookup_funcs' -Arguments @{ queries = @($label.Address) }
  $result = $lookup.structuredContent.result | Select-Object -First 1

  if (-not $result -or -not $result.fn) {
    $skipped += $label
    continue
  }

  $renames += @{ addr = $label.Address; name = $label.IdaName }
  $comments += @{
    addr = $label.Address
    comment = "SW2E: $($label.Label)`nGenerated symbol: $($label.Symbol)`nConfidence: $($label.Confidence)`nNotes: $($label.Notes)"
  }
}

if ($skipped.Count -gt 0) {
  Write-Warning ("Skipped {0} entries because IDA did not report a function at that address. Load the XEX IDB to apply guest labels." -f $skipped.Count)
  $skipped | Select-Object Address,Symbol,Label | Format-Table -AutoSize
}

if ($renames.Count -eq 0) {
  Write-Host 'No matching IDA functions found to label.'
  exit 0
}

if (-not $Apply) {
  Write-Host 'Dry run only. Re-run with -Apply to rename and comment these IDA functions:'
  $renames | ForEach-Object { [pscustomobject]@{ Address = $_.addr; Name = $_.name } } |
    Format-Table -AutoSize
  exit 0
}

Invoke-IdaTool -Name 'rename' -Arguments @{ batch = @{ func = $renames } } | Out-Null
Invoke-IdaTool -Name 'set_comments' -Arguments @{ items = $comments } | Out-Null

Write-Host ("Applied {0} IDA names/comments." -f $renames.Count)
