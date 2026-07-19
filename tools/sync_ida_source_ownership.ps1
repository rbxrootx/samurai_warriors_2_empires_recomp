param(
  [string]$McpUrl = 'http://127.0.0.1:13337/mcp',
  [string]$OutputPath = '',
  [switch]$Apply,
  [switch]$RenameSourceAnchors
)
$ErrorActionPreference = 'Stop'
$script:NextRpcId = 1000
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\ida_source_ownership.generated.md' }
function Invoke-IdaTool {
  param([string]$Name, [hashtable]$Arguments)
  $body = @{jsonrpc='2.0';id=$script:NextRpcId++;method='tools/call';params=@{name=$Name;arguments=$Arguments}} | ConvertTo-Json -Depth 20
  $response = Invoke-RestMethod -Uri $McpUrl -Method Post -Body $body -ContentType 'application/json'
  if ($response.error) { throw "IDA MCP: $($response.error.message)" }
  if ($response.result.isError) { throw ($response.result.content.text -join [Environment]::NewLine) }
  return $response.result
}
$python = @'
import ida_funcs, ida_name, idautils, re, json
rows = {}
for s in idautils.Strings():
    text = str(s)
    if not re.match(r'^\.\\(?:application|xbox)\\.*\.(?:cpp|c|h)$', text, re.I):
        continue
    for xref in idautils.XrefsTo(s.ea):
        fn = ida_funcs.get_func(xref.frm)
        if not fn: continue
        key = (fn.start_ea, text)
        row = rows.setdefault(key, {"func_ea":"0x%08X" % fn.start_ea, "name":ida_name.get_name(fn.start_ea) or ("sub_%X" % fn.start_ea), "source":text, "xref_count":0})
        row["xref_count"] += 1
result = json.dumps(sorted(rows.values(), key=lambda x: (x["source"].lower(), x["func_ea"])))
'@
$result = Invoke-IdaTool -Name 'py_eval' -Arguments @{code=$python}
$rows = $result.structuredContent.result | ConvertFrom-Json
$rows = @($rows)
if ($rows.Count -eq 0) { throw 'No source-owned functions found; confirm the SW2E XEX database is active.' }
$comments=@(); $renames=@(); $usedNames=@{}
foreach ($row in $rows) {
  $comments += @{addr=$row.func_ea;comment="Original source ownership (embedded path evidence): $($row.source)"}
  if ($RenameSourceAnchors -and $row.name -match '^(sub_|nullsub_)') {
    $stem=[IO.Path]::GetFileNameWithoutExtension($row.source).ToLowerInvariant() -replace '[^a-z0-9]+','_'
    $candidate='sw2e_'+$stem+'_src_'+$row.func_ea.Substring(2).ToLowerInvariant()
    if (-not $usedNames.ContainsKey($candidate)) { $usedNames[$candidate]=$true; $renames += @{addr=$row.func_ea;name=$candidate} }
  }
}
if ($Apply) {
  Invoke-IdaTool -Name 'set_comments' -Arguments @{items=$comments} | Out-Null
  if ($renames.Count -gt 0) { Invoke-IdaTool -Name 'rename' -Arguments @{batch=@{func=$renames}} | Out-Null }
}
$grouped=$rows | Group-Object source | Sort-Object Name
$lines=[Collections.Generic.List[string]]::new()
$lines.Add('# IDA Source Ownership Index'); $lines.Add('')
$lines.Add('Generated from embedded source-path references and their code xrefs in the active default.xex IDA database.'); $lines.Add('')
$lines.Add(('- Source files represented: {0}' -f $grouped.Count)); $lines.Add(('- Direct function/source anchors: {0}' -f $rows.Count))
$lines.Add('- Evidence class: embedded path string referenced from a containing function'); $lines.Add('')
$lines.Add('| Original source file | Function | IDA name | Xrefs |'); $lines.Add('| --- | ---: | --- | ---: |')
foreach ($group in $grouped) {
  foreach ($row in ($group.Group | Sort-Object func_ea)) {
    $source=[string]$row.source -replace '\\','/'; $name=[string]$row.name -replace '\|','\|'
    $lines.Add(('| {0} | {1} | {2} | {3} |' -f $source,$row.func_ea,$name,$row.xref_count))
  }
}
$lines.Add(''); $lines.Add('This index proves source ownership for listed anchors. Nearby callees require additional control-flow evidence.')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null
[IO.File]::WriteAllLines($OutputPath,$lines,[Text.UTF8Encoding]::new($false))
Write-Host ('Indexed {0} function/source anchors across {1} original source files.' -f $rows.Count,$grouped.Count)
if ($Apply) { Write-Host ('Applied {0} source comments and {1} source-anchor names in IDA.' -f $comments.Count,$renames.Count) }
