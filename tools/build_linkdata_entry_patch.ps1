param(
  [Parameter(Mandatory = $true)]
  [int]$EntryIndex,
  [Parameter(Mandatory = $true)]
  [string]$SplitDirectory,
  [Parameter(Mandatory = $true)]
  [string]$ReplacementDirectory,
  [string]$CatalogPath = "L:\SM2\samurai_warriors_2_empires_recomp\extracted\linkdata_bns_catalog\catalog.csv",
  [string]$PatchRoot = "L:\SM2\samurai_warriors_2_empires_recomp\mods\linkdata_bns_patch",
  [string]$WorkDirectory = "",
  [switch]$CompressLzp2,
  [switch]$DryRunArchive,
  [switch]$RebuildArchive,
  [string]$ArchiveOutputDirectory = "L:\SM2\samurai_warriors_2_empires_recomp\out\modded_linkdata_bns",
  [switch]$Force
)

$ErrorActionPreference = "Stop"

function Copy-TreeFiles([string]$SourceRoot, [string]$DestRoot) {
  $sourcePrefix = $SourceRoot.TrimEnd('\', '/')
  foreach ($file in Get-ChildItem -LiteralPath $sourcePrefix -Recurse -File) {
    $relative = $file.FullName.Substring($sourcePrefix.Length).TrimStart('\', '/')
    $dest = Join-Path $DestRoot $relative
    $parent = Split-Path -Parent $dest
    if ($parent -and !(Test-Path -LiteralPath $parent)) {
      [void][IO.Directory]::CreateDirectory($parent)
    }
    Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
  }
}

function Get-FirstMagic([string]$Path) {
  if (!(Test-Path -LiteralPath $Path)) {
    return ""
  }

  $stream = [IO.File]::OpenRead($Path)
  try {
    $buffer = New-Object byte[] 8
    $read = $stream.Read($buffer, 0, $buffer.Length)
    if ($read -le 0) {
      return ""
    }
    $builder = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $read; $i++) {
      $c = $buffer[$i]
      [void]$builder.Append($(if ($c -ge 32 -and $c -le 126) { [char]$c } else { "." }))
    }
    return $builder.ToString()
  }
  finally {
    $stream.Dispose()
  }
}

if ($DryRunArchive -and $RebuildArchive) {
  throw "Use either -DryRunArchive or -RebuildArchive, not both."
}

if (!(Test-Path -LiteralPath $SplitDirectory)) {
  throw "Missing split directory: $SplitDirectory"
}
if (!(Test-Path -LiteralPath $ReplacementDirectory)) {
  throw "Missing replacement directory: $ReplacementDirectory"
}
if (!(Test-Path -LiteralPath $CatalogPath)) {
  throw "Missing LINKDATA_BNS catalog: $CatalogPath"
}

$resolvedSplit = (Resolve-Path -LiteralPath $SplitDirectory).Path
$resolvedReplacement = (Resolve-Path -LiteralPath $ReplacementDirectory).Path
$resolvedCatalog = (Resolve-Path -LiteralPath $CatalogPath).Path

$splitCatalogPath = Join-Path $resolvedSplit "catalog.csv"
if (!(Test-Path -LiteralPath $splitCatalogPath)) {
  throw "Missing split catalog: $splitCatalogPath"
}

if (!$WorkDirectory) {
  $WorkDirectory = Join-Path "L:\SM2\samurai_warriors_2_empires_recomp\extracted\mod_build" ("entry_{0:D4}" -f $EntryIndex)
}
if (!(Test-Path -LiteralPath $WorkDirectory)) {
  [void][IO.Directory]::CreateDirectory($WorkDirectory)
}

$workSplit = Join-Path $WorkDirectory "split_overlay"
if (!(Test-Path -LiteralPath $workSplit)) {
  [void][IO.Directory]::CreateDirectory($workSplit)
}
Copy-TreeFiles $resolvedSplit $workSplit

$splitRows = Import-Csv -LiteralPath $splitCatalogPath
$splitByLeaf = @{}
foreach ($row in $splitRows) {
  $leaf = Split-Path $row.RelativePath -Leaf
  if ($splitByLeaf.ContainsKey($leaf)) {
    throw "Split catalog has duplicate leaf name '$leaf'. Use a more specific patch workflow."
  }
  $splitByLeaf[$leaf] = $row.RelativePath
}

$matched = New-Object System.Collections.Generic.List[object]
$unmatched = New-Object System.Collections.Generic.List[string]
foreach ($file in Get-ChildItem -LiteralPath $resolvedReplacement -Recurse -File) {
  if ($file.Name -eq "catalog.csv" -or $file.Name -like "*.obj" -or $file.Name -like "*.csv") {
    continue
  }

  if (!$splitByLeaf.ContainsKey($file.Name)) {
    [void]$unmatched.Add($file.FullName)
    continue
  }

  $relative = $splitByLeaf[$file.Name]
  $dest = Join-Path $workSplit $relative
  $parent = Split-Path -Parent $dest
  if ($parent -and !(Test-Path -LiteralPath $parent)) {
    [void][IO.Directory]::CreateDirectory($parent)
  }
  Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
  [void]$matched.Add([pscustomobject]@{
    Source = $file.FullName
    RelativePath = $relative
    Bytes = $file.Length
  })
}

if ($matched.Count -eq 0) {
  throw "No replacement files matched the split catalog in $resolvedSplit"
}

$archiveRows = Import-Csv -LiteralPath $resolvedCatalog
$entryRow = $archiveRows | Where-Object { [int]$_.Index -eq $EntryIndex } | Select-Object -First 1
if (!$entryRow) {
  throw "Entry $EntryIndex was not found in $resolvedCatalog"
}

$packedContainer = Join-Path $WorkDirectory ("entry_{0:D4}.repacked.offset_table.bin" -f $EntryIndex)
$packArgs = @{
  SplitDirectory = $workSplit
  OutputPath = $packedContainer
}
if ($Force) {
  $packArgs["Force"] = $true
}
$packResult = & (Join-Path $PSScriptRoot "pack_offset_table_container.ps1") @packArgs

$patchPath = Join-Path $PatchRoot $entryRow.RelativePath
$patchParent = Split-Path -Parent $patchPath
if ($patchParent -and !(Test-Path -LiteralPath $patchParent)) {
  [void][IO.Directory]::CreateDirectory($patchParent)
}

if ($CompressLzp2) {
  $compressArgs = @{
    InputPath = $packedContainer
    OutputPath = $patchPath
  }
  if ($Force) {
    $compressArgs["Force"] = $true
  }
  $patchResult = & (Join-Path $PSScriptRoot "compress_lzp2_literal.ps1") @compressArgs
} else {
  if ((Test-Path -LiteralPath $patchPath) -and !$Force) {
    throw "Patch output already exists, pass -Force to overwrite: $patchPath"
  }
  Copy-Item -LiteralPath $packedContainer -Destination $patchPath -Force
  $patchResult = [pscustomobject]@{
    Input = $packedContainer
    Output = $patchPath
    TotalBytes = (Get-Item -LiteralPath $patchPath).Length
  }
}

$archiveResult = $null
if ($DryRunArchive -or $RebuildArchive) {
  $repackArgs = @{
    CatalogPath = $resolvedCatalog
    PatchRoot = $PatchRoot
    OutputDirectory = $ArchiveOutputDirectory
  }
  if ($DryRunArchive) {
    $repackArgs["DryRun"] = $true
  }
  if ($Force) {
    $repackArgs["Force"] = $true
  }
  $archiveResult = & (Join-Path $PSScriptRoot "repack_linkdata_bns.ps1") @repackArgs
}

$reportPath = Join-Path $WorkDirectory ("entry_{0:D4}.patch_report.csv" -f $EntryIndex)
$matched | Export-Csv -LiteralPath $reportPath -NoTypeInformation

[pscustomobject]@{
  EntryIndex = $EntryIndex
  EntryRelativePath = $entryRow.RelativePath
  OriginalEntryMagic = $entryRow.Magic8
  OriginalEntrySize = [uint32]$entryRow.Size
  Replacements = $matched.Count
  UnmatchedReplacementFiles = $unmatched.Count
  WorkSplitDirectory = $workSplit
  PackedContainer = $packedContainer
  PackedContainerBytes = (Get-Item -LiteralPath $packedContainer).Length
  PatchPath = $patchPath
  PatchMagic = Get-FirstMagic $patchPath
  PatchBytes = (Get-Item -LiteralPath $patchPath).Length
  CompressLzp2 = [bool]$CompressLzp2
  ReplacementReport = $reportPath
  ArchiveAction = $(if ($DryRunArchive) { "dry_run" } elseif ($RebuildArchive) { "rebuilt" } else { "none" })
  ArchivePatchedEntries = $(if ($archiveResult) { $archiveResult.PatchedEntries } else { $null })
  ArchiveOutputBytes = $(if ($archiveResult) { $archiveResult.OutputBytes } else { $null })
}
